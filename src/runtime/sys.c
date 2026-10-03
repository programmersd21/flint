/* SPDX-License-Identifier: MIT */
/*
 * Natives for writing small unix programs. See sys.h for why these are not in
 * native.c.
 */
#include "sys.h"

#include "jsonp.h"
#include "http.h"

#include <stdint.h>

#include "memory.h"
#include "object.h"
#include "value.h"
#include "vm.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h> /* stat, mkdir */
#include <sys/wait.h>
#include <time.h>
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
/*
 * Where the standard library lives.
 *
 * A bare name like `math` is not a file in the caller's directory, it is a
 * library, and something has to know where libraries are kept. Three places
 * are tried, in order:
 *
 *   $FLINT_STDLIB         the user, or a packager, sets it
 *   <executable>/lib      so a copy of the tree works from anywhere
 *   $HOME/.flint/stdlib   the ordinary install location
 *
 * The executable-relative one is what makes a tarball relocatable. The
 * compiled-in FLINT_STDLIB_DIR from the build directory is the last resort
 * and is only right for a build tree that is not moved, which is exactly the
 * case it is for.
 */
static const char *stdlib_dir(void)
{
	static const char *cached = NULL;
	static bool looked = false;

	if (looked)
		return cached;
	looked = true;

	const char *env = getenv("FLINT_STDLIB");
	if (env != NULL && env[0] != '\0') {
		cached = env;
		return cached;
	}

	/* next to the binary: readlink on /proc/self/exe rather than argv[0],
	 * because argv[0] is whatever the shell felt like and /proc is where
	 * the truth is. if it is not there, the other two still work. */
	{
		char buf[4096];
		ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			char *slash = strrchr(buf, '/');
			if (slash != NULL) {
				/* the buffer is halved before the
				 * suffix so the two halves cannot
				 * collide. snprintf into a buffer that
				 * already holds the source would
				 * truncate silently, and a truncated
				 * path here is a confusing "cannot open"
				 * several steps away. */
				static char lib[4096];
				size_t dirlen = (size_t)(slash - buf);
				if (dirlen + 5 < sizeof(lib)) {
					memcpy(lib, buf, dirlen);
					memcpy(lib + dirlen, "/lib", 5);
					lib[dirlen + 4] = '\0';
					if (access(lib, R_OK) == 0) {
						cached = lib;
						return cached;
					}
				}
			}
		}
	}

	const char *home = getenv("HOME");
	if (home != NULL) {
		static char lib[4096];
		snprintf(lib, sizeof(lib), "%s/.flint/stdlib", home);
		if (access(lib, R_OK) == 0) {
			cached = lib;
			return cached;
		}
	}

#ifdef FLINT_STDLIB_DIR
	if (access(FLINT_STDLIB_DIR, R_OK) == 0) {
		cached = FLINT_STDLIB_DIR;
		return cached;
	}
#endif

	cached = "";
	return cached;
}

