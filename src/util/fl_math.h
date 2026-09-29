/* SPDX-License-Identifier: MIT */
/*
 * Thin wrappers over C math functions, used by the flint math library.
 * Written by Artem Tsitronov. Main library by Soumalya Das.
 */

#ifndef FL_MATH_H
#define FL_MATH_H

#include <stdint.h>

double   fl_math_floor(double x);
double   fl_math_sqrt(double x);
double   fl_math_fma(double a, double b, double c);
double   fl_math_ldexp(double x, int n);
double   fl_math_logb(double x);
uint64_t fl_math_bits(double x);
double   fl_math_double(uint64_t u);
double   fl_math_hi32(double x);
double   fl_math_lo32(double x);
double   fl_math_fabs(double x);
double   fl_math_copysign(double x, double y);

#endif /* FL_MATH_H */
