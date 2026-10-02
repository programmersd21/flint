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
#include "common.h"
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
 * import_file(path) -> table of the module's exports
 *
 * This is the only place the runtime reenters the interpreter, and it is
 * where most of 0.6.0's module semantics live.
 *
 * What it does, in order:
 *
 *   1. resolve the path against the importing file, not the process cwd
 *   2. look it up in the module cache: loaded, loading, or failed
 *   3. push a fresh globals table and run the module inside it
 *   4. copy only the names the module exported into a fresh table
 *   5. bind that table in the *importing* module under the asked-for name
 *
 * Step 3 is the whole of module isolation. `vm->globals` points at the
 * running module's table, so every OP_DEFINE_GLOBAL inside the module lands
 * there and nowhere else. Two modules can both define a private `scale` and
 * they stay separate, which they did not before: a shared table meant the
 * second one to load silently overwrote the first.
 *
 * Step 4 is why `export` finally means something. It used to be a comment:
 * the declaration was compiled as if the keyword were not there, and the
 * "exports" were whatever the module had added to the shared table, found by
 * diffing the table before and after the run. Diffing cannot know which names
 * were meant to be private -- a module's own helper looked exactly like an
 * export -- and it cannot survive a module that fails partway. The compiler
 * emits an explicit export list now, and this reads it.
 *
 * Step 5 is where the module becomes visible, and it binds in the importer's
 * table. A module cannot define a name in its importer, which is what makes
 * "private" mean private.
 *
 * Failure is transactional: on any error the module is marked failed, its
 * table is discarded, and nothing it defined is bound anywhere. A later
 * import of the same path reports the failure rather than retrying a
 * half-initialised module.
 */
