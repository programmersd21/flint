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
#include "native_math.h"
#include "object.h"
#include "stdint.h"
#include "stringx.h"
#include "sys.h"
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

/*
 * input([prompt])
 *
 * Write the prompt if there is one, read one line, return it without the
 * newline. Returns nil at end of file, so `if line == nil` is how a script
 * tells "the user pressed enter" from "there is nothing left to read".
 *
 * Three things here are not the obvious version.
 *
 * The buffer grows. A fixed size would silently truncate a long line, and a
 * script that reads a file-like amount of text through stdin has no way to
 * know it happened. Starting at 64 and doubling is the same shape as every
 * other array in the runtime.
 *
 * The buffer is malloc, not ALLOCATE. It is raw bytes on the way in and a
 * flint string on the way out, and it never needs to be traced by the
 * collector. Using ALLOCATE would mean the GC could collect while a C
 * pointer into the middle of it is still live, and the only way to be right
 * there is to keep it out of the heap the collector walks entirely.
 *
 * The return value is interned, and copy_string() can collect, so nothing
 * derived from the buffer is read after that point. The NUL is written before
 * the copy for the same reason: a value that lives only in a C local while
 * the collector runs is a value that can be freed under your feet.
 */
static Value input_native(VM *vm, int argc, Value *argv)
{
	/* the prompt is optional. arity is checked by the caller for a
	 * fixed-arity native, so with two forms we check it here. */
	if (argc > 1) {
		vm_runtime_error(
		        vm, "expected 0 or 1 arguments but got %d.", argc);
		return NIL_VAL;
	}

	if (argc == 1) {
		if (!IS_STRING(argv[0])) {
			vm_runtime_error(
			        vm, "argument to input() must be a string.");
			return NIL_VAL;
		}
		/* no newline. the user is standing there waiting. */
		fputs(AS_CSTRING(argv[0]), stdout);
		fflush(stdout);
	}

	size_t capacity = 64;
	size_t length = 0;
	char *buffer = malloc(capacity);
	if (buffer == NULL) {
		vm_runtime_error(vm, "out of memory reading input.");
		return NIL_VAL;
	}

	/* read byte by byte: fgets would cap the line and getline is not
	 * portable C11. one getchar per byte is the only version that
	 * cannot truncate and the only one that works everywhere. */
	int c;
	while ((c = getchar()) != EOF) {
		if (c == '\n')
			break;

		/* tolerate CRLF from a windows terminal, or a file written on
		 * one. without this a script sees a trailing \r in every
		 * line and the user does not. */
		if (c == '\r')
			continue;

		if (length + 1 >= capacity) {
			capacity *= 2;
			char *grown = realloc(buffer, capacity);
			if (grown == NULL) {
				free(buffer);
				vm_runtime_error(vm,
				        "out of memory reading "
				        "input.");
				return NIL_VAL;
			}
			buffer = grown;
		}

		buffer[length++] = (char)c;
	}

	/* a read that ended at EOF rather than at a newline. a final line
	 * with no trailing newline is still a line, not nothing. */
	if (c == EOF && length == 0) {
		free(buffer);
		return NIL_VAL;
	}

	buffer[length] = '\0';

	/* copy_string() interns and can collect. buffer is malloc'd and is
	 * not a gc object, so it survives, but nothing below this point
	 * reads it again once the copy is in flight. */
	ObjString *line = new_string(vm, buffer, (int)length);
	free(buffer);

	return STR_VAL(line);
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
	vm_runtime_error(vm, "argument to len() must be a string or list.");
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
		        vm, "first argument to push() must be a list.");
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
		vm->counters.list_grows++;
	}
	list->items[list->count++] = item;
	return item;
}

