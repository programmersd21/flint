/* SPDX-License-Identifier: MIT */
/*
 * The built-in library.
 *
 * Six functions for scripts, plus import_file, which the compiler emits a
 * call to for every `import`. A native gets a pointer to its arguments on
 * the VM stack and returns a single value. It runs on the C stack inside the
 * caller's frame, so it cannot be preempted, and if it calls vm_interpret()
 * the whole thing is reentrant. import_file does exactly that.
 */
#include "native.h"
#include "memory.h"
#include "object.h"
#include "stdint.h"
#include "table.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* process cpu time, not wall clock. makes benchmarks reproducible. */
static Value clock_native(VM *vm, int argc, Value *argv)
{
	(void)vm;
	(void)argc;
	(void)argv;
	return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

/* strings and lists. byte length for strings, element count for lists. */
static Value len_native(VM *vm, int argc, Value *argv)
{
	(void)vm;
	(void)argc;
	Value val = argv[0];
	if (IS_STRING(val))
		return NUMBER_VAL((double)AS_STRING(val)->length);
	if (IS_LIST(val))
		return NUMBER_VAL((double)AS_LIST(val)->count);
	vm_runtime_error(vm, "Argument to len() must be a string or list.");
	return NIL_VAL;
}

/*
 * Append in place. The list is mutated, not copied, so push() inside a loop
 * is O(1) amortized rather than O(n) per element. returns the item, so
 * push(l, x) is usable as an expression.
 */
static Value push_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_LIST(argv[0])) {
		vm_runtime_error(
		        vm, "First argument to push() must be a list.");
		return NIL_VAL;
	}
	ObjList *list = AS_LIST(argv[0]);
	Value item = argv[1];

	/* argv is still on the stack, so list and item are both rooted
	 * across the grow */
	if (list->capacity < list->count + 1) {
		int old_cap = list->capacity;
		list->capacity = GROW_CAPACITY(old_cap);
		list->items = GROW_ARRAY(
		        vm, Value, list->items, old_cap, list->capacity);
	}
	list->items[list->count++] = item;
	return item;
}

/* shrink by one. the slot is not cleared, so the value stays reachable. */
static Value pop_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_LIST(argv[0])) {
		vm_runtime_error(vm, "Argument to pop() must be a list.");
		return NIL_VAL;
	}
	ObjList *list = AS_LIST(argv[0]);
	if (list->count == 0) {
		vm_runtime_error(vm, "Cannot pop from an empty list.");
		return NIL_VAL;
	}
	return list->items[--list->count];
}

/*
 * string form of any value. strings are returned as themselves, so this is
 * free in the common case. numbers reuse the integral check from print():
 * "4" not "4.000000".
 */
static Value str_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	Value val = argv[0];
	if (IS_STRING(val))
		return val;
	if (IS_NUMBER(val)) {
		char buf[64];
		double d = AS_NUMBER(val);
		if (fl_double_is_printable_int(d))
			snprintf(buf, sizeof(buf), "%ld", fl_double_to_long(d));
		else
			snprintf(buf, sizeof(buf), "%.15g", d);
		return OBJ_VAL(copy_string(vm, buf, (int)strlen(buf)));
	}
	if (IS_BOOL(val))
		return OBJ_VAL(copy_string(vm,
		        AS_BOOL(val) ? "true" : "false",
		        AS_BOOL(val) ? 4 : 5));
	if (IS_NIL(val))
		return OBJ_VAL(copy_string(vm, "nil", 3));
	/* no structure is rendered, so everything else is one opaque token */
	return OBJ_VAL(copy_string(vm, "<object>", 8));
}

/* the type name, as a string. type() is the only way to introspect. */
static Value type_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	/*
	 * flint_type_name() knows the mapping and a cast quotes it in its
	 * error message, so there is exactly one place that decides what a
	 * value's type is called. Two lists would drift.
	 *
	 * it returns a string literal, and copy_string interns it, so the
	 * result is a normal interned string. only "unknown" is built here,
	 * because no other name in the language can reach that branch.
	 */
	const char *name = flint_type_name(argv[0]);
	return OBJ_VAL(copy_string(vm, name, (int)strlen(name)));
}

