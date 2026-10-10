/* SPDX-License-Identifier: MIT */
/*
 * The host side of the public C ABI: handle management, the vtable the
 * extension calls back through, registration, and the dlopen loader.
 *
 * Everything the extension can reach goes through fl_api, a table of
 * function pointers built once per VM. An extension is handed the vtable
 * rather than linked against these symbols, because a dlopened module
 * cannot call back into the executable unless the symbols are exported
 * from it -- which would put the ABI's implementation in the dynamic
 * symbol table of every flint binary. The vtable keeps the boundary
 * explicit and the linkage one-directional.
 */
#include "ext.h"
#include "../include/flint.h"

#include "../src/runtime/vm.h"
#include "../src/runtime/object.h"
#include "../src/core/memory.h"
#include "../src/core/value.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#	include <dlfcn.h>
#endif

/*
 * One retained handle: the value, and the module that has to release it.
 * The list is per-VM so a handle cannot outlive the collector that owns
 * the object it points at.
 */
typedef struct FlRetained {
	Value value;
	struct FlRetained *next;
} FlRetained;

/* one trampoline's private state: which module, which function.
 * Registration-only code: on Windows the loader refuses up front, so
 * nothing calls the trampoline and an uncalled static function is a
 * -Werror failure. Kept beside the loader rather than deleted, because
 * the Windows loader is the part that changes. */
#ifndef _WIN32
typedef struct {
	FlModule *module;
	FlNativeFn fn;
} ExtBinding;
#endif /* registration-only; the loader below is POSIX-only */

/* a module's place in the lifecycle: loaded, quiescing after an unload
 * request, or unloaded (which only exists as a return value -- an
 * unloaded record is removed, not kept). */
#define FL_EXT_LOADED    0
#define FL_EXT_QUIESCING 1

/* every library this process loaded, so an unload request can find it.
 * process-wide because dlopen is: two VMs loading one path share the
 * library, and unloading under one VM would pull it from under the
 * other. POSIX-only like the loader: the Windows build keeps no
 * registry because it loads nothing. */
#ifndef _WIN32
typedef struct ExtLoaded {
	char path[1024];
	void *lib;
	FlModule *module;
	struct ExtLoaded *next;
} ExtLoaded;

static ExtLoaded *ext_loaded;
#endif

#define FL_EXT_MAX_ARGS 16

struct FlModule {
	VM *vm;
	char name[256];
	FlRetained *retained;
	size_t retained_count;
	/* calls currently inside this module's functions. unloading with
	 * one in flight would return while its code is on the C stack. */
	int active_calls;
	/* FL_EXT_LOADED until fl_ext_request_unload asks; then quiescing,
	 * which refuses new calls at the trampoline. */
	int state;
	/* set once init returns; registration after that is an error */
	bool init_done;
	/* the exported functions, for the export table at import time */
	struct {
		char name[128];
		FlNativeFn fn;
		int arity;
	} funcs[128];
	int func_count;
};

/*
 * The vtable. Layout is part of the ABI: an extension compiled against
 * FL_ABI_VERSION N expects these slots in this order. Adding a slot is an
 * ABI version bump, not an append.
 */
typedef struct {
	uint32_t (*abi_version)(void);
	int (*has_capability)(uint32_t);
	int (*module_name)(FlModule *, const char *);
	int (*module_func)(FlModule *, const char *, FlNativeFn, int);
	void *(*module_vm)(FlModule *);
	FlValue (*nil)(void);
	FlValue (*boolean)(int);
	FlValue (*number)(double);
	FlValue (*string)(FlModule *, const char *, size_t);
	FlType (*type)(FlValue);
	int (*to_bool)(FlValue);
	int (*to_number)(FlValue, double *);
	int (*to_string)(FlValue, const char **, size_t *);
	FlValue (*new_list)(FlModule *);
	FlValue (*new_table)(FlModule *);
	int (*list_push)(FlModule *, FlValue, FlValue);
	int (*list_length)(FlValue, size_t *);
	int (*list_get)(FlValue, size_t, FlValue *);
	int (*table_set)(FlModule *, FlValue, const char *, size_t, FlValue);
	int (*table_get)(FlValue, const char *, size_t, FlValue *);
	int (*table_has)(FlValue, const char *, size_t);
	int (*table_length)(FlValue, size_t *);
	FlValue (*retain)(FlModule *, FlValue);
	void (*release)(FlModule *, FlValue);
	size_t (*handle_count)(FlModule *);
	void (*raise)(FlModule *, const char *);
	void (*raise_fmt)(FlModule *, const char *, ...);
	const char *(*error_category)(void);
} FlApi;

