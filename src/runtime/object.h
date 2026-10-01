/* SPDX-License-Identifier: MIT */
/*
 * Heap objects.
 *
 * Every object starts with an Obj header, which is what lets the collector
 * walk vm->objects without knowing the concrete type.
 */
#ifndef FL_OBJECT_H
#define FL_OBJECT_H

#include "chunk.h"
#include "common.h"
#include "value.h"

typedef struct VM VM;
typedef struct Obj Obj;
typedef struct ObjString ObjString;

typedef enum {
	OBJ_STRING,
	OBJ_FUNCTION,
	OBJ_NATIVE,
	OBJ_CLOSURE,
	OBJ_UPVALUE,
	OBJ_LIST,
	OBJ_TABLE
} ObjType;

struct Obj {
	ObjType type;
	bool is_marked;
	struct Obj *next; /* the collector's list of all objects */
};

/*
 * Header and bytes in one allocation. chars is a flexible array member, so
 * the string is copied into space that is already there.
 *
 * flags rather than separate subclasses, because the whole point of the flag
 * is to decide whether a comparison can stop at the first byte. See
 * value_strings_equal() for what each one buys.
 *
 * FL_STRING_INTERNED means "this exact object is registered in vm->strings",
 * which implies two equal interned strings are the same pointer. Uninterned
 * strings carry no such promise, so equality has to compare bytes.
 *
 * FL_STRING_ASCII means every byte is below 0x80. It is set on the way in for
 * the literals the compiler produces and computed for short strings, and it
 * makes the byte scans (upper, lower, index) cheaper: they can work on plain
 * char instead of worrying about multi-byte sequences, and more usefully, a
 * UTF-8 search for an ASCII needle can stop at the first byte >= 0x80.
 */
#define FL_STRING_INTERNED 1u
#define FL_STRING_ASCII    2u

struct ObjString {
	Obj obj;
	int length;
	uint32_t hash;
	uint8_t flags;
	char chars[];
};

/* the flags, as read from a string value. */
#define FL_IS_INTERNED(s) (((s)->flags & FL_STRING_INTERNED) != 0)
#define FL_IS_ASCII(s)    (((s)->flags & FL_STRING_ASCII) != 0)

/*
 * Wrap a string object as a Value, with the type checked.
 *
 * This is the one to use at runtime call sites. STR_VAL in value.h cannot
 * check its argument because it does not know what a string is; by the time
 * the layout is known this can, so a mistake here is a compiler error rather
 * than a value that lies about being a string.
 */
static inline Value fl_str_val(const ObjString *s)
{
	return STR_VAL((void *)(uintptr_t)s);
}

#define STR_VAL(s) fl_str_val(s)

typedef struct {
	Obj obj;
	int arity;
	int upvalue_count;
	Chunk chunk;
	ObjString *name; /* NULL for the top-level script */

	/*
	 * Hotness, and where a compiled body would go.
	 *
	 * call_count and loop_count are read by the profiler and written by
	 * the interpreter on two instructions, which is cheap enough to leave
	 * in a release build: a field in a struct the VM already touched,
	 * rather than a branch on a mode flag at every call site.
	 *
	 * jit_code is a struct pointer rather than void* because the JIT
	 * needs the code pointer, the size, and the deoptimization table
	 * together, and reinterpreting a void* at each use would hide the
	 * only interesting field behind three casts. It is a forward
	 * declaration so the runtime does not have to include jit.h, and
	 * the collector has to learn about it in blacken_object() -- the
	 * compiled code holds no Value references of its own, but a future
	 * specialization cache might, and a missed root is a use-after-free
	 * that only shows up in optimized builds.
	 */
	uint32_t call_count;
	uint32_t loop_count;
	struct FlJitCode *jit_code;
} ObjFunction;

/* a C function exposed to flint. arity of -1 means variadic. */
typedef Value (*NativeFn)(VM *vm, int argc, Value *argv);

typedef struct {
	Obj obj;
	NativeFn function;
	int arity;
} ObjNative;

/*
 * A captured variable. location points into the value stack while the
 * enclosing frame is alive, then at closed once the frame returns. Reads and
 * writes go through location either way, so nothing else has to care.
 */
typedef struct ObjUpvalue {
	Obj obj;
	Value *location;
	Value closed;
	struct ObjUpvalue *next;
} ObjUpvalue;

typedef struct {
	Obj obj;
	ObjFunction *function;
	ObjUpvalue **upvalues;
	int upvalue_count;
} ObjClosure;

typedef struct {
	Obj obj;
	int count;
	int capacity;
	Value *items;
} ObjList;

/*
 * A flint-level table. This is not the Table from table.h, which is the
 * internal hash table behind globals and string interning. They share a name
 * and nothing else.
 */
typedef struct {
	Obj obj;
	int count;
	int capacity;
	ObjString **keys; /* parallel arrays: insertion ordered */
	Value *values;
} ObjTable;

#define AS_OBJ(value)   ((Obj *)AS_OBJ_PTR(value))
#define OBJ_TYPE(value) (AS_OBJ(value)->type)

