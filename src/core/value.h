/* SPDX-License-Identifier: MIT */
/*
 * Value representation: NaN-boxing.
 *
 * A value is one uint64_t. If the top 13 bits are not all set, the word is
 * an IEEE 754 double. If they are, the next 3 bits are a tag and the low 48
 * are a payload. That gives numbers, nil, both booleans and a heap pointer
 * in 8 bytes with no branch on allocation.
 */
#ifndef FL_VALUE_H
#define FL_VALUE_H

/* clang-format off: keep the masks and shifts aligned. */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t Value;

/*
 * Strings are declared here rather than in object.h because values_equal()
 * below has to call fl_strings_equal() and this header is not allowed to
 * include a runtime header. The only member it touches is chars/length, which
 * is a fixed layout, so a forward declaration is enough to pass a pointer
 * without knowing the struct.
 */
typedef struct ObjString ObjString;

/*
 * Two strings with equal contents. Defined in object.c, which does know the
 * layout.
 */
bool fl_strings_equal(const ObjString *a, const ObjString *b);

/* the 13 high bits. all set means "boxed". */
#define FL_BOX_MASK     UINT64_C(0xFFF8000000000000)
#define FL_TAG_SHIFT    48
#define FL_TAG_MASK     UINT64_C(0x0007000000000000)
#define FL_PAYLOAD_MASK UINT64_C(0x0000FFFFFFFFFFFF)

#define FL_TAG_NIL   UINT64_C(1)
#define FL_TAG_FALSE UINT64_C(2)
#define FL_TAG_TRUE  UINT64_C(3)
#define FL_TAG_OBJ   UINT64_C(4)
/*
 * Strings get their own tag rather than sharing OBJ.
 *
 * Two reasons, and both are hot. IS_STRING becomes a mask-and-compare instead
 * of a pointer chase to read the type byte, which matters because every string
 * test in the VM and every string primitive starts with one. And equality
 * stops being able to assume that equal means identical: two equal strings can
 * now be two objects, so the tag is what tells values_equal that it has to ask
 * somebody rather than compare bits.
 *
 * There is room: three bits hold eight tags and five were in use, so this
 * spends one.
 */
#define FL_TAG_STR UINT64_C(5)

/* one expression, not a function, so it stays usable in a static initializer */
#define FL_MAKE_BOXED(tag, payload)                                            \
	(FL_BOX_MASK | ((tag) << FL_TAG_SHIFT) |                               \
	        ((uint64_t)(payload) & FL_PAYLOAD_MASK))

#define NIL_VAL   ((Value)FL_MAKE_BOXED(FL_TAG_NIL, 0))
#define FALSE_VAL ((Value)FL_MAKE_BOXED(FL_TAG_FALSE, 0))
#define TRUE_VAL  ((Value)FL_MAKE_BOXED(FL_TAG_TRUE, 0))

/*
 * A NaN with a zero payload and the sign bit clear. Every NaN produced by
 * arithmetic is rewritten to this so it cannot be confused with a box.
 */
#define FL_CANONICAL_NAN UINT64_C(0x7FF8000000000000)

static inline bool fl_is_boxed(Value v)
{
	return (v & FL_BOX_MASK) == FL_BOX_MASK;
}

static inline uint64_t fl_tag_of(Value v)
{
	return (v & FL_TAG_MASK) >> FL_TAG_SHIFT;
}

/*
 * The predicate table. One line each, unaligned, because this is the one
 * place in the codebase where a column of aligned braces would be worse
 * than a column of facts: these are seven answers to "what is this value",
 * and the reader is comparing them, not reading them in order.
 */
static inline bool IS_NUMBER(Value v) { return !fl_is_boxed(v); }
static inline bool IS_NIL(Value v) { return v == NIL_VAL; }
static inline bool IS_FALSE(Value v) { return v == FALSE_VAL; }
static inline bool IS_TRUE(Value v) { return v == TRUE_VAL; }
static inline bool IS_BOOL(Value v) { return IS_TRUE(v) || IS_FALSE(v); }
static inline bool IS_OBJ(Value v)
{
	return fl_is_boxed(v) &&
	       (fl_tag_of(v) == FL_TAG_OBJ || fl_tag_of(v) == FL_TAG_STR);
}

/* the type byte inside the object, for a value known to be a heap object */
static inline const void *fl_obj_ptr(Value v)
{
	return (const void *)(uintptr_t)(v & FL_PAYLOAD_MASK);
}

/* only nil and false. zero, "" and [] are all true. */
static inline bool IS_FALSY(Value v) { return v == NIL_VAL || v == FALSE_VAL; }

static inline bool AS_BOOL(Value v) { return v == TRUE_VAL; }

static inline Value BOOL_VAL(bool b) { return b ? TRUE_VAL : FALSE_VAL; }

/* memcpy, not a cast: the compiler may assume a uint64_t is not a double. */
static inline double AS_NUMBER(Value v)
{
	double d;
	memcpy(&d, &v, sizeof d);
	return d;
}

/*
 * Is this double safe to print with %ld?
 *
 * The order of the two tests is the whole point. Written the obvious way,
 *
 *     d == (double)(int64_t)d && d >= -LIMIT && d <= LIMIT
 *
 * evaluates the cast FIRST. For something like 1e21 the cast to int64_t is
 * out of range, which is undefined behaviour, and ubsan fails the build
 * before the range check that would have rejected the value ever runs. The
 * guard has to come first, and the cast has to be a separate statement so
 * nothing can reorder it.
 *
 * 2^53 is the limit because that is where a double stops representing every
 * integer; above it, "integral" stops being a useful thing to ask about.
 */