/* shrink by one. the slot is not cleared, so the value stays reachable. */
static Value pop_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_LIST(argv[0])) {
		vm_runtime_error(vm, "argument to pop() must be a list.");
		return NIL_VAL;
	}
	ObjList *list = AS_LIST(argv[0]);
	if (list->count == 0) {
		vm_runtime_error(vm, "cannot pop from an empty list.");
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
		/* the digit loop first. a loop that builds a hundred
		 * thousand strings with str() is otherwise dominated by
		 * snprintf, which costs ~250ns for a call that a digit
		 * loop does in about twenty. */
		if (fl_double_is_printable_int(d) &&
		        fl_itoa(fl_double_to_long(d), buf, sizeof(buf)) > 0) {
			/* buf already holds the digits */
		} else if (fl_double_is_printable_int(d)) {
			snprintf(buf, sizeof(buf), "%ld", fl_double_to_long(d));
		} else {
			snprintf(buf, sizeof(buf), "%.15g", d);
		}
		return STR_VAL(new_string(vm, buf, (int)strlen(buf)));
	}
	if (IS_BOOL(val))
		return STR_VAL(new_string(vm,
		        AS_BOOL(val) ? "true" : "false",
		        AS_BOOL(val) ? 4 : 5));
	if (IS_NIL(val))
		return STR_VAL(new_string(vm, "nil", 3));
	/* no structure is rendered, so everything else is one opaque token */
	return STR_VAL(new_string(vm, "<object>", 8));
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
	return STR_VAL(copy_string(vm, name, (int)strlen(name)));
}