/*
 * Read a module and run it in the same VM, so its top-level `let` lands in
 * the same globals table and its `export` is just a definition.
 *
 * This is the only place the runtime reenters the interpreter. There is no
 * module cache, so importing the same file twice runs it twice, and no cycle
 * detection, so a file that imports itself recurses until the stack gives
 * out. Both are known and neither has been worth fixing.
 */
static Value import_file_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0])) {
		vm_runtime_error(vm,
		        "Argument to import_file() must be a file path "
		        "string.");
		return NIL_VAL;
	}

	/*
	 * The path is relative to the process working directory, not to the
	 * importing file. Fixing that means tracking a directory per frame,
	 * which is more machinery than a one-file import loop needs.
	 */
	const char *path = AS_CSTRING(argv[0]);
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		vm_runtime_error(vm, "Could not open module file '%s'.", path);
		return NIL_VAL;
	}
	/*
	 * ftell returns -1 on failure, and a module path can just as easily
	 * be a pipe or a directory as a regular file. Test it before
	 * storing: as a size_t that -1 becomes SIZE_MAX, the +1 below wraps
	 * to 0, and the fread writes past a zero byte allocation.
	 */
	if (fseek(file, 0L, SEEK_END) != 0) {
		fclose(file);
		vm_runtime_error(
		        vm, "Could not seek in module file '%s'.", path);
		return NIL_VAL;
	}
	long length = ftell(file);
	if (length < 0) {
		fclose(file);
		vm_runtime_error(vm, "Could not size module file '%s'.", path);
		return NIL_VAL;
	}
	size_t size = (size_t)length;
	/* rewind() swallows the seek error; fseek() does not */
	if (fseek(file, 0L, SEEK_SET) != 0) {
		fclose(file);
		vm_runtime_error(
		        vm, "Could not rewind module file '%s'.", path);
		return NIL_VAL;
	}

	/* malloc, not ALLOCATE: the buffer is handed to vm_interpret, which
	 * roots what it needs, and it must survive a collection. */
	char *buffer = (char *)malloc(size + 1);
	if (buffer == NULL) {
		fclose(file);
		vm_runtime_error(
		        vm, "Out of memory reading module '%s'.", path);
		return NIL_VAL;
	}

	/* a short read means a directory, a pipe, or a race. an unterminated
	 * buffer would be handed to the compiler as a truncated script */
	size_t bytes_read = fread(buffer, 1, size, file);
	if (bytes_read < size) {
		free(buffer);
		fclose(file);
		vm_runtime_error(vm, "Could not read module file '%s'.", path);
		return NIL_VAL;
	}
	/*
	 * The NUL lands in the byte the +1 above was allocated for. size is
	 * the same value used in the malloc, so this is in bounds by
	 * construction; the analyser reports a tainted index because size
	 * came from ftell on a path the script chose, and it does not tie
	 * the index back to the allocation.
	 */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	buffer[size] = '\0';
	fclose(file);

	InterpretResult res = vm_interpret(vm, buffer);
	free(buffer);
	if (res != INTERPRET_OK)
		return NIL_VAL;

	return TRUE_VAL;
}

/* called from vm_init(), before any user code runs. */
void register_natives(VM *vm)
{
	vm_define_native(vm, "clock", clock_native, 0);
	vm_define_native(vm, "len", len_native, 1);
	vm_define_native(vm, "push", push_native, 2);
	vm_define_native(vm, "pop", pop_native, 1);
	vm_define_native(vm, "str", str_native, 1);
	vm_define_native(vm, "type", type_native, 1);
	/* not in the manual: the compiler emits this for `import` */
	vm_define_native(vm, "import_file", import_file_native, 1);
}
