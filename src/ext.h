/* SPDX-License-Identifier: MIT */
/*
 * The internal seam between the public C ABI and the runtime.
 *
 * include/flint.h is the contract a native module sees. This header is the
 * host side of it: installing the vtable an extension calls back through,
 * loading a shared object, and registering what it exported.
 *
 * None of this is part of the ABI, and none of it is exposed to an
 * extension -- an extension sees only include/flint.h.
 */
#ifndef FL_EXT_H
#define FL_EXT_H

#include <stdbool.h>

#include "runtime/vm.h"

/* The vtable is installed by fl_ext_load_native() before an extension's
 * init runs, so an extension's very first call has somewhere to go. There is
 * no way to have a native module without going through that function, so
 * there is nothing for a caller to install. */

/* hand a shared object to the ABI and register what it exported. on failure
 * returns false and fills error with a message the script can print. */
bool fl_ext_load_native(VM *vm,
        const char *path,
        const char *module_name,
        char *error,
        size_t error_size);

#endif /* FL_EXT_H */
