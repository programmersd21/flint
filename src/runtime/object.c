/* SPDX-License-Identifier: MIT */
/*
 * Object construction, string interning, and debug printing.
 *
 * All heap objects are born in allocate_object(). A new object type needs
 * three edits: the struct in object.h, allocate its constructor here, and
 * add cases to blacken_object() and free_object() in memory.c.
 */
#include "object.h"
#include "chunk.h"
#include "memory.h"
#include "stdint.h"
#include "table.h"
#include "value.h"
#include "vm.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Allocate and link into the collector's object list. Linking first and
 * stamping the type second would be faster; the compiler does not reorder
 * the two because table_set() below can allocate, and the object must be
 * fully initialized before anything can reach it.
 */
static Obj *allocate_object(VM *vm, size_t size, ObjType type)
{
	Obj *object = (Obj *)fl_reallocate(vm, NULL, 0, size);
	object->type = type;
	object->is_marked = false;

	if (vm != NULL) {
		object->next = vm->objects;
		vm->objects = object;
	} else {
		object->next = NULL;
	}
	return object;
}

/*
 * FNV-1a. Not cryptographic, not trying to be. It just has to spread
 * short identifier-like strings across the table, and it does that with one
 * multiply per byte.
 */
static uint32_t hash_string(const char *key, int length)
{
	uint32_t hash = 2166136261u;
	for (int i = 0; i < length; i++) {
		hash ^= (uint8_t)key[i];
		hash *= 16777619;
	}
	return hash;
}

/*
 * Copy the bytes and intern. The string is pushed on the stack across the
 * table_set() call because that call can trigger a collection, and an
 * unrooted new object does not survive one.
 */
static ObjString *allocate_string(
        VM *vm, const char *chars, int length, uint32_t hash)
{
	/* length is signed, and every caller passes a strlen or a compile-time
	 * literal. The assert is here because a negative one would size the
	 * allocation as sizeof(ObjString) - n and write below the chars
	 * member, which the analyser cannot rule out from the signature. */
	assert(length >= 0 && "string length must not be negative");

	ObjString *string = (ObjString *)allocate_object(
	        vm, sizeof(ObjString) + (size_t)length + 1, OBJ_STRING);
	string->length = length;
	string->hash = hash;
	memcpy(string->chars, chars, length);
	string->chars[length] = '\0';

	if (vm != NULL) {
		/* root before the table_set, which can collect */
		vm_push(vm, OBJ_VAL(string));
		table_set(vm, &vm->strings, string, NIL_VAL, false);
		vm_pop(vm);
	}

	return string;
}

/*
 * Takes a buffer that came from ALLOCATE. The buffer is freed either way, so
 * this works for the concat path, which already has a heap buffer and would
 * otherwise pay for a second copy. If the bytes are already interned the
 * buffer is thrown away and the old object is returned.
 */
ObjString *take_string(VM *vm, char *chars, int length)
{
	uint32_t hash = hash_string(chars, length);
	if (vm != NULL) {
		ObjString *interned =
		        table_find_string(&vm->strings, chars, length, hash);
		if (interned != NULL) {
			fl_reallocate(vm, chars, length + 1, 0);
			return interned;
		}
	}

	ObjString *result = allocate_string(vm, chars, length, hash);
	fl_reallocate(vm, chars, length + 1, 0);
	return result;
}

/*
 * Copy and intern. Returns the existing object if these bytes have been seen
 * before, which is what makes string equality a pointer compare everywhere
 * else in the VM.
 */
ObjString *copy_string(VM *vm, const char *chars, int length)
{
	uint32_t hash = hash_string(chars, length);
	if (vm != NULL) {
		ObjString *interned =
		        table_find_string(&vm->strings, chars, length, hash);
		if (interned != NULL)
			return interned;
	}
	return allocate_string(vm, chars, length, hash);
}

ObjFunction *new_function(VM *vm)
{
	ObjFunction *function = (ObjFunction *)allocate_object(
	        vm, sizeof(ObjFunction), OBJ_FUNCTION);
	function->arity = 0;
	function->upvalue_count = 0;
	function->name = NULL;
	chunk_init(&function->chunk);
	return function;
}

