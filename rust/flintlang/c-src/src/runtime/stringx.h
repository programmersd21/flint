/* SPDX-License-Identifier: MIT */
/*
 * String primitives. See stringx.c for why these are separate from the
 * language's core natives and from the operating-system ones.
 */
#ifndef FL_STRINGX_H
#define FL_STRINGX_H

#include "vm.h"

/* register split, join, trim, contains, starts_with, ends_with, replace,
 * lower, upper. called from register_natives(). */
void register_string_natives(VM *vm);

#endif /* FL_STRINGX_H */
