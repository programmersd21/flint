/* SPDX-License-Identifier: MIT */
/*
 * Natives for writing small unix programs. See sys.h for why these are not in
 * native.c.
 */
#include "sys.h"

#include "memory.h"
#include "object.h"
#include "value.h"
#include "vm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * The command line, as one allocation.
 *
 * argc/argv points into this, never into the caller's own argv, so the block
 * is copied once and never touched again. sys_free_args is the only thing
 * that reads it.
 */
static char *args_block = NULL;
static char **args_vec = NULL;
static int args_count = 0;

/*
 * The directory imports resolve against. Set when the VM starts running a
 * file, so `import "lib/math.fl"` means the same thing no matter which
 * directory the user happened to be in.
 *
 * malloc'd and leaked on purpose, same as args_block: it is set once at
 * startup and lives until the process ends. A static buffer would be one
 * fixed size, and a fixed size is exactly the kind of limit that shows up
 * as a bug report from someone with a deep directory.
 */
static char *source_dir = NULL;

void sys_set_args(int argc, char **argv)
{
	if (args_block != NULL)
		return; /* already set. vm_init runs before main's args land. */

	if (argc <= 0 || argv == NULL) {
		args_count = 0;
		return;
	}

	/* one block: the vector, then the bytes, each NUL terminated */
	size_t total = 0;
	for (int i = 0; i < argc; i++)
		total += strlen(argv[i]) + 1;

	char *block = malloc(sizeof(char *) * (size_t)argc + total);
	if (block == NULL) {
		/* no room for a command line is a broken process, but it is
		 * not a broken script, so args() reports nothing and returns
		 * an empty list rather than aborting before main() runs */
		args_count = 0;
		return;
	}

	char **vec = (char **)block;
	char *bytes = block + sizeof(char *) * (size_t)argc;
	for (int i = 0; i < argc; i++) {
		size_t n = strlen(argv[i]) + 1;
		memcpy(bytes, argv[i], n);
		vec[i] = bytes;
		bytes += n;
	}

	args_block = block;
	args_vec = vec;
	args_count = argc;
}

void sys_free_args(void)
{
	free(args_block);
	args_block = NULL;
	args_vec = NULL;
	args_count = 0;
}

void sys_set_source_dir(const char *dir)
{
	free(source_dir);
	source_dir = NULL;
	size_t len = strlen(dir);
	char *fresh = malloc(len + 1);
	if (fresh == NULL) {
		/* out of memory for a directory name. leaving the old one
		 * is better than a NULL deref, and imports then resolve
		 * against the process directory, which is wrong but not
		 * fatal. */
		source_dir = NULL;
		return;
	}
	memcpy(fresh, dir, len);
	fresh[len] = '\0';
	source_dir = fresh;
}

/*
 * Imports in this file resolve beside it. A bare filename has no directory,
 * so its imports resolve from the working directory.
 */
void sys_set_source_dir_for_file(const char *file)
{
	const char *slash = strrchr(file, '/');
	if (slash == NULL) {
		/* no directory component */
		sys_set_source_dir("");
		return;
	}

	/* the directory is everything before the last slash. an absolute
	 * path "/x.fl" gives "", which would be wrong, so keep the leading
	 * slash for the root case. */
	size_t len = slash == file ? 1 : (size_t)(slash - file);
	char *dir = malloc(len + 1);
	if (dir == NULL) {
		sys_set_source_dir("");
		return;
	}
	memcpy(dir, file, len);
	dir[len] = '\0';
	sys_set_source_dir(dir);
	free(dir);
}

/*
 * Resolve relative to the importing file. Absolute paths pass through.
 * The caller owns the returned buffer.
 */