/* true when a name is a bare library name: no slash, no .fl, not a keyword */
static bool is_library_name(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (dot != NULL && strcmp(dot, ".fl") == 0)
		return false;
	return strchr(path, '/') == NULL;
}

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

	/*
	 * A bare name is a library, not a relative path. This is the one
	 * place the module loader learns that libraries exist, and it learns
	 * exactly that: resolve a name to a file and get out. The package
	 * manager, when there is one, will sit in front of this rather
	 * than beside it, so there is one module loader and not two.
	 */
	if (is_library_name(path)) {
		const char *dir = stdlib_dir();
		if (dir[0] != '\0') {
			size_t dirlen = strlen(dir);
			size_t need = dirlen + 1 + pathlen + 3 + 1;
			char *out = malloc(need);
			if (out != NULL)
				snprintf(out, need, "%s/%s.fl", dir, path);
			return out;
		}
		/* nowhere to look. fall through and produce the ordinary
		 * relative path, so the error names the file the script
		 * asked for rather than a path nobody recognises. */
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

bool sys_module_file_exists(const char *name)
{
	size_t namelen = strlen(name);
	if (namelen == 0 || !is_library_name(name))
		return false;

	/* <stdlib>/<name>.fl. the same search order the loader uses, so the
	 * suggestion and the import agree about where the file is. */
	const char *dir = stdlib_dir();
	if (dir[0] != '\0') {
		size_t dirlen = strlen(dir);
		if (dirlen <= (size_t)-1 - namelen - 5) {
			size_t need = dirlen + 1 + namelen + 3 + 1;
			char *candidate = malloc(need);
			if (candidate != NULL) {
				snprintf(
				        candidate, need, "%s/%s.fl", dir, name);
				bool found = access(candidate, R_OK) == 0;
				free(candidate);
				if (found)
					return true;
			}
		}
	}

	/* <source_dir>/<name>.fl, for a sibling file the script forgot to
	 * import. only when there is a source directory: with -e or stdin
	 * there is none, and a bare name then means the working directory,
	 * which is where the user is standing rather than where the file is. */
	if (source_dir != NULL && source_dir[0] != '\0') {
		size_t dirlen = strlen(source_dir);
		if (dirlen <= (size_t)-1 - namelen - 5) {
			size_t need = dirlen + 1 + namelen + 3 + 1;
			char *candidate = malloc(need);
			if (candidate != NULL) {
				snprintf(candidate,
				        need,
				        "%s/%s.fl",
				        source_dir,
				        name);
				bool found = access(candidate, R_OK) == 0;
				free(candidate);
				if (found)
					return true;
			}
		}
	}
	return false;
}

/*
 * The directory `flint sync` writes into.
 *
 * Deliberately not stdlib_dir(), which is the read-side answer and prefers
 * <executable>/lib so a tarball works from anywhere. That directory belongs
 * to whoever unpacked the tarball, and a package-installed binary has none:
 * /usr/local/bin is full of other things. Writing there would need root and
 * would be a surprise, so sync installs where `make install` does:
 *
 *   $FLINT_STDLIB         when the user set it, that is what they meant
 *   $HOME/.flint/stdlib   the ordinary location
 *
 * NULL when neither is available, which is the case worth a message rather
 * than a guess: HOME unset is rare but real inside some containers.
 */
const char *sys_stdlib_install_dir(void)
{
	static char buf[4096];
	const char *env = getenv("FLINT_STDLIB");
	if (env != NULL && env[0] != '\0') {
		snprintf(buf, sizeof(buf), "%s", env);
		return buf;
	}
	const char *home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return NULL;
	snprintf(buf, sizeof(buf), "%s/.flint/stdlib", home);
	return buf;
}

/*
 * mkdir -p. Every missing component is created, in order, and an existing
 * directory is not an error: that is what makes this idempotent, which is
 * what `flint sync` needs on every run.
 */
bool sys_make_dirs(const char *path)
{
	if (path == NULL || path[0] == '\0')
		return false;

	size_t len = strlen(path);
	char work[4096];
	if (len >= sizeof(work))
		return false;
	memcpy(work, path, len + 1);

	/* the trailing slash is dropped so the last component is not doubled */
	while (len > 1 && work[len - 1] == '/')
		work[--len] = '\0';

	for (size_t i = 1; i <= len; i++) {
		if (work[i] != '/' && work[i] != '\0')
			continue;
		char saved = work[i];
		work[i] = '\0';
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		if (mkdir(work, 0755) != 0 && errno != EEXIST) {
			work[i] = saved;
			return false;
		}
		work[i] = saved;
	}
	return true;
}

/*
 * Fetch a URL to a file.
 *
 * The one thing flint does not do itself. github serves raw files over https,
 * and https means TLS, and TLS means either a dependency this language does
 * not have or forty megabytes of source that would dwarf the interpreter. So
 * the download runs curl(1) or wget(1) through execvp: two programs that are
 * already installed on most machines, that keep their certificate store up to
 * date on their own, and that nobody has to trust us to reimplement.
 *
 * Returns 0 on success. On failure the reason is on stderr from the downloader
 * itself, and this returns non-zero; a missing downloader is reported here,
 * because "command not found" from a forked child is not an explanation.
 */
int sys_fetch_url(const char *url, const char *dest)
{
	/* curl first, then wget. -f makes an HTTP error a failure instead of a
	 * file full of "404: Not Found", which matters because the write
	 * target is checked by compiling afterwards and a 404 body would be
	 * caught -- but by the wrong layer, with the wrong message.
	 *
	 * The vectors are built here rather than kept in file-scope tables
	 * because two of the entries are arguments, and a static array
	 * initializer has to be constant. */
	static const char *const curl_head[] = {
	        "curl",
	        "-fsS",
	        "--proto",
	        "=https,http",
	        "--location",
	        "--connect-timeout",
	        "10",
	        "--max-time",
	        "120",
	        "--output",
	};
	static const char *const wget_head[] = {
	        "wget",
	        "-q",
	        "--timeout=10",
	        "--tries=2",
	        "-O",
	};
	const char *const *head = curl_head;
	size_t headlen = sizeof(curl_head) / sizeof(curl_head[0]);
	const char *program = curl_head[0];

	fflush(stdout);
	fflush(stderr);

	for (int which = 0; which < 2; which++) {
		if (which == 1) {
			head = wget_head;
			headlen = sizeof(wget_head) / sizeof(wget_head[0]);
			program = wget_head[0];
		}

		/* the program's own arguments live in the vector, so the
		 * child needs no environment at all */
		char *cmd[16];
		if (headlen + 2 > sizeof(cmd) / sizeof(cmd[0]))
			return -1;
		size_t n = 0;
		for (size_t i = 0; i < headlen; i++)
			cmd[n++] = (char *)(uintptr_t)head[i];
		cmd[n++] = (char *)(uintptr_t)dest;
		cmd[n++] = (char *)(uintptr_t)url;
		cmd[n] = NULL;

		pid_t pid = fork();
		if (pid < 0)
			return -1;
		if (pid == 0) {
			/* child: exec and nothing else, so it cannot run a
			 * parent's stdio buffer or malloc lock */
			/* NOLINTNEXTLINE(misc-include-cleaner) */
			execvp(program, cmd);
			/* 127 is the shell's "not found", and the parent turns
			 * it into "try the next downloader" */
			/* NOLINTNEXTLINE(misc-include-cleaner) */
			_exit(127);
		}

		int status = 0;
		while (waitpid(pid, &status, 0) < 0) {
			/* NOLINTNEXTLINE(misc-include-cleaner) */
			if (errno != EINTR)
				return -1;
		}
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			return 0;

		/* 127 means the program is not installed, so try the other
		 * one. any other status is a real failure and the downloader
		 * already explained it. */
		if (which == 0 && WIFEXITED(status) &&
		        WEXITSTATUS(status) == 127)
			continue;
		return -1;
	}

	fprintf(stderr,
	        "flint sync needs curl(1) or wget(1) to download the standard\n"
	        "library. Install one of them, or set FLINT_STDLIB to a\n"
	        "directory you maintain yourself.\n");
	return -1;
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
			        STR_VAL(new_string(vm,
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
		vm_runtime_error(vm, "argument to env() must be a string.");
		return NIL_VAL;
	}

	const char *name = AS_CSTRING(argv[0]);
	const char *value = getenv(name);
	if (value == NULL)
		return NIL_VAL;

	/* getenv returns a pointer the C library owns. copy it into a flint
	 * string so the script is not holding a pointer into environ. */
	return STR_VAL(new_string(vm, value, (int)strlen(value)));
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
/*
 * OS information for lib/os.fl. These are thin and honest: each one answers
 * a single question the C library already knows, with no caching, no
 * abstraction, and platform branches only where the call genuinely differs.
 *
 * Windows gets best-effort answers where the call exists there (_getcwd,
 * _chdir, GetCurrentProcessId) and nil or "unknown" where it does not. The
 * primary target is POSIX; pretending otherwise would be dishonest, and so
 * would refusing to build there.
 */
static Value os_name_native(VM *vm, int argc, Value *argv)
{
	(void)vm;
	(void)argc;
	(void)argv;
#if defined(_WIN32)
	return STR_VAL(new_string(vm, "windows", 7));
#elif defined(__APPLE__)
	return STR_VAL(new_string(vm, "darwin", 6));
#elif defined(__linux__)
	return STR_VAL(new_string(vm, "linux", 5));
#elif defined(__FreeBSD__)
	return STR_VAL(new_string(vm, "freebsd", 7));
#else
	return STR_VAL(new_string(vm, "unknown", 7));
#endif
}

static Value os_arch_native(VM *vm, int argc, Value *argv)
{
	(void)vm;
	(void)argc;
	(void)argv;
#if defined(__x86_64__) || defined(_M_X64)
	return STR_VAL(new_string(vm, "x86_64", 6));
#elif defined(__aarch64__) || defined(_M_ARM64)
	return STR_VAL(new_string(vm, "aarch64", 7));
#elif defined(__i386__) || defined(_M_IX86)
	return STR_VAL(new_string(vm, "x86", 3));
#elif defined(__arm__) || defined(_M_ARM)
	return STR_VAL(new_string(vm, "arm", 3));
#else
	return STR_VAL(new_string(vm, "unknown", 7));
#endif
}

static Value os_pathsep_native(VM *vm, int argc, Value *argv)
{
	(void)vm;
	(void)argc;
	(void)argv;
#ifdef _WIN32
	return STR_VAL(new_string(vm, "\\", 1));
#else
	return STR_VAL(new_string(vm, "/", 1));
#endif
}

/* the working directory, or nil when it cannot be read. a directory the
 * process cannot stat is not a string, and inventing one would be worse. */
static Value os_getcwd_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	(void)argv;
#ifdef _WIN32
	char *cwd = _getcwd(NULL, 0);
#else
	char *cwd = getcwd(NULL, 0);
#endif
	if (cwd == NULL)
		return NIL_VAL;
	/* getcwd allocates with malloc, and new_string copies: free ours. */
	Value out = STR_VAL(new_string(vm, cwd, (int)strlen(cwd)));
	free(cwd);
	return out;
}

static Value os_chdir_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0])) {
		vm_runtime_error(vm, "argument to chdir() must be a string.");
		return NIL_VAL;
	}
