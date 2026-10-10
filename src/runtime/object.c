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
#include "profile.h"
#include "table.h"
#include "value.h"
#include "vm.h"

#include <assert.h>
#include <stdint.h>
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
	if (vm != NULL)
		vm->counters.objects++;

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
 *
 * Only interned strings need a hash now -- it is the intern table's probe
 * function -- so new_string() does not compute one. That is part of why
 * building a string at run time got cheaper.
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

bool fl_bytes_are_ascii(const char *chars, int length)
{
	/* word-at-a-time, because this runs on every string the program makes
	 * and a byte loop would show up. The load is safe because the caller
	 * has at least `length` valid bytes and the read stops at the first
	 * non-ASCII byte, which is by definition within them. */
	const uint8_t *p = (const uint8_t *)chars;
	while (length >= (int)sizeof(uint64_t)) {
		uint64_t word;
		memcpy(&word, p, sizeof word);
		if ((word & UINT64_C(0x8080808080808080)) != 0)
			return false;
		p += sizeof word;
		length -= (int)sizeof(uint64_t);
	}
	while (length-- > 0) {
		if (*p++ >= 0x80)
			return false;
	}
	return true;
}

bool fl_strings_equal(const ObjString *a, const ObjString *b)
{
	if (a == b)
		return true;
	if (a->length != b->length)
		return false;
	/*
	 * memcmp, not a loop. The compiler turns a short fixed-length memcmp
	 * into a couple of loads and compares, and a loop has a branch per
	 * byte, so this is the version that matters for the one-byte and
	 * two-byte strings that most string operations produce.
	 *
	 * Length zero is the case worth calling out: memcmp with a zero
	 * length is defined to return zero, which is the right answer here,
	 * and it is reachable -- slicing to an empty string is legal.
	 */
	return memcmp(a->chars, b->chars, (size_t)a->length) == 0;
}

/*
 * Allocate, copy, and optionally intern.
 *
 * `intern` is a parameter rather than two near-identical functions because
 * everything except the last six lines is shared, and a duplicated
 * constructor is a duplicated bug. See the two public wrappers for which one a
 * caller should reach for.
 *
 * The string is pushed on the stack across the table_set() call because that
 * call can trigger a collection, and an unrooted new object does not survive
 * one.
 */
static ObjString *allocate_string(
        VM *vm, const char *chars, int length, bool intern)
{
	/* length is signed, and every caller passes a strlen or a compile-time
	 * literal. The assert is here because a negative one would size the
	 * allocation as sizeof(ObjString) - n and write below the chars
	 * member, which the analyser cannot rule out from the signature. */
	assert(length >= 0 && "string length must not be negative");

	ObjString *string = (ObjString *)allocate_object(
	        vm, sizeof(ObjString) + (size_t)length + 1, OBJ_STRING);
	string->length = length;
	/*
	 * Only an interned string is ever looked up by hash, so only an
	 * interned string pays for a hash. This used to be unconditional and
	 * was one of the two costs that made building a string at run time
	 * slower than it needed to be.
	 */
	string->hash = intern ? hash_string(chars, length) : 0;
	string->flags =
	        (uint8_t)(fl_bytes_are_ascii(chars, length) ? FL_STRING_ASCII
		                                            : 0);
	memcpy(string->chars, chars, length);
	string->chars[length] = '\0';

	if (vm != NULL) {
		vm->counters.strings_created++;
		vm->counters.string_bytes += (uint64_t)length;
		if (intern) {
			string->flags |= FL_STRING_INTERNED;
			/* root before the table_set, which can collect */
			vm_push(vm, STR_VAL(string));
			table_set(vm, &vm->strings, string, NIL_VAL);
			vm_pop(vm);
		}
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
	ObjString *result = allocate_string(vm, chars, length, false);
	fl_reallocate(vm, chars, length + 1, 0);
	return result;
}

/*
 * The fast path for a string that is about to be built from two others.
 *
 * The length-1 and length-0 cases are the ones that actually occur. Every
 * concatenation of a number calls str(), which produces one digit, and every
 * element of a split produces a short piece, so if concatenation is going to
 * avoid an allocation at all it is going to do it here. Interning does not
 * help: the operand cannot be found in the intern table unless it was put
 * there, and putting it there is the cost being avoided.
 *
 * The caller has to do the rooting: pushing `a` is not enough if the buffer
 * allocation below collects, because `b` is only reachable through `a`.
 */
ObjString *concat_strings(VM *vm, const ObjString *a, const ObjString *b)
{
	int length = a->length + b->length;

	if (length <= FL_SMALL_STRING_MAX) {
		/* one allocation: the string and its bytes together */
		ObjString *result = (ObjString *)allocate_object(
		        vm, sizeof(ObjString) + (size_t)length + 1, OBJ_STRING);
		result->length = length;
		result->hash = 0;
		result->flags = (uint8_t)((FL_IS_ASCII(a) && FL_IS_ASCII(b))
		                                  ? FL_STRING_ASCII
		                                  : 0);
		memcpy(result->chars, a->chars, (size_t)a->length);
		memcpy(result->chars + a->length, b->chars, (size_t)b->length);
		result->chars[length] = '\0';
		if (vm != NULL) {
			vm->counters.strings_created++;
			vm->counters.string_bytes += (uint64_t)length;
		}
		return result;
	}

	int a_length = a->length;
	char *chars = (char *)fl_reallocate(vm, NULL, 0, (size_t)length + 1);
	memcpy(chars, a->chars, (size_t)a_length);
	memcpy(chars + a_length, b->chars, (size_t)b->length);
	return take_string(vm, chars, length);
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
		if (interned != NULL) {
			vm->counters.intern_hits++;
			return interned;
		}
		vm->counters.intern_misses++;
	}
	return allocate_string(vm, chars, length, true);
}

