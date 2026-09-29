/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L /* NOLINT(bugprone-reserved-identifier) */
/*
 * Command line entry point, file loading, the repl, and the CLI.
 *
 * Exit codes follow sysexits, so a script can be checked by a shell without
 * parsing its output: 64 usage, 65 did not compile, 70 blew up at run time,
 * 74 could not read the file.
 */
#include "sys.h"
#include "vm.h"
#include "compiler.h"
#include "diagnostic.h"
#include "object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
	InterpretResult result = vm_interpret_named(vm, source, path);
	free(source);

	if (result == INTERPRET_COMPILE_ERROR)
		exit(65);
	if (result == INTERPRET_RUNTIME_ERROR)
		exit(70);
}

static int fix_file(const char *path, FlDiagFormat format, FlColorMode color)
{
	char *source = read_file(path);
	VM vm;
	vm_init(&vm);
	if (format == FL_DIAG_LEGACY)
		format = FL_DIAG_HUMAN;
	vm_set_diagnostics(&vm, format, color);
	sys_set_source_dir_for_file(path);
	size_t source_len = strlen(source);
	ObjFunction *function = compile_named(&vm, source, path);
	size_t count = compiler_fix_count();
	if (function != NULL || count == 0) {
		free(source);
		vm_free(&vm);
		return function != NULL ? 0 : 65;
	}
	if (count > 64 || (size_t)count > ((size_t)-1 - source_len - 1) / 2) {
		free(source);
		vm_free(&vm);
		return 65;
	}

	const FlDiagSuggestion *edits[64];
	for (size_t i = 0; i < count; i++)
		edits[i] = compiler_fix_at(i);
	for (size_t i = 1; i < count; i++) {
		const FlDiagSuggestion *edit = edits[i];
		size_t j = i;
		while (j > 0 && edits[j - 1]->span.start > edit->span.start) {
			edits[j] = edits[j - 1];
			j--;
		}
		edits[j] = edit;
	}
	size_t output_len = source_len;
	for (size_t i = 0; i < count; i++) {
		const FlDiagSuggestion *edit = edits[i];
		size_t start = edit->span.start;
		size_t end = edit->span.end;
		if (start > end || end > source_len ||
		        (i > 0 && (edits[i - 1]->span.end > start ||
		                          (edits[i - 1]->span.start ==
		                                          edits[i - 1]
		                                                  ->span.end &&
		                                  start == end &&
		                                  edits[i - 1]->span.start ==
		                                          start)))) {
			free(source);
			vm_free(&vm);
			return 65;
		}
		if (edit->applicability != FL_APPLICABILITY_MACHINE ||
		        edit->replacement == NULL) {
			free(source);
			vm_free(&vm);
			return 65;
		}
		size_t replacement_len = strlen(edit->replacement);
		if (replacement_len >= end - start)
			output_len += replacement_len - (end - start);
		else
			output_len -= (end - start) - replacement_len;
	}
	char *output = malloc(output_len + 1);
	if (output == NULL) {
		free(source);
		vm_free(&vm);
		return 74;
	}
	size_t from = 0;
	size_t to = 0;
	for (size_t i = 0; i < count; i++) {
		const FlDiagSuggestion *edit = edits[i];
		size_t start = edit->span.start;
		size_t end = edit->span.end;
		memcpy(output + to, source + from, start - from);
		to += start - from;
		size_t replacement_len = strlen(edit->replacement);
		memcpy(output + to, edit->replacement, replacement_len);
		to += replacement_len;
		from = end;
	}
	memcpy(output + to, source + from, source_len - from);
	to += source_len - from;
	output[to] = '\0';
	if (compile_named(&vm, output, path) == NULL) {
		free(output);
		free(source);
		vm_free(&vm);
		return 65;
	}

	struct stat st;
	if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		free(output);
		free(source);
		vm_free(&vm);
		return 74;
	}
	size_t temp_len = strlen(path) + 48;
	char *temp = malloc(temp_len);
	if (temp == NULL) {
		free(output);
		free(source);
		vm_free(&vm);
		return 74;
	}
	snprintf(temp, temp_len, "%s.flint.%ld.tmp", path, (long)getpid());
	int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 0777);
	if (fd < 0) {
		free(temp);
		free(output);
		free(source);
		vm_free(&vm);
		return 74;
	}
	if (fchmod(fd, st.st_mode & 07777) != 0) {
		close(fd);
		unlink(temp);
		free(temp);
		free(output);
		free(source);
		vm_free(&vm);
		return 74;
	}
	FILE *file = fdopen(fd, "wb");
	bool ok = file != NULL;
	if (ok && fwrite(output, 1, output_len, file) != output_len)
		ok = false;
	if (ok && fflush(file) != 0)
		ok = false;
	if (ok && fsync(fd) != 0)
		ok = false;
	if (file != NULL) {
		if (fclose(file) != 0)
			ok = false;
	} else {
		close(fd);
	}
	if (ok && rename(temp, path) != 0)
		ok = false;
	if (!ok)
		unlink(temp);
	else
		printf("fixed: %s (%zu edit%s)\n",
		        path,
		        count,
		        count == 1 ? "" : "s");
	free(temp);
	free(output);
	free(source);
	vm_free(&vm);
	return ok ? 0 : 74;
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
	fprintf(stream,
	        "Anything after the script path is passed to the script and\n");
	fprintf(stream, "read back with args().\n");
	fprintf(stream, "\n");
	fprintf(stream, "Options:\n");
	fprintf(stream, "\n");
	fprintf(stream, "  -h, --help              show this help and exit\n");
	fprintf(stream,
	        "  -v, --version           show the version and exit\n");
	fprintf(stream, "  -e <code>               execute code and exit\n");
	fprintf(stream, "  -                       read a script from stdin\n");
	fprintf(stream, "\n");
	fprintf(stream, "Diagnostics:\n");
	fprintf(stream, "\n");
	fprintf(stream, "  --error-format=human   excerpt, caret and label\n");
	fprintf(stream, "  --error-format=short   one location line\n");
	fprintf(stream, "  --error-format=json    one object per diagnostic\n");
	fprintf(stream,
	        "  --color=auto           color on a terminal. the default\n");
	fprintf(stream, "  --color=always         color even when piped\n");
	fprintf(stream, "  --color=never          never color\n");
	fprintf(stream,
	        "  --explain CODE         what a diagnostic code means\n");
	fprintf(stream,
	        "  --fix                  apply machine-applicable fixes\n");
	fprintf(stream, "\n");
	fprintf(stream,
	        "These work before or after the script path. Everything "
	        "else\n");
	fprintf(stream,
	        "after it belongs to the script. Use -- to end flint's own\n");
	fprintf(stream, "options:  flint t.fl -- --error-format is mine\n");
	fprintf(stream, "\n");
	fprintf(stream, "With no arguments, starts a repl.\n");
}