#ifdef _WIN32
	int rc = _chdir(AS_CSTRING(argv[0]));
#else
	int rc = chdir(AS_CSTRING(argv[0]));
#endif
	return rc == 0 ? TRUE_VAL : FALSE_VAL;
}

/*
 * getenv with a default. env(name) already exists and returns nil for a
 * missing variable; this one takes the fallback explicitly, which is what a
 * config reader wants: `os.getenv("HOME", "")` is a string either way, and
 * the caller never branches on nil.
 */
static Value os_getenv_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) {
		vm_runtime_error(vm, "arguments to getenv() must be strings.");
		return NIL_VAL;
	}
	const char *value = getenv(AS_CSTRING(argv[0]));
	const char *text = value != NULL ? value : AS_CSTRING(argv[1]);
	return STR_VAL(new_string(vm, text, (int)strlen(text)));
}

static Value os_setenv_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) {
		vm_runtime_error(vm, "arguments to setenv() must be strings.");
		return NIL_VAL;
	}
	const char *name = AS_CSTRING(argv[0]);
	/* setenv rejects '=' in the name and an empty name; check before the
	 * call so the error names the problem rather than the call. */
	if (name[0] == '\0' || strchr(name, '=') != NULL) {
		vm_runtime_error(vm, "invalid environment variable name.");
		return NIL_VAL;
	}
#ifdef _WIN32
	int rc = _putenv_s(name, AS_CSTRING(argv[1]));
#else
	int rc = setenv(name, AS_CSTRING(argv[1]), 1);
#endif
	return rc == 0 ? TRUE_VAL : FALSE_VAL;
}

static Value os_unsetenv_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0])) {
		vm_runtime_error(
		        vm, "argument to unsetenv() must be a string.");
		return NIL_VAL;
	}
#ifdef _WIN32
	int rc = _putenv_s(AS_CSTRING(argv[0]), "");
#else
	int rc = unsetenv(AS_CSTRING(argv[0]));
#endif
	/* unsetting a name that was never set succeeds on every platform
	 * here, which matches what a script means by it. */
	return rc == 0 ? TRUE_VAL : FALSE_VAL;
}

/* the user's home directory, or nil when the platform will not say. */
static Value os_homedir_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	(void)argv;
#ifdef _WIN32
	const char *home = getenv("USERPROFILE");
	if (home == NULL) {
		const char *drive = getenv("HOMEDRIVE");
		const char *path = getenv("HOMEPATH");
		if (drive == NULL || path == NULL)
			return NIL_VAL;
		size_t n = strlen(drive) + strlen(path);
		char *joined = malloc(n + 1);
		if (joined == NULL)
			return NIL_VAL;
		memcpy(joined, drive, strlen(drive));
		memcpy(joined + strlen(drive), path, strlen(path) + 1);
		Value out = STR_VAL(new_string(vm, joined, (int)n));
		free(joined);
		return out;
	}
#else
	const char *home = getenv("HOME");
	if (home == NULL)
		return NIL_VAL;
#endif
	return STR_VAL(new_string(vm, home, (int)strlen(home)));
}

/* a writable scratch directory, or nil when none is configured. */
static Value os_tmpdir_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	(void)argv;
#ifdef _WIN32
	const char *tmp = getenv("TEMP");
	if (tmp == NULL)
		tmp = getenv("TMP");
	if (tmp == NULL)
		return NIL_VAL;
	return STR_VAL(new_string(vm, tmp, (int)strlen(tmp)));
#else
	const char *tmp = getenv("TMPDIR");
	if (tmp == NULL)
		tmp = "/tmp";
	return STR_VAL(new_string(vm, tmp, (int)strlen(tmp)));
#endif
}

static Value os_pid_native(VM *vm, int argc, Value *argv)
{
	(void)vm;
	(void)argc;
	(void)argv;
#ifdef _WIN32
	return NUMBER_VAL((double)GetCurrentProcessId());
#else
	return NUMBER_VAL((double)getpid());
#endif
}