ObjString *new_string(VM *vm, const char *chars, int length)
{
	return allocate_string(vm, chars, length, false);
}

ObjFunction *new_function(VM *vm)
{
	ObjFunction *function = (ObjFunction *)allocate_object(
	        vm, sizeof(ObjFunction), OBJ_FUNCTION);
	function->arity = 0;
	function->upvalue_count = 0;
	function->name = NULL;
	function->call_count = 0;
	function->loop_count = 0;
	chunk_init(&function->chunk);
	return function;
}

ObjNative *new_native_with_data(
        VM *vm, NativeFn function, int arity, void *user_data)
{
	ObjNative *native =
	        (ObjNative *)allocate_object(vm, sizeof(ObjNative), OBJ_NATIVE);
	native->function = function;
	native->arity = arity;
	native->user_data = user_data;
	return native;
}

ObjNative *new_native(VM *vm, NativeFn function, int arity)
{
	return new_native_with_data(vm, function, arity, NULL);
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
	/* the module being compiled, which is the one whose globals the
	 * body will resolve names against */
	closure->module = vm != NULL ? vm->globals : NULL;
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
	if (vm != NULL)
		vm->counters.lists_created++;
	return list;
}

ObjEnumType *new_enum_type(VM *vm, ObjString *name, int variant_count)
{
	ObjEnumType *type = (ObjEnumType *)allocate_object(
	        vm, sizeof(ObjEnumType), OBJ_ENUM_TYPE);
	type->name = name;
	type->variant_count = variant_count;
	table_init(&type->variants);
	type->has_payload = NULL;
	if (variant_count > 0) {
		type->has_payload = ALLOCATE(vm, bool, (size_t)variant_count);
		for (int i = 0; i < variant_count; i++)
			type->has_payload[i] = false;
	}
	return type;
}

ObjEnumValue *new_enum_value(
        VM *vm, ObjEnumType *enum_type, int tag, Value payload)
{
	ObjEnumValue *value = (ObjEnumValue *)allocate_object(
	        vm, sizeof(ObjEnumValue), OBJ_ENUM_VALUE);
	value->enum_type = enum_type;
	value->tag = tag;
	value->payload = payload;
	return value;
}