static const FlApi *fl_api;

/* the installed vtable; see fl_ext_install_api at the bottom */

/* ---------------------------------------------------------------------- */
/* handles                                                                  */
/* ---------------------------------------------------------------------- */

static FlValue handle_of(Value value)
{
	FlValue handle;
	handle.opaque = (void *)(uintptr_t)(value + 1);
	return handle;
}

static Value value_of(FlValue handle)
{
	if (handle.opaque == NULL)
		return NIL_VAL;
	return (Value)((uintptr_t)handle.opaque - 1);
}

/* ---------------------------------------------------------------------- */
/* vtable implementations                                                  */
/* ---------------------------------------------------------------------- */

static uint32_t api_abi_version(void) { return FL_ABI_VERSION; }

static int api_has_capability(uint32_t capability)
{
	switch (capability) {
	case FL_CAP_RETAIN:
	case FL_CAP_LIST:
	case FL_CAP_TABLE:
		return 1;
	default:
		return 0;
	}
}

static int api_module_name(FlModule *module, const char *name)
{
	if (module == NULL || name == NULL)
		return 0;
	snprintf(module->name, sizeof(module->name), "%s", name);
	return 1;
}

static int api_module_func(
        FlModule *module, const char *name, FlNativeFn fn, int arity)
{
	if (module == NULL || name == NULL || fn == NULL)
		return 0;
	/* a late registration could be called before it exists, so it is
	 * refused rather than queued */
	if (module->init_done)
		return 0;
	if (module->func_count >=
	        (int)(sizeof(module->funcs) / sizeof(module->funcs[0])))
		return 0;
	for (int i = 0; i < module->func_count; i++) {
		if (strcmp(module->funcs[i].name, name) == 0)
			return 0;
	}
	snprintf(module->funcs[module->func_count].name,
	        sizeof(module->funcs[0].name),
	        "%s",
	        name);
	module->funcs[module->func_count].fn = fn;
	module->funcs[module->func_count].arity = arity;
	module->func_count++;
	return 1;
}

static void *api_module_vm(FlModule *module)
{
	return module != NULL ? (void *)module->vm : NULL;
}

static FlValue api_nil(void) { return handle_of(NIL_VAL); }

static FlValue api_bool(int value)
{
	return handle_of(BOOL_VAL(value ? true : false));
}

static FlValue api_number(double value) { return handle_of(NUMBER_VAL(value)); }

static FlValue api_string(FlModule *module, const char *bytes, size_t length)
{
	if (module == NULL || module->vm == NULL)
		return api_nil();
	/* the string is allocated in the VM's heap, so it is a normal flint
	 * object: collectable, interned, and printable */
	ObjString *str = new_string(module->vm, bytes, (int)length);
	return handle_of(STR_VAL(str));
}

static FlType api_type(FlValue value)
{
	Value v = value_of(value);
	if (IS_NUMBER(v))
		return FL_TYPE_NUMBER;
	if (IS_STRING(v))
		return FL_TYPE_STRING;
	if (IS_BOOL(v))
		return FL_TYPE_BOOL;
	if (IS_NIL(v))
		return FL_TYPE_NIL;
	if (IS_LIST(v))
		return FL_TYPE_LIST;
	if (IS_FLINT_TABLE(v))
		return FL_TYPE_TABLE;
	if (IS_FUNCTION(v) || IS_CLOSURE(v) || IS_NATIVE(v))
		return FL_TYPE_FUNCTION;
	return FL_TYPE_NIL;
}