static Value exit_native(VM *vm, int argc, Value *argv)
{
	int status = 0;

	if (argc >= 1) {
		if (!IS_NUMBER(argv[0])) {
			vm_runtime_error(
			        vm, "argument to exit() must be a number.");
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
		        vm, "cannot read '%s': %s.", path, strerror(errno));
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
			vm_runtime_error(vm, "file '%s' is too large.", path);
			return NIL_VAL;
		}
		size = (size_t)length;
		buffer = malloc(size + 1);
		if (buffer == NULL) {
			fclose(file);
			vm_runtime_error(
			        vm, "out of memory reading '%s'.", path);
			return NIL_VAL;
		}
		size_t got = fread(buffer, 1, size, file);
		if (got < size && ferror(file)) {
			free(buffer);
			fclose(file);
			vm_runtime_error(vm,
			        "cannot read '%s': %s.",
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
			        vm, "out of memory reading '%s'.", path);
			return NIL_VAL;
		}
		for (;;) {
			if (used == capacity) {
				if (capacity > (size_t)-1 / 2) {
					free(buffer);
					fclose(file);
					vm_runtime_error(vm,
					        "file '%s' is too large.",
					        path);
					return NIL_VAL;
				}
				size_t grown = capacity * 2;
				char *bigger = realloc(buffer, grown);
				if (bigger == NULL) {
					free(buffer);
					fclose(file);
					vm_runtime_error(vm,
					        "out of memory reading "
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
				        "out of memory reading '%s'.",
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
	ObjString *text = new_string(vm, buffer, (int)size);
	free(buffer);
	return STR_VAL(text);
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
		        vm, "cannot write '%s': %s.", path, strerror(errno));
		return NIL_VAL;
	}

	size_t wrote = fwrite(content->chars, 1, (size_t)content->length, file);
	if (wrote != (size_t)content->length) {
		/* a full disk is not an fwrite failure you can ignore. the
		 * message says which, because "cannot write" with no
		 * reason is the least useful error in the world. */
		vm_runtime_error(vm,
		        "cannot write '%s': %s.",
		        path,
		        ferror(file) ? strerror(errno) : "short write");
		fclose(file);
		return NIL_VAL;
	}

	if (fclose(file) != 0) {
		/* a close that fails means the data may not have landed,
		 * and for a script writing a file that matters */
		vm_runtime_error(
		        vm, "cannot write '%s': %s.", path, strerror(errno));
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
 * So this takes the program and its arguments separately and calls execvp,
 * which searches PATH and does not interpret anything. A script that
 * genuinely wants a shell can say so, with system(), and owns the quoting.
 *
 * Returns the exit status: 0 for success, the child's code for a normal
 * failure, and 127 or 126 for the shell's own "not found" and "not
 * executable" conventions, which is what a shell would have reported.
 */
/*
 * process.run(cmd) and process.run(cmd, opts) -> table.
 *
 * The captured-output counterpart to exec(). Where exec() hands the child's
 * streams to the terminal and returns a bare status, this captures both
 * streams into strings and returns everything at once:
 *
 *     {stdout: "...", stderr: "...", code: 0, timed_out: false}
 *
 * `cmd` is a non-empty list of strings: the program, then its arguments.
 * There is deliberately no string form. A single string would have to be
 * split somewhere, and splitting on spaces breaks on filenames that contain
 * them -- which is the injection-shaped bug exec() exists to avoid. An
 * argument list has no such question.
 *
 * `opts` is a table, and every key is optional:
 *   cwd      run there instead of here. must be a string.
 *   stdin    piped to the child's standard input. must be a string.
 *   timeout  milliseconds before the child is killed. must be a
 *            non-negative number; missing means wait as long as it takes.
 *
 * Unknown keys are an error rather than ignored. An option the runtime does
 * not understand is almost certainly a misspelled option it does, and
 * silently dropping it would run the child with different behaviour than
 * the script asked for.
 *
 * Both streams are drained with poll() while the child runs. Reading one to
 * EOF and then the other deadlocks as soon as the child fills the second
 * pipe's buffer while the parent is still reading the first -- 64K of
 * stderr with nobody reading it is all it takes. poll() waits on both and
 * reads whatever is ready, so neither can fill.
 *
 * On timeout the child gets SIGKILL, the pipes are drained of whatever it
 * wrote before dying, and timed_out comes back true with the code the wait
 * reported. A timeout is not subtle and the result does not pretend it was
 * clean.
 *
 * code follows the shell convention exec() uses: the exit status, 128+signo
 * for a signal death, 127 for "not found", 126 for anything else that kept
 * exec from running. timed_out disambiguates a SIGKILL the timeout sent
 * from one the child earned on its own.
 */
static Value process_run_native(VM *vm, int argc, Value *argv)
{
	if (argc < 1 || argc > 2) {
		vm_runtime_error(vm,
		        "process.run() takes a command list and an optional "
		        "options table.");
		return NIL_VAL;
	}
	if (!IS_LIST(argv[0]) || AS_LIST(argv[0])->count == 0) {
		vm_runtime_error(vm,
		        "process.run() takes a non-empty list of strings: "
		        "the program, then its arguments.");
		return NIL_VAL;
	}
	ObjList *cmd = AS_LIST(argv[0]);
	for (int i = 0; i < cmd->count; i++) {
		if (!IS_STRING(cmd->items[i])) {
			vm_runtime_error(vm,
			        "process.run() argument %d must be a string.",
			        i + 1);
			return NIL_VAL;
		}
	}

	/* options, all optional. defaults mean inherit: same directory, no
	 * input, wait as long as it takes. */
	const char *cwd = NULL;
	const char *input = NULL;
	size_t input_left = 0;
	long timeout_ms = -1;
	if (argc == 2) {
		if (!IS_FLINT_TABLE(argv[1])) {
			vm_runtime_error(
			        vm, "process.run() options must be a table.");
			return NIL_VAL;
		}
		ObjTable *opts = AS_FLINT_TABLE(argv[1]);
		for (int i = 0; i < opts->count; i++) {
			const char *key = opts->keys[i]->chars;
			if (strcmp(key, "cwd") == 0) {
				if (!IS_STRING(opts->values[i])) {
					vm_runtime_error(vm,
					        "process.run() option 'cwd' "
					        "must be a string.");
					return NIL_VAL;
				}
				cwd = AS_CSTRING(opts->values[i]);
			} else if (strcmp(key, "stdin") == 0) {
				if (!IS_STRING(opts->values[i])) {
					vm_runtime_error(vm,
					        "process.run() option 'stdin' "
					        "must be a string.");
					return NIL_VAL;
				}
				input = AS_CSTRING(opts->values[i]);
				input_left = (size_t)AS_STRING(opts->values[i])
				                     ->length;
			} else if (strcmp(key, "timeout") == 0) {
				/*
				 * Validated as finite and fitting in an int
				 * before the cast below. (long) on NaN, infinity
				 * or a double past LONG_MAX is undefined
				 * behaviour, not a large timeout -- and a
				 * script asking to wait forever should say so
				 * by omitting the key rather than by passing
				 * infinity and getting whatever the cast
				 * produces.
				 */
				if (!IS_NUMBER(opts->values[i]) ||
				        isnan(AS_NUMBER(opts->values[i])) ||
				        isinf(AS_NUMBER(opts->values[i])) ||
				        AS_NUMBER(opts->values[i]) < 0 ||
				        AS_NUMBER(opts->values[i]) >
				                (double)INT_MAX) {
					vm_runtime_error(vm,
					        "process.run() option "
					        "'timeout' "
					        "must be a non-negative number "
					        "of milliseconds.");
					return NIL_VAL;
				}
				timeout_ms = (long)AS_NUMBER(opts->values[i]);
			} else {
				vm_runtime_error(vm,
				        "unknown process.run() option '%s'.",
				        key);
				return NIL_VAL;
			}
		}
	}

	/* child_argv borrows the flint strings on the vm stack, which stay
	 * rooted across the fork below -- the same arrangement exec() uses. */
	char **child_argv = malloc(sizeof(char *) * (size_t)(cmd->count + 1));
	if (child_argv == NULL) {
		vm_runtime_error(vm, "out of memory in process.run().");
		return NIL_VAL;
	}
	for (int i = 0; i < cmd->count; i++)
		child_argv[i] = AS_CSTRING(cmd->items[i]);
	child_argv[cmd->count] = NULL;

	/* one pipe per captured stream, plus one for stdin when there is
	 * input to send. all created before the fork, so a pipe failure is
	 * an ordinary error rather than a half-built child. */
	int out_pipe[2] = {-1, -1};
	int err_pipe[2] = {-1, -1};
	int in_pipe[2] = {-1, -1};
	if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0 ||
	        (input != NULL && pipe(in_pipe) != 0)) {
		if (out_pipe[0] != -1) {
			close(out_pipe[0]);
			close(out_pipe[1]);
		}
		if (err_pipe[0] != -1) {
			close(err_pipe[0]);
			close(err_pipe[1]);
		}
		free(child_argv);
		vm_runtime_error(
		        vm, "cannot create pipes: %s.", strerror(errno));
		return NIL_VAL;
	}

	fflush(stdout);
	fflush(stderr);

	pid_t pid = fork();
	if (pid < 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		close(err_pipe[0]);
		close(err_pipe[1]);
		if (in_pipe[0] != -1) {
			close(in_pipe[0]);
			close(in_pipe[1]);
		}
		free(child_argv);
		vm_runtime_error(vm, "cannot fork: %s.", strerror(errno));
		return NIL_VAL;
	}

	if (pid == 0) {
		/*
		 * Child: wire the pipes onto 0/1/2, move directories, run.
		 * Nothing here may allocate, lock, or return -- the child
		 * shares the parent's buffers and heap, so anything but
		 * dup2/chdir/exec/_exit risks running them twice.
		 */
		if (input != NULL) {
			close(in_pipe[1]);
			if (in_pipe[0] != STDIN_FILENO) {
				dup2(in_pipe[0], STDIN_FILENO);
				close(in_pipe[0]);
			}
		}
		close(out_pipe[0]);
		close(err_pipe[0]);
		dup2(out_pipe[1], STDOUT_FILENO);
		dup2(err_pipe[1], STDERR_FILENO);
		close(out_pipe[1]);
		close(err_pipe[1]);
		if (cwd != NULL && chdir(cwd) != 0)
			/* NOLINTNEXTLINE(misc-include-cleaner) */
			_exit(126);
		execvp(child_argv[0], child_argv);
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		_exit(errno == ENOENT ? 127 : 126);
	}

	/* parent from here. the write ends belong to the child now; closing
	 * ours is what lets us see EOF, and forgetting any one of them hangs
	 * the drain loop below on a pipe that can never close. */
	free(child_argv);
	close(out_pipe[1]);
	close(err_pipe[1]);
	if (input != NULL)
		close(in_pipe[0]);

	/* feed stdin first, before reading anything back. a child that reads
	 * all of its input before writing -- `cat`, `grep`, most filters --
	 * would otherwise block on an empty stdin pipe while the parent blocks
	 * on an empty stdout pipe, and neither would move again. */
	if (input != NULL) {
		while (input_left > 0) {
			ssize_t n = write(in_pipe[1], input, input_left);
			if (n < 0) {
				/* NOLINTNEXTLINE(misc-include-cleaner) */
				if (errno == EINTR)
					continue;
				/* EPIPE means the child exited without reading
				 * everything, which is its right. anything
				 * else is reported below through the wait. */
				break;
			}
			input += n;
			input_left -= (size_t)n;
		}
		close(in_pipe[1]);
	}

	/* drain both streams. geometric growth from one page, like every
	 * other growing buffer in the runtime. */
	size_t out_cap = 4096;
	size_t out_len = 0;
	char *out_buf = malloc(out_cap);
	size_t err_cap = 4096;
	size_t err_len = 0;
	char *err_buf = malloc(err_cap);
	if (out_buf == NULL || err_buf == NULL) {
		free(out_buf);
		free(err_buf);
		close(out_pipe[0]);
		close(err_pipe[0]);
		vm_runtime_error(vm, "out of memory in process.run().");
		return NIL_VAL;
	}

	bool out_done = false;
	bool err_done = false;
	bool timed_out = false;
	for (;;) {
		if (out_done && err_done)
			break;
		/* <poll.h> and <errno.h> are included at the top; the cleaner
		 * does not see through the loop. same class of false positive
		 * as the _exit one below. */
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		struct pollfd fds[2];
		fds[0].fd = out_pipe[0];
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		fds[0].events = out_done ? 0 : POLLIN;
		fds[1].fd = err_pipe[0];
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		fds[1].events = err_done ? 0 : POLLIN;
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		int ready = poll(fds, 2, timeout_ms < 0 ? -1 : (int)timeout_ms);
		if (ready < 0) {
			/* NOLINTNEXTLINE(misc-include-cleaner) */
			if (errno == EINTR)
				continue;
			break;
		}
		if (ready == 0) {
			/* the timeout fired with output still coming. kill,
			 * then fall out of the loop to drain whatever is left
			 * and reap: the pipes hold it whether or not the
			 * child is alive to write more. */
			kill(pid, SIGKILL);
			timed_out = true;
			break;
		}
		for (int s = 0; s < 2; s++) {
			if (fds[s].revents == 0)
				continue;
			char **buf = s == 0 ? &out_buf : &err_buf;
			size_t *len = s == 0 ? &out_len : &err_len;
			size_t *cap = s == 0 ? &out_cap : &err_cap;
			if (*len == *cap) {
				size_t fresh = *cap * 2;
				char *grown = realloc(*buf, fresh);
				if (grown == NULL) {
					free(out_buf);
					free(err_buf);
					close(out_pipe[0]);
					close(err_pipe[0]);
					vm_runtime_error(vm,
					        "out of memory in "
					        "process.run().");
					return NIL_VAL;
				}
				*buf = grown;
				*cap = fresh;
			}
			ssize_t n = read(fds[s].fd, *buf + *len, *cap - *len);
			if (n <= 0) {
				/* EOF or a dead pipe: this stream is done.
				 * errors other than EINTR end it too, because
				 * a stream that cannot be read has nothing
				 * left to give and the wait below still
				 * reports what happened. */
				if (n < 0 && errno == EINTR)
					continue;
				if (s == 0)
					out_done = true;
				else
					err_done = true;
			} else {
				*len += (size_t)n;
			}
		}
	}

	/* whatever is left after a timeout kill: the child is dead or dying,
	 * but its last writes may still be in the pipes. */
	while (!out_done || !err_done) {
		struct pollfd fds[2];
		fds[0].fd = out_pipe[0];
		fds[0].events = out_done ? 0 : POLLIN;
		fds[1].fd = err_pipe[0];
		fds[1].events = err_done ? 0 : POLLIN;
		int ready = poll(fds, 2, 0);
		if (ready <= 0)
			break;
		for (int s = 0; s < 2; s++) {
			if (fds[s].revents == 0)
				continue;
			char **buf = s == 0 ? &out_buf : &err_buf;
			size_t *len = s == 0 ? &out_len : &err_len;
			size_t *cap = s == 0 ? &out_cap : &err_cap;
			if (*len == *cap) {
				size_t fresh = *cap * 2;
				char *grown = realloc(*buf, fresh);
				if (grown == NULL)
					break;
				*buf = grown;
				*cap = fresh;
			}
			ssize_t n = read(fds[s].fd, *buf + *len, *cap - *len);
			if (n <= 0) {
				if (n < 0 && errno == EINTR)
					continue;
				if (s == 0)
					out_done = true;
				else
					err_done = true;
			} else {
				*len += (size_t)n;
			}
		}
	}
	close(out_pipe[0]);
	close(err_pipe[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		if (errno != EINTR) {
			free(out_buf);
			free(err_buf);
			vm_runtime_error(vm,
			        "cannot wait for child: %s.",
			        strerror(errno));
			return NIL_VAL;
		}
	}

	double code;
	if (WIFEXITED(status))
		code = (double)WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		code = 128.0 + (double)WTERMSIG(status);
	else
		code = 127.0;

	/*
	 * The result table. Built field by field with each string pushed
	 * across the allocations that can collect, which is the same rooting
	 * discipline as everywhere else a native builds a value: nothing
	 * unrooted survives a GROW_ARRAY.
	 *
	 * Lengths are checked against INT_MAX before the conversion, because
	 * a child can write more than a flint string can hold and (int) on
	 * that size is undefined behaviour, not a long string.
	 */
	if (out_len > (size_t)INT_MAX || err_len > (size_t)INT_MAX) {
		free(out_buf);
		free(err_buf);
		vm_runtime_error(
		        vm, "process.run() output is too large to hold.");
		return NIL_VAL;
	}
	ObjTable *result = new_flint_table(vm);
	vm_push(vm, OBJ_VAL(result));

	ObjString *out_str = new_string(vm, out_buf, (int)out_len);
	vm_push(vm, STR_VAL(out_str));
	ObjString *err_str = new_string(vm, err_buf, (int)err_len);
	vm_push(vm, STR_VAL(err_str));
	free(out_buf);
	free(err_buf);

	static const char *field_names[4] = {
	        "stdout", "stderr", "code", "timed_out"};
	Value field_vals[4] = {STR_VAL(out_str),
	        STR_VAL(err_str),
	        NUMBER_VAL(code),
	        timed_out ? TRUE_VAL : FALSE_VAL};
	for (int i = 0; i < 4; i++) {
		ObjString *name = copy_string(
		        vm, field_names[i], (int)strlen(field_names[i]));
		vm_push(vm, STR_VAL(name));
		if (result->capacity < result->count + 1) {
			int old_cap = result->capacity;
			result->capacity = GROW_CAPACITY(old_cap);
			result->keys = GROW_ARRAY(vm,
			        ObjString *,
			        result->keys,
			        old_cap,
			        result->capacity);
			result->values = GROW_ARRAY(vm,
			        Value,
			        result->values,
			        old_cap,
			        result->capacity);
		}
		result->keys[result->count] = name;
		result->values[result->count] = field_vals[i];
		result->count++;
		vm_pop(vm);
	}
	vm_pop(vm); /* err_str */
	vm_pop(vm); /* out_str */
	vm_pop(vm); /* the result */
	return OBJ_VAL(result);
}

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
			        "argument %d to exec() must be a string.",
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
		vm_runtime_error(vm, "out of memory in exec().");
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
		vm_runtime_error(vm, "cannot fork: %s.", strerror(errno));
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
			        "cannot wait for child: %s.",
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

/*
 * The primitives the fs, random and time modules are built from.
 *
 * Each one is deliberately thin. The module in lib/ is the public api and
 * this is the implementation; neither one should grow into the other, and a
 * user should never have to know a __ name exists.
 */

/* exists(path) -> bool. a directory counts: fs is about paths existing, not
 * about what they are. */
static Value exists_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0])) {
		vm_runtime_error(vm, "Argument to exists() must be a string.");
		return NIL_VAL;
	}
	return access(AS_CSTRING(argv[0]), F_OK) == 0 ? TRUE_VAL : FALSE_VAL;
}

