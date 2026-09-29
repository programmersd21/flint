/* SPDX-License-Identifier: MIT */
/*
 * String primitives for text processing.
 *
 * A separate file from sys.c because these are pure computation and that is
 * a different concern from talking to the operating system. No io here, no
 * process state, nothing that can fail for reasons outside the arguments.
 *
 * Every one of these takes and returns a flint string, never a char*, so the
 * collector sees the results the same way it sees any other. The C code
 * between push and pop is allowed to allocate freely; that is what the roots
 * on the stack are for.
 */
#include "string.h"
#include "memory.h"
#include "object.h"
#include "value.h"
#include "vm.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * every function here starts with this, and it is the reason none of them
 * need to think about garbage collection
 */
#define REQUIRE_STRING(vm, v, name)                                            \
	do {                                                                   \
		if (!IS_STRING(v)) {                                           \
			vm_runtime_error(                                      \
			        vm, "Argument to " name " must be a string."); \
			return NIL_VAL;                                        \
		}                                                              \
	} while (false)

/*
 * The simplest possible copy, and the building block everything else here is
 * made of. A list of bytes to a flint string.
 *
 * The result is not pushed. It is returned, and the caller pushes it
 * immediately, which is the same instruction either way; a helper that
 * returned an unrooted value and left the rooting to its caller would be one
 * more place to forget.
 */
static ObjString *make_string(VM *vm, const char *chars, int length)
{
	return copy_string(vm, chars, length);
}

/*
 * split(s, sep) -> list of strings
 *
 * The workhorse. A line from stdin, a csv row, a log field: this is what a
 * text-processing script spends its time in.
 *
 * An empty separator splits into single characters, because there is no other
 * sensible reading of "split on nothing" and returning the whole string would
 * hide a bug. A separator longer than the string yields the string itself. A
 * string that does not contain the separator yields one element, not zero, so
 * the common case needs no check at the call site.
 */
static Value split_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	REQUIRE_STRING(vm, argv[0], "split()");
	REQUIRE_STRING(vm, argv[1], "split()");

	ObjString *s = AS_STRING(argv[0]);
	ObjString *sep = AS_STRING(argv[1]);

	ObjList *out = new_list(vm);
	vm_push(vm, OBJ_VAL(out)); /* the list, across every allocation below */
	Value *out_stack_base = vm->stack_top; /* strings go above here */

	/* an empty separator: one character per element */
	if (sep->length == 0) {
		for (int i = 0; i < s->length; i++) {
			char c[2] = {s->chars[i], '\0'};
			vm_push(vm, OBJ_VAL(make_string(vm, c, 1)));
		}
		int n = (int)(vm->stack_top - out_stack_base);
		if (n > 0) {
			Value *items = ALLOCATE(vm, Value, (size_t)n);
			for (int k = 0; k < n; k++)
				items[k] = vm->stack_top[-n + k];
			out->items = items;
			out->capacity = n;
			out->count = n;
			vm->stack_top -= n;
		}
		vm_pop(vm); /* the list */
		return OBJ_VAL(out);
	}

	/* the common case: search forward, emit the gap, skip the separator.
	 * memmem is not portable C11 and glibc-specific, so this is a plain
	 * scan. a quadratic search over one line of text is not the thing
	 * that makes a text script slow; allocation is. */
	int start = 0;
	int i = 0;
	while (i <= s->length - sep->length) {
		bool hit = true;
		for (int k = 0; k < sep->length; k++) {
			if (s->chars[i + k] != sep->chars[k]) {
				hit = false;
				break;
			}
		}
		if (hit) {
			vm_push(vm,
			        OBJ_VAL(make_string(
			                vm, s->chars + start, i - start)));
			start = i + sep->length;
			i = start;
		} else {
			i++;
		}
	}

	/* the tail. always emitted, so "a,b" gives two elements and "a"
	 * gives one, and a script never has to distinguish "no separator"
	 * from "trailing separator". */
	vm_push(vm,
	        OBJ_VAL(make_string(vm, s->chars + start, s->length - start)));

	/* Move the pushed strings into the list, then drop them. The count is
	 * however many we pushed, which is the number of stack slots above
	 * the list we rooted first. I track it explicitly rather than
	 * scanning, because the list is one slot below them. */
	int n = (int)(vm->stack_top - out_stack_base);
	if (n > 0) {
		Value *items = ALLOCATE(vm, Value, (size_t)n);
		for (int k = 0; k < n; k++)
			items[k] = vm->stack_top[-n + k];
		out->items = items;
		out->capacity = n;
		out->count = n;
		vm->stack_top -= n;
	}
	vm_pop(vm); /* the list */
	return OBJ_VAL(out);
}