char *sys_resolve_module(const char *path)
{
	size_t pathlen = strlen(path);

	/* absolute: the leading slash means the user knows what they want, so
	 * hand back a copy and let the caller free it like any other */
	if (path[0] == '/') {
		char *out = malloc(pathlen + 1);
		if (out != NULL)
			memcpy(out, path, pathlen + 1);
		return out;
	}

	const char *dir = source_dir != NULL ? source_dir : "";
	size_t dirlen = strlen(dir);

	/* +1 for the slash, +1 for the NUL. checked, because a path near
	 * SIZE_MAX would wrap the sum and malloc a short buffer. */
	if (dirlen > (size_t)-1 - pathlen - 2)
		return NULL;

	char *out = malloc(dirlen + 1 + pathlen + 1);
	if (out == NULL)
		return NULL;

	if (dirlen == 0) {
		/* no source dir (running -c or from stdin): the path is
		 * already relative to the process, use it as-is */
		memcpy(out, path, pathlen);
		out[pathlen] = '\0';
	} else {
		memcpy(out, dir, dirlen);
		out[dirlen] = '/';
		memcpy(out + dirlen + 1, path, pathlen);
		out[dirlen + 1 + pathlen] = '\0';
	}
	return out;
}

/*
 * args() -> list of strings
 *
 * The arguments after the script path. argv[0] is not included: a script that
 * wants its own name can be told, and one that does not should not have to
 * index past it. This matches how a shell script reads "$@" and how argv
 * looks to everyone who has ever been surprised by it.
 */
static Value args_native(VM *vm, int argc, Value *argv)
{
	(void)vm;
	(void)argc;
	(void)argv;

	ObjList *out = new_list(vm);
	vm_push(vm, OBJ_VAL(out)); /* rooted: the pushes below can collect */

	/*
	 * Skip argv[0] (the interpreter) and argv[1] (the script path), so
	 * the arguments a script sees are the ones after its own name.
	 *
	 * Each string is pushed first and then copied into the list, because
	 * the push can collect and the copy_string that made it can too.
	 * Copying straight into a malloc'd items array while the source
	 * strings sit unrooted would be a use-after-free waiting for a gc
	 * pass; pushing keeps every one of them reachable until the array is
	 * full.
	 */
	int n = args_count - 2;
	if (n < 0)
		n = 0;

	if (n > 0) {
		Value *items = ALLOCATE(vm, Value, (size_t)n);
		for (int k = 0; k < n; k++)
			vm_push(vm,
			        OBJ_VAL(copy_string(vm,
			                args_vec[k + 2],
			                (int)strlen(args_vec[k + 2]))));

		/* the strings are the top n values on the stack, in order */
		for (int k = 0; k < n; k++)
			items[k] = vm->stack_top[-n + k];

		out->items = items;
		out->capacity = n;
		out->count = n;
		vm->stack_top -= n; /* the strings, now copied */
	}

	vm_pop(vm); /* the list itself */
	return OBJ_VAL(out);
}

/*
 * env(name) -> string or nil
 *
 * nil for a variable that is not set, which is what `if not env("X")` wants
 * to read. An empty variable and an unset one are different things in
 * unix and this keeps them that way.
 */
static Value env_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0])) {
		vm_runtime_error(vm, "Argument to env() must be a string.");
		return NIL_VAL;
	}

	const char *name = AS_CSTRING(argv[0]);
	const char *value = getenv(name);
	if (value == NULL)
		return NIL_VAL;

	/* getenv returns a pointer the C library owns. copy it into a flint
	 * string so the script is not holding a pointer into environ. */
	return OBJ_VAL(copy_string(vm, value, (int)strlen(value)));
}

/*
 * exit(status) -> never returns
 *
 * Exits the process with the given status. stdout and stderr are flushed
 * first: a script that prints and then exits must not lose the print, and
 * the buffers are stdio's, not ours, so fflush is the only correct answer.
 *
 * There is no "return" path, and that is deliberate. exit() means the script
 * is done, and a flint that tried to continue after it would need an
 * exception mechanism that does not exist. A status of 0 is success, anything
 * else is failure, which is what a shell checks.
 */