/* remove(path) -> bool. false for "it was not there", which is a filesystem
 * fact and not a failure worth stopping a script for. */
static Value remove_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0])) {
		vm_runtime_error(vm, "Argument to remove() must be a string.");
		return NIL_VAL;
	}
	return unlink(AS_CSTRING(argv[0])) == 0 ? TRUE_VAL : FALSE_VAL;
}

/* mkdir(path) -> bool. one level. recursive creation is a decision, and
 * making it silently is how scripts end up with trees nobody planned. */
static Value mkdir_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0])) {
		vm_runtime_error(vm, "Argument to mkdir() must be a string.");
		return NIL_VAL;
	}
	return mkdir(AS_CSTRING(argv[0]), 0777) == 0 ? TRUE_VAL : FALSE_VAL;
}

/* isdir(path) -> bool */
static Value isdir_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0])) {
		vm_runtime_error(vm, "Argument to isdir() must be a string.");
		return NIL_VAL;
	}
	struct stat st;
	if (stat(AS_CSTRING(argv[0]), &st) != 0)
		return FALSE_VAL;
	return S_ISDIR(st.st_mode) ? TRUE_VAL : FALSE_VAL;
}

/*
 * listdir(path) -> list of names, or nil when the directory cannot be read.
 *
 * Names, not full paths: the caller joins with path.join, which keeps this
 * honest about what it knows. "." and ".." are included, exactly as the
 * filesystem reports them -- filtering them would be a policy this function
 * has no business making, and every caller that cares already skips dotfiles
 * for its own reasons.
 *
 * Order is whatever the filesystem returns. Not sorted, not insertion
 * order, not promised: a script that needs determinism sorts the result
 * itself (or should, once there is a sort to call).
 *
 * Nil on any failure -- missing directory, permission denied -- because a
 * listing that cannot be read has no entries to report, and the caller that
 * needs the reason already checked fs.exists() first. That two-step is the
 * documented pattern, not an accident: exists-then-list has a TOCTOU window,
 * and nil covers it.
 */