static int api_to_bool(FlValue value)
{
	Value v = value_of(value);
	return IS_BOOL(v) && AS_BOOL(v) ? 1 : 0;
}

static int api_to_number(FlValue value, double *out)
{
	Value v = value_of(value);
	if (!IS_NUMBER(v))
		return 0;
	if (out != NULL)
		*out = AS_NUMBER(v);
	return 1;
}

static int api_to_string(FlValue value, const char **bytes, size_t *length)
{
	Value v = value_of(value);
	if (!IS_STRING(v))
		return 0;
	if (bytes != NULL)
		*bytes = AS_CSTRING(v);
	if (length != NULL)
		*length = (size_t)AS_STRING(v)->length;
	return 1;
}

static FlValue api_new_list(FlModule *module)
{
	if (module == NULL || module->vm == NULL)
		return api_nil();
	return handle_of(OBJ_VAL(new_list(module->vm)));
}

static FlValue api_new_table(FlModule *module)
{
	if (module == NULL || module->vm == NULL)
		return api_nil();
	return handle_of(OBJ_VAL(new_flint_table(module->vm)));
}

static int api_list_push(FlModule *module, FlValue list, FlValue value)
{
	VM *vm = module != NULL ? module->vm : NULL;
	if (vm == NULL)
		return 0;
	Value l = value_of(list);
	if (!IS_LIST(l))
		return 0;
	ObjList *target = AS_LIST(l);
	/* the list is on the VM's stack as this call's argument, and the
	 * value being pushed is the argument above it, so both stay rooted
	 * across a grow */
	if (target->capacity < target->count + 1) {
		int old_cap = target->capacity;
		target->capacity = old_cap < 4 ? 4 : old_cap * 2;
		target->items = GROW_ARRAY(
		        vm, Value, target->items, old_cap, target->capacity);
	}
	target->items[target->count++] = value_of(value);
	return 1;
}

static int api_list_length(FlValue list, size_t *out)
{
	Value l = value_of(list);
	if (!IS_LIST(l))
		return 0;
	if (out != NULL)
		*out = (size_t)AS_LIST(l)->count;
	return 1;
}

static int api_list_get(FlValue list, size_t index, FlValue *out)
{
	Value l = value_of(list);
	if (!IS_LIST(l) || index >= (size_t)AS_LIST(l)->count)
		return 0;
	if (out != NULL)
		*out = handle_of(AS_LIST(l)->items[index]);
	return 1;
}

static int api_table_set(FlModule *module,
        FlValue table,
        const char *key,
        size_t key_length,
        FlValue value)
{
	if (module == NULL || module->vm == NULL)
		return 0;
	VM *vm = module->vm;
	Value t = value_of(table);
	if (!IS_FLINT_TABLE(t))
		return 0;
	ObjTable *tbl = AS_FLINT_TABLE(t);
	ObjString *k = copy_string(vm, key, (int)key_length);
	/* rooted across the grow below: the table is this call's argument,
	 * but the key is a fresh string nothing else points at */
	vm_push(vm, STR_VAL(k));
	if (tbl->count == tbl->capacity) {
		int grown = tbl->capacity < 4 ? 4 : tbl->capacity * 2;
		ObjString **keys = GROW_ARRAY(
		        vm, ObjString *, tbl->keys, tbl->capacity, grown);
		tbl->keys = keys;
		Value *values = GROW_ARRAY(
		        vm, Value, tbl->values, tbl->capacity, grown);
		tbl->values = values;
		tbl->capacity = grown;
	}
	tbl->keys[tbl->count] = k;
	tbl->values[tbl->count] = value_of(value);
	tbl->count++;
	vm_pop(vm);
	return 1;
}

static int api_table_get(
        FlValue table, const char *key, size_t key_length, FlValue *out)
{
	Value t = value_of(table);
	if (!IS_FLINT_TABLE(t))
		return 0;
	ObjTable *tbl = AS_FLINT_TABLE(t);
	for (int i = 0; i < tbl->count; i++) {
		if ((size_t)tbl->keys[i]->length == key_length &&
		        memcmp(tbl->keys[i]->chars, key, key_length) == 0) {
			if (out != NULL)
				*out = handle_of(tbl->values[i]);
			return 1;
		}
	}
	return 0;
}

