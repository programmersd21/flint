/* SPDX-License-Identifier: MIT */
#ifndef FL_PKG_H
#define FL_PKG_H

/*
 * `flint pkg`: path dependencies, end to end, and nothing else.
 *
 * A project declares them in flint.toml, `pkg install` copies them into
 * flint_modules/, and imports resolve from there. No registry, no network,
 * no solver beyond exact and caret versions: the registry phase inherits
 * this manifest format instead of inventing one, and what exists here
 * keeps working when it arrives.
 *
 * Returns a sysexits code. Writing goes in this file, not main.c, for the
 * same reason fmt lives in its own file: main.c dispatches; it does not
 * implement.
 */
int flint_pkg(int argc, char **argv, int start);

#endif
