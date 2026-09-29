/* SPDX-License-Identifier: MIT */
/*
 * The math natives: the bridge between flint values and fl_math.c.
 */

#include "native_math.h"
#include "fl_math.h"
#include "object.h"
#include "value.h"
#include "vm.h"

#include <math.h>
#include <stdint.h>

/*
 * Every math native takes numbers. One place checks it, so every message
 * reads the same.
 */
static bool number_arg(VM *vm, Value v, const char *fn, double *out)
{
	if (!IS_NUMBER(v)) {
		vm_runtime_error(vm, "Argument to %s() must be a number.", fn);
		return false;
	}
	*out = AS_NUMBER(v);
	return true;
}

static Value floor_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "floor", &x))
		return NIL_VAL;
	return NUMBER_VAL(fl_math_floor(x));
}

static Value fma_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double a, b, c;
	if (!number_arg(vm, argv[0], "fma_a", &a) ||
	    !number_arg(vm, argv[1], "fma_b", &b) ||
	    !number_arg(vm, argv[2], "fma_c", &c))
		return NIL_VAL;
	return NUMBER_VAL(fl_math_fma(a, b, c));
}

static Value sqrt_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "sqrt", &x))
		return NIL_VAL;
	return NUMBER_VAL(fl_math_sqrt(x));
}

static Value logb_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "logb", &x))
		return NIL_VAL;
	return NUMBER_VAL(fl_math_logb(x));
}

static Value hi32_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "hi32", &x))
		return NIL_VAL;
	return NUMBER_VAL(fl_math_hi32(x));
}

static Value lo32_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "lo32", &x))
		return NIL_VAL;
	return NUMBER_VAL(fl_math_lo32(x));
}

static Value fabs_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "fabs", &x))
		return NIL_VAL;
	return NUMBER_VAL(fl_math_fabs(x));
}

static Value copysign_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x, y;
	if (!number_arg(vm, argv[0], "copysign_x", &x) ||
	    !number_arg(vm, argv[1], "copysign_y", &y))
		return NIL_VAL;
	return NUMBER_VAL(fl_math_copysign(x, y));
}

static Value ldexp_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x, e;
	if (!number_arg(vm, argv[0], "ldexp", &x) ||
	        !number_arg(vm, argv[1], "ldexp", &e))
		return NIL_VAL;
	if (isnan(e) || e != floor(e)) {
		vm_runtime_error(vm,
		        "Exponent to ldexp() must be a whole number.");
		return NIL_VAL;
	}
	if (e > 4096)
		e = 4096;
	if (e < -4096)
		e = -4096;
	return NUMBER_VAL(fl_math_ldexp(x, (int)e));
}

static bool half_arg(VM *vm, Value v, uint32_t *out)
{
	double d;
	if (!number_arg(vm, v, "from_bits", &d))
		return false;
	if (!(d >= 0 && d <= 4294967295.0) || d != floor(d)) {
		vm_runtime_error(vm,
		        "Arguments to from_bits() must be whole numbers "
		        "from 0 to 4294967295.");
		return false;
	}
	*out = (uint32_t)d;
	return true;
}

static Value from_bits_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	uint32_t hi, lo;
	if (!half_arg(vm, argv[0], &hi) || !half_arg(vm, argv[1], &lo))
		return NIL_VAL;
	uint64_t u = ((uint64_t)hi << 32) | lo;
	return NUMBER_VAL(fl_math_double(u));
}

void register_math_natives(VM *vm)
{
    vm_define_native(vm, "__floor", floor_native, 1);
    vm_define_native(vm, "__sqrt", sqrt_native, 1);
    vm_define_native(vm, "__fma", fma_native, 3);
    vm_define_native(vm, "__ldexp", ldexp_native, 2);
    vm_define_native(vm, "__logb", logb_native, 1);
    vm_define_native(vm, "__fabs", fabs_native, 1);
    vm_define_native(vm, "__copysign", copysign_native, 2);
    vm_define_native(vm, "__hi32", hi32_native, 1);
    vm_define_native(vm, "__lo32", lo32_native, 1);
    vm_define_native(vm, "__from_bits", from_bits_native, 2);
}
