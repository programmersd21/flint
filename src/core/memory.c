/* SPDX-License-Identifier: MIT */
/*
 * Allocation and the garbage collector: non-moving, stop-the-world,
 * mark and sweep.
 *
 * Objects are never moved, so nothing in the VM holds a raw interior pointer
 * that a collector could invalidate. The one place that comes close is an open
 * upvalue, which points into the value stack, and that is handled by closing
 * upvalues before the frame goes away.
 *
 * Running out of memory is not recoverable and is not worth pretending
 * otherwise, so the collector panics on allocation failure.
 */
#include "config.h"
#include "memory.h"
#include "chunk.h"
#include "compiler.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>

static void mark_value(VM *vm, Value value);

/*
 * Should this allocation trigger a collection?
 *
 * Two tests, and both are load-bearing. The size cutoff is the large-allocation
 * exemption in config.h: one big buffer says nothing about how many small
 * objects are alive, and counting it makes the next small allocation collect a
 * heap that has nothing to do with the buffer. The bytes_allocated test comes
 * second, because next_gc is only meaningful once the accounting runs.
 *
 * Kept out of fl_reallocate's body only because it needs to run *before* the
 * realloc, while the caller is still free to grow the counter.
 */
static bool should_collect(VM *vm, size_t old_size, size_t new_size)
{
	/*
	 * Growth only.
	 *
	 * A free can push the heap below next_gc -- sweeping does exactly
	 * that -- and collecting at that moment is collecting *because* we
	 * freed, which re-enters mark_roots over a runtime that is halfway
	 * through tearing itself down. At exit that walks the globals table
	 * after vm_free has already freed it, and ASan reports a
	 * use-after-free inside the collector.
	 *
	 * The threshold is only ever crossed by growth, so nothing is lost by
	 * requiring it.
	 */
	if (new_size <= old_size)
		return false;
	if (vm->counters.bytes <= vm->next_gc)
		return false;
	if (new_size - old_size > FL_GC_UNCOUNTED_ABOVE)
		return false;
	/*
	 * FL_GC_STRESS collects on every *allocation* regardless of the
	 * threshold. A missing GC root almost always shows up as a
	 * use-after-free within a few allocations rather than never, and
	 * `make stress` is the only build that defines it.
	 */
	return true;
}

/*
 * The one allocator. Also the GC trigger, which is why every growing
 * array in the VM goes through it: the threshold is only checked on
 * allocation, so a collection happens when memory is actually needed.
 *
 * A vm of NULL means no collector, which is what the unit tests use.
 */
void *fl_reallocate(VM *vm, void *pointer, size_t old_size, size_t new_size)
{
	if (vm != NULL) {
		/*
		 * Accounting before the realloc, because the collection this
		 * may trigger runs reentrantly and anything the realloc would
		 * have done to the counter afterwards would be attributed to
		 * the wrong allocation.
		 *
		 * bytes_allocated stays as a field rather than becoming a
		 * macro over counters.bytes because collect_garbage() and
		 * free_objects() both read it and neither should have to know
		 * how the number is spelled.
		 */
		size_t delta = new_size - old_size;
		if (new_size > old_size)
			vm->counters.allocations++;
		else if (new_size == 0)
			vm->counters.frees++;

		if (should_collect(vm, old_size, new_size))
			collect_garbage(vm);

		/*
		 * Applied after the collection, not before. free_object()
		 * goes through here too, so the bytes a sweep reclaims come
		 * straight back off the total, and the threshold stays
		 * measured against what is actually live. Doing it the other
		 * way round -- add, then collect -- would let the collector
		 * see a heap one allocation larger than it really is and
		 * would leave the total permanently inflated by whatever the
		 * last sweep reclaimed.
		 */
		vm->counters.bytes += delta;
		vm->bytes_allocated = vm->counters.bytes;
		if (vm->counters.bytes > vm->counters.peak_bytes)
			vm->counters.peak_bytes = vm->counters.bytes;
	}

	if (new_size == 0) {
		free(pointer);
		return NULL;
	}

	void *result = realloc(pointer, new_size);
	if (result == NULL) {
		fprintf(stderr, "Out of memory.\n");
		exit(1);
	}
	return result;
}

/*
 * Grey an object and queue it for tracing. The mark bit makes this
 * idempotent, so a shared object is queued once and an object graph with
 * cycles terminates.
 *
 * This is the only correct way to mark something. is_marked = true on its own
 * survives the object but not what it references, because nothing ever
 * blackens it.
 */
