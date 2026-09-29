/* SPDX-License-Identifier: MIT */
/*
 * Entry point: argument handling, the repl, and file loading.
 *
 * Exit codes follow sysexits, so scripts can tell the difference between
 * "you called me wrong", "your file does not compile", and "your program
 * blew up": 64, 65, 70.
 */
#include "vm.h"
#include "object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The version comes from the build, not from here. The Makefile passes
 * -DFLINT_VERSION="$(git describe)" on every compile, so the binary always
 * reports the tag that produced it. A version string edited by hand in this
 * file is how v0.2.0 shipped identifying as v0.1.0, and that only needs to
 * happen once.
 *
 * Building outside a git checkout (from a tarball, for example) defines
 * nothing, so this fallback is what those binaries report.
 */
#ifndef FLINT_VERSION
#	define FLINT_VERSION "dev"
#endif

/*
 * Read-eval-print, one line at a time.
 *
 * Each line is compiled and run as a fresh script, so state carries over
 * only through globals, and a syntax error on one line does not poison the
 * next. There is no multi-line input: a block that is not closed yet is a
 * syntax error. That is a real limitation, not a design decision.
 */
static void repl(VM *vm)
{
	char line[1024];
	for (;;) {
		printf("> ");

		/* fgets returns NULL on EOF and on error alike. either way
		 * we are done. */
		if (!fgets(line, sizeof(line), stdin)) {
			printf("\n");
			break;
		}

		vm_interpret(vm, line);
	}
}

/* slurp a whole file. the caller frees the result. */
static char *read_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "Could not open file \"%s\".\n", path);
		exit(74);
	}

	/*
	 * ftell reports the size as a long, and returns -1 on failure. A
	 * pipe, a socket or /dev/stdin is not seekable, so the fseek below
	 * fails and that -1 arrives here. It must be tested before it is
	 * stored: assigned to a size_t it becomes SIZE_MAX, and the +1 in
	 * the malloc then wraps to 0, leaving a zero byte allocation that
	 * the fread writes straight past.
	 */
	if (fseek(file, 0L, SEEK_END) != 0) {
		fprintf(stderr, "Could not seek in \"%s\".\n", path);
		fclose(file);
		exit(74);
	}
	long length = ftell(file);
	if (length < 0) {
		fprintf(stderr, "Could not size \"%s\".\n", path);
		fclose(file);
		exit(74);
	}
	size_t file_size = (size_t)length;
	/* rewind() swallows the seek error; fseek() does not */
	if (fseek(file, 0L, SEEK_SET) != 0) {
		fprintf(stderr, "Could not rewind \"%s\".\n", path);
		fclose(file);
		exit(74);
	}

	/*
	 * malloc rather than ALLOCATE: this buffer is not an object and
	 * nothing else points at it, so a collection does not need to know
	 * about it. The compiler roots tokens by pointing into it, which
	 * works because tokens are only live during compile().
	 *
	 * file_size is at most LONG_MAX, so this +1 cannot wrap.
	 */
	char *buffer = (char *)malloc(file_size + 1);
	if (buffer == NULL) {
		fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
		fclose(file);
		exit(74);
	}

	/* a short read means a directory, a pipe, or a race. treat it as an
	 * error rather than running half a file */
	size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
	if (bytes_read < file_size) {
		fprintf(stderr, "Could not read file \"%s\".\n", path);
		free(buffer);
		fclose(file);
		exit(74);
	}

	/*
	 * The NUL lands in the byte the +1 above was allocated for, so this
	 * is the last index the buffer owns. bytes_read is equal to
	 * file_size here, proven by the check above; indexing with
	 * file_size says that directly, where indexing with the fread
	 * result would leave the bound to be inferred.
	 *
	 * The analyser still reports this as a tainted index, because
	 * file_size arrives from ftell on a stream opened with a
	 * user-supplied path, and it does not relate the index back to the
	 * malloc that sized the buffer. It is the same value in both.
	 */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	buffer[file_size] = '\0';

	fclose(file);
	return buffer;
}

static void run_file(VM *vm, const char *path)
{
	char *source = read_file(path);
	InterpretResult result = vm_interpret(vm, source);
	free(source);

	if (result == INTERPRET_COMPILE_ERROR)
		exit(65);
	if (result == INTERPRET_RUNTIME_ERROR)
		exit(70);
}

static void print_usage(FILE *stream)
{
	fprintf(stream, "Usage: flint [options] [path]\n");
	fprintf(stream, "Options:\n");
	fprintf(stream, "  -h, --help       show this help and exit\n");
	fprintf(stream, "  -v, --version    show the version and exit\n");
	fprintf(stream, "  -e <code>        execute code directly\n");
}

int main(int argc, char *argv[])
{
	/* options first, and only in the first argument position */
	if (argc > 1) {
		const char *flag = argv[1];

		if (strcmp(flag, "-h") == 0 || strcmp(flag, "--help") == 0) {
			print_usage(stdout);
			return 0;
		}
		if (strcmp(flag, "-v") == 0 || strcmp(flag, "--version") == 0) {
			printf("Flint %s\n", FLINT_VERSION);
			return 0;
		}
		if (strcmp(flag, "-e") == 0) {
			/* a whole VM for one expression, then out. the
			 * repl and the file path both need a longer life */
			if (argc < 3) {
				fprintf(stderr, "-e requires an argument\n");
				print_usage(stderr);
				return 64;
			}
			VM vm;
			vm_init(&vm);
			InterpretResult result = vm_interpret(&vm, argv[2]);
			vm_free(&vm);
			if (result == INTERPRET_COMPILE_ERROR)
				return 65;
			if (result == INTERPRET_RUNTIME_ERROR)
				return 70;
			return 0;
		}
	}

	VM vm;
	vm_init(&vm);

	if (argc == 1) {
		repl(&vm);
	} else if (argc == 2) {
		run_file(&vm, argv[1]);
	} else {
		/* more than one path. there is only one path. */
		print_usage(stderr);
		exit(64);
	}

	vm_free(&vm);
	return 0;
}