ObjNative *new_native(VM *vm, NativeFn function, int arity)
{
	ObjNative *native =
	        (ObjNative *)allocate_object(vm, sizeof(ObjNative), OBJ_NATIVE);
	native->function = function;
	native->arity = arity;
	return native;
}

/*
 * The upvalue array is allocated before the closure, not after. The closure
 * allocation can collect, and an array only reachable from a local variable
 * would be swept. The reverse order cannot happen, which is why this looks
 * backwards.
 */
ObjClosure *new_closure(VM *vm, ObjFunction *function)
{
	ObjUpvalue **upvalues =
	        ALLOCATE(vm, ObjUpvalue *, function->upvalue_count);
	for (int i = 0; i < function->upvalue_count; i++)
		upvalues[i] = NULL;

	ObjClosure *closure = (ObjClosure *)allocate_object(
	        vm, sizeof(ObjClosure), OBJ_CLOSURE);
	closure->function = function;
	closure->upvalues = upvalues;
	closure->upvalue_count = function->upvalue_count;
	return closure;
}

/*
 * location points straight into the value stack. The run loop fills in next
 * when it links the upvalue into the open list.
 */
ObjUpvalue *new_upvalue(VM *vm, Value *slot)
{
	ObjUpvalue *upvalue = (ObjUpvalue *)allocate_object(
	        vm, sizeof(ObjUpvalue), OBJ_UPVALUE);
	upvalue->location = slot;
	upvalue->closed = NIL_VAL;
	upvalue->next = NULL;
	return upvalue;
}

/* lists and tables start empty with no buffer. capacity 0 means no array. */
ObjList *new_list(VM *vm)
{
	ObjList *list =
	        (ObjList *)allocate_object(vm, sizeof(ObjList), OBJ_LIST);
	list->count = 0;
	list->capacity = 0;
	list->items = NULL;
	return list;
}

ObjTable *new_flint_table(VM *vm)
{
	ObjTable *table =
	        (ObjTable *)allocate_object(vm, sizeof(ObjTable), OBJ_TABLE);
	table->count = 0;
	table->capacity = 0;
	table->keys = NULL;
	table->values = NULL;
	return table;
}

static void print_function(ObjFunction *function)
{
	if (function->name == NULL) {
		printf("<script>");
		return;
	}
	printf("<fn %s>", function->name->chars);
}

/*
 * The name of a value's type.
 *
 * These are the seven words `type()` returns, and a cast quotes them in its
 * error message. One vocabulary, written down once, so `x as string` and
 * `type(x)` cannot drift apart and start disagreeing about what a string is.
 *
 * String literals rather than interned ObjStrings on purpose: this runs on the
 * error path, and allocating to build a message would mean allocating while
 * the collector is partway through unwinding.
 */
const char *flint_type_name(Value value)
{
	if (IS_NUMBER(value))
		return "number";
	if (IS_BOOL(value))
		return "bool";
	if (IS_NIL(value))
		return "nil";
	if (IS_STRING(value))
		return "string";
	if (IS_LIST(value))
		return "list";
	if (IS_FLINT_TABLE(value))
		return "table";
	/* a function, a closure and a native are all "function" to a script,
	 * and a cast has no reason to be more particular than type() is */
	if (IS_FUNCTION(value) || IS_CLOSURE(value) || IS_NATIVE(value))
		return "function";
	return "unknown";
}

const char *flint_type_name_of(FlType type)
{
	switch (type) {
	case FL_TYPE_NUMBER:
		return "number";
	case FL_TYPE_STRING:
		return "string";
	case FL_TYPE_BOOL:
		return "bool";
	case FL_TYPE_NIL:
		return "nil";
	case FL_TYPE_LIST:
		return "list";
	case FL_TYPE_TABLE:
		return "table";
	case FL_TYPE_FUNCTION:
		return "function";
	}
	/* unreachable: the compiler only emits tags from the enum. returning
	 * something printable beats handing a null pointer to printf */
	return "unknown";
}