ObjEnumType *flint_register_enum(VM *vm,
        ObjString *name,
        ObjString **variants,
        bool *has_payload,
        int variant_count)
{
	/* a struct and an enum cannot share a name: both are looked up by
	 * the same `Name` at a call site, and the one found first would
	 * silently win. */
	Value existing;
	if (table_get(&vm->struct_types, name, &existing))
		return NULL;
	ObjEnumType *type = new_enum_type(vm, name, variant_count);
	/* rooted across the insertions below: each table_set can allocate,
	 * and the variant names are held only by this table */
	vm_push(vm, OBJ_VAL(type));
	for (int i = 0; i < variant_count; i++) {
		table_set(vm,
		        &type->variants,
		        variants[i],
		        NUMBER_VAL((double)i));
		if (type->has_payload != NULL)
			type->has_payload[i] = has_payload[i];
	}
	vm_pop(vm);
	table_set(vm, &vm->struct_types, name, OBJ_VAL(type));
	return type;
}

ObjEnumType *flint_lookup_enum(VM *vm, ObjString *name)
{
	Value type;
	if (!table_get(&vm->struct_types, name, &type))
		return NULL;
	return IS_ENUM_TYPE(type) ? AS_ENUM_TYPE(type) : NULL;
}

ObjStructCtor *new_struct_ctor(VM *vm, ObjString *name)
{
	ObjStructCtor *ctor = (ObjStructCtor *)allocate_object(
	        vm, sizeof(ObjStructCtor), OBJ_STRUCT_CTOR);
	ctor->name = name;
	return ctor;
}

ObjTable *flint_register_struct(
        VM *vm, ObjString *name, ObjString **fields, int field_count)
{
	Value existing;
	if (table_get(&vm->struct_types, name, &existing))
		return NULL;
	ObjTable *shape = new_flint_table(vm);
	table_set(vm, &vm->struct_types, name, OBJ_VAL(shape));
	/* rooted across the field insertions below: growing the parallel
	 * arrays allocates, and the collector does not scan a C local */
	vm_push(vm, OBJ_VAL(shape));
	ObjString *count_key = copy_string(vm, "*count", 6);
	vm_push(vm, STR_VAL(count_key));
	/* one entry per field plus the count, filled directly: a Flint-level
	 * table is parallel arrays, not the internal Table this file's
	 * table_set() writes. */
	int total = field_count + 1;
	shape->keys = ALLOCATE(vm, ObjString *, (size_t)total);
	shape->values = ALLOCATE(vm, Value, (size_t)total);
	shape->capacity = total;
	for (int i = 0; i < field_count; i++) {
		shape->keys[i] = fields[i];
		shape->values[i] = NUMBER_VAL((double)i);
	}
	shape->keys[field_count] = count_key;
	shape->values[field_count] = NUMBER_VAL((double)field_count);
	shape->count = total;
	vm_pop(vm);
	vm_pop(vm);
	return shape;
}

ObjTable *flint_lookup_struct(VM *vm, ObjString *name)
{
	Value shape;
	if (!table_get(&vm->struct_types, name, &shape))
		return NULL;
	return AS_FLINT_TABLE(shape);
}

ObjTable *new_flint_table(VM *vm)
{
	ObjTable *table =
	        (ObjTable *)allocate_object(vm, sizeof(ObjTable), OBJ_TABLE);
	table->count = 0;
	table->capacity = 0;
	table->keys = NULL;
	table->values = NULL;
	/* a plain table until a struct constructor stamps it */
	table->struct_name = NULL;
	if (vm != NULL)
		vm->counters.tables_created++;
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
	/* an enum type names itself; a value reports the enum it belongs
	 * to, which is the question a script is asking when it prints one */
	if (IS_ENUM_TYPE(value))
		return AS_ENUM_TYPE(value)->name != NULL
		               ? AS_ENUM_TYPE(value)->name->chars
			       : "enum";
	if (IS_ENUM_VALUE(value))
		return AS_ENUM_VALUE(value)->enum_type != NULL &&
		                       AS_ENUM_VALUE(value)->enum_type->name !=
		                               NULL
		               ? AS_ENUM_VALUE(value)->enum_type->name->chars
			       : "enum";
	if (IS_FLINT_TABLE(value)) {
		/*
		 * A struct reports its own name. `type(point)` is "Point" --
		 * which is the question a script is asking when it calls type on
		 * a struct, and "table" would answer a different one.
		 */
		ObjString *name = AS_FLINT_TABLE(value)->struct_name;
		return name != NULL ? name->chars : "table";
	}
	/* a function, a closure and a native are all "function" to a script,
	 * and a cast has no reason to be more particular than type() is */
	if (IS_FUNCTION(value) || IS_CLOSURE(value) || IS_NATIVE(value))
		return "function";
	return "unknown";
}

