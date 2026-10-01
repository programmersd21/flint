/* SPDX-License-Identifier: MIT */
/*
 * Runtime tuning parameters, in one file.
 *
 * Every threshold the runtime uses lives here rather than at the point of
 * use. Two reasons. A number written inline at its use site is a number
 * nobody can find, and a number spread over a dozen call sites is a number
 * nobody can change coherently when the workload changes. Both are how a
 * runtime accumulates a pile of unexamined magic constants.
 *
 * These are defaults, not verdicts. Each one is here because the code around
 * it made a decision that needed a number, and the comment says what that
 * decision is. If a benchmark moves, change it here and re-measure; do not
 * sprinkle new copies through the runtime.
 */
#ifndef FL_CONFIG_H
#define FL_CONFIG_H

#include <stddef.h>

/*
 * GC heap growth. Collect when the live heap has grown by this factor since
 * the last collection.
 *
 * 2 is the classic mark-and-sweep compromise: doubling trades a longer pause
 * for fewer pauses. A smaller factor collects more often and costs time in
 * the collector; a larger one makes any surviving program pause harder.
 *
 * The first collection is a fixed byte count rather than a multiple of
 * nothing, because at startup the live heap is "whatever vm_init built" and
 * multiplying that by two is a number in the low hundreds of bytes, so the
 * first real allocation would collect before the program has done anything.
 */
#define FL_GC_HEAP_GROW_FACTOR 2

/* bytes allocated before the first collection. see the note above. */
#define FL_GC_FIRST_THRESHOLD ((size_t)1024 * 1024)

/*
 * Bytes above which a byte-counted buffer stops being tracked against the GC
 * threshold.
 *
 * A large allocation is worth keeping outside the accounting, because the
 * decision "is the heap big enough to collect" is about *many small objects*
 * surviving, not about one buffer that a program asked to be large. Counting
 * a 4 MB array makes the next small allocation look like it should collect,
 * which collects a heap of a thousand small objects to satisfy a request
 * about an array that has nothing to do with them.
 *
 * The cost is that a program whose live set really is one enormous buffer
 * never collects until it frees it. That is the correct trade: collecting
 * cannot free a buffer the program is still holding.
 */
#define FL_GC_UNCOUNTED_ABOVE ((size_t)1024 * 1024)

/*
 * Interpreter tiers do not exist yet, but the hotness numbers the eventual
 * JIT needs are the same numbers the profiler reports, so they are defined
 * here now, once, rather than being invented per use when the JIT lands.
 */

/* function calls before a function is worth compiling rather than interpreting */
#define FL_JIT_CALL_THRESHOLD 512

/* backward branches (loop iterations) before a loop is worth compiling */
#define FL_JIT_BACKEDGE_THRESHOLD 1024

/*
 * How many times a tier-1 function may deoptimize at one site before the JIT
 * stops trusting its type feedback for that site.
 *
 * A guard that keeps failing means the assumption was wrong, not that the
 * program is unusual. Re-entering the tier-1 code on the next call, failing
 * the same guard, and deoptimizing again, forever, is a program that runs at
 * tier-1 speed and pays tier-1 entry cost on top. Three is enough to be sure
 * without being slow about it.
 */
#define FL_JIT_MAX_DEOPTS 3

/* machine-code bytes a single function may compile to before we give up on it */
#define FL_JIT_MAX_CODE_BYTES ((size_t)64 * 1024)

/* how many machine-code bytes the whole program may occupy */
#define FL_JIT_MAX_TOTAL_CODE ((size_t)16 * 1024 * 1024)

/*
 * Ceiling on inline caches, per kind, per call site.
 *
 * Caches are keyed by the thing they looked up (a shape, a constant, a
 * callee), so a program that generates keys dynamically can ask for one per
 * key forever. A bound turns that from a slow leak into a slow miss, which
 * is the correct answer to "this program has no repeated pattern".
 */
#define FL_INLINE_CACHE_SIZE 8

/* how many functions may have a compiled body at once */
#define FL_JIT_MAX_COMPILED 4096

#endif /* FL_CONFIG_H */