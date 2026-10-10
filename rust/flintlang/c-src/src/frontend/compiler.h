/* SPDX-License-Identifier: MIT */
/*
 * Single-pass pratt parser and bytecode compiler.
 */
#ifndef FL_COMPILER_H
#define FL_COMPILER_H

#include "chunk.h"
#include "object.h"
#include "vm.h"
#include "../util/diagnostic.h"

/*
 * Compiles source into a top-level ObjFunction. Returns NULL if the source
 * had errors, in which case the function must not be used.
 */
ObjFunction *compile(VM *vm, const char *source);
ObjFunction *compile_named(VM *vm, const char *source, const char *name);
void compiler_set_diagnostics(
        const FlSource *source, FlDiagFormat format, FlColorMode color);
size_t compiler_fix_count(void);
const FlDiagSuggestion *compiler_fix_at(size_t index);

/* called by the GC to mark functions that are still being compiled. */
void compiler_mark_roots(VM *vm);

/*
 * Repl echo.
 *
 * In a script, `1 + 2` on its own is an expression statement: it is
 * evaluated and the value thrown away, because a script that printed every
 * bare expression would be unusable. At a repl the person typing it is asking
 * for the answer, so the value is left on the stack for the caller.
 *
 * compiler_repl_echo turns that on. compiler_repl_value then reports whether
 * the last compile left something to print, and clears the flag.
 */
void compiler_repl_echo(bool on);
bool compiler_repl_value(void);

#endif /* FL_COMPILER_H */