void mark_object(VM *vm, Obj *object)
{
	if (object == NULL || object->is_marked)
		return;

	object->is_marked = true;

	if (vm->gray_capacity < vm->gray_count + 1) {
		vm->gray_capacity = GROW_CAPACITY(vm->gray_capacity);
		/*
		 * Plain realloc, deliberately. Going through fl_reallocate
		 * here would let growing the gray stack trigger a collection
		 * in the middle of a collection.
		 */
		vm->gray_stack = (Obj **)realloc(
		        vm->gray_stack, sizeof(Obj *) * vm->gray_capacity);
		if (vm->gray_stack == NULL) {
			fprintf(stderr, "Out of memory in gray stack.\n");
			exit(1);
		}
	}

	vm->gray_stack[vm->gray_count++] = object;
}

static void mark_value(VM *vm, Value value)
{
	if (IS_OBJ(value))
		mark_object(vm, AS_OBJ(value));
}

static void mark_array(VM *vm, ValueArray *array)
{
	for (int i = 0; i < array->count; i++)
		mark_value(vm, array->values[i]);
}

/*
 * Mark everything one object points at. This is where the collector needs to
 * know about every new object type: add a case here or the new type's
 * children get swept out from under it.
 */
static void blacken_object(VM *vm, Obj *object)
{
	switch (object->type) {
	case OBJ_STRING:
		/*
		 * A flexible array member of bytes, and a C function pointer for
		 * OBJ_NATIVE. Neither is a heap pointer the collector traces,
		 * so they share the one empty arm.
		 *
		 * Strings used to also be keys in vm->strings, which the table
		 * marks as a root. Runtime strings are not interned any more,
		 * so most of them are in no table at all and are reached
		 * exactly once, from whatever holds them.
		 */
	case OBJ_NATIVE:
		break;
	case OBJ_UPVALUE:
		mark_value(vm, ((ObjUpvalue *)object)->closed);
		break;
	case OBJ_FUNCTION: {
		ObjFunction *function = (ObjFunction *)object;
		mark_object(vm, (Obj *)function->name);
		mark_array(vm, &function->chunk.constants);
		break;
	}
	case OBJ_CLOSURE: {
		ObjClosure *closure = (ObjClosure *)object;
		mark_object(vm, (Obj *)closure->function);
		for (int i = 0; i < closure->upvalue_count; i++)
			mark_object(vm, (Obj *)closure->upvalues[i]);
		break;
	}
	case OBJ_LIST: {
		ObjList *list = (ObjList *)object;
		for (int i = 0; i < list->count; i++)
			mark_value(vm, list->items[i]);
		break;
	}
	case OBJ_TABLE: {
		ObjTable *table = (ObjTable *)object;
		for (int i = 0; i < table->count; i++) {
			mark_object(vm, (Obj *)table->keys[i]);
			mark_value(vm, table->values[i]);
		}
		break;
	}
	}
}

static void mark_table(VM *vm, Table *table)
{
	for (int i = 0; i < table->capacity; i++) {
		Entry *entry = &table->entries[i];
		mark_object(vm, (Obj *)entry->key);
		mark_value(vm, entry->value);
	}
}

/*
 * Everything the collector can reach without being pointed to. If you add a
 * place the VM stashes an object, add it here.
 */
static void mark_roots(VM *vm)
{
	/* the value stack, up to the live top */
	for (Value *slot = vm->stack; slot < vm->stack_top; slot++)
		mark_value(vm, *slot);

	/* the function running in each frame */
	for (int i = 0; i < vm->frame_count; i++)
		mark_object(vm, (Obj *)vm->frames[i].closure);

	/* captured locals still pointing into the stack */
	for (ObjUpvalue *upvalue = vm->open_upvalues; upvalue != NULL;
	        upvalue = upvalue->next) {
		mark_object(vm, (Obj *)upvalue);
	}

	/* top-level variables */
	mark_table(vm, &vm->globals);

	/*
	 * functions being compiled right now. They are not on the stack and
	 * not in any table, so without this a collection in the middle of a
	 * compile frees the half-built function.
	 */
	compiler_mark_roots(vm);
}

/*
 * Drain the gray stack iteratively. An explicit worklist rather than
 * recursion, so a deep list nesting does not blow the C stack.
 */
