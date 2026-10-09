/* SPDX-License-Identifier: MIT */
/*
 * A real native module, built and loaded by tests/native_test.sh.
 *
 * It exercises the parts of the ABI that are easy to get wrong and
 * impossible to test from flint: argument conversion in both directions,
 * a list and a table built natively, an error raised natively and caught
 * by flint code, a retained handle outliving its call, and a version
 * refusal.
 */
#include "flint.h"

#include <string.h>

static FlValue ext_greet(FlModule *m, int argc, const FlValue *argv)
{
	static char buf[256];
	const char *who = "world";
	size_t len = 0;
	if (argc > 0 && fl_is_string(argv[0]))
		fl_to_string(argv[0], &who, &len);
	/* a flint string may contain NUL, so the length is used rather than
	 * strlen -- the same rule the ABI documents for its own strings */
	size_t at = 0;
	const char *prefix = "hello, ";
	size_t plen = 7;
	for (size_t i = 0; i < plen && at + 1 < sizeof(buf); i++)
		buf[at++] = prefix[i];
	for (size_t i = 0; i < len && at + 1 < sizeof(buf); i++)
		buf[at++] = who[i];
	return fl_string(m, buf, at);
}

static FlValue ext_add(FlModule *m, int argc, const FlValue *argv)
{
	(void)m;
	double a = 0, b = 0;
	if (argc > 0)
		fl_to_number(argv[0], &a);
	if (argc > 1)
		fl_to_number(argv[1], &b);
	return fl_number(a + b);
}

/* builds a list natively and returns it */
static FlValue ext_build(FlModule *m, int argc, const FlValue *argv)
{
	FlValue list = fl_new_list(m);
	double count = 0;
	if (argc > 0)
		fl_to_number(argv[0], &count);
	size_t n = count > 0 ? (size_t)count : 0;
	for (size_t i = 0; i < n; i++)
		fl_list_push(m, list, fl_number((double)i));
	return list;
}

/* builds a table natively */
static FlValue ext_table(FlModule *m, int argc, const FlValue *argv)
{
	(void)argc;
	(void)argv;
	FlValue t = fl_new_table(m);
	fl_table_set(m, t, "kind", 4, fl_string(m, "native", 6));
	fl_table_set(m, t, "arity", 5, fl_number(2));
	return t;
}

/* raises an ordinary flint error, catchable by flint */
static FlValue ext_boom(FlModule *m, int argc, const FlValue *argv)
{
	(void)argc;
	(void)argv;
	fl_raise(m, "native module refused");
	return fl_nil();
}

/* retained handle: the value must survive the call that made it */
static FlValue ext_remember(FlModule *m, int argc, const FlValue *argv)
{
	FlValue kept = fl_retain(m, argc > 0 ? argv[0] : fl_nil());
	/* drop the local reference; the retained one keeps it alive */
	fl_release(m, kept);
	fl_release(m, kept);
	return fl_number((double)fl_handle_count(m));
}

/* deliberately refuses to initialise, for the failure path */
static FlValue ext_never(FlModule *m, int argc, const FlValue *argv)
{
	(void)m;
	(void)argc;
	(void)argv;
	return fl_nil();
}

int flint_module_init(FlModule *module, uint32_t abi_version);

int flint_module_init(FlModule *module, uint32_t abi_version)
{
	/* the host checks the version before calling, but an extension that
	 * does not check is relying on that; checking here makes a
	 * mismatch a refusal rather than a mystery */
	if (abi_version != FL_ABI_VERSION)
		return FL_INIT_ERROR;
	fl_module_name(module, "native_demo");
	fl_module_func(module, "greet", ext_greet, 1);
	fl_module_func(module, "add", ext_add, 2);
	fl_module_func(module, "build", ext_build, 1);
	fl_module_func(module, "table", ext_table, 0);
	fl_module_func(module, "boom", ext_boom, 0);
	fl_module_func(module, "remember", ext_remember, 1);
	fl_module_func(module, "never", ext_never, 0);
	return FL_INIT_OK;
}
