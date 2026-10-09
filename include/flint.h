/* SPDX-License-Identifier: MIT */
/*
 * flint.h -- the public C ABI for native flint modules.
 *
 * This is the whole contract between flint and a compiled extension. It is
 * C, it is versioned independently of the language release, and it exposes
 * no flint internals: no Value, no Obj, no VM, no stack slots, no compiler
 * state. Everything a native module can touch arrives through a function in
 * this file.
 *
 * ## versioning
 *
 * FL_ABI_VERSION is the version of *this* header's layout and semantics. A
 * module compiled against version N is rejected by a host that implements
 * a different version, before any of its code runs -- a wrong struct layout
 * is undefined behaviour, so the only safe response is to refuse to load.
 * The check is exact rather than a range: an ABI that can change shape in
 * a minor version cannot be versioned by major.minor alone, and pretending
 * otherwise is how a plugin built for the future loads into the present.
 *
 * Adding a *function* does not change the version. Adding a field to a
 * struct, changing a signature, or changing what a handle means, does.
 *
 * ## handles and lifetime
 *
 * A FlValue is a handle: an opaque, non-owning reference to a flint value,
 * valid for the duration of the native call that received it. It is valid
 * only while that call is on the stack. Code that needs a value to outlive
 * the call -- a callback stored in a table, a value returned from a
 * function later -- uses fl_retain() and fl_release(), which move it into
 * and out of the host's handle table.
 *
 * Handles are per-VM. A handle from one VM means nothing in another, and
 * passing one across threads is undefined: the VM, its collector and its
 * handle table are not synchronised.
 *
 * ## strings
 *
 * Flint strings are byte sequences, not unicode: UTF-8 is not decoded
 * anywhere in the language, and an ABI that decoded them here would be
 * the only part of flint that disagreed. fl_string_bytes() returns a
 * length; a string may contain NUL, and callers must use the length rather
 * than strlen.
 *
 * ## errors
 *
 * A native function reports failure by calling fl_raise() and returning
 * nil. That raises an ordinary flint runtime error, catchable by flint
 * code, with the same {type, message} shape as any other error. There is
 * no separate error channel: one error model is the point.
 *
 * ## memory
 *
 * Functions returning a FlValue return a handle, never a pointer into the
 * VM. Handles into the VM's own storage are never exposed, so a native
 * module cannot corrupt the collector's roots by keeping one.
 *
 * ## safety
 *
 * A native module runs with the host process's privileges. Loading an
 * untrusted module executes arbitrary code. This ABI is not a sandbox and
 * does not pretend to be one.
 */
#ifndef FLINT_H
#define FLINT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The ABI version. Incremented for any change to struct layout, function
 * signature, or the meaning of an existing function. Adding a new function
 * does not increment it.
 */
#define FL_ABI_VERSION 1u

/*
 * The entry point every native module must export:
 *
 *     int flint_module_init(FlModule *module, uint32_t abi_version);
 *
 * Return FL_INIT_OK on success, FL_INIT_ERROR on failure. The host has
 * already checked abi_version against its own and called nothing else; a
 * mismatch is reported by the host and the module is never entered.
 *
 * Inside init, register the module's exported functions with
 * fl_module_func(). Registration after init returns has no effect and is
 * reported as an error, because a function registered late could be called
 * before it exists.
 */
#define FL_INIT_OK 0
#define FL_INIT_ERROR 1

/* the symbols an extension looks up in the host */
#define FL_MODULE_FUNC "flint_module_init"

/*
 * An opaque handle to a flint value. The struct is one pointer so it can be
 * passed by value and compared; its contents are private to the host.
 * Never dereference it.
 */
typedef struct {
	void *opaque;
} FlValue;

/* opaque host state handed to a native module */
typedef struct FlModule FlModule;

/* the value kinds flint has. There is no flint type beyond these. */
typedef enum {
	FL_TYPE_NIL = 0,
	FL_TYPE_BOOL,
	FL_TYPE_NUMBER,
	FL_TYPE_STRING,
	FL_TYPE_LIST,
	FL_TYPE_TABLE,
	FL_TYPE_FUNCTION
} FlType;

/*
 * A native function. argc is the declared arity, or -1 for variadic.
 *
 * Return a FlValue to produce a result. To report an error, call
 * fl_raise() and return fl_nil() -- returning a value after raising is
 * allowed but the value is discarded when the error propagates.
 */
typedef FlValue (*FlNativeFn)(FlModule *module,
        int argc,
        const FlValue *argv);

/* ---------------------------------------------------------------------- */
/* version and capability                                                   */
/* ---------------------------------------------------------------------- */

/* the ABI version this host implements */
uint32_t fl_abi_version(void);

/*
 * Nonzero when the host implements the given capability. Capabilities are
 * small integer tags; an unknown tag returns 0, so a module can ask about
 * a feature from a newer host without breaking.
 */
int fl_has_capability(uint32_t capability);