static void trace_references(VM *vm)
{
	while (vm->gray_count > 0) {
		Obj *object = vm->gray_stack[--vm->gray_count];
		blacken_object(vm, object);
	}
}

/*
 * Walk every object. Marked ones survive and get their mark bit cleared for
 * the next cycle; the rest are unlinked and freed. Unlinking while walking
 * is fine because we saved the next pointer first.
 */
static void sweep(VM *vm)
{
	Obj *previous = NULL;
	Obj *object = vm->objects;

	while (object != NULL) {
		if (object->is_marked) {
			object->is_marked = false;
			previous = object;
			object = object->next;
		} else {
			Obj *unreached = object;
			object = object->next;
			if (previous != NULL)
				previous->next = object;
			else
				vm->objects = object;

			vm->counters.gc_swept++;
			free_object(vm, unreached);
		}
	}
}

void collect_garbage(VM *vm)
{
	if (vm == NULL)
		return;

	uint64_t started = fl_now_ns();

	mark_roots(vm);
	vm->counters.gc_cycles++;
	vm->counters.gc_visited += (uint64_t)vm->gray_count;
	trace_references(vm);

	/*
	 * Drop dead keys from the intern table before the sweep, not after.
	 * The table holds weak references, so sweeping first would leave it
	 * pointing at memory that has already been freed.
	 */
	table_remove_white(&vm->strings);

	/* measured here, before the sweep, because sweep() frees through
	 * fl_reallocate() which has its own accounting. the delta below is
	 * what the collector itself reclaimed, not what the program did. */
	size_t before = vm->counters.bytes;
	sweep(vm);
	vm->counters.gc_freed_bytes += vm->counters.bytes - before;

	vm->counters.gc_ns += fl_now_ns() - started;

	vm->next_gc = vm->counters.bytes * FL_GC_HEAP_GROW_FACTOR;
	if (vm->next_gc < FL_GC_FIRST_THRESHOLD)
		vm->next_gc = FL_GC_FIRST_THRESHOLD;
}

/*
 * Free one object and everything hanging off it. Sizes must match what
 * allocate_object() asked for or realloc misbehaves; that is why the string
 * case recomputes sizeof(ObjString) + length + 1 instead of using sizeof
 * alone.
 */
void free_object(VM *vm, Obj *object)
{
	switch (object->type) {
	case OBJ_STRING: {
		ObjString *string = (ObjString *)object;
		fl_reallocate(
		        vm, object, sizeof(ObjString) + string->length + 1, 0);
		break;
	}
	case OBJ_FUNCTION: {
		ObjFunction *function = (ObjFunction *)object;
		chunk_free(vm, &function->chunk);
		fl_reallocate(vm, object, sizeof(ObjFunction), 0);
		break;
	}
	case OBJ_NATIVE: {
		fl_reallocate(vm, object, sizeof(ObjNative), 0);
		break;
	}
	case OBJ_CLOSURE: {
		ObjClosure *closure = (ObjClosure *)object;
		FREE_ARRAY(vm,
		        ObjUpvalue *,
		        closure->upvalues,
		        closure->upvalue_count);
		fl_reallocate(vm, object, sizeof(ObjClosure), 0);
		break;
	}
	case OBJ_UPVALUE: {
		fl_reallocate(vm, object, sizeof(ObjUpvalue), 0);
		break;
	}
	case OBJ_LIST: {
		ObjList *list = (ObjList *)object;
		FREE_ARRAY(vm, Value, list->items, list->capacity);
		fl_reallocate(vm, object, sizeof(ObjList), 0);
		break;
	}
	case OBJ_TABLE: {
		ObjTable *table = (ObjTable *)object;
		FREE_ARRAY(vm, ObjString *, table->keys, table->capacity);
		FREE_ARRAY(vm, Value, table->values, table->capacity);
		fl_reallocate(vm, object, sizeof(ObjTable), 0);
		break;
	}
	}
}

/* shutdown. frees everything that is left, collector or no collector. */
void free_objects(VM *vm)
{
	Obj *object = vm->objects;
	while (object != NULL) {
		Obj *next = object->next;
		free_object(vm, object);
		object = next;
	}
	free(vm->gray_stack);
	vm->gray_stack = NULL;
	vm->gray_capacity = 0;
	vm->gray_count = 0;
}
