/* SPDX-License-Identifier: MIT */
/*
 * Math.c exposes fundamental basic math utils.
 * It exposes floor, sqrt, fma, ldexp, logb,
 *            hi32, lo32, bits, double,
 *            fabs, copysign
 * It is kept very minimal, acting as a thin
 * wrapper for C math functions.
 * It will be highly used by the math flint library.
 *
 * This code was written by Artem Tsitronov
 * Main library is written by Soumalya Das
 */

#include <math.h>
#include <string.h>
#include <stdint.h>

#include "fl_math.h"

/*
 * I know, I know, these wrappers are pretty unneccessary.
 * But! But, it is nice to separate the logic from the actual
 * language, so I prefer to write them, so that if we will
 * amplify making more complex wrappers, they don't interfere
 * with the core.
 */

double fl_math_floor(double x) {
    return floor(x);
}

 double fl_math_sqrt(double x) {
    return sqrt(x);
}

 double fl_math_fma(double a, double b, double c) {
    return fma(a,b, c);
}

 double fl_math_ldexp(double x, int n) {
    return ldexp(x, n);
}

 double fl_math_logb(double x) {
    return logb(x);
}

 uint64_t fl_math_bits(double x) {
    uint64_t u;
    memcpy(&u, &x, sizeof u);
    return u;
}

double fl_math_double(uint64_t u) {
    double x;
    memcpy(&x, &u, sizeof x);
    return x;
}

 double fl_math_hi32(double x) {
    return (double)(uint32_t)(fl_math_bits(x) >> 32);
}

 double fl_math_lo32(double x) {
    return (double)(uint32_t)(fl_math_bits(x) & 0xFFFFFFFFu);
}

 double fl_math_fabs(double x) {
    return fabs(x);
}

 double fl_math_copysign(double x, double y) {
    return copysign(x, y);
}
