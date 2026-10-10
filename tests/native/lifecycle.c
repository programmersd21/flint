/* SPDX-License-Identifier: MIT */
/*
 * lifecycle.c -- a fixture for `flint native-unload`.
 *
 * init retains a string handle and never releases it, so an unload
 * request must answer busy naming the retained handle rather than
 * closing the library. ping() exists so the module also has the
 * exported-function route covered; the retained handle is checked
 * first, so that is the reason reported.
 */
#include "flint.h"

#include <string.h>

static FlValue ping(FlModule *module, int argc, const FlValue *argv)
{
	(void)module;
	(void)argc;
	(void)argv;
	return fl_bool(1);
}

int flint_module_init(FlModule *module, uint32_t abi_version)
{
	if (abi_version != FL_ABI_VERSION)
		return FL_INIT_ERROR;
	if (!fl_module_name(module, "lifecycle"))
		return FL_INIT_ERROR;
	FlValue kept = fl_string(module, "kept", strlen("kept"));
	fl_retain(module, kept);
	if (!fl_module_func(module, "ping", ping, 0))
		return FL_INIT_ERROR;
	return FL_INIT_OK;
}