static Value listdir_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0])) {
		vm_runtime_error(vm, "Argument to listdir() must be a string.");
		return NIL_VAL;
	}
#ifdef _WIN32
	/* FindFirstFile needs a pattern, not a bare directory. */
	ObjString *pattern = NULL;
	{
		const char *dir = AS_CSTRING(argv[0]);
		size_t n = strlen(dir);
		char *pat = malloc(n + 3);
		if (pat == NULL) {
			vm_runtime_error(vm, "out of memory in listdir().");
			return NIL_VAL;
		}
		memcpy(pat, dir, n);
		memcpy(pat + n, "\\*", 3);
		pattern = copy_string(vm, pat, (int)(n + 2));
		free(pat);
	}
	vm_push(vm, STR_VAL(pattern));
	WIN32_FIND_DATAA found;
	HANDLE handle = FindFirstFileA(pattern->chars, &found);
	vm_pop(vm);
	if (handle == INVALID_HANDLE_VALUE)
		return NIL_VAL;
	ObjList *out = new_list(vm);
	vm_push(vm, OBJ_VAL(out));
	do {
		ObjString *name = copy_string(
		        vm, found.cFileName, (int)strlen(found.cFileName));
		vm_push(vm, STR_VAL(name));
		if (out->count == out->capacity) {
			int old_cap = out->capacity;
			out->capacity = GROW_CAPACITY(old_cap);
			out->items = GROW_ARRAY(
			        vm, Value, out->items, old_cap, out->capacity);
		}
		out->items[out->count++] = vm->stack_top[-1];
		vm_pop(vm);
	} while (FindNextFileA(handle, &found));
	FindClose(handle);
	vm_pop(vm);
	return OBJ_VAL(out);
