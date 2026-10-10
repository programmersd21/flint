/* SPDX-License-Identifier: MIT */
#ifndef FL_FMT_H
#define FL_FMT_H

#include <stdbool.h>

/*
 * Format the files named by argv[start..argc]. With check, list the files
 * that would change and return 1 when any would; otherwise rewrite them
 * in place. Returns a sysexits code.
 */
int flint_fmt(int argc, char **argv, int start, bool check);

#endif