static int api_table_has(FlValue table, const char *key, size_t key_length)
{
	FlValue ignored;
	return api_table_get(table, key, key_length, &ignored);
}

static int api_table_length(FlValue table, size_t *out)
{
	Value t = value_of(table);
	if (!IS_FLINT_TABLE(t))
		return 0;
	if (out != NULL)
		*out = (size_t)AS_FLINT_TABLE(t)->count;
	return 1;
}

static FlValue api_retain(FlModule *module, FlValue value)
{
	if (module == NULL)
		return api_nil();
	FlRetained *entry = malloc(sizeof(FlRetained));
	if (entry == NULL)
		return api_nil();
	entry->value = value_of(value);
	entry->next = module->retained;
	module->retained = entry;
	module->retained_count++;
	return value;
}

static void api_release(FlModule *module, FlValue value)
{
	if (module == NULL)
		return;
	FlRetained **link = &module->retained;
	while (*link != NULL) {
		FlRetained *entry = *link;
		if (entry->value == value_of(value)) {
			*link = entry->next;
			free(entry);
			module->retained_count--;
			return;
		}
		link = &entry->next;
	}
}

static size_t api_handle_count(FlModule *module)
{
	return module != NULL ? module->retained_count : 0;
}

static void api_raise(FlModule *module, const char *message)
{
	if (module == NULL || module->vm == NULL)
		return;
	vm_runtime_error(module->vm,
	        "%s",
	        message != NULL ? message : "native module failed");
}

