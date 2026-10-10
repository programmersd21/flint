/* SPDX-License-Identifier: MIT */
/*
 * The built-in library.
 *
 * Six functions for scripts, plus import_file, which the compiler emits a
 * call to for every `import`. Anything that needs more than this is a
 * question about whether it belongs in the language at all.
 */
#ifndef FL_NATIVE_H
#define FL_NATIVE_H

#include "vm.h"

void register_natives(VM *vm);

#endif /* FL_NATIVE_H */
