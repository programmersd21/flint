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
/* our own header first, which is the rule and also what makes
 * register_string_natives() have external linkage */
#include "stringx.h"

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
			        vm, "argument to " name " must be a string."); \
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
/*
 * Build a string that came out of an operation: a slice, a split piece, a case
 * mapping.
 *
 * Not interned, and that is the change that matters. These strings are produced
 * by text processing and used once; interning each of them meant a hash, a
 * probe and an insertion into a weak table the collector then had to walk and
 * clean, in exchange for making a second identical string share an object --
 * which for a split of a log file never happens, because two identical pieces
 * of a log file are rare.
 *
 * Equality still works: fl_strings_equal() compares lengths and bytes. The cost
 * moved from creation to comparison, and comparison happens far less often than
 * creation.
 */
static ObjString *make_string(VM *vm, const char *chars, int length)
{
	return new_string(vm, chars, length);
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
	vm_push(vm, OBJ_VAL(out)); /* the list is a root for everything below */

	/*
	 * Append each piece as it is found, and pop it immediately.
	 *
	 * The obvious implementation pushes every piece on the stack and
	 * moves them into the array at the end. That is wrong: the value
	 * stack is 65536 slots, so splitting a string with more than
	 * that many separators wrote straight off the end of vm.stack and
	 * into the caller's frame. There was no bounds check on vm_push to
	 * catch it, so a large split was a silent memory corruption and
	 * a segfault somewhere unrelated.
	 *
	 * One value on the stack at a time has no such limit, which is
	 * also the right shape: a list of a million items costs a list,
	 * not a stack.
	 */
	/*
	 * Find the separators.
	 *
	 * memchr for the first byte, then memcmp to confirm. Scanning byte
	 * by byte and calling memcmp at every position is quadratic in the
	 * separator length and does a call per character; memchr is
	 * vectorised by the compiler, so the common case -- a separator
	 * that almost never matches -- costs a few instructions per
	 * *candidate* rather than a function call per byte. This is what
	 * CPython does and it is why its split beats ours by about 3x
	 * before this change.
	 *
	 * An empty separator is one character per element, handled
	 * separately because there is nothing to memchr for.
	 */
	int start = 0;
	if (sep->length == 0) {
		for (int i = 0; i < s->length; i++) {
			char one = s->chars[i];
			ObjString *piece = make_string(vm, &one, 1);
			vm_push(vm, STR_VAL(piece));
			if (out->count == out->capacity) {
				int old_cap = out->capacity;
				out->capacity = GROW_CAPACITY(old_cap);
				out->items = GROW_ARRAY(vm,
				        Value,
				        out->items,
				        old_cap,
				        out->capacity);
			}
			out->items[out->count++] = vm->stack_top[-1];
			vm_pop(vm);
		}
		goto emit_tail;
	}

	{
		char first = sep->chars[0];
		int limit = s->length - sep->length;
		int i = 0;
		while (i <= limit) {
			const char *hit = memchr(
			        s->chars + i, first, (size_t)(limit - i) + 1);
			if (hit == NULL)
				break;
			int at = (int)(hit - s->chars);
			/* memchr found the first byte. confirm the rest.
			 * sep->length == 1 needs no confirm. */
			if (sep->length == 1 ||
			        memcmp(hit + 1,
			                sep->chars + 1,
			                (size_t)sep->length - 1) == 0) {
				ObjString *piece = make_string(
				        vm, s->chars + start, at - start);
				vm_push(vm, STR_VAL(piece));
				if (out->count == out->capacity) {
					int old_cap = out->capacity;
					out->capacity = GROW_CAPACITY(old_cap);
					out->items = GROW_ARRAY(vm,
					        Value,
					        out->items,
					        old_cap,
					        out->capacity);
				}
				out->items[out->count++] = vm->stack_top[-1];
				vm_pop(vm);
				start = at + sep->length;
				i = start;
			} else {
				/* first byte matched, rest did not. resume just
				 * past it so the same position is not retried
				 * forever. */
				i = at + 1;
			}
		}
	}

emit_tail:;
	/* the tail. always emitted, so "a,b" gives two elements and "a"
	 * gives one, and a script never has to tell "no separator" from
	 * "trailing separator". */
	ObjString *tail = make_string(vm, s->chars + start, s->length - start);
	vm_push(vm, STR_VAL(tail));
	if (out->count == out->capacity) {
		int old_cap = out->capacity;
		out->capacity = GROW_CAPACITY(old_cap);
		out->items = GROW_ARRAY(
		        vm, Value, out->items, old_cap, out->capacity);
	}
	out->items[out->count++] = vm->stack_top[-1];
	vm_pop(vm); /* the piece */

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
	return STR_VAL(make_string(vm, s->chars + from, to - from));
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
		        vm, "first argument to join() must be a list.");
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

	char *buffer = ALLOCATE(vm, char, (size_t)total + 1);

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

	/* take_string owns buffer. */
	return STR_VAL(take_string(vm, buffer, (int)total));
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

	/*
	 * memchr for the first byte, memcmp to confirm. a call per byte was
	 * the bottleneck here: for a needle that does not occur, the naive
	 * loop makes s->length memcmp calls, and each one is a real call.
	 * memchr is vectorised, so this is a few instructions per
	 * *candidate*.
	 */
	int limit = s->length - sub->length;
	int i = 0;
	while (i <= limit) {
		const char *hit = memchr(
		        s->chars + i, sub->chars[0], (size_t)(limit - i) + 1);
		if (hit == NULL)
			return FALSE_VAL;
		int at = (int)(hit - s->chars);
		if (sub->length == 1 || memcmp(hit + 1,
		                                sub->chars + 1,
		                                (size_t)sub->length - 1) == 0)
			return TRUE_VAL;
		i = at + 1;
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

	/*
	 * Count the hits first, for the length of the result. a miss is the
	 * common case in a filtering loop, so the count is also the test for
	 * "did we match at all", and returning s unchanged is then free.
	 *
	 * memchr for the first byte, memcmp to confirm, for the same
	 * reason as contains(): a memcmp call per byte is what made this
	 * three times slower than the python it is being compared to.
	 */
	int hits = 0;
	{
		int limit = s->length - from->length;
		int i = 0;
		while (i <= limit) {
			const char *hit = memchr(s->chars + i,
			        from->chars[0],
			        (size_t)(limit - i) + 1);
			if (hit == NULL)
				break;
			int at = (int)(hit - s->chars);
			if (from->length == 1 ||
			        memcmp(hit + 1,
			                from->chars + 1,
			                (size_t)from->length - 1) == 0) {
				hits++;
				i = at + from->length;
			} else {
				i = at + 1;
			}
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

	char *buffer = ALLOCATE(vm, char, (size_t)total + 1);

	/*
	 * The second pass. Same shape as the counting pass, but copying
	 * runs of non-matching bytes with memcpy rather than one byte at a
	 * time: between two matches, the bytes are copied wholesale, and
	 * that is the bulk of the work in a string that mostly matches.
	 */
	int used = 0;
	int copied_from = 0;
	int i = 0;
	int limit = s->length - from->length;
	while (i <= limit) {
		const char *hit = memchr(
		        s->chars + i, from->chars[0], (size_t)(limit - i) + 1);
		if (hit == NULL)
			break;
		int at = (int)(hit - s->chars);
		if (from->length != 1 &&
		        memcmp(hit + 1,
		                from->chars + 1,
		                (size_t)from->length - 1) != 0) {
			i = at + 1;
			continue;
		}

		/* the run before this match, in one copy */
		int run = at - copied_from;
		if (run > 0) {
			memcpy(buffer + used,
			        s->chars + copied_from,
			        (size_t)run);
			used += run;
		}
		/* the replacement */
		memcpy(buffer + used, to->chars, (size_t)to->length);
		used += to->length;
		i = at + from->length;
		copied_from = i;
	}
	/* whatever is after the last match */
	int tail_len = s->length - copied_from;
	if (tail_len > 0) {
		memcpy(buffer + used, s->chars + copied_from, (size_t)tail_len);
		used += tail_len;
	}
	buffer[used] = '\0';

	return STR_VAL(take_string(vm, buffer, used));
}

/* the C library's case mapping, which is ASCII-only and therefore does not
 * depend on the locale the way towlower does */
static char to_lower_ascii(char c)
{
	/*
	 * The check below objects to the narrowing and is right to be
	 * suspicious in general: int-to-signed-char is implementation-defined
	 * when the value does not fit. Here the range guard above proves it
	 * fits -- 'A' + 32 is 'a', 'Z' + 32 is 'z', both representable in any
	 * char -- and any correct implementation narrows at exactly this point,
	 * because C promotes the operands to int. Verified rather than
	 * assumed: the ASCII fast path is covered by str_ascii and str_utf8.
	 */
	/* NOLINTNEXTLINE(bugprone-narrowing-conversions) */
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static char to_upper_ascii(char c)
{
	/* As to_lower_ascii: 'a' - 32 is 'A', representable in any char. */
	/* NOLINTNEXTLINE(bugprone-narrowing-conversions) */
	return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* lower(s) -> string. leaves non-ASCII bytes alone: flint strings are bytes,
 * and pretending otherwise would corrupt utf-8.
 *
 * Uses ALLOCATE so take_string can own the buffer and avoid a second copy.
 * The malloc+make_string+free pattern paid for three operations; this pays
 * for one allocation and one memcpy. */
static Value lower_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	REQUIRE_STRING(vm, argv[0], "lower()");
	ObjString *s = AS_STRING(argv[0]);

	char *buffer = ALLOCATE(vm, char, s->length + 1);
	for (int i = 0; i < s->length; i++)
		buffer[i] = to_lower_ascii(s->chars[i]);
	buffer[s->length] = '\0';

	/* take_string owns buffer and frees it if the string is interned. */
	return STR_VAL(take_string(vm, buffer, s->length));
}

/* upper(s) -> string */
static Value upper_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	REQUIRE_STRING(vm, argv[0], "upper()");
	ObjString *s = AS_STRING(argv[0]);

	char *buffer = ALLOCATE(vm, char, s->length + 1);
	for (int i = 0; i < s->length; i++)
		buffer[i] = to_upper_ascii(s->chars[i]);
	buffer[s->length] = '\0';

	return STR_VAL(take_string(vm, buffer, s->length));
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