static inline bool is_obj_type(Value value, ObjType type)
{
	return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

/*
 * Strings answer from the tag alone, with no dereference. Every string
 * primitive and every OP_ADD begins with this test, so the version that reads
 * a type byte out of the heap is the version that shows up in a profile.
 */
static inline bool IS_STRING(Value v)
{
	return fl_is_boxed(v) && fl_tag_of(v) == FL_TAG_STR;
}

#define IS_FUNCTION(value)    is_obj_type(value, OBJ_FUNCTION)
#define IS_NATIVE(value)      is_obj_type(value, OBJ_NATIVE)
#define IS_CLOSURE(value)     is_obj_type(value, OBJ_CLOSURE)
#define IS_LIST(value)        is_obj_type(value, OBJ_LIST)
#define IS_FLINT_TABLE(value) is_obj_type(value, OBJ_TABLE)

#define AS_STRING(value)      ((ObjString *)AS_OBJ_PTR(value))
#define AS_CSTRING(value)     (((ObjString *)AS_OBJ_PTR(value))->chars)
#define AS_FUNCTION(value)    ((ObjFunction *)AS_OBJ_PTR(value))
#define AS_NATIVE(value)      ((ObjNative *)AS_OBJ_PTR(value))
#define AS_CLOSURE(value)     ((ObjClosure *)AS_OBJ_PTR(value))
#define AS_LIST(value)        ((ObjList *)AS_OBJ_PTR(value))
#define AS_FLINT_TABLE(value) ((ObjTable *)AS_OBJ_PTR(value))

/*
 * Are these bytes all below 0x80?
 *
 * Run once per string at creation, over bytes that are about to be copied
 * anyway, so it rides along with an operation the constructor was performing
 * regardless. It is what lets the ASCII fast paths skip UTF-8 reasoning.
 */
bool fl_bytes_are_ascii(const char *chars, int length);

/*
 * Strings up to this length come out of one allocation together with their
 * bytes: no separate buffer, no copy through an intermediate.
 *
 * Not tuned to anything measured. It is the point below which a string's
 * payload is smaller than the bookkeeping an operation needs anyway, so below
 * it the extra step is pure overhead, and above it the copy is a small
 * fraction of the work. Eight is where those two cross.
 */
#define FL_SMALL_STRING_MAX 8

/*
 * Concatenate two strings.
 *
 * Rooting contract: `a` must already be reachable from a GC root when this is
 * called, because the allocation below can collect and `b` is reached only
 * through it.
 */
ObjString *concat_strings(VM *vm, const ObjString *a, const ObjString *b);

/*
 * Copy and intern. This is for strings whose identity matters: identifiers,
 * literals, and anything used as a table key. Two copies of the same bytes
 * give the same pointer, and equality is then a pointer compare.
 *
 * Do not use it for strings a program builds at run time. Interning a string
 * that is used once costs a hash, a probe and an insertion, and buys nothing,
 * because nothing will look the string up again. See new_string().
 */
ObjString *copy_string(VM *vm, const char *chars, int length);

/*
 * Copy, do not intern.
 *
 * This is the right constructor for a string that came out of an operation:
 * concatenation, a slice, str(), parsing, formatting. The bytes are compared
 * for equality when equality is asked for, which costs a length check and a
 * memcmp and is cheaper than the interning this skips.
 *
 * The one thing to be careful about is using the result as a table key.
 * vm->strings and vm->globals both compare keys by pointer, so a key that was
 * never interned is never found. Field names in the bytecode come from
 * constants the compiler interned, so t.name is safe; a key computed at run
 * time must be interned explicitly with copy_string().
 */
ObjString *new_string(VM *vm, const char *chars, int length);

/* takes ownership of chars, which must come from ALLOCATE. frees it either way. */
ObjString *take_string(VM *vm, char *chars, int length);

ObjFunction *new_function(VM *vm);
ObjNative *new_native(VM *vm, NativeFn function, int arity);
ObjClosure *new_closure(VM *vm, ObjFunction *function);
ObjUpvalue *new_upvalue(VM *vm, Value *slot);
ObjList *new_list(VM *vm);
ObjTable *new_flint_table(VM *vm);

/*
 * The name of a value's type, as a string. These are the same seven words
 * `type()` returns, so the language has exactly one vocabulary for types and
 * a cast can quote it in its error message without inventing a second
 * spelling. the returned pointer is a static string: do not free it.
 */
const char *flint_type_name(Value value);

/* the name of a cast target, given the tag the compiler emitted. */
const char *flint_type_name_of(FlType type);

/*
 * Does this value match the tag? The check behind `x as T`.
 *
 * Kept next to the type predicates on purpose: this is where the two agree,
 * and having the mapping written down twice is how a cast ends up accepting
 * something `type()` would call something else.
 */
bool value_has_type(Value value, FlType type);

/* debug printing. no trailing newline. */
void print_object(Value value);

/*
 * The same, but for any value rather than only an object.
 *
 * print_object() switches on OBJ_TYPE(), which reads a pointer out of the
 * value. A number, boolean or nil has no such pointer, so calling it on one
 * is a segfault. Anything that recurses into a container has to go through
 * this instead, or `print([1, 2])` dies on the first element.
 */
void print_value(Value value);

#endif /* FL_OBJECT_H */