/*
 * a shared substring helper. returns a fresh string covering [from, to).
 * flint strings are immutable, so there is no slicing primitive; every
 * operation that produces part of a string builds a new one.
 */
static Value substring(VM *vm, ObjString *s, int from, int to)
{
	if (from < 0)
		from = 0;
	if (to > s->length)
		to = s->length;
	if (from > to)
		from = to;
	return OBJ_VAL(make_string(vm, s->chars + from, to - from));
}

/*
 * join(list, sep) -> string
 *
 * the inverse of split. a script that reads csv, edits a field, and writes
 * it back is four lines with these and twenty without.
 */
static Value join_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_LIST(argv[0])) {
		vm_runtime_error(
		        vm, "First argument to join() must be a list.");
		return NIL_VAL;
	}
	REQUIRE_STRING(vm, argv[1], "join()");

	ObjList *list = AS_LIST(argv[0]);
	ObjString *sep = AS_STRING(argv[1]);

	/* total length first, so the result is one allocation. checked,
	 * because a list of a million long strings adds up past INT_MAX and
	 * the wrap would hand a short buffer to a long copy. */
	long total = 0;
	for (int i = 0; i < list->count; i++) {
		if (!IS_STRING(list->items[i])) {
			vm_runtime_error(vm,
			        "join() needs a list of strings, but "
			        "element %d is a %s.",
			        i,
			        flint_type_name(list->items[i]));
			return NIL_VAL;
		}
		ObjString *piece = AS_STRING(list->items[i]);
		if (total > (long)INT_MAX - piece->length - sep->length) {
			vm_runtime_error(vm, "join() result is too long.");
			return NIL_VAL;
		}
		total += piece->length;
		if (i < list->count - 1)
			total += sep->length;
	}

	char *buffer = malloc((size_t)total + 1);
	if (buffer == NULL) {
		vm_runtime_error(vm, "Out of memory in join().");
		return NIL_VAL;
	}

	long used = 0;
	for (int i = 0; i < list->count; i++) {
		ObjString *piece = AS_STRING(list->items[i]);
		memcpy(buffer + used, piece->chars, (size_t)piece->length);
		used += piece->length;
		if (i < list->count - 1) {
			memcpy(buffer + used, sep->chars, (size_t)sep->length);
			used += sep->length;
		}
	}
	buffer[total] = '\0';

	ObjString *out = make_string(vm, buffer, (int)total);
	free(buffer);
	return OBJ_VAL(out);
}

/* the C library's whitespace set, spelled out so flint's trim does not
 * depend on the platform's locale */
static bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
	       c == '\v';
}

/*
 * trim(s) -> string
 *
 * whitespace off both ends. a text-processing script reads lines, and lines
 * come with trailing newlines, and nothing good comes of a script that has to
 * remember to strip one.
 */
static Value trim_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	REQUIRE_STRING(vm, argv[0], "trim()");

	ObjString *s = AS_STRING(argv[0]);
	int from = 0;
	int to = s->length;
	while (from < to && is_space(s->chars[from]))
		from++;
	while (to > from && is_space(s->chars[to - 1]))
		to--;
	return substring(vm, s, from, to);
}

/* contains(s, sub) -> bool */
static Value contains_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	REQUIRE_STRING(vm, argv[0], "contains()");
	REQUIRE_STRING(vm, argv[1], "contains()");

	ObjString *s = AS_STRING(argv[0]);
	ObjString *sub = AS_STRING(argv[1]);

	/* an empty substring is in everything, and is what a caller passing
	 * a computed-and-possibly-empty needle actually wants */
	if (sub->length == 0)
		return TRUE_VAL;
	if (sub->length > s->length)
		return FALSE_VAL;

	for (int i = 0; i <= s->length - sub->length; i++) {
		if (memcmp(s->chars + i, sub->chars, (size_t)sub->length) == 0)
			return TRUE_VAL;
	}
	return FALSE_VAL;
}

/* starts_with(s, prefix) -> bool */
static Value starts_with_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	REQUIRE_STRING(vm, argv[0], "starts_with()");
	REQUIRE_STRING(vm, argv[1], "starts_with()");

	ObjString *s = AS_STRING(argv[0]);
	ObjString *pre = AS_STRING(argv[1]);
	if (pre->length > s->length)
		return FALSE_VAL;
	return memcmp(s->chars, pre->chars, (size_t)pre->length) == 0
	               ? TRUE_VAL
	               : FALSE_VAL;
}

/* ends_with(s, suffix) -> bool */
static Value ends_with_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	REQUIRE_STRING(vm, argv[0], "ends_with()");
	REQUIRE_STRING(vm, argv[1], "ends_with()");

	ObjString *s = AS_STRING(argv[0]);
	ObjString *suf = AS_STRING(argv[1]);
	if (suf->length > s->length)
		return FALSE_VAL;
	return memcmp(s->chars + s->length - suf->length,
	               suf->chars,
	               (size_t)suf->length) == 0
	               ? TRUE_VAL
	               : FALSE_VAL;
}