/*
 * Read a module and run it in the same VM, so its top-level `let` lands in
 * the same globals table and its `export` is just a definition.
 *
 * This is the only place the runtime reenters the interpreter. Resolved
 * paths are cached, and an in-flight entry catches import cycles.
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

	const char *raw = AS_CSTRING(argv[0]);

	/*
	 * Resolve against the importing file's directory, not the process
	 * working directory. A module path is part of the source, so it
	 * means the same thing no matter where the user is standing. This
	 * used to be process-relative and meant a script only worked from
	 * one directory, which is not a property any language should have.
	 *
	 * An absolute path is left alone by sys_resolve_module().
	 */
	char *path = sys_resolve_module(raw);
	if (path == NULL) {
		vm_runtime_error(vm, "cannot resolve module path '%s'.", raw);
		return NIL_VAL;
	}

	/*
	 * The cache. The key is the resolved path, interned, so two imports
	 * of the same file spelled the same way are the same key, and two
	 * files that are actually different are different keys even if one
	 * is a symlink to the other only in a way flint cannot see. That is
	 * the correct definition of "the same module": same resolved name.
	 *
	 * TRUE means loaded, NIL means in flight. The NIL case is a cycle:
	 * this file is already being executed further up the stack, so
	 * running it again would recurse forever.
	 */
	ObjString *key = copy_string(vm, path, (int)strlen(path));
	vm_push(vm, STR_VAL(key)); /* rooted: every call below can collect */

	Value cached;
	if (table_get(&vm->modules, key, &cached)) {
		vm_pop(vm); /* the key */
		free(path);
		if (IS_NIL(cached)) {
			vm_runtime_error(vm,
			        "import cycle: '%s' is already being "
			        "loaded.",
			        raw);
			return NIL_VAL;
		}
		return TRUE_VAL; /* already loaded. nothing to do. */
	}

	/* mark it in flight before running, so a cycle inside sees this */
	table_set(vm, &vm->modules, key, NIL_VAL);

	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		vm_runtime_error(vm, "could not open module file '%s'.", raw);
		/* remove the in-flight marker: the file did not load, and
		 * leaving it marked would make a later attempt look like a
		 * cycle rather than a missing file. */
		table_delete(&vm->modules, key);
		vm_pop(vm);
		free(path);
		return NIL_VAL;
	}
	/* from here every path out must free(path) */
	/*
	 * ftell returns -1 on failure, and a module path can just as easily
	 * be a pipe or a directory as a regular file. Test it before
	 * storing: as a size_t that -1 becomes SIZE_MAX, the +1 below wraps
	 * to 0, and the fread writes past a zero byte allocation.
	 */
	if (fseek(file, 0L, SEEK_END) != 0) {
		fclose(file);
		free(path);
		vm_runtime_error(
		        vm, "could not seek in module file '%s'.", raw);
		return NIL_VAL;
	}
	long length = ftell(file);
	if (length < 0) {
		fclose(file);
		free(path);
		vm_runtime_error(vm, "could not size module file '%s'.", raw);
		return NIL_VAL;
	}
	size_t size = (size_t)length;
	/* rewind() swallows the seek error; fseek() does not */
	if (fseek(file, 0L, SEEK_SET) != 0) {
		fclose(file);
		free(path);
		vm_runtime_error(vm, "could not rewind module file '%s'.", raw);
		return NIL_VAL;
	}

	/* malloc, not ALLOCATE: the buffer is handed to vm_interpret, which
	 * roots what it needs, and it must survive a collection. */
	char *buffer = (char *)malloc(size + 1);
	if (buffer == NULL) {
		fclose(file);
		free(path);
		vm_runtime_error(vm, "out of memory reading module '%s'.", raw);
		return NIL_VAL;
	}

	/* a short read means a directory, a pipe, or a race. an unterminated
	 * buffer would be handed to the compiler as a truncated script */
	size_t bytes_read = fread(buffer, 1, size, file);
	if (bytes_read < size) {
		free(buffer);
		fclose(file);
		free(path);
		vm_runtime_error(vm, "could not read module file '%s'.", raw);
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

	/*
	 * vm_interpret below can fail and, worse, can import further modules
	 * that resolve against *their* importing file. the source directory is
	 * a single VM-wide setting, so a nested import overwrites it and the
	 * next import in the outer file would resolve against the wrong
	 * directory. push the current one, run, restore. this is the
	 * difference between 'lib/math.fl' meaning one thing and meaning
	 * whatever the last nested import left behind.
	 */
	/*
	 * Snapshot the global names so we can tell which ones the module
	 * added. only needed for a library, and only for a few hundred
	 * entries, so the scan is linear and the array is short-lived.
	 */
	ObjString **before_keys = NULL;
	Value *before_vals = NULL;
	int before_count = 0;
	bool is_library = strchr(raw, '/') == NULL;
	if (is_library && vm->globals.count > 0) {
		before_count = vm->globals.count;
		before_keys =
		        malloc(sizeof(ObjString *) * (size_t)before_count);
		before_vals = malloc(sizeof(Value) * (size_t)before_count);
		/*
		 * Both frees, not just the one that failed.
		 *
		 * malloc returning NULL for the second call and not the first is
		 * entirely ordinary, and taking the early return while holding
		 * the successful one is a leak that only shows up under memory
		 * pressure -- which is exactly when a leak is most expensive and
		 * least likely to be noticed. This is the shape clang-analyzer
		 * flags as a leak on both buffers, and it was a real leak.
		 */
		if (before_keys == NULL || before_vals == NULL) {
			free(before_keys);
			free(before_vals);
			free(path);
			vm_pop(vm);
			vm_runtime_error(vm, "Out of memory in import.");
			return NIL_VAL;
		}
		/*
		 * count is decremented as entries are written, not fixed up
		 * afterwards.
		 *
		 * The loop below stops early when it runs out of slots with a
		 * real key, so a fixed before_count would leave the tail of both
		 * arrays uninitialised -- and the comparison loops further down
		 * read up to before_count. Sizing the count to what was actually
		 * written makes the arrays' extent and the loop bound the same
		 * number, which is the only way they cannot disagree.
		 */
		int written = 0;
		for (int i = 0;
		        i < vm->globals.capacity && written < before_count;
		        i++) {
			if (vm->globals.entries[i].key == NULL)
				continue;
			before_keys[written] = vm->globals.entries[i].key;
			before_vals[written] = vm->globals.entries[i].value;
			written++;
		}
		before_count = written;
	}

	InterpretResult res = vm_interpret_named(vm, buffer, path);
	free(buffer);
	free(path);
	if (res != INTERPRET_OK) {
		free(before_keys);
		free(before_vals);
		/* a module that failed partway is not "loaded". drop the
		 * marker so a retry re-runs it rather than looking like
		 * a cycle. its partial globals stay, which is flint's
		 * documented behaviour for a failed import. */
		table_delete(&vm->modules, key);
		vm_pop(vm);
		return NIL_VAL;
	}

	/*
	 * A library import binds what the module defined to a table named
	 * after the library, so `import math` gives you `math.sqrt` rather
	 * than a flat `sqrt` that collides with whatever the caller
	 * already had.
	 *
	 * "what the module defined" is found by diffing the globals table
	 * across the run, which is crude. it is also honest about why: a
	 * real export list is a compiler change, and this needs no new
	 * syntax, no new state, and no second module system. `export` stays
	 * a convention and this is the one place it is enforced.
	 */
	if (is_library) {
		ObjTable *bag = new_flint_table(vm);
		vm_push(vm, OBJ_VAL(bag));

		for (int i = 0; i < vm->globals.capacity; i++) {
			ObjString *gname = vm->globals.entries[i].key;
			if (gname == NULL)
				continue;
			bool redefined = false;
			for (int k = 0; k < before_count; k++) {
				if (before_keys[k] == gname &&
				        !values_equal(before_vals[k],
				                vm->globals.entries[i].value)) {
					redefined = true;
					break;
				}
			}
			bool existed = false;
			for (int k = 0; k < before_count; k++) {
				if (before_keys[k] == gname) {
					existed = true;
					break;
				}
			}
			if (existed && !redefined)
				continue;
			if (bag->count == bag->capacity) {
				int old = bag->capacity;
				bag->capacity = old > 0 ? old * 2 : 8;
				bag->keys = realloc(bag->keys,
				        sizeof(ObjString *) *
				                (size_t)bag->capacity);
				bag->values = realloc(bag->values,
				        sizeof(Value) * (size_t)bag->capacity);
				if (bag->keys == NULL || bag->values == NULL) {
					/*
					 * Both frees. `before_keys` and `before_vals`
					 * are snapshots taken before the module ran,
					 * still live on this path, and the bag is
					 * rooted on the value stack so the collector
					 * will find that one.
					 */
					free(before_keys);
					free(before_vals);
					vm_pop(vm); /* the bag */
					vm_runtime_error(
					        vm, "Out of memory in import.");
					return NIL_VAL;
				}
			}
			vm_push(vm, STR_VAL(gname));
			bag->keys[bag->count] = gname;
			bag->values[bag->count] = vm->globals.entries[i].value;
			bag->count++;
			vm_pop(vm);
		}

		/*
		 * Bind under the name that was *asked for*, not the
		 * resolved path. `key` is the interned resolved path, so
		 * using it here would define a global called
		 * "lib/math.fl" and leave `math` undefined, which is
		 * exactly the bug this replaced.
		 */
		ObjString *libname = copy_string(vm, raw, (int)strlen(raw));
		vm_push(vm, STR_VAL(libname));
		table_set(vm, &vm->globals, libname, OBJ_VAL(bag));
		vm_pop(vm);
		vm_pop(vm); /* the bag */
	}
	free(before_keys);
	free(before_vals);

	table_set(vm, &vm->modules, key, TRUE_VAL);
	vm_pop(vm); /* the key */
	return TRUE_VAL;
}