static Value exit_native(VM *vm, int argc, Value *argv)
{
	int status = 0;

	if (argc >= 1) {
		if (!IS_NUMBER(argv[0])) {
			vm_runtime_error(
			        vm, "Argument to exit() must be a number.");
			return NIL_VAL;
		}
		/* the shell only has 8 bits of status, and a flint number
		 * is a double. clamp rather than wrap: a negative or huge
		 * value should not turn into a random exit code. */
		double d = AS_NUMBER(argv[0]);
		if (d < 0 || d > 255) {
			vm_runtime_error(vm,
			        "exit() status must be between 0 and 255, "
			        "got %g.",
			        d);
			return NIL_VAL;
		}
		status = (int)d;
	}

	/* a whole number was required by the range check above: a fractional
	 * status is a bug in the caller and a shell would see something
	 * arbitrary, so reject it explicitly. */
	if (argc >= 1 && IS_NUMBER(argv[0]) &&
	        AS_NUMBER(argv[0]) != (double)(int)AS_NUMBER(argv[0])) {
		vm_runtime_error(vm, "exit() status must be a whole number.");
		return NIL_VAL;
	}

	fflush(stdout);
	fflush(stderr);
	exit(status);

	/* not reached. exit() does not come back, and the return is here
	 * only to satisfy the compiler. */
	return NIL_VAL;
}

/*
 * read_file(path) -> string
 *
 * The whole file as one string, newline and all. A script that wants lines
 * calls split() on it; a script that wants to echo a file should not have to
 * learn a line protocol to do it.
 *
 * The length comes from fseek/ftell, which fails on anything that is not a
 * regular file, and a pipe is a file as far as a user is concerned. So for
 * a non-seekable stream the whole thing is read in a growing buffer instead,
 * and the only difference the script sees is that it works.
 */
static Value read_file_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0])) {
		vm_runtime_error(
		        vm, "Argument to read_file() must be a string.");
		return NIL_VAL;
	}

	const char *path = AS_CSTRING(argv[0]);
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		vm_runtime_error(
		        vm, "Cannot read '%s': %s.", path, strerror(errno));
		return NIL_VAL;
	}

	/* try the cheap path: a regular file, size known up front */
	long length = -1;
	if (fseek(file, 0L, SEEK_END) == 0) {
		length = ftell(file);
		if (length >= 0 && fseek(file, 0L, SEEK_SET) != 0)
			length = -1;
	}

	char *buffer;
	size_t size;

	if (length >= 0) {
		/* a length of 0 is a real file, an empty one. one byte for
		 * the NUL either way. checked, because length + 1 on a
		 * huge file wraps to zero and the read then writes past a
		 * zero byte allocation. */
		if ((unsigned long)length >= (unsigned long)-1) {
			fclose(file);
			vm_runtime_error(vm, "File '%s' is too large.", path);
			return NIL_VAL;
		}
		size = (size_t)length;
		buffer = malloc(size + 1);
		if (buffer == NULL) {
			fclose(file);
			vm_runtime_error(
			        vm, "Out of memory reading '%s'.", path);
			return NIL_VAL;
		}
		size_t got = fread(buffer, 1, size, file);
		if (got < size && ferror(file)) {
			free(buffer);
			fclose(file);
			vm_runtime_error(vm,
			        "Cannot read '%s': %s.",
			        path,
			        strerror(errno));
			return NIL_VAL;
		}
		size = got;
	} else {
		/* not seekable. grow as we go, doubling, which is the same
		 * shape as every other buffer in the runtime. */
		size_t capacity = 8192;
		size_t used = 0;
		buffer = malloc(capacity);
		if (buffer == NULL) {
			fclose(file);
			vm_runtime_error(
			        vm, "Out of memory reading '%s'.", path);
			return NIL_VAL;
		}
		for (;;) {
			if (used == capacity) {
				if (capacity > (size_t)-1 / 2) {
					free(buffer);
					fclose(file);
					vm_runtime_error(vm,
					        "File '%s' is too large.",
					        path);
					return NIL_VAL;
				}
				size_t grown = capacity * 2;
				char *bigger = realloc(buffer, grown);
				if (bigger == NULL) {
					free(buffer);
					fclose(file);
					vm_runtime_error(vm,
					        "Out of memory reading "
					        "'%s'.",
					        path);
					return NIL_VAL;
				}
				buffer = bigger;
				capacity = grown;
			}
			/*
			 * The analyser flags fread as possibly running on a
			 * stream already at EOF, because `got == 0` is
			 * treated as "we may have hit EOF" and the next
			 * iteration reads again. It cannot prove the loop
			 * exits, but it does: a fread that returns 0 with
			 * no error is end of file, and the break below is
			 * unconditional on that. The re-read it fears does
			 * not happen.
			 *
			 * Stop on end of file or a real error, not merely on
			 * a short read. A pipe is allowed to return fewer
			 * bytes than asked for without being at EOF, and
			 * treating that as the end would silently truncate
			 * whatever came next. After ferror the stream
			 * position is indeterminate, so calling fread
			 * again is undefined behaviour, not merely
			 * useless: this is the check that prevents it.
			 */
			/*
			 * got == 0 with no error is EOF, and EOF does not
			 * change. ferror leaves the position indeterminate,
			 * so reading again is undefined. Either way the
			 * loop is done, and the check has to be the last
			 * thing before the next fread rather than after
			 * it, or the analyser is right that a re-read can
			 * happen on a spent stream. clang-format moves a
			 * NOLINTNEXTLINE off the statement it guards, so
			 * this is written as control flow rather than a
			 * suppression.
			 */
			if (feof(file) || ferror(file))
				break;

			size_t got =
			        fread(buffer + used, 1, capacity - used, file);
			used += got;
			if (got == 0)
				break;
		}
		size = used;

		/*
		 * One byte for the NUL, always. The loop above only grows
		 * when used == capacity, so a fill that exactly reaches
		 * capacity leaves no spare byte and the terminator below
		 * would be one past the end. Every non-seekable read can
		 * land on that boundary; it is not a corner case, it is
		 * every file whose size is a power of two.
		 */
		if (size == capacity) {
			char *bigger = realloc(buffer, capacity + 1);
			if (bigger == NULL) {
				free(buffer);
				fclose(file);
				vm_runtime_error(vm,
				        "Out of memory reading '%s'.",
				        path);
				return NIL_VAL;
			}
			buffer = bigger;
		}
	}

	fclose(file);
	/* in bounds: on the seekable path the malloc was size + 1, and on
	 * the growing path the capacity is forced one past used immediately
	 * above. the analyser cannot prove either through the realloc. */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	buffer[size] = '\0';

	/* copy_string can collect. buffer is malloc'd and is not a gc
	 * object, so it survives, but nothing reads it after this. */
	ObjString *text = copy_string(vm, buffer, (int)size);
	free(buffer);
	return OBJ_VAL(text);
}