const char *flint_type_name_of(FlTypeTag type)
{
	switch (type) {
	case FL_INT_TYPE_NUMBER:
		return "number";
	case FL_INT_TYPE_STRING:
		return "string";
	case FL_INT_TYPE_BOOL:
		return "bool";
	case FL_INT_TYPE_NIL:
		return "nil";
	case FL_INT_TYPE_LIST:
		return "list";
	case FL_INT_TYPE_TABLE:
		return "table";
	case FL_INT_TYPE_FUNCTION:
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
bool value_has_type(Value value, FlTypeTag type)
{
	switch (type) {
	case FL_INT_TYPE_NUMBER:
		return IS_NUMBER(value);
	case FL_INT_TYPE_STRING:
		return IS_STRING(value);
	case FL_INT_TYPE_BOOL:
		return IS_BOOL(value);
	case FL_INT_TYPE_NIL:
		return IS_NIL(value);
	case FL_INT_TYPE_LIST:
		return IS_LIST(value);
	case FL_INT_TYPE_TABLE:
		return IS_FLINT_TABLE(value);
	case FL_INT_TYPE_FUNCTION:
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
static void print_object_to(FILE *out, Value value);

static void print_value_to(FILE *out, Value value)
{
	if (IS_NUMBER(value)) {
		/*
		 * fl_double_to_text() rather than a third copy of the rule.
		 * print() at the top level and print() of a nested value each
		 * had their own spelling of a double before, and they
		 * disagreed: the top-level one tried 15, 16, then 17 digits
		 * until the text read back identically, the nested one tried
		 * 15 then jumped to 17. str(1/3) printed 17 digits while
		 * print(1/3) printed 16.
		 */
		char buf[64];
		int n = fl_double_to_text(AS_NUMBER(value), buf, sizeof(buf));
		if (n > 0 && n < (int)sizeof(buf))
			buf[n] = '\0';
		fprintf(out, "%s", buf);
		return;
	}
	if (IS_BOOL(value)) {
		fprintf(out, "%s", AS_BOOL(value) ? "true" : "false");
		return;
	}
	if (IS_NIL(value)) {
		fprintf(out, "nil");
		return;
	}
	if (IS_OBJ(value)) {
		print_object_to(out, value);
		return;
	}
	fprintf(out, "<unknown>");
}

void print_value(Value value) { print_value_to(stdout, value); }

/*
 * Render a value into a fresh string.
 *
 * Used by `str()` on containers, which is the one place a value has to
 * become text without going to a stream. The output is byte-for-byte what
 * print() would have written, because it *is* the same code with a
 * different FILE*: a second renderer for one language disagrees with the
 * first inside a release, and every caller then disagrees with every other
 * caller.
 *
 * Captured through a tmpfile rather than open_memstream: the latter is
 * glibc-only, and the former is C89. The buffer is malloc'd and
 * NUL-terminated; the caller frees it. NULL on any I/O or allocation
 * failure.
 */
char *flint_value_to_string(VM *vm, Value value)
{
	(void)vm;
	FILE *out = tmpfile();
	if (out == NULL)
		return NULL;
	print_value_to(out, value);
	if (fflush(out) != 0) {
		fclose(out);
		return NULL;
	}
	if (fseek(out, 0, SEEK_END) != 0) {
		fclose(out);
		return NULL;
	}
	long len = ftell(out);
	if (len < 0 || fseek(out, 0, SEEK_SET) != 0) {
		fclose(out);
		return NULL;
	}
	char *buf = malloc((size_t)len + 1);
	if (buf == NULL) {
		fclose(out);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)len, out);
	fclose(out);
	if (got != (size_t)len) {
		free(buf);
		return NULL;
	}
	buf[len] = '\0';
	return buf;
}

/*
 * Print an object. No trailing newline, the caller adds one.
 *
 * Only valid for an object. Numbers, booleans and nil are not objects, and
 * OBJ_TYPE() on one of those reads a pointer that does not exist, so a caller
 * holding a value of unknown kind has to go through print_value() instead.
 */
static void print_object_to(FILE *out, Value value)
{
	switch (OBJ_TYPE(value)) {
	case OBJ_STRING:
		fprintf(out, "%s", AS_CSTRING(value));
		break;
	case OBJ_FUNCTION:
		print_function(AS_FUNCTION(value));
		break;
	case OBJ_NATIVE:
		fprintf(out, "<native fn>");
		break;
	case OBJ_CLOSURE:
		/* a closure prints as the function it wraps */
		print_function(AS_CLOSURE(value)->function);
		break;
	case OBJ_UPVALUE:
		fprintf(out, "<upvalue>");
		break;
	case OBJ_LIST: {
		ObjList *list = AS_LIST(value);
		fprintf(out, "[");
		for (int i = 0; i < list->count; i++) {
			if (i > 0)
				fprintf(out, ", ");
			/*
			 * Strings get quoted here, at the list level, so a
			 * list of them is distinguishable from a list of
			 * bare words. Everything else goes through
			 * print_value_to, which handles numbers, booleans, nil
			 * and nested lists; print_object_to would read a
			 * pointer out of a number and segfault.
			 */
			if (IS_STRING(list->items[i]))
				fprintf(out,
				        "\"%s\"",
				        AS_CSTRING(list->items[i]));
			else
				print_value_to(out, list->items[i]);
		}
		fprintf(out, "]");
		break;
	}
	case OBJ_STRUCT_CTOR: {
		/* the constructor prints as the name it binds, because that
		 * is what a script writes to reach it. */
		fprintf(out,
		        "<struct %s>",
		        AS_STRUCT_CTOR(value)->name != NULL
		                ? AS_STRUCT_CTOR(value)->name->chars
				: "?");
		break;
	}
	case OBJ_ENUM_TYPE: {
		/* the type prints as its name; a value prints as
		 * Enum.Variant, or Enum.Variant(payload) when it carries
		 * one. that is the form a script writes, and a value that
		 * printed as a bare tag would be unreadable in a log. */
		fprintf(out,
		        "<enum %s>",
		        AS_ENUM_TYPE(value)->name != NULL
		                ? AS_ENUM_TYPE(value)->name->chars
				: "?");
		break;
	}
	case OBJ_ENUM_VALUE: {
		ObjEnumValue *ev = AS_ENUM_VALUE(value);
		const char *enum_name =
		        ev->enum_type != NULL && ev->enum_type->name != NULL
		                ? ev->enum_type->name->chars
		                : "?";
		const char *variant = "?";
		if (ev->enum_type != NULL) {
			for (int i = 0; i < ev->enum_type->variants.capacity;
			        i++) {
				Entry *e = &ev->enum_type->variants.entries[i];
				if (e->key == NULL)
					continue;
				if (IS_NUMBER(e->value) &&
				        (int)AS_NUMBER(e->value) == ev->tag) {
					variant = e->key->chars;
					break;
				}
			}
		}
		fprintf(out, "%s.%s", enum_name, variant);
		if (!IS_NIL(ev->payload)) {
			fprintf(out, "(");
			print_value_to(out, ev->payload);
			fprintf(out, ")");
		}
		break;
	}
	case OBJ_TABLE: {
		/*
		 * The contents, in insertion order. This used to print
		 * `<table>`, which made `print(t)` useless for the one
		 * thing people print tables to see.
		 */
		ObjTable *t = AS_FLINT_TABLE(value);
		/*
		 * A struct names itself, so `print(point)` reads as the thing
		 * it is rather than as an anonymous bag. The braces are kept
		 * because a struct is a table and `str()` of one should still
		 * be recognizable as text.
		 */
		if (t->struct_name != NULL)
			fprintf(out, "%s{", t->struct_name->chars);
		else
			fprintf(out, "{");
		for (int i = 0; i < t->count; i++) {
			if (i > 0)
				fprintf(out, ", ");
			fprintf(out, "%s: ", t->keys[i]->chars);
			print_value_to(out, t->values[i]);
		}
		fprintf(out, "}");
		break;
	}
	}
}

void print_object(Value value) { print_object_to(stdout, value); }