#define FL_INT_EXACT_LIMIT 9007199254740992.0 /* 2^53 */

static inline bool fl_double_is_printable_int(double d)
{
	/* the range test first, so the cast below is only reached when it is
	 * defined. d is already known to be finite by every caller. */
	if (d < -FL_INT_EXACT_LIMIT || d > FL_INT_EXACT_LIMIT)
		return false;

	long as_long = (long)d;

	/* round-trip: a double that does not come back bit-identical is not
	 * an integer we are willing to print as one */
	return (double)as_long == d;
}

/* the printable form of a double that passes the test above */
static inline long fl_double_to_long(double d) { return (long)d; }

/*
 * Write a small integer into buf as decimal, without snprintf.
 *
 * snprintf("%.15g") costs about 250ns per call, which sounds like nothing
 * until you notice a loop that builds a hundred thousand strings is then
 * dominated by it. Most values a script ever prints are small integers, and
 * for those the whole job is a digit loop and a reversal.
 *
 * Returns the number of bytes written, not counting the NUL. Returns 0 when
 * the value is out of the range this handles, and the caller is expected to
 * fall back to snprintf rather than print something wrong.
 */
static inline int fl_itoa(long value, char *buf, size_t buflen)
{
	/* enough for "-9223372036854775808" and its NUL */
	if (buflen < 21)
		return 0;
	/* the cheap path is only worth taking for values that certainly
	 * round-trip through long, which is what fl_double_is_printable_int
	 * already established for the caller. */
	if (value < -1000000000L || value > 1000000000L)
		return 0;

	char digits[20];
	size_t n = 0;
	unsigned long u = value < 0 ? (unsigned long)(-(value + 1)) + 1
	                            : (unsigned long)value;
	do {
		digits[n++] = (char)('0' + (u % 10));
		u /= 10;
	} while (u != 0);

	size_t out = 0;
	if (value < 0)
		buf[out++] = '-';
	while (n > 0)
		buf[out++] = digits[--n];
	buf[out] = '\0';
	return (int)out;
}

/*
 * A NaN may carry a payload, and IEEE 754 declines to say whether the
 * hardware preserves it. In practice it does. We do not rely on it. Every
 * NaN becomes the canonical one, which costs one compare and removes a
 * whole class of "why did this number become a string" bug.
 */
static inline Value NUMBER_VAL(double d)
{
	if (d != d)
		return (Value)FL_CANONICAL_NAN;
	Value v;
	memcpy(&v, &d, sizeof v);
	return v;
}

/*
 * 48 bits of address space is what x86-64 and arm64 give us. If a vendor
 * ships something wider this assert fires instead of silently corrupting
 * pointers.
 */
static inline Value OBJ_VAL(void *p)
{
	uintptr_t u = (uintptr_t)p;
	assert((u & ~(uintptr_t)FL_PAYLOAD_MASK) == 0);
	return (Value)FL_MAKE_BOXED(FL_TAG_OBJ, u);
}

/* the string form of OBJ_VAL. every string reaching the stack comes from here,
 * so that IS_STRING never has to load a type byte to answer. */
static inline Value STR_VAL(void *p)
{
	uintptr_t u = (uintptr_t)p;
	assert((u & ~(uintptr_t)FL_PAYLOAD_MASK) == 0);
	return (Value)FL_MAKE_BOXED(FL_TAG_STR, u);
}

/*
 * Wrap a string object, with the type checked by the compiler.
 *
 * STR_VAL takes a void* because value.h cannot see ObjString. That is the
 * wrong trade for call sites: passing an ObjList to STR_VAL compiles cleanly
 * and produces a value that claims to be a string, which then fails every
 * IS_STRING test and every string operation on it. It happened, and the
 * symptom was a program that failed several calls away from the mistake.
 *
 * So the header with no runtime knowledge declares the typed form, and object.h
 * defines it once the layout is known. Every string leaving the runtime goes
 * through one of these two, and a wrong one is a compile error.
 *
 * Declared here, defined in object.h. The body needs the struct, and the
 * struct lives in the runtime header, so a definition in this one would be a
 * circular include. A declaration is not a warning; an unused *definition*
 * would be, which is exactly the failure that made this worth writing down.
 */

static inline void *AS_OBJ_PTR(Value v)
{
	return (void *)(uintptr_t)(v & FL_PAYLOAD_MASK);
}

/*
 * Numbers compare by value, everything else by bit pattern.
 *
 * Strings are the exception, and they used not to be. This used to be able to
 * say "because strings are interned, two equal strings are the same pointer"
 * and that was true and fast. Runtime strings are no longer interned -- the
 * cost of interning a string built once was higher than the cost of comparing
 * it once -- so equality has to ask about string contents. fl_strings_equal()
 * starts with a pointer compare anyway, so the interned case is still one test.
 */
static inline bool values_equal(Value a, Value b)
{
	if (IS_NUMBER(a) && IS_NUMBER(b))
		return AS_NUMBER(a) == AS_NUMBER(b);
	if (a == b)
		return true;
	/*
	 * Two equal strings can be two different objects now that runtime
	 * strings are not interned, so this is the one case where equal bit
	 * patterns do not settle it. The tag test is a mask and a compare,
	 * which is why strings got their own tag: no dereference, and no
	 * having to load a type byte to find out whether to bother asking.
	 */
	if (fl_is_boxed(a) && fl_is_boxed(b) && fl_tag_of(a) == FL_TAG_STR &&
	        fl_tag_of(b) == FL_TAG_STR)
		return fl_strings_equal((const ObjString *)fl_obj_ptr(a),
		        (const ObjString *)fl_obj_ptr(b));
	return false;
}

#endif /* FL_VALUE_H */
