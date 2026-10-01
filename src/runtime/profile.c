/* SPDX-License-Identifier: MIT */
/*
 * The counters themselves, and the report. See profile.h for why this is
 * split between always-on and build-flag work.
 */
#define _POSIX_C_SOURCE 200809L /* NOLINT(bugprone-reserved-identifier) */
#include "profile.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

bool fl_profile_opcode_counting(void)
{
#ifdef FL_PROFILE
	return true;
#else
	return false;
#endif
}

uint64_t fl_now_ns(void)
{
	struct timespec ts;
	/*
	 * CLOCK_MONOTONIC, with CLOCK_REALTIME as a fallback.
	 *
	 * The NOLINT is not suppressing a real finding. `<time.h>` is included
	 * directly above and does declare both of these; misc-include-cleaner
	 * cannot see a declaration that only exists when a POSIX feature-test
	 * macro is defined, so it reports a missing include for a header that is
	 * present. The file compiles clean with -std=c11 -D_POSIX_C_SOURCE and
	 * without this comment, which is the test that matters.
	 *
	 * The fallback is there because CLOCK_MONOTONIC is not available on
	 * every kernel flint runs on. Falling back makes the number wrong --
	 * a wall clock can jump -- but makes it a number rather than a zero,
	 * which is the better failure for a duration.
	 */
	/* NOLINTNEXTLINE(misc-include-cleaner) */
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
	       (uint64_t)ts.tv_nsec;
}

static void report_rate(
        FILE *out, const char *label, uint64_t n, const char *unit)
{
	/* Large counts are printed with separators because they are read by
	 * humans. "1048576 allocations" is harder to check at a glance than
	 * "1,048,576", and the whole point of a report is the glance. */
	char buf[64];
	snprintf(buf, sizeof(buf), "%" PRIu64, n);

	size_t len = strlen(buf);
	if (len <= 3) {
		fprintf(out, "  %-22s %s %s\n", label, buf, unit);
		return;
	}
	fprintf(out, "  %-22s ", label);
	for (size_t i = 0; i < len; i++) {
		if (i > 0 && (len - i) % 3 == 0)
			fputc(',', out);
		fputc(buf[i], out);
	}
	fprintf(out, " %s\n", unit);
}

static void report_kb(FILE *out, const char *label, uint64_t bytes)
{
	/* one decimal, because 1.4 MB and 1 MB are different stories and
	 * "1400000" and "1048576" are not comparable at a glance. */
	fprintf(out, "  %-22s %8.1f KiB\n", label, (double)bytes / 1024.0);
}

void fl_profile_report(void *out_handle, const FlCounters *c)
{
	FILE *out = (FILE *)out_handle;

	fprintf(out, "execution\n");
	report_rate(out, "calls", c->calls, "");
	report_rate(out, "returns", c->returns, "");
	report_rate(out, "loop iterations", c->backedges, "");
	report_rate(out, "native primitives", c->primitives, "");
	if (fl_profile_opcode_counting())
		fprintf(out,
		        "  (this build counts opcodes; see "
		        "--opcode-profile)\n");

	fprintf(out, "\nmemory\n");
	report_rate(out, "allocations", c->allocations, "");
	report_rate(out, "frees", c->frees, "");
	report_rate(out, "objects created", c->objects, "");
	report_kb(out, "live bytes", c->peak_bytes);

	fprintf(out, "\ncollector\n");
	report_rate(out, "collections", c->gc_cycles, "");
	report_kb(out, "time in gc", c->gc_ns / 1000);
	report_rate(out, "objects visited", c->gc_visited, "");
	report_rate(out, "objects swept", c->gc_swept, "");
	report_kb(out, "bytes reclaimed", c->gc_freed_bytes);

	fprintf(out, "\nstrings\n");
	report_kb(out, "string data", c->string_bytes);
	report_rate(out, "strings created", c->strings_created, "");
	report_rate(out, "intern hits", c->intern_hits, "");
	report_rate(out, "intern misses", c->intern_misses, "");

	fprintf(out, "\ncollections\n");
	report_rate(out, "lists created", c->lists_created, "");
	report_rate(out, "list growths", c->list_grows, "");
	report_rate(out, "tables created", c->tables_created, "");

	/*
	 * The two derived numbers, because a raw count of collections means
	 * nothing without knowing how much work each one avoided. Bytes
	 * allocated between collections is the heap growth factor times the
	 * live set, and gc time as a fraction of everything is the number
	 * that decides whether the collector needs work at all.
	 */
	if (c->allocations > 0) {
		fprintf(out, "\nderived\n");
		fprintf(out,
		        "  %-22s %8.1f KiB per collection\n",
		        "allocation between gc",
		        c->gc_cycles > 0
		                ? ((double)c->bytes / (double)c->gc_cycles) /
		                          1024.0
		                : (double)c->bytes / 1024.0);
	}
}