/*
 * write_file(path, content) -> true
 *
 * Truncates. A script that wants to append opens the file itself, or reads
 * and adds, both of which are two lines and neither of which is worth a
 * second builtin.
 *
 * The write is binary. A flint string is bytes and a file is bytes, and
 * translating line endings on the way out would make flint the only language
 * on the machine that does.
 */
static Value write_file_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) {
		vm_runtime_error(
		        vm, "Both arguments to write_file() must be strings.");
		return NIL_VAL;
	}

	const char *path = AS_CSTRING(argv[0]);
	ObjString *content = AS_STRING(argv[1]);

	FILE *file = fopen(path, "wb");
	if (file == NULL) {
		vm_runtime_error(
		        vm, "Cannot write '%s': %s.", path, strerror(errno));
		return NIL_VAL;
	}

	size_t wrote = fwrite(content->chars, 1, (size_t)content->length, file);
	if (wrote != (size_t)content->length) {
		/* a full disk is not an fwrite failure you can ignore. the
		 * message says which, because "cannot write" with no
		 * reason is the least useful error in the world. */
		vm_runtime_error(vm,
		        "Cannot write '%s': %s.",
		        path,
		        ferror(file) ? strerror(errno) : "short write");
		fclose(file);
		return NIL_VAL;
	}

	if (fclose(file) != 0) {
		/* a close that fails means the data may not have landed,
		 * and for a script writing a file that matters */
		vm_runtime_error(
		        vm, "Cannot write '%s': %s.", path, strerror(errno));
		return NIL_VAL;
	}

	return TRUE_VAL;
}

