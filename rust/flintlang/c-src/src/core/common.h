/* SPDX-License-Identifier: MIT */
/*
 * Compile-time limits and shared types.
 *
 * Every limit here is a hard error at compile time rather than a runtime
 * check. A script that needs more than 256 locals has a design problem, and
 * failing at compile time says so in one line instead of three.
 */
#ifndef FL_COMMON_H
#define FL_COMMON_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * NaN-boxing needs pointers to fit in 48 bits of payload. A 32-bit host
 * would fit easily, but supporting one means a second value representation
 * and a second set of bugs. Not worth it.
 */
_Static_assert(sizeof(void *) == 8, "flint requires a 64-bit platform");

/* call frames. 256 nested calls is far past anything sane in a script. */
#define FRAMES_MAX 256

/* value stack slots. generous, since it is one flat array in the VM. */
#define STACK_MAX (FRAMES_MAX * 256)

#define MAX_LOCALS   256
#define MAX_UPVALUES 256

/* constant pool ceiling, which is what a 24-bit index can address. */
#define MAX_CONSTANTS 16777216

/*
 * How many modules may be on the import stack at once.
 *
 * This is the depth of nested imports -- main importing a, which imports b,
 * which imports c -- not a limit on how many modules exist. Every module that
 * has finished loading is popped off and lives in its own heap table, so this
 * only bounds recursion.
 *
 * 64 is generous for hand-written and library code, and the failure is a
 * diagnostic rather than a crash, which is the property that matters: a
 * runaway import reports "too deep" instead of exhausting the C stack.
 */
#define FL_MODULE_DEPTH 64

#endif /* FL_COMMON_H */
