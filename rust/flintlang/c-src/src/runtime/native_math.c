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
	if (!isfinite(e) || e != floor(e)) {
		vm_runtime_error(
		        vm, "Exponent to ldexp() must be a whole number.");
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

static Value exp_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "exp", &x))
		return NIL_VAL;
	return NUMBER_VAL(exp(x));
}

static Value exp2_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "exp2", &x))
		return NIL_VAL;
	return NUMBER_VAL(exp2(x));
}

static Value log_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "log", &x))
		return NIL_VAL;
	return NUMBER_VAL(log(x));
}

static Value log2_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "log2", &x))
		return NIL_VAL;
	return NUMBER_VAL(log2(x));
}

static Value log10_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "log10", &x))
		return NIL_VAL;
	return NUMBER_VAL(log10(x));
}

static Value sin_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "sin", &x))
		return NIL_VAL;
	return NUMBER_VAL(sin(x));
}

static Value cos_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "cos", &x))
		return NIL_VAL;
	return NUMBER_VAL(cos(x));
}

static Value tan_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "tan", &x))
		return NIL_VAL;
	return NUMBER_VAL(tan(x));
}

static Value asin_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "asin", &x))
		return NIL_VAL;
	return NUMBER_VAL(asin(x));
}

static Value acos_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "acos", &x))
		return NIL_VAL;
	return NUMBER_VAL(acos(x));
}

static Value atan_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "atan", &x))
		return NIL_VAL;
	return NUMBER_VAL(atan(x));
}

static Value sinh_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "sinh", &x))
		return NIL_VAL;
	return NUMBER_VAL(sinh(x));
}

static Value cosh_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "cosh", &x))
		return NIL_VAL;
	return NUMBER_VAL(cosh(x));
}

static Value tanh_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "tanh", &x))
		return NIL_VAL;
	return NUMBER_VAL(tanh(x));
}

static Value asinh_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "asinh", &x))
		return NIL_VAL;
	return NUMBER_VAL(asinh(x));
}

static Value acosh_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "acosh", &x))
		return NIL_VAL;
	return NUMBER_VAL(acosh(x));
}

static Value atanh_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "atanh", &x))
		return NIL_VAL;
	return NUMBER_VAL(atanh(x));
}

static Value ceil_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "ceil", &x))
		return NIL_VAL;
	return NUMBER_VAL(ceil(x));
}

static Value trunc_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x;
	if (!number_arg(vm, argv[0], "trunc", &x))
		return NIL_VAL;
	return NUMBER_VAL(trunc(x));
}

static Value pow_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x, y;
	if (!number_arg(vm, argv[0], "pow_x", &x) ||
	        !number_arg(vm, argv[1], "pow_y", &y))
		return NIL_VAL;
	return NUMBER_VAL(pow(x, y));
}

static Value atan2_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x, y;
	if (!number_arg(vm, argv[0], "atan2_x", &x) ||
	        !number_arg(vm, argv[1], "atan2_y", &y))
		return NIL_VAL;
	return NUMBER_VAL(atan2(x, y));
}

static Value fmod_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x, y;
	if (!number_arg(vm, argv[0], "fmod_x", &x) ||
	        !number_arg(vm, argv[1], "fmod_y", &y))
		return NIL_VAL;
	return NUMBER_VAL(fmod(x, y));
}

static Value remainder_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	double x, y;
	if (!number_arg(vm, argv[0], "remainder_x", &x) ||
	        !number_arg(vm, argv[1], "remainder_y", &y))
		return NIL_VAL;
	return NUMBER_VAL(remainder(x, y));
}

void register_math_natives(VM *vm)
{
	vm_define_native(vm, "__exp", exp_native, 1);
	vm_define_native(vm, "__exp2", exp2_native, 1);
	vm_define_native(vm, "__log", log_native, 1);
	vm_define_native(vm, "__log2", log2_native, 1);
	vm_define_native(vm, "__log10", log10_native, 1);
	vm_define_native(vm, "__sin", sin_native, 1);
	vm_define_native(vm, "__cos", cos_native, 1);
	vm_define_native(vm, "__tan", tan_native, 1);
	vm_define_native(vm, "__asin", asin_native, 1);
	vm_define_native(vm, "__acos", acos_native, 1);
	vm_define_native(vm, "__atan", atan_native, 1);
	vm_define_native(vm, "__sinh", sinh_native, 1);
	vm_define_native(vm, "__cosh", cosh_native, 1);
	vm_define_native(vm, "__tanh", tanh_native, 1);
	vm_define_native(vm, "__asinh", asinh_native, 1);
	vm_define_native(vm, "__acosh", acosh_native, 1);
	vm_define_native(vm, "__atanh", atanh_native, 1);
	vm_define_native(vm, "__ceil", ceil_native, 1);
	vm_define_native(vm, "__trunc", trunc_native, 1);
	vm_define_native(vm, "__pow", pow_native, 2);
	vm_define_native(vm, "__atan2", atan2_native, 2);
	vm_define_native(vm, "__fmod", fmod_native, 2);
	vm_define_native(vm, "__remainder", remainder_native, 2);
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
