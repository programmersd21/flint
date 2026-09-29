/* SPDX-License-Identifier: MIT */
/*
 * The math natives: the bridge between flint values and fl_math.c.
 */
#ifndef FL_NATIVE_MATH_H
#define FL_NATIVE_MATH_H

#include "vm.h"

void register_math_natives(VM *vm);

#endif /* FL_NATIVE_MATH_H */