/* capability tags */
#define FL_CAP_RETAIN 1u    /* fl_retain / fl_release */
#define FL_CAP_LIST 2u      /* list construction and access */
#define FL_CAP_TABLE 3u     /* table construction and access */

/* ---------------------------------------------------------------------- */
/* registration                                                             */
/* ---------------------------------------------------------------------- */

/*
 * Declare the module's name. Call this first; the name appears in
 * `flint pkg list` and in the error for an import of a module that is
 * already loaded under a different name. Returns nonzero on success.
 */
int fl_module_name(FlModule *module, const char *name);

/*
 * Register an exported function. arity is the declared argument count, or
 * -1 for variadic. The name should match what flint code will call.
 *
 * Returns 0 on failure: after init has returned, when the name is already
 * registered, or when out of memory.
 */
int fl_module_func(FlModule *module,
        const char *name,
        FlNativeFn fn,
        int arity);

/* the VM this module is registered in, as an opaque host pointer. Null in
 * a host that has not yet created one. */
void *fl_module_vm(FlModule *module);

/* ---------------------------------------------------------------------- */
/* values                                                                   */
/* ---------------------------------------------------------------------- */

/* the immutable singletons; safe to return from any native function */
FlValue fl_nil(void);
FlValue fl_bool(int value);
FlValue fl_number(double value);
FlValue fl_string(FlModule *module, const char *bytes, size_t length);

/*
 * The type of a value. Never fails: an unknown tag is reported as
 * FL_TYPE_NIL rather than as an error, because the question is always
 * answerable.
 */
FlType fl_type(FlValue value);

int fl_is_nil(FlValue value);
int fl_is_bool(FlValue value);
int fl_is_number(FlValue value);
int fl_is_string(FlValue value);
int fl_is_list(FlValue value);
int fl_is_table(FlValue value);
int fl_is_function(FlValue value);

/* the singleton values, for comparison */
int fl_to_bool(FlValue value);

/*
 * Read a number. Returns 0 and leaves *out untouched if the value is not a
 * number -- which is what makes the check-then-read pattern safe:
 *
 *     double x;
 *     if (fl_to_number(v, &x)) { ... }
 */
int fl_to_number(FlValue value, double *out);

/*
 * Read a string. The returned pointer is owned by the VM and is valid for
 * the duration of the call; it is not NUL-terminated, so the length is
 * the authority. Returns 0 when the value is not a string.
 */
int fl_to_string(FlValue value, const char **bytes, size_t *length);

/* ---------------------------------------------------------------------- */
/* collections                                                              */
/* ---------------------------------------------------------------------- */

/* an empty list, or nil when out of memory */
FlValue fl_new_list(FlModule *module);
FlValue fl_new_table(FlModule *module);

/* append to a list. Returns 0 when value is not a list. */
int fl_list_push(FlModule *module, FlValue list, FlValue value);
int fl_list_length(FlValue list, size_t *out);
int fl_list_get(FlValue list, size_t index, FlValue *out);

/* set a field on a table. Returns 0 when value is not a table. */
int fl_table_set(FlModule *module,
        FlValue table,
        const char *key,
        size_t key_length,
        FlValue value);
/* read a field. Returns 0 when the table has no such key. */
int fl_table_get(FlValue table,
        const char *key,
        size_t key_length,
        FlValue *out);
int fl_table_has(FlValue table, const char *key, size_t key_length);
int fl_table_length(FlValue table, size_t *out);

/* ---------------------------------------------------------------------- */
/* handles that outlive a call                                              */
/* ---------------------------------------------------------------------- */

/*
 * Retain a value past the end of the current native call, and release it
 * when done. A retained handle is rooted against the collector, so it
 * cannot be freed underneath the module.
 *
 * Every retain needs a matching release. There is no automatic release on
 * return, because a retained handle is usually stored somewhere the host
 * cannot see; leaking one is a bug in the module, and the count is
 * reported by the host at shutdown so it is at least a visible one.
 *
 * Retained handles are per-VM and are not thread-safe.
 */
FlValue fl_retain(FlModule *module, FlValue value);
void fl_release(FlModule *module, FlValue value);

/* how many handles this module currently holds. For tests and diagnostics. */
size_t fl_handle_count(FlModule *module);

/* ---------------------------------------------------------------------- */
/* errors                                                                   */
/* ---------------------------------------------------------------------- */

/*
 * Raise a runtime error with a message. Returns immediately; the caller
 * should return fl_nil().
 *
 * The error is an ordinary flint error: catchable, with a `type` and a
 * `message`, and the trace names the native function.
 */
void fl_raise(FlModule *module, const char *message);

/* raise with a %s-style format, for messages that include a value */
void fl_raise_fmt(FlModule *module, const char *format, ...);

/* the VM's current error category name, for a native module that wants to
 * raise something narrower. "Error" when there is no current error. */
const char *fl_error_category(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FLINT_H */