/*
 * exec(command, arg...) -> exit status
 *
 * Runs a program directly. No shell, ever.
 *
 * This is the one place where the obvious implementation is a security bug.
 * `system(cmd)` and `popen` hand a string to /bin/sh, which then splits it
 * on spaces, expands $VAR, and interprets ; | & > and everything else. A
 * filename with a space in it breaks. A filename with a semicolon in it
 * executes whatever comes after the semicolon. Building the command as a
 * string and handing it to a shell is how command injection happens, and it
 * is the single most common way a scripting language ships a CVE.
 *
 * So this takes the program and its arguments separately and calls execvp,
 * which searches PATH and does not interpret anything. A script that
 * genuinely wants a shell can say so, with system(), and owns the quoting.
 *
 * Returns the exit status: 0 for success, the child's code for a normal
 * failure, and 127 or 126 for the shell's own "not found" and "not
 * executable" conventions, which is what a shell would have reported.
 */
static Value exec_native(VM *vm, int argc, Value *argv)
{
	if (argc < 1 || !IS_STRING(argv[0])) {
		vm_runtime_error(vm,
		        "exec() takes a command string and any number of "
		        "argument strings.");
		return NIL_VAL;
	}

	for (int i = 1; i < argc; i++) {
		if (!IS_STRING(argv[i])) {
			vm_runtime_error(vm,
			        "Argument %d to exec() must be a string.",
			        i + 1);
			return NIL_VAL;
		}
	}

	/* argv for the child: the program name, then the arguments, then
	 * NULL. one allocation, and the strings are borrowed from the
	 * flint values on the vm stack, which are rooted and stay rooted
	 * across the fork below. */
	char **child_argv = malloc(sizeof(char *) * (size_t)(argc + 1));
	if (child_argv == NULL) {
		vm_runtime_error(vm, "Out of memory in exec().");
		return NIL_VAL;
	}
	for (int i = 0; i < argc; i++)
		child_argv[i] = AS_CSTRING(argv[i]);
	child_argv[argc] = NULL;

	/* fork before anything that can allocate or lock. the child does
	 * nothing but execvp and _exit, which is the only way to be sure
	 * the child cannot run a parent's stdio buffer or malloc lock. */
	fflush(stdout);
	fflush(stderr);

	pid_t pid = fork();
	if (pid < 0) {
		free(child_argv);
		vm_runtime_error(vm, "Cannot fork: %s.", strerror(errno));
		return NUMBER_VAL(127);
	}

	if (pid == 0) {
		/* child. execvp searches PATH, which is what a script means
		 * by exec("ls") rather than exec("/bin/ls"). */
		execvp(child_argv[0], child_argv);
		/* only reached if exec failed. _exit, not exit: the child
		 * shares the parent's buffers and exit() would flush them
		 * twice and run atexit handlers that belong to flint. */
		/* errno.h is included at the top of this file; the include
		 * cleaner reports the first *use* of a macro rather than the
		 * header, and cannot see through the system headers. */
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		_exit(errno == ENOENT ? 127 : 126);
	}

	/* parent. free before the wait, so a slow child does not hold an
	 * allocation that a collecting child would never see anyway. */
	free(child_argv);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		if (errno != EINTR) {
			vm_runtime_error(vm,
			        "Cannot wait for child: %s.",
			        strerror(errno));
			return NUMBER_VAL(127);
		}
	}

	if (WIFEXITED(status))
		return NUMBER_VAL((double)WEXITSTATUS(status));
	if (WIFSIGNALED(status))
		/* report a signal as 128+signo, which is the shell's
		 * convention and keeps the number in the 0-255 range a
		 * script is told to expect */
		return NUMBER_VAL(128.0 + (double)WTERMSIG(status));
	return NUMBER_VAL(1);
}

void register_sys_natives(VM *vm)
{
	vm_define_native(vm, "args", args_native, 0);
	vm_define_native(vm, "env", env_native, 1);
	/* -1 because exit() takes zero or one, and the fixed-arity check
	 * cannot express that. it checks inside. */
	vm_define_native(vm, "exit", exit_native, -1);
	vm_define_native(vm, "read_file", read_file_native, 1);
	vm_define_native(vm, "write_file", write_file_native, 2);
	vm_define_native(vm, "exec", exec_native, -1);
}