/*
 * replace(s, from, to) -> string
 *
 * every occurrence, not just the first. a global replace is what log
 * scrubbing and template filling both want, and "replace all" as a separate
 * function is a name nobody remembers.
 */
static Value replace_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	REQUIRE_STRING(vm, argv[0], "replace()");
	REQUIRE_STRING(vm, argv[1], "replace()");
	REQUIRE_STRING(vm, argv[2], "replace()");

	ObjString *s = AS_STRING(argv[0]);
	ObjString *from = AS_STRING(argv[1]);
	ObjString *to = AS_STRING(argv[2]);

	/* replacing a string with itself, or replacing nothing, changes
	 * nothing. returning s directly is the right answer and the fast
	 * one. */
	if (from->length == 0 ||
	        (from->length == to->length &&
	                memcmp(from->chars, to->chars, (size_t)from->length) ==
	                        0))
		return argv[0];

	/* count the hits first, for the length of the result. a miss is the
	 * common case in a filtering loop, so the count is also the test
	 * for "did we match at all". */
	int hits = 0;
	for (int i = 0; i <= s->length - from->length; i++) {
		if (memcmp(s->chars + i, from->chars, (size_t)from->length) ==
		        0) {
			hits++;
			i += from->length - 1; /* skip past this match */
		}
	}
	if (hits == 0)
		return argv[0];

	/* checked: a string with a million short matches replaced by long
	 * ones adds up fast, and the wrap would be a short buffer for a
	 * long copy. */
	if (s->length > INT_MAX - hits * to->length) {
		vm_runtime_error(vm, "replace() result is too long.");
		return NIL_VAL;
	}
	int total = s->length + hits * (to->length - from->length);

	char *buffer = malloc((size_t)total + 1);
	if (buffer == NULL) {
		vm_runtime_error(vm, "Out of memory in replace().");
		return NIL_VAL;
	}

	int used = 0;
	int i = 0;
	while (i < s->length) {
		if (i <= s->length - from->length &&
		        memcmp(s->chars + i,
		                from->chars,
		                (size_t)from->length) == 0) {
			memcpy(buffer + used, to->chars, (size_t)to->length);
			used += to->length;
			i += from->length;
		} else {
			buffer[used++] = s->chars[i++];
		}
	}
	buffer[used] = '\0';

	ObjString *out = make_string(vm, buffer, used);
	free(buffer);
	return OBJ_VAL(out);
}

/* the C library's case mapping, which is ASCII-only and therefore does not
 * depend on the locale the way towlower does */
static char to_lower_ascii(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static char to_upper_ascii(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* lower(s) -> string. leaves non-ASCII bytes alone: flint strings are bytes,
 * and pretending otherwise would corrupt utf-8. */
static Value lower_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	REQUIRE_STRING(vm, argv[0], "lower()");
	ObjString *s = AS_STRING(argv[0]);

	char *buffer = malloc((size_t)s->length + 1);
	if (buffer == NULL) {
		vm_runtime_error(vm, "Out of memory in lower().");
		return NIL_VAL;
	}
	for (int i = 0; i < s->length; i++)
		buffer[i] = to_lower_ascii(s->chars[i]);
	buffer[s->length] = '\0';

	ObjString *out = make_string(vm, buffer, s->length);
	free(buffer);
	return OBJ_VAL(out);
}

/* upper(s) -> string */
static Value upper_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	REQUIRE_STRING(vm, argv[0], "upper()");
	ObjString *s = AS_STRING(argv[0]);

	char *buffer = malloc((size_t)s->length + 1);
	if (buffer == NULL) {
		vm_runtime_error(vm, "Out of memory in upper().");
		return NIL_VAL;
	}
	for (int i = 0; i < s->length; i++)
		buffer[i] = to_upper_ascii(s->chars[i]);
	buffer[s->length] = '\0';

	ObjString *out = make_string(vm, buffer, s->length);
	free(buffer);
	return OBJ_VAL(out);
}

void register_string_natives(VM *vm)
{
	vm_define_native(vm, "split", split_native, 2);
	vm_define_native(vm, "join", join_native, 2);
	vm_define_native(vm, "trim", trim_native, 1);
	vm_define_native(vm, "contains", contains_native, 2);
	vm_define_native(vm, "starts_with", starts_with_native, 2);
	vm_define_native(vm, "ends_with", ends_with_native, 2);
	vm_define_native(vm, "replace", replace_native, 3);
	vm_define_native(vm, "lower", lower_native, 1);
	vm_define_native(vm, "upper", upper_native, 1);
}