#else
	DIR *dir = opendir(AS_CSTRING(argv[0]));
	if (dir == NULL)
		return NIL_VAL;
	ObjList *out = new_list(vm);
	vm_push(vm, OBJ_VAL(out));
	for (;;) {
		/*
		 * readdir is the one libc call here that can fail without
		 * returning NULL for end-of-stream, so errno is cleared
		 * first and checked after: a stale errno from an earlier
		 * call would otherwise report an error that never happened.
		 */
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (entry == NULL) {
			if (errno != 0) {
				closedir(dir);
				vm_pop(vm);
				return NIL_VAL;
			}
			break;
		}
		ObjString *name = copy_string(
		        vm, entry->d_name, (int)strlen(entry->d_name));
		vm_push(vm, STR_VAL(name));
		if (out->count == out->capacity) {
			int old_cap = out->capacity;
			out->capacity = GROW_CAPACITY(old_cap);
			out->items = GROW_ARRAY(
			        vm, Value, out->items, old_cap, out->capacity);
		}
		out->items[out->count++] = vm->stack_top[-1];
		vm_pop(vm);
	}
	closedir(dir);
	vm_pop(vm);
	return OBJ_VAL(out);
#endif
}

/* __rand() -> number in [0, 1)
 *
 * xorshift64*, seeded from the clock and the pid on first use. this is not
 * cryptography and the module says so: it is for sampling, shuffling and
 * jitter, and anyone reaching for it for a token or a key should be using
 * something else entirely.
 *
 * the state is a function-static because there is nowhere better to put it,
 * and a random module that cannot generate a second number would be a joke.
 */