/* called from vm_init(), before any user code runs. */
/*
 * __slice(s, from, to) -> string
 *
 * The one string operation flint was missing. half-open, like everything
 * else here, and out-of-range clamps rather than erroring, which is what a
 * slice wants: asking for past the end of a string is a normal thing to do
 * when the length is not known in advance.
 *
 * a negative index counts from the end, so a caller that does not know the
 * length can still take the last n.
 */
static Value slice_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0]) || !IS_NUMBER(argv[1]) || !IS_NUMBER(argv[2])) {
		vm_runtime_error(
		        vm, "__slice() takes a string and two numbers.");
		return NIL_VAL;
	}

	ObjString *s = AS_STRING(argv[0]);
	int len = s->length;
	double fd = AS_NUMBER(argv[1]);
	double td = AS_NUMBER(argv[2]);

	if (fd != (double)(int)fd || td != (double)(int)td) {
		vm_runtime_error(vm, "__slice() bounds must be whole numbers.");
		return NIL_VAL;
	}

	int from = (int)fd;
	int to = (int)td;
	if (from < 0)
		from += len;
	if (to < 0)
		to += len;
	if (from < 0)
		from = 0;
	if (to > len)
		to = len;
	if (to < from)
		to = from;

	ObjString *out = new_string(vm, s->chars + from, to - from);
	return STR_VAL(out);
}

void register_natives(VM *vm)
{
	vm_define_native(vm, "clock", clock_native, 0);
	/* -1 for the arity because input() takes zero or one argument, and
	 * a fixed-arity native cannot express that. the check is inside. */
	vm_define_native(vm, "input", input_native, -1);
	vm_define_native(vm, "len", len_native, 1);
	vm_define_native(vm, "push", push_native, 2);
	vm_define_native(vm, "pop", pop_native, 1);
	vm_define_native(vm, "str", str_native, 1);
	vm_define_native(vm, "type", type_native, 1);
	vm_define_native(vm, "__slice", slice_native, 3);
	/* args, env, exit, read_file, write_file, exec. a different kind of
	 * thing from the ones above, which is why they live in sys.c. */
	register_sys_natives(vm);
	register_string_natives(vm);
	/* not in the manual: the compiler emits this for `import` */
	vm_define_native(vm, "import_file", import_file_native, 1);
	register_math_natives(vm);
}