static int explain_code(const char *code)
{
	static const struct {
		const char *code;
		const char *text;
	} entries[] = {
	        {"E0001",
	                "Unexpected source character. Remove it or replace it "
	                "with a valid token."},
	        {"E0002",
	                "The exponent has no digits. Add digits after the "
	                "exponent marker."},
	        {"E0003",
	                "The string literal is unterminated. Add its closing "
	                "quote."},
	        {"E0100",
	                "The parser could not use the token at this location. "
	                "Check the surrounding expression."},
	        {"E0102",
	                "A required delimiter is missing. Add the delimiter "
	                "named by the diagnostic."},
	        {"E0202",
	                "A name is not defined in the current scope. Check its "
	                "spelling or define it before use."},
	        {"E0301",
	                "An operator received incompatible operands. Check the "
	                "operator and operand types."},
	        {"E0302", "A value did not match the required type."},
	        {"E0401",
	                "A call failed. Check that the callee and its "
	                "arguments "
	                "are valid."},
	        {"E0501",
	                "A module operation failed. Check the import path and "
	                "module state."},
	        {"E0601",
	                "An index operation failed. Check the index and "
	                "indexed "
	                "value."},
	        {"E0600",
	                "Execution failed. Read the message and source "
	                "location for the failing operation."},
	};
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		if (strcmp(code, entries[i].code) == 0) {
			printf("%s: %s\n", entries[i].code, entries[i].text);
			return 0;
		}
	}
	fprintf(stderr, "unknown diagnostic code '%s'\n", code);
	return 64;
}

