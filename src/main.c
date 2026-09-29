/* SPDX-License-Identifier: MIT */
/*
 * Command line entry point, file loading, the repl, and the CLI.
 *
 * Exit codes follow sysexits, so a script can be checked by a shell without
 * parsing its output: 64 usage, 65 did not compile, 70 blew up at run time,
 * 74 could not read the file.
 */
#include "sys.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* vm.h is included at the top of this file and defines VM. the include
 * cleaner loses track through sys.h, which also pulls vm.h in, and reports
 * the first use rather than the header. */
/* NOLINTNEXTLINE(misc-include-cleaner) */
static void repl(VM *vm)
{
	char line[1024];
	for (;;) {
		printf("> ");
		fflush(stdout);

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
	/* a length of 0 is a real file, an empty one. the +1 is for the NUL.
	 * checked, because a length near SIZE_MAX wraps the sum to 0 and the
	 * fread then writes past a zero byte allocation. */
	if ((unsigned long)length >= (unsigned long)-1) {
		fprintf(stderr, "\"%s\" is too large.\n", path);
		fclose(file);
		exit(74);
	}
	size_t size = (size_t)length;
	if (fseek(file, 0L, SEEK_SET) != 0) {
		fprintf(stderr, "Could not rewind \"%s\".\n", path);
		fclose(file);
		exit(74);
	}

	char *buffer = (char *)malloc(size + 1);
	if (buffer == NULL) {
		fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
		fclose(file);
		exit(74);
	}

	/* a short read means a directory, a pipe, or a race. an unterminated
	 * buffer would be handed to the compiler as a truncated script */
	size_t got = fread(buffer, 1, size, file);
	if (got < size) {
		fprintf(stderr, "Could not read \"%s\".\n", path);
		free(buffer);
		fclose(file);
		exit(74);
	}
	/* the NUL lands in the byte the malloc(size + 1) above reserved for
	 * it, so this is in bounds by construction. the analyser reports a
	 * tainted index because size came from ftell on a path the user
	 * chose and it does not tie the index back to the allocation. */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	buffer[size] = '\0';
	fclose(file);
	return buffer;
}

/* run one file, with the imports resolved relative to *its* directory */
static void run_file(VM *vm, const char *path)
{
	/*
	 * Set the source directory so `import "lib/x.fl"` inside this file
	 * resolves next to it, not relative to wherever the user is standing.
	 * this is what makes a script runnable from any directory. see
	 * sys_resolve_module().
	 */
	sys_set_source_dir_for_file(path);

	char *source = read_file(path);
	InterpretResult result = vm_interpret(vm, source);
	free(source);

	if (result == INTERPRET_COMPILE_ERROR)
		exit(65);
	if (result == INTERPRET_RUNTIME_ERROR)
		exit(70);
}

/* read the whole of stdin as a script. used by `flint -` and `flint` with a
 * pipe. the buffer grows, because a piped script has no size. */
static char *read_stdin(void)
{
	size_t capacity = 8192;
	size_t used = 0;
	char *buffer = malloc(capacity);
	if (buffer == NULL) {
		fprintf(stderr, "Not enough memory to read stdin.\n");
		exit(74);
	}

	for (;;) {
		if (used == capacity) {
			if (capacity > (size_t)-1 / 2) {
				free(buffer);
				fprintf(stderr, "stdin is too large.\n");
				exit(74);
			}
			size_t grown = capacity * 2;
			char *bigger = realloc(buffer, grown);
			if (bigger == NULL) {
				free(buffer);
				fprintf(stderr,
				        "Not enough memory to read stdin.\n");
				exit(74);
			}
			buffer = bigger;
			capacity = grown;
		}
		size_t got = fread(buffer + used, 1, capacity - used, stdin);
		used += got;
		/* a short read on a pipe is not end of file. stop on a real
		 * EOF or error, and on error do not call fread again: the
		 * stream position is indeterminate after ferror. */
		if (got == 0 || ferror(stdin))
			break;
	}
	/* the loop grows only when used == capacity, so a fill that lands
	 * exactly on the boundary has no spare byte for this NUL. that is
	 * every input whose length is a power of two, so it is not a
	 * corner. */
	if (used == capacity) {
		char *bigger = realloc(buffer, capacity + 1);
		if (bigger == NULL) {
			free(buffer);
			fprintf(stderr, "Not enough memory to read stdin.\n");
			exit(74);
		}
		buffer = bigger;
	}
	/* in bounds: the grow loop above guarantees capacity > used before
	 * this point, by growing when used == capacity. the analyser cannot
	 * follow the realloc, so it reports a tainted index it cannot
	 * prove. */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	buffer[used] = '\0';
	return buffer;
}

static void print_usage(FILE *stream)
{
	fprintf(stream, "Usage: flint [options] [script.fl] [args...]\n");
	fprintf(stream, "\n");
	fprintf(stream,
	        "Run a script, read one from stdin, or start a repl.\n");
	fprintf(stream, "Anything after the script path is passed to the\n");
	fprintf(stream, "script and read back with args().\n");
	fprintf(stream, "\n");
	fprintf(stream, "Options:\n");
	fprintf(stream, "  -h, --help       show this help and exit\n");
	fprintf(stream, "  -v, --version    show the version and exit\n");
	fprintf(stream, "  -e <code>        execute code and exit\n");
	fprintf(stream, "  -                read a script from stdin\n");
	fprintf(stream, "\n");
	fprintf(stream, "With no arguments, starts a repl.\n");
}

int main(int argc, char *argv[])
{
	/*
	 * Hand the whole command line to the runtime before anything else.
	 * args() reads from this, and a script run by `flint x.fl a b` should
	 * see [a, b] regardless of which of the code paths below runs it.
	 */
	sys_set_args(argc, argv);

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
			if (argc < 3) {
				fprintf(stderr, "-e requires an argument\n");
				print_usage(stderr);
				return 64;
			}
			VM vm;
			vm_init(&vm);
			/* code has no file, so imports resolve against the
			 * working directory, which is the only thing they can
			 * sensibly mean */
			InterpretResult result = vm_interpret(&vm, argv[2]);
			vm_free(&vm);
			if (result == INTERPRET_COMPILE_ERROR)
				return 65;
			if (result == INTERPRET_RUNTIME_ERROR)
				return 70;
			return 0;
		}
		if (strcmp(flag, "-") == 0) {
			/* the script comes from stdin. imports inside it
			 * resolve against the working directory, because
			 * stdin has no path of its own to be relative
			 * to. */
			VM vm;
			vm_init(&vm);
			char *source = read_stdin();
			InterpretResult result = vm_interpret(&vm, source);
			free(source);
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
	} else {
		/* one path plus any number of arguments. the arguments belong
		 * to the script, not to flint, so they are not parsed here:
		 * `flint x.fl -v` runs x.fl with "-v" as an argument rather
		 * than printing the version and running nothing. that is what
		 * a script author expects, and it is what args() is for. */
		run_file(&vm, argv[1]);
	}

	vm_free(&vm);
	sys_free_args();
	return 0;
}