/*
 * The check behind `x as T`.
 *
 * Deliberately written with the same predicates in the same order as
 * flint_type_name() above. Two independent lists of "what type is this" is
 * exactly how a cast and a type() end up disagreeing about a value near a
 * boundary.
 */
bool value_has_type(Value value, FlType type)
{
	switch (type) {
	case FL_TYPE_NUMBER:
		return IS_NUMBER(value);
	case FL_TYPE_STRING:
		return IS_STRING(value);
	case FL_TYPE_BOOL:
		return IS_BOOL(value);
	case FL_TYPE_NIL:
		return IS_NIL(value);
	case FL_TYPE_LIST:
		return IS_LIST(value);
	case FL_TYPE_TABLE:
		return IS_FLINT_TABLE(value);
	case FL_TYPE_FUNCTION:
		return IS_FUNCTION(value) || IS_CLOSURE(value) ||
		       IS_NATIVE(value);
	}
	return false;
}

/*
 * Print a value of any kind, without a trailing newline.
 *
 * The reason this exists separately from print_object(): a list can hold
 * numbers, booleans and nil, and print_object() switches on OBJ_TYPE() which
 * dereferences a pointer that those do not have. Routing the elements here
 * is what keeps `print([1, 2])` from segfaulting on the first element.
 *
 * Numbers use the shortest representation that round-trips, matching what
 * print() does at the top level, so a list and a bare value do not disagree
 * about how the same double is spelled.
 */
void print_value(Value value)
{
	if (IS_NUMBER(value)) {
		double d = AS_NUMBER(value);
		if (fl_double_is_printable_int(d)) {
			printf("%ld", fl_double_to_long(d));
			return;
		}
		char buf[64];
		snprintf(buf, sizeof(buf), "%.15g", d);
		if (strtod(buf, NULL) == d) {
			printf("%s", buf);
			return;
		}
		snprintf(buf, sizeof(buf), "%.17g", d);
		printf("%s", buf);
		return;
	}
	if (IS_BOOL(value)) {
		printf("%s", AS_BOOL(value) ? "true" : "false");
		return;
	}
	if (IS_NIL(value)) {
		printf("nil");
		return;
	}
	if (IS_OBJ(value)) {
		print_object(value);
		return;
	}
	printf("<unknown>");
}

/*
 * Print an object. No trailing newline, the caller adds one.
 *
 * Only valid for an object. Numbers, booleans and nil are not objects, and
 * OBJ_TYPE() on one of those reads a pointer that does not exist, so a caller
 * holding a value of unknown kind has to go through print_value() instead.
 */
void print_object(Value value)
{
	switch (OBJ_TYPE(value)) {
	case OBJ_STRING:
		printf("%s", AS_CSTRING(value));
		break;
	case OBJ_FUNCTION:
		print_function(AS_FUNCTION(value));
		break;
	case OBJ_NATIVE:
		printf("<native fn>");
		break;
	case OBJ_CLOSURE:
		/* a closure prints as the function it wraps */
		print_function(AS_CLOSURE(value)->function);
		break;
	case OBJ_UPVALUE:
		printf("<upvalue>");
		break;
	case OBJ_LIST: {
		ObjList *list = AS_LIST(value);
		printf("[");
		for (int i = 0; i < list->count; i++) {
			if (i > 0)
				printf(", ");
			/*
			 * Strings get quoted here, at the list level, so a
			 * list of them is distinguishable from a list of
			 * bare words. Everything else goes through
			 * print_value, which handles numbers, booleans, nil
			 * and nested lists; print_object() would read a
			 * pointer out of a number and segfault.
			 */
			if (IS_STRING(list->items[i]))
				printf("\"%s\"", AS_CSTRING(list->items[i]));
			else
				print_value(list->items[i]);
		}
		printf("]");
		break;
	}
	case OBJ_TABLE:
		/* contents are not printed. add it if you are debugging a table. */
		printf("<table>");
		break;
	}
}