int main(int argc, char *argv[])
{
	FlDiagFormat diag_format = FL_DIAG_LEGACY;
	FlColorMode diag_color = FL_COLOR_AUTO;
	int arg = 1;
	bool apply_fixes = false;

	/*
	 * Pull flint's own options out of the whole command line, wherever
	 * they appear.
	 *
	 * They used to be recognised only before the script path, which made
	 * `flint bad.fl --error-format=human` silently print the old
	 * format while `flint --error-format=human bad.fl` printed the new
	 * one. Two spellings of one command is the kind of thing that costs
	 * an hour of confusion, and it was found in about a minute.
	 *
	 * Only the long, namespaced options are moved. `-h`, `-v`, `-e` and
	 * `-` stay positional, because those are the ones a script is
	 * likely to want for itself: `flint t.fl -v` is a script asking
	 * for the argument "-v", not a request for flint's version. `--`
	 * ends flint's option scanning for the same reason, and is the
	 * escape hatch for a script that really does want
	 * `--error-format=...` as an argument.
	 *
	 * The pass rebuilds argv for the script, so the script sees only
	 * what was left after flint took its share.
	 */
	{
		char **kept = malloc(sizeof(char *) * (size_t)(argc + 1));
		if (kept == NULL) {
			fprintf(stderr, "Out of memory.\n");
			return 74;
		}
		int nkept = 0;
		kept[nkept++] = argv[0];
		bool options_done = false;

		for (int i = 1; i < argc; i++) {
			const char *a = argv[i];

			if (options_done) {
				kept[nkept++] = (char *)a;
				continue;
			}
			if (strcmp(a, "--") == 0) {
				options_done = true;
				continue;
			}
			if (strncmp(a, "--error-format=", 15) == 0) {
				const char *value = a + 15;
				if (strcmp(value, "human") == 0)
					diag_format = FL_DIAG_HUMAN;
				else if (strcmp(value, "short") == 0)
					diag_format = FL_DIAG_SHORT;
				else if (strcmp(value, "json") == 0)
					diag_format = FL_DIAG_JSON;
				else {
					fprintf(stderr,
					        "error: unknown error format "
					        "'%s'\n"
					        "  expected human, short or "
					        "json\n",
					        value);
					free(kept);
					return 64;
				}
				continue;
			}
			if (strncmp(a, "--color=", 8) == 0) {
				const char *value = a + 8;
				if (strcmp(value, "auto") == 0)
					diag_color = FL_COLOR_AUTO;
				else if (strcmp(value, "always") == 0)
					diag_color = FL_COLOR_ALWAYS;
				else if (strcmp(value, "never") == 0)
					diag_color = FL_COLOR_NEVER;
				else {
					fprintf(stderr,
					        "error: unknown color mode "
					        "'%s'\n"
					        "  expected auto, always or "
					        "never\n",
					        value);
					free(kept);
					return 64;
				}
				continue;
			}
			if (strcmp(a, "--fix") == 0) {
				apply_fixes = true;
				continue;
			}
			kept[nkept++] = (char *)a;
		}

		/* rewrite argv in place so everything below sees the
		 * reduced command line */
		for (int i = 0; i < nkept; i++)
			argv[i] = kept[i];
		argc = nkept;
		arg = 1;
		free(kept);
	}

	/* what the script will see: itself, then everything flint did not
	 * claim. args() skips the first two. */
	int script_argc = argc - arg + 1;
	char **script_argv = malloc(sizeof(char *) * (size_t)script_argc);
	if (script_argv != NULL) {
		script_argv[0] = argv[0];
		for (int i = arg; i < argc; i++)
			script_argv[i - arg + 1] = argv[i];
		sys_set_args(script_argc, script_argv);
		free(script_argv);
	} else {
		sys_set_args(0, NULL);
	}
	if (apply_fixes) {
		if (arg >= argc) {
			fprintf(stderr, "--fix requires a source file\n");
			return 64;
		}
		return fix_file(argv[arg], diag_format, diag_color);
	}

	if (arg < argc) {
		const char *flag = argv[arg];
		if (strcmp(flag, "--explain") == 0) {
			if (arg + 1 >= argc) {
				fprintf(stderr,
				        "--explain requires a diagnostic "
				        "code\n");
				return 64;
			}
			return explain_code(argv[arg + 1]);
		}

		if (strcmp(flag, "-h") == 0 || strcmp(flag, "--help") == 0) {
			print_usage(stdout);
			return 0;
		}
		if (strcmp(flag, "-v") == 0 || strcmp(flag, "--version") == 0) {
			printf("Flint %s\n", FLINT_VERSION);
			return 0;
		}
		if (strcmp(flag, "-e") == 0) {
			if (argc <= arg + 1) {
				fprintf(stderr, "-e requires an argument\n");
				print_usage(stderr);
				return 64;
			}
			VM vm;
			vm_init(&vm);
			vm_set_diagnostics(&vm, diag_format, diag_color);
			/* code has no file, so imports resolve against the
			 * working directory, which is the only thing they can
			 * sensibly mean */
			InterpretResult result = vm_interpret_named(
			        &vm, argv[arg + 1], "<command line>");
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
			vm_set_diagnostics(&vm, diag_format, diag_color);
			char *source = read_stdin();
			InterpretResult result =
			        vm_interpret_named(&vm, source, "<stdin>");
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
	vm_set_diagnostics(&vm, diag_format, diag_color);

	if (arg == argc) {
		repl(&vm);
	} else {
		/* one path plus any number of arguments. the arguments belong
		 * to the script, not to flint, so they are not parsed here:
		 * `flint x.fl -v` runs x.fl with "-v" as an argument rather
		 * than printing the version and running nothing. that is what
		 * a script author expects, and it is what args() is for. */
		run_file(&vm, argv[arg]);
	}

	vm_free(&vm);
	sys_free_args();
	return 0;
}
