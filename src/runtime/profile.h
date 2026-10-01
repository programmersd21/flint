/* SPDX-License-Identifier: MIT */
/*
 * Runtime counters.
 *
 * Split by cost, because "profiling" means two different things and charging
 * them the same price would mean charging the program for the second one.
 *
 * Always on: calls, backward branches, allocations, GC cycles and pause time.
 * These are one increment on a counter the JIT will consult anyway, so a
 * release build pays for them whether or not anyone looks at --profile. The
 * alternative -- compiling them out of release -- means the numbers
 * --profile prints were produced by a differently-compiled binary, which is
 * the kind of discrepancy that costs an afternoon.
 *
 * Build flag (FL_PROFILE): per-opcode execution counts. One increment per
 * dispatched instruction on *every* program to serve a mode almost nobody
 * uses is a bad trade, so this is compiled into a separate binary.
 */
#ifndef FL_PROFILE_H
#define FL_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	/* execution */
	uint64_t calls; /* call_value entries, natives included */
	uint64_t backedges; /* backward branches: loop iterations */
	uint64_t primitives; /* native calls that reached C */
	uint64_t returns;

	/* memory */
	uint64_t allocations; /* successful fl_reallocate calls */
	uint64_t frees; /* fl_reallocate calls that freed */
	uint64_t bytes; /* net bytes allocated */
	uint64_t peak_bytes; /* high-water mark of net bytes */
	uint64_t objects; /* heap objects created */

	/* collector */
	uint64_t gc_cycles;
	uint64_t gc_visited; /* objects reached by mark */
	uint64_t gc_swept; /* objects freed by sweep */
	uint64_t gc_freed_bytes;
	uint64_t gc_ns; /* nanoseconds inside the collector */

	/* strings */
	uint64_t string_bytes; /* bytes of string data created */
	uint64_t strings_created; /* distinct ObjStrings created */
	uint64_t intern_hits; /* intern table lookups that matched */
	uint64_t intern_misses;

	/* collections */
	uint64_t lists_created;
	uint64_t list_grows;
	uint64_t tables_created;
} FlCounters;

/* true when this build counts every dispatched instruction. */
bool fl_profile_opcode_counting(void);

/* Human-readable report. Goes to `out`, which the caller owns. */
void fl_profile_report(void *out, const FlCounters *counters);

/*
 * Nanoseconds from an unspecified epoch. Monotonic where the platform says it
 * is, which is all the GC needs: it is measuring a duration, not a time.
 */
uint64_t fl_now_ns(void);

#endif /* FL_PROFILE_H */