static Value import_file_native(VM *vm, int argc, Value *argv)
{
	if (argc != 1) {
		vm_runtime_error(vm, "import_file() takes one argument.");
		return NIL_VAL;
	}
	if (!IS_STRING(argv[0])) {
		vm_runtime_error(vm,
		        "argument to import_file() must be a file path "
		        "string.");
		return NIL_VAL;
	}

	const char *raw = AS_CSTRING(argv[0]);

	/*
	 * Resolve against the importing file's directory, not the process
	 * working directory. A module path is part of the source, so it means
	 * the same thing no matter where the user is standing.
	 *
	 * An absolute path is left alone by sys_resolve_module().
	 */
	char *path = sys_resolve_module(raw);
	if (path == NULL) {
		vm_runtime_error(vm, "cannot resolve module path '%s'.", raw);
		return NIL_VAL;
	}

	/* Rooted across everything below: every step can allocate. */
	ObjString *key = copy_string(vm, path, (int)strlen(path));
	vm_push(vm, STR_VAL(key));

	/*
	 * The cache holds one of three things, and the value is the state:
	 *
	 *   a table   loaded. this is also the module's exports, so a
	 *             repeated import is a lookup rather than a second run
	 *   nil       in flight. the same path is already being executed
	 *             further up the import stack
	 *   false     failed. re-running would repeat a failure the user
	 *             has not changed anything about
	 *
	 * The loaded case has to be checked first and by type, because a
	 * module's exports are an ordinary Flint table and could in
	 * principle be any value -- testing for TRUE instead would report
	 * every successful module as failed.
	 */
	Value cached;
	if (table_get(&vm->modules, key, &cached)) {
		if (IS_FLINT_TABLE(cached)) {
			vm_pop(vm); /* the key */
			free(path);
			return cached;
		}
		if (IS_NIL(cached)) {
			/*
			 * NIL means in flight, which means this file is already
			 * being executed further up the import stack. Running it
			 * again would recurse until the frame limit, so the
			 * cycle is reported here with the path that caused it.
			 */
			vm_runtime_error(vm,
			        "import cycle: '%s' is already being loaded.",
			        raw);
			vm_pop(vm); /* the key */
			free(path);
			return NIL_VAL;
		}
		/* a previous attempt failed */
		vm_runtime_error(vm,
		        "module '%s' failed to load earlier in this run.",
		        raw);
		vm_pop(vm); /* the key */
		free(path);
		return NIL_VAL;
	}

	/* mark in flight before running, so a cycle inside sees this */
	table_set(vm, &vm->modules, key, NIL_VAL);

	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		vm_runtime_error(vm, "could not open module file '%s'.", raw);
		table_set(vm, &vm->modules, key, FALSE_VAL);
		vm_pop(vm);
		free(path);
		return NIL_VAL;
	}

	if (fseek(file, 0L, SEEK_END) != 0) {
		vm_runtime_error(vm, "could not seek in '%s'.", raw);
		fclose(file);
		table_set(vm, &vm->modules, key, FALSE_VAL);
		vm_pop(vm);
		free(path);
		return NIL_VAL;
	}
	long size = ftell(file);
	if (size < 0) {
		/*
		 * ftell returns -1 on failure, and a module path can just as
		 * easily be a directory or a pipe as a regular file.
		 */
		vm_runtime_error(vm, "could not size '%s'.", raw);
		fclose(file);
		table_set(vm, &vm->modules, key, FALSE_VAL);
		vm_pop(vm);
		free(path);
		return NIL_VAL;
	}
	size_t bytes = (size_t)size;
	if (bytes >= (size_t)-1) {
		vm_runtime_error(vm, "'%s' is too large.", raw);
		fclose(file);
		table_set(vm, &vm->modules, key, FALSE_VAL);
		vm_pop(vm);
		free(path);
		return NIL_VAL;
	}
	if (fseek(file, 0L, SEEK_SET) != 0) {
		vm_runtime_error(vm, "could not rewind '%s'.", raw);
		fclose(file);
		table_set(vm, &vm->modules, key, FALSE_VAL);
		vm_pop(vm);
		free(path);
		return NIL_VAL;
	}

	char *buffer = malloc(bytes + 1);
	if (buffer == NULL) {
		vm_runtime_error(vm, "out of memory loading '%s'.", raw);
		fclose(file);
		table_set(vm, &vm->modules, key, FALSE_VAL);
		vm_pop(vm);
		free(path);
		return NIL_VAL;
	}
	size_t got = fread(buffer, 1, bytes, file);
	if (got < bytes) {
		/* a short read is a directory, a pipe, or a race. handing a
		 * truncated buffer to the compiler would be worse. */
		vm_runtime_error(vm, "could not read '%s'.", raw);
		free(buffer);
		fclose(file);
		table_set(vm, &vm->modules, key, FALSE_VAL);
		vm_pop(vm);
		free(path);
		return NIL_VAL;
	}
	/*
	 * In bounds: the allocation above is `bytes + 1`. The analyzer cannot
	 * tie `bytes` back to it across the ftell/fseek sequence, which is the
	 * same complaint it makes about the identical read_file() in main.c,
	 * where the same annotation is already in place.
	 */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	buffer[bytes] = '\0';
	fclose(file);

	/*
	 * Push a fresh environment. The module's top-level definitions go
	 * here and nowhere else, and `vm->globals` points at it for the
	 * duration, so the bytecode needs no change to be isolated.
	 */
	if (vm->globals_count >= FL_MODULE_DEPTH + 1) {
		vm_runtime_error(vm,
		        "modules nested more than %d deep. is an import "
		        "loop that the cycle check missed?",
		        FL_MODULE_DEPTH);
		free(buffer);
		table_set(vm, &vm->modules, key, FALSE_VAL);
		vm_pop(vm);
		free(path);
		return NIL_VAL;
	}

	/*
	 * Save the importer's environment and install a fresh one.
	 *
	 * Every import gets a new environment, even two imports in a row at
	 * the same depth. globals_used is the high-water mark of slots ever
	 * taken and only grows: reusing a slot would hand the second module
	 * the first module's table, and its `scale` would already be defined
	 * as a const -- which is how two unrelated modules ended up reporting
	 * "cannot redefine constant" against each other's names.
	 */
	Table *saved_globals = vm->globals;
	int want = vm->globals_used + 1;
	if (want > vm->globals_capacity) {
		int old_cap = vm->globals_capacity;
		int fresh = old_cap * 2;
		if (fresh < want)
			fresh = want;
		Table **grown = GROW_ARRAY(vm,
		        Table *,
		        vm->globals_envs,
		        (size_t)old_cap,
		        (size_t)fresh);
		for (int i = old_cap; i < fresh; i++)
			grown[i] = NULL;
		vm->globals_envs = grown;
		vm->globals_capacity = fresh;
	}
	vm->globals_envs[vm->globals_used] = ALLOCATE(vm, Table, 1);
	table_init(vm->globals_envs[vm->globals_used]);
	vm->globals_used++;
	vm->globals = vm->globals_envs[vm->globals_used - 1];
	vm->globals_count++;

	InterpretResult res = vm_interpret_named(vm, buffer, path);

	/* always restore, on every path out. this is the transaction's
	 * rollback: the importer's names are untouched by whatever the module
	 * did, whether it succeeded or failed. */
	vm->globals_count--;
	vm->globals = saved_globals;

	if (res != INTERPRET_OK) {
		/*
		 * The module failed. Nothing it defined was bound anywhere --
		 * it was never bound into the importer -- so the transaction
		 * has already rolled back by the restore above, and the cache
		 * entry is about to record the failure.
		 *
		 * The module's own environment is deliberately NOT freed, and
		 * the slot is kept.
		 *
		 * A failed module can still have handed out closures: it ran far
		 * enough to build them, and it may have stored one somewhere the
		 * importer can reach. Those closures carry a pointer to this
		 * environment, and freeing it turns every later call into a
		 * use-after-free -- which is precisely the class of bug the
		 * importer-survives-a-failed-module test exists to catch, and
		 * which ASan caught here first.
		 *
		 * Leaving it to the collector is the right answer for a GC
		 * runtime anyway: unreachable things get reclaimed, reachable
		 * things stay alive, and no amount of careful bookkeeping here
		 * can out-guess who held a reference.
		 */
		free(buffer);
		table_set(vm, &vm->modules, key, FALSE_VAL);
		vm_pop(vm);
		free(path);
		return NIL_VAL;
	}

	/*
	 * Build the export table from what the module actually exported.
	 *
	 * The list comes from the compiler, not from inspecting the
	 * environment: it emitted an OP_MODULE_EXPORTS naming the names marked
	 * `export`, so "private" is a decision the module made rather than
	 * something guessed after the fact.
	 */
	ObjTable *bag = new_flint_table(vm);
	vm_push(vm, OBJ_VAL(bag));

	/* vm->globals_count was decremented above; point at the module's table
	 * again to read its exports out. */
	Table *module_env = vm->globals_envs[vm->globals_used - 1];
	for (int i = 0; i < module_env->capacity; i++) {
		ObjString *name = module_env->entries[i].key;
		if (name == NULL)
			continue;
		if (!module_env->entries[i].is_exported)
			continue;
		Value value = module_env->entries[i].value;
		vm_push(vm, STR_VAL(name));
		/* a Flint-level table, so `import geometry` gives
		 * `geometry.area(5)` through ordinary field access rather
		 * than through anything import-specific. */
		if (bag->count == bag->capacity) {
			int old = bag->capacity;
			bag->capacity = old > 0 ? old * 2 : 8;
			bag->keys = GROW_ARRAY(
			        vm, ObjString *, bag->keys, old, bag->capacity);
			bag->values = GROW_ARRAY(
			        vm, Value, bag->values, old, bag->capacity);
		}
		bag->keys[bag->count] = name;
		bag->values[bag->count] = value;
		bag->count++;
		vm_pop(vm);
	}

	free(buffer);

	/* cache the finished module under its resolved path */
	table_set(vm, &vm->modules, key, OBJ_VAL(bag));
	vm_pop(vm); /* the bag */
	vm_pop(vm); /* the key */
	free(path);
	return OBJ_VAL(bag);
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
