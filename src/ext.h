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

#include "../include/flint.h"
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

/*
 * Ask a loaded module to shut down and unload.
 *
 * The request moves the module to quiescing: already-running calls finish,
 * new calls are refused with a runtime error. Then every route to native
 * code is checked -- active calls, retained handles, and registered
 * functions, which stay callable through the VM's globals for as long as
 * the VM lives. Unloading happens only when nothing references the
 * library; otherwise the module stays loaded and the reason is written
 * to error.
 *
 * Returns 0 when the library was actually closed, 1 when it is busy and
 * stays loaded, and -1 when the path is not a module this process loaded.
 * A 1 is a structured answer, not a failure: release the references and
 * ask again.
 */
int fl_ext_request_unload(VM *vm,
        const char *path,
        char *error,
        size_t error_size);

#endif /* FL_EXT_H */