static void api_raise_fmt(FlModule *module, const char *format, ...)
{
	if (module == NULL || module->vm == NULL || format == NULL)
		return;
	char buf[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	api_raise(module, buf);
}

static const char *api_error_category(void) { return "Error"; }

/* ---------------------------------------------------------------------- */
/* the vtable, and the exported trampolines an extension calls             */
/* ---------------------------------------------------------------------- */

static const FlApi flint_api = {
        api_abi_version,
        api_has_capability,
        api_module_name,
        api_module_func,
        api_module_vm,
        api_nil,
        api_bool,
        api_number,
        api_string,
        api_type,
        api_to_bool,
        api_to_number,
        api_to_string,
        api_new_list,
        api_new_table,
        api_list_push,
        api_list_length,
        api_list_get,
        api_table_set,
        api_table_get,
        api_table_has,
        api_table_length,
        api_retain,
        api_release,
        api_handle_count,
        api_raise,
        api_raise_fmt,
        api_error_category,
};

#ifndef _WIN32
static void set_api(const FlApi *table) { fl_api = table; }

static void fl_ext_install_api(void) { set_api(&flint_api); }
#endif /* the Windows loader refuses up front, so it installs nothing */

/*
 * The functions an extension actually calls. Each reads the vtable slot
 * rather than calling the implementation directly, so there is exactly
 * one definition of the behaviour and the extension never links against
 * anything private.
 *
 * A null vtable means no module was ever initialised in this process --
 * an extension loaded outside flint. Every entry point returns the inert
 * answer rather than dereferencing null, so a misuse is a wrong result
 * instead of a segfault.
 */
static const FlApi *api(void) { return fl_api; }

uint32_t fl_abi_version(void)
{
	return api() != NULL ? api()->abi_version() : FL_ABI_VERSION;
}

int fl_has_capability(uint32_t capability)
{
	return api() != NULL ? api()->has_capability(capability) : 0;
}

int fl_module_name(FlModule *module, const char *name)
{
	return api() != NULL ? api()->module_name(module, name) : 0;
}

int fl_module_func(FlModule *module, const char *name, FlNativeFn fn, int arity)
{
	return api() != NULL ? api()->module_func(module, name, fn, arity) : 0;
}

void *fl_module_vm(FlModule *module)
{
	return api() != NULL ? api()->module_vm(module) : NULL;
}

FlValue fl_nil(void)
{
	return api() != NULL ? api()->nil() : handle_of(NIL_VAL);
}

FlValue fl_bool(int value)
{
	return api() != NULL ? api()->boolean(value) : handle_of(NIL_VAL);
}

FlValue fl_number(double value)
{
	return api() != NULL ? api()->number(value) : handle_of(NIL_VAL);
}

FlValue fl_string(FlModule *module, const char *bytes, size_t length)
{
	return api() != NULL ? api()->string(module, bytes, length)
	                     : handle_of(NIL_VAL);
}

FlType fl_type(FlValue value)
{
	return api() != NULL ? api()->type(value) : FL_TYPE_NIL;
}

int fl_is_nil(FlValue v) { return fl_type(v) == FL_TYPE_NIL; }
int fl_is_bool(FlValue v) { return fl_type(v) == FL_TYPE_BOOL; }
int fl_is_number(FlValue v) { return fl_type(v) == FL_TYPE_NUMBER; }
int fl_is_string(FlValue v) { return fl_type(v) == FL_TYPE_STRING; }
int fl_is_list(FlValue v) { return fl_type(v) == FL_TYPE_LIST; }
int fl_is_table(FlValue v) { return fl_type(v) == FL_TYPE_TABLE; }
int fl_is_function(FlValue v) { return fl_type(v) == FL_TYPE_FUNCTION; }

int fl_to_bool(FlValue value)
{
	return api() != NULL ? api()->to_bool(value) : 0;
}

int fl_to_number(FlValue value, double *out)
{
	return api() != NULL ? api()->to_number(value, out) : 0;
}

int fl_to_string(FlValue value, const char **bytes, size_t *length)
{
	return api() != NULL ? api()->to_string(value, bytes, length) : 0;
}

FlValue fl_new_list(FlModule *module)
{
	return api() != NULL ? api()->new_list(module) : handle_of(NIL_VAL);
}

FlValue fl_new_table(FlModule *module)
{
	return api() != NULL ? api()->new_table(module) : handle_of(NIL_VAL);
}

int fl_list_push(FlModule *module, FlValue list, FlValue value)
{
	return api() != NULL ? api()->list_push(module, list, value) : 0;
}

int fl_list_length(FlValue list, size_t *out)
{
	return api() != NULL ? api()->list_length(list, out) : 0;
}

int fl_list_get(FlValue list, size_t index, FlValue *out)
{
	return api() != NULL ? api()->list_get(list, index, out) : 0;
}

int fl_table_set(FlModule *module,
        FlValue table,
        const char *key,
        size_t key_length,
        FlValue value)
{
	return api() != NULL
	               ? api()->table_set(module, table, key, key_length, value)
		       : 0;
}

int fl_table_get(
        FlValue table, const char *key, size_t key_length, FlValue *out)
{
	return api() != NULL ? api()->table_get(table, key, key_length, out)
	                     : 0;
}

int fl_table_has(FlValue table, const char *key, size_t key_length)
{
	return api() != NULL ? api()->table_has(table, key, key_length) : 0;
}

int fl_table_length(FlValue table, size_t *out)
{
	return api() != NULL ? api()->table_length(table, out) : 0;
}

FlValue fl_retain(FlModule *module, FlValue value)
{
	return api() != NULL ? api()->retain(module, value)
	                     : handle_of(NIL_VAL);
}

void fl_release(FlModule *module, FlValue value)
{
	if (api() != NULL)
		api()->release(module, value);
}

size_t fl_handle_count(FlModule *module)
{
	return api() != NULL ? api()->handle_count(module) : 0;
}

void fl_raise(FlModule *module, const char *message)
{
	if (api() != NULL)
		api()->raise(module, message);
}

void fl_raise_fmt(FlModule *module, const char *format, ...)
{
	if (api() == NULL || format == NULL)
		return;
	char buf[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	api()->raise(module, buf);
}

const char *fl_error_category(void)
{
	return api() != NULL ? api()->error_category() : "Error";
}
/* ---------------------------------------------------------------------- */
/* loading                                                                  */
/* ---------------------------------------------------------------------- */

#ifndef _WIN32
/*
 * One trampoline serves every function of every module: it recovers which
 * function it is from the ObjNative it was reached through, whose private
 * user_data is the binding. The interpreter publishes the callee on the VM
 * for the duration of the call, which is what makes a single static-free
 * trampoline correct under nesting -- each call saves and restores the
 * previous value, so a module that calls back into flint gets its own.
 */
static Value ext_trampoline(VM *vm, int argc, Value *argv)
{
	const ObjNative *callee = vm->current_native;
	if (callee == NULL || callee->user_data == NULL)
		return NIL_VAL;
	ExtBinding *binding = (ExtBinding *)callee->user_data;
	if (binding->fn == NULL)
		return NIL_VAL;
	if (binding->module != NULL &&
	        binding->module->state == FL_EXT_QUIESCING) {
		/* an unload was requested: no new work enters. the calls
		 * already inside finish normally, which is what lets the
		 * reference check below ever pass. */
		fl_raise(binding->module,
		        "module is shutting down; no new calls accepted");
		return NIL_VAL;
	}

	int count = argc;
	if (count > FL_EXT_MAX_ARGS)
		count = FL_EXT_MAX_ARGS;

	/* handles, not pointers: an extension cannot keep one past this call
	 * without fl_retain, which is the entire reason a handle is a value
	 * and not an address into the VM */
	FlValue args[FL_EXT_MAX_ARGS];
	for (int i = 0; i < count; i++)
		args[i] = handle_of(argv[i]);

	if (binding->module != NULL)
		binding->module->active_calls++;
	FlValue result = binding->fn(binding->module, argc, args);
	if (binding->module != NULL)
		binding->module->active_calls--;
	if (vm->has_pending || vm->pending_catch)
		/* the extension raised; whatever it returned is not a value */
		return NIL_VAL;
	return value_of(result);
}

/*
 * Register one function. Each gets its own ObjNative carrying its own
 * binding, so dispatch is a pointer read rather than a name lookup on
 * every call -- the same reason the built-ins are natives and not a
 * dictionary.
 */
static bool ext_register_one(
        VM *vm, FlModule *module, const char *name, FlNativeFn fn, int arity)
{
	ExtBinding *binding = calloc(1, sizeof(ExtBinding));
	if (binding == NULL)
		return false;
	binding->module = module;
	binding->fn = fn;
	vm_define_native_with_data(vm, name, ext_trampoline, arity, binding);
	return true;
}
#endif /* registration-only code; see ExtBinding above */

bool fl_ext_load_native(VM *vm,
        const char *path,
        const char *module_name,
        char *error,
        size_t error_size)
{
#ifndef _WIN32
	void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) {
		const char *why = dlerror();
		snprintf(error,
		        error_size,
		        "cannot load '%s': %s",
		        path,
		        why != NULL ? why : "unknown error");
		return false;
	}
	/* deliberately not dlclose(): a function it registered stays callable
	 * for the life of the VM, and a value it produced can outlive the call
	 * that made it. Unloading would be a use-after-free waiting for the
	 * collector. One handle per library per process is the price. */
	dlerror();
	typedef int (*FlInitFn)(FlModule *, uint32_t);
	FlInitFn init = (FlInitFn)(uintptr_t)dlsym(handle, FL_MODULE_FUNC);
	if (init == NULL) {
		/* entered nothing, registered nothing: safe to close. */
		dlclose(handle);
		snprintf(error,
		        error_size,
		        "'%s' is not a flint module: no " FL_MODULE_FUNC
		        " symbol",
		        path);
		return false;
	}

	FlModule *module = calloc(1, sizeof(FlModule));
	if (module == NULL) {
		dlclose(handle);
		snprintf(error, error_size, "out of memory");
		return false;
	}
	module->vm = vm;
	module->state = FL_EXT_LOADED;
	fl_module_name(module, module_name);

	/* the vtable has to be in place before init runs: the very first call
	 * an extension makes may be fl_module_name */
	fl_ext_install_api();

	if (init(module, FL_ABI_VERSION) != FL_INIT_OK) {
		/* nothing registered yet, so closing is safe: no flint code
		 * can reach into this library. */
		dlclose(handle);
		free(module);
		snprintf(error,
		        error_size,
		        "module '%s' failed to initialise",
		        module_name);
		return false;
	}
	module->init_done = true;

	for (int i = 0; i < module->func_count; i++) {
		if (!ext_register_one(vm,
		            module,
		            module->funcs[i].name,
		            module->funcs[i].fn,
		            module->funcs[i].arity)) {
			/* partial registration is rolled back by not being
			 * rolled back: the functions already defined are
			 * harmless without the module being reachable, and
			 * the loader reports the failure. the library stays
			 * open for the same reason a loaded one does -- those
			 * functions are callable now. */
			snprintf(error,
			        error_size,
			        "module '%s' registered too many functions",
			        module_name);
			return false;
		}
	}
	if (module->func_count == 0) {
		dlclose(handle);
		free(module);
		snprintf(error,
		        error_size,
		        "module '%s' exported no functions",
		        module_name);
		return false;
	}
	ExtLoaded *record = calloc(1, sizeof(ExtLoaded));
	if (record == NULL) {
		snprintf(error, error_size, "out of memory");
		return false;
	}
	snprintf(record->path, sizeof(record->path), "%s", path);
	record->lib = handle;
	record->module = module;
	record->next = ext_loaded;
	ext_loaded = record;
	return true;
#else
	(void)vm;
	(void)path;
	(void)module_name;
	snprintf(error,
	        error_size,
	        "native modules need a POSIX system; this build is windows");
	return false;
#endif
}

/*
 * Ask a loaded module to shut down and unload. See ext.h for the
 * contract; here is the mechanism.
 *
 * The check order is the point. Active calls first: unloading with one in
 * flight returns while its code is on the C stack, which no later check
 * can undo. Retained handles next: the collector roots them, so the value
 * survives, but the code that interprets a retained function handle does
 * not. Registered functions last, and this is the one that never passes:
 * registration put ObjNatives into the VM's globals, reachable for as long
 * as the VM lives, so there is always a route to the library's code.
 *
 * That makes physical unloading unreachable today, and the code below says
 * so rather than deleting the path: the day deregistration exists, the
 * reference check is already in the right order. Until then every request
 * answers busy, the library stays open, and nothing is force-closed.
 */
int fl_ext_request_unload(
        VM *vm, const char *path, char *error, size_t error_size)
{
	(void)vm;
#ifndef _WIN32
	ExtLoaded *record = ext_loaded;
	while (record != NULL && strcmp(record->path, path) != 0)
		record = record->next;
	if (record == NULL) {
		snprintf(error,
		        error_size,
		        "'%s' is not a native module this process loaded",
		        path);
		return -1;
	}
	FlModule *module = record->module;
	module->state = FL_EXT_QUIESCING;
	if (module->active_calls > 0) {
		snprintf(error,
		        error_size,
		        "module '%s' is busy: %d call(s) still inside it",
		        module->name,
		        module->active_calls);
		return 1;
	}
	if (module->retained_count > 0) {
		snprintf(error,
		        error_size,
		        "module '%s' is busy: %lu retained handle(s) "
		        "outstanding -- release them and ask again",
		        module->name,
		        (unsigned long)module->retained_count);
		return 1;
	}
	if (module->func_count > 0) {
		snprintf(error,
		        error_size,
		        "module '%s' is busy: %d exported function(s) stay "
		        "callable for the life of the VM, so the library "
		        "stays loaded until process exit",
		        module->name,
		        module->func_count);
		return 1;
	}
	dlclose(record->lib);
	ExtLoaded **link = &ext_loaded;
	while (*link != NULL && *link != record)
		link = &(*link)->next;
	if (*link != NULL)
		*link = record->next;
	free(module);
	free(record);
	return 0;
#else
	(void)path;
	snprintf(error,
	        error_size,
	        "native modules need a POSIX system; this build is windows");
	return -1;
#endif
}