static uint64_t rand_state = 0;

static uint64_t next_random(void)
{
	if (rand_state == 0) {
		/* seeding from both, because two flint scripts started in the
		 * same microsecond should not produce the same stream */
		rand_state = (uint64_t)time(NULL) * 6364136223846793005ULL +
		             (uint64_t)getpid();
		if (rand_state == 0)
			rand_state = 0x9E3779B97F4A7C15ULL;
	}
	/* xorshift64*: three shifts and three xors */
	uint64_t x = rand_state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rand_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static Value rand_native(VM *vm, int argc, Value *argv)
{
	(void)vm;
	(void)argc;
	(void)argv;
	/* 53 bits is the most a double can hold exactly, and the divisor is
	 * 2^53. anything more and the result is not uniformly distributed over
	 * the range it claims. */
	return NUMBER_VAL((double)(next_random() >> 11) / 9007199254740992.0);
}

/* __seed(n). makes a test reproducible and nothing else. returns nil because
 * there is nothing useful to report and a value would suggest there is. */
static Value seed_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	(void)vm;
	if (!IS_NUMBER(argv[0])) {
		vm_runtime_error(vm, "Argument to seed() must be a number.");
		return NIL_VAL;
	}
	rand_state = (uint64_t)(long)AS_NUMBER(argv[0]);
	if (rand_state == 0)
		rand_state = 0x9E3779B97F4A7C15ULL;
	return NIL_VAL;
}

/* time.now() -> seconds, unix. a number, not a table, because formatting
 * a date is somebody else's problem and a good library's. */
static Value time_now_native(VM *vm, int argc, Value *argv)
{
	(void)vm;
	(void)argc;
	(void)argv;
	return NUMBER_VAL((double)time(NULL));
}

/* __clock_ms() -> cpu milliseconds, for measuring a thing. clock() is
 * seconds as a double and loses resolution below a millisecond on some
 * platforms; this is the one to use in a benchmark. */
static Value clock_ms_native(VM *vm, int argc, Value *argv)
{
	(void)vm;
	(void)argc;
	(void)argv;
	return NUMBER_VAL((double)clock() * 1000.0);
}

/* __sleep(seconds). interrupts, so ctrl-c works while a script waits. */
static Value sleep_native(VM *vm, int argc, Value *argv)
{
	(void)vm;
	(void)argc;
	if (!IS_NUMBER(argv[0])) {
		vm_runtime_error(vm, "Argument to sleep() must be a number.");
		return NIL_VAL;
	}
	double d = AS_NUMBER(argv[0]);
	if (d < 0) {
		vm_runtime_error(vm, "sleep() needs a non-negative number.");
		return NIL_VAL;
	}
	struct timespec ts;
	ts.tv_sec = (time_t)d;
	ts.tv_nsec = (long)((d - (double)ts.tv_sec) * 1e9);
	/* a truncated nanos field would be a short sleep nobody notices,
	 * and every caller would be subtly wrong */
	if (ts.tv_nsec < 0)
		ts.tv_nsec = 0;
	if (ts.tv_nsec > 999999999L)
		ts.tv_nsec = 999999999L;
	nanosleep(&ts, NULL);
	return NIL_VAL;
}

/* __time_str(t) -> "YYYY-MM-DD HH:MM:SS" in UTC.
 *
 * gmtime, not localtime. a formatting function that silently depends on the
 * machine's timezone is a library that is right on one machine and wrong on
 * the next, and a test that depends on it fails at midnight. */
static Value time_str_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_NUMBER(argv[0])) {
		vm_runtime_error(
		        vm, "Argument to time_str() must be a number.");
		return NIL_VAL;
	}
	time_t t = (time_t)AS_NUMBER(argv[0]);
	struct tm tm;
	if (gmtime_r(&t, &tm) == NULL)
		return STR_VAL(new_string(vm, "", 0));
	char buf[64];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
	return STR_VAL(new_string(vm, buf, (int)strlen(buf)));
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
	/* -1 because process.run() takes one or two arguments, and the
	 * fixed-arity check cannot express that. it checks inside. */
	vm_define_native(vm, "__process_run", process_run_native, -1);

	/* the fs, random and time modules are written in flint over these.
	 * they are the only system-level entry points they need, so the
	 * public api stays in lib/ where it can be read. */
	vm_define_native(vm, "__exists", exists_native, 1);
	vm_define_native(vm, "__remove", remove_native, 1);
	vm_define_native(vm, "__mkdir", mkdir_native, 1);
	vm_define_native(vm, "__isdir", isdir_native, 1);
	vm_define_native(vm, "__listdir", listdir_native, 1);
	vm_define_native(vm, "__rand", rand_native, 0);
	vm_define_native(vm, "__seed", seed_native, 1);
	vm_define_native(vm, "__now", time_now_native, 0);
	vm_define_native(vm, "__clock_ms", clock_ms_native, 0);
	vm_define_native(vm, "__sleep", sleep_native, 1);
	vm_define_native(vm, "__time_str", time_str_native, 1);

	/* the os module is written in flint over these, like fs above. */
	vm_define_native(vm, "__os_name", os_name_native, 0);
	vm_define_native(vm, "__os_arch", os_arch_native, 0);
	vm_define_native(vm, "__os_pathsep", os_pathsep_native, 0);
	vm_define_native(vm, "__os_getcwd", os_getcwd_native, 0);
	vm_define_native(vm, "__os_chdir", os_chdir_native, 1);
	vm_define_native(vm, "__os_getenv", os_getenv_native, 2);
	vm_define_native(vm, "__os_setenv", os_setenv_native, 2);
	vm_define_native(vm, "__os_unsetenv", os_unsetenv_native, 1);
	vm_define_native(vm, "__os_homedir", os_homedir_native, 0);
	vm_define_native(vm, "__os_tmpdir", os_tmpdir_native, 0);
	vm_define_native(vm, "__os_pid", os_pid_native, 0);
	register_json_natives(vm);
	register_http_natives(vm);
}
