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

/* the 13 high bits. all set means "boxed". */
#define FL_BOX_MASK     UINT64_C(0xFFF8000000000000)
#define FL_TAG_SHIFT    48
#define FL_TAG_MASK     UINT64_C(0x0007000000000000)
#define FL_PAYLOAD_MASK UINT64_C(0x0000FFFFFFFFFFFF)

#define FL_TAG_NIL   UINT64_C(1)
#define FL_TAG_FALSE UINT64_C(2)
#define FL_TAG_TRUE  UINT64_C(3)
#define FL_TAG_OBJ   UINT64_C(4)

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
	return fl_is_boxed(v) && fl_tag_of(v) == FL_TAG_OBJ;
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

static inline void *AS_OBJ_PTR(Value v)
{
	return (void *)(uintptr_t)(v & FL_PAYLOAD_MASK);
}

/*
 * Numbers compare by value, everything else by bit pattern. Because strings
 * are interned, two equal strings are the same pointer, and because boxes
 * are canonical, no two distinct values share a bit pattern.
 */
static inline bool values_equal(Value a, Value b)
{
	if (IS_NUMBER(a) && IS_NUMBER(b))
		return AS_NUMBER(a) == AS_NUMBER(b);
	return a == b;
}

#endif /* FL_VALUE_H */
