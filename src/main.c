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
#include "value.h"
#include "vm.h"
#include "compiler.h"
#include "diagnostic.h"
#include "object.h"
#include "debug.h"
#include "profile.h"
#include "verify.h"

#include <stdint.h>
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
/*
 * The welcome. Short, and it says what to type next.
 *
 * A repl that opens to a bare cursor tells a first-time user nothing and
 * gives an experienced one nothing either. This is the same bargain python
 * makes, minus the colour, and the reason to bother is that the most
 * common way a new person meets a language is by typing at a prompt.
 */
static void print_banner(VM *vm)
{
	if (vm->quiet)
		return;
	printf("flint %s\n", FLINT_VERSION);
	printf("a small scripting language. type an expression and press "
	       "enter.\n");
	printf(":help for what works here, ctrl-d to leave.\n\n");
}

/*
 * Is this input a complete program, or the first half of one?
 *
 * Depth over the three bracket kinds, tracked through the text with strings
 * and comments skipped. Double-quoted strings can contain escaped quotes, so
 * `\"` does not end one. A `#` starts a comment that runs to the newline.
 * Single quotes are not strings in Flint -- see the scanner -- so they do
 * not open or close anything here either.
 *
 * Returns true when every opener is closed and no string is unterminated.
 * An unterminated string is incomplete rather than wrong: the user may just
 * be typing the second half of it on the next line.
 */
static bool repl_complete(const char *text)
{
	int depth = 0;
	bool in_string = false;
	for (const char *p = text; *p != '\0'; p++) {
		if (in_string) {
			if (*p == '\\' && p[1] != '\0')
				p++;
			else if (*p == '"')
				in_string = false;
			continue;
		}
		if (*p == '"') {
			in_string = true;
		} else if (*p == '#') {
			while (*p != '\0' && *p != '\n')
				p++;
		} else if (*p == '{' || *p == '(' || *p == '[') {
			depth++;
		} else if (*p == '}' || *p == ')' || *p == ']') {
			depth--;
			/* more closers than openers is a syntax error, not an
			 * incomplete program. report it now rather than waiting
			 * for more input that will not fix it. */
			if (depth < 0)
				return true;
		}
	}
	return depth == 0 && !in_string;
}

/* the repl's own commands, kept short because they are not the language */
static void repl_help(void)
{
	printf("\n");
	printf("  :help      this text\n");
	printf("  :quit      leave, same as ctrl-d\n");
	printf("\n");
	printf("  each line is its own script. state carries over through\n");
	printf("  globals, so `x = 1` then `x + 1` works. a block that is\n");
	printf("  not closed yet is a syntax error; there is no multi-line\n");
	printf("  input, which is a real limitation and not a design "
	       "choice.\n");
	printf("\n");
	printf("  try:\n");
	printf("    1 + 2\n");
	printf("    let xs = [1, 2, 3]\n");
	printf("    for x in xs { print(x * 2) }\n");
	printf("    split(\"a,b,c\", \",\")\n");
	printf("\n");
}

/*
 * The repl. Returns the worst result any line produced, so that a session
 * which failed somewhere still exits non-zero. A script that fails exits 70;
 * a repl that swallowed that would report success for a session that clearly
 * did not, and a shell has no other way to know.
 */
static int repl(VM *vm)
{
	int worst = 0;
	char line[1024];
	print_banner(vm);
	for (;;) {
		/*
		 * Accumulate until the input is complete.
		 *
		 * A single physical line is often half a statement -- an open
		 * brace, an unclosed paren, a function with no body yet. The
		 * check walks the text tracking depth over { } ( ) [ ], and
		 * skips over string literals and comments, because a brace
		 * inside "{" is not an open block and a brace after # is not
		 * one either. Counting braces blindly would wait for input the
		 * user already finished, on any string containing one.
		 *
		 * Incomplete input prompts with `... ` and keeps reading.
		 * Complete input executes. An actual syntax error stays an
		 * error on the first line -- it is not incomplete, it is wrong.
		 */
		char buf[8192];
		size_t used = 0;
		bool done = false;
		bool first = true;
		bool eof = false;
		while (!done) {
			if (!vm->quiet)
				printf(first ? "> " : "... ");
			fflush(stdout);
			if (used + sizeof(line) > sizeof(buf)) {
				printf("input too long.\n");
				break;
			}
			if (!fgets(line, sizeof(line), stdin)) {
				if (!vm->quiet)
					printf("\n");
				if (used == 0) {
					/* EOF with nothing accumulated: the session
					 * is over. The flag is checked below,
					 * because `break` here only leaves
					 * this inner loop. */
					eof = true;
					break;
				}
				/* EOF mid-block: run what there is, which will
				 * report the unclosed construct properly. The
				 * break alone leaves the inner loop with
				 * `used > 0`, which is what runs it; setting
				 * `done` here would never be read. */
				break;
			}
			size_t n = strlen(line);
			memcpy(buf + used, line, n);
			used += n;
			buf[used] = '\0';
			if (repl_complete(buf))
				done = true;
			first = false;
		}
		/*
		 * Leave the session, rather than looping back to a prompt that
		 * can never be answered. Without this the loop reads EOF, finds
		 * nothing accumulated, breaks out of the *inner* accumulation
		 * loop, and comes straight back for more input that does not
		 * exist.
		 */
		if (eof)
			break;
		if (used == 0)
			continue;
		memmove(line, buf, used + 1);

		/* a leading colon is a repl command, not flint, so it
		 * cannot collide with a variable or a keyword */
		const char *typed = line;
		while (*typed == ' ' || *typed == '\t')
			typed++;
		if (*typed == ':') {
			if (strcmp(typed, ":help\n") == 0 ||
			        strcmp(typed, ":help\r\n") == 0)
				repl_help();
			else if (strncmp(typed, ":q", 2) == 0)
				break;
			else
				printf("unknown command. try :help\n");
			continue;
		}

		/*
		 * Echo mode, and it is on for the repl only. In a script
		 * `1 + 2` is an expression statement whose value is
		 * thrown away, which is right for a script. Here the
		 * person typing is asking for the answer, so the value is
		 * left on the stack and printed below.
		 */
		compiler_repl_echo(true);
		vm->repl_leaves_value = true;
		InterpretResult r = vm_interpret(vm, line);
		vm->repl_leaves_value = false;
		compiler_repl_echo(false);

		/* a line that failed leaves the session running, but it is
		 * still a failure, and a shell has no other way to know */
		if (r != INTERPRET_OK)
			worst = 70;

		if (r == INTERPRET_OK && compiler_repl_value()) {
			Value v;
			if (vm_pop_value(vm, &v))
				vm_print_value(v);
		}
	}
	return worst;
}

/*
 * What to do with a script, beyond running it. One struct so that adding a mode
 * is one field rather than another parameter threaded through every call site,
 * and so that main() can build it once and hand the same value everywhere.
 */
typedef struct {
	bool check; /* compile and verify, do not run */
	bool dump_bytecode; /* print the bytecode before running */
	bool trace; /* print every instruction as it executes */
	bool profile; /* print counters after the run */
} FlRunMode;

/*
 * Compile a source text and either dump it or run it.
 *
 * The three modes share a setup path on purpose. --check, --dump-bytecode and
 * running are the same pipeline up to the point where bytecode stops being an
 * internal representation, and three separate copies of "make a VM, set the
 * diagnostics, set the flags" is three places for them to disagree about
 * whether verification runs.
 *
 * Returns a sysexits code.
 */
static int compile_and_maybe_run(
        VM *vm, const char *source, const char *name, const FlRunMode *mode)
{
	bool check_only = mode->check;
	bool dump_bytecode = mode->dump_bytecode;
	bool profile = mode->profile;
	ObjFunction *function = compile_named(vm, source, name);
	if (function == NULL)
		return 65;

	/* the verifier runs before the dump, so a dump of malformed bytecode
	 * is reported rather than printed. */
	FlVerifyError error;
	if (!fl_verify_function(function, &error)) {
		fprintf(stderr,
		        "internal error: malformed bytecode in %s at byte %d "
		        "(%s)\n",
		        function->name != NULL ? function->name->chars : name,
		        error.offset,
		        error.message != NULL ? error.message : "unknown");
		return 65;
	}

	if (dump_bytecode) {
		chunk_disassemble(&function->chunk,
		        function->name != NULL ? function->name->chars : name);
		/* a dump of one function's chunk is not a program: the nested
		 * functions are separate chunks with their own code. */
		for (int i = 0; i < function->chunk.constants.count; i++) {
			Value constant = function->chunk.constants.values[i];
			if (IS_FUNCTION(constant)) {
				ObjFunction *nested = AS_FUNCTION(constant);
				printf("\n; %s\n",
				        nested->name != NULL
				                ? nested->name->chars
						: "<anonymous>");
				chunk_disassemble(&nested->chunk,
				        nested->name != NULL
				                ? nested->name->chars
						: "<anonymous>");
			}
		}
	}

	if (check_only)
		return 0;

	uint64_t started = profile ? fl_now_ns() : 0;
	InterpretResult result =
	        vm_interpret_function(vm, function, source, name);
	if (profile) {
		uint64_t elapsed = fl_now_ns() - started;
		fprintf(stderr, "\n");
		fl_profile_report(stderr, &vm->counters);
		fprintf(stderr,
		        "\nwall clock                  %8.2f ms\n",
		        (double)elapsed / 1e6);
	}

	if (result == INTERPRET_COMPILE_ERROR)
		return 65;
	if (result == INTERPRET_RUNTIME_ERROR)
		return 70;
	return 0;
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
static int run_file(VM *vm, const char *path, const FlRunMode *mode)
{
	/*
	 * Set the source directory so `import "lib/x.fl"` inside this file
	 * resolves next to it, not relative to wherever the user is standing.
	 * this is what makes a script runnable from any directory. see
	 * sys_resolve_module().
	 */
	sys_set_source_dir_for_file(path);

	char *source = read_file(path);
	int code = compile_and_maybe_run(vm, source, path, mode);
	free(source);
	return code;
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
	fprintf(stream, "Output:\n");
	fprintf(stream, "\n");
	fprintf(stream, "  --quiet                no repl prompt\n");
	fprintf(stream,
	        "  --warnings=default     errors, and warnings once there "
	        "are any\n");
	fprintf(stream, "  --warnings=none       errors only\n");
	fprintf(stream,
	        "  --warnings=all         everything the compiler can "
	        "produce\n");
	fprintf(stream, "\n");
	fprintf(stream,
	        "these work before or after the script path. Everything "
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
	                "unexpected source character. Remove it or replace it "
	                "with a valid token."},
	        {"E0002",
	                "the exponent has no digits. Add digits after the "
	                "exponent marker."},
	        {"E0003",
	                "the string literal is unterminated. Add its closing "
	                "quote."},
	        {"E0100",
	                "the parser could not use the token at this location. "
	                "check the surrounding expression."},
	        {"E0102",
	                "A required delimiter is missing. Add the delimiter "
	                "named by the diagnostic."},
	        {"E0202",
	                "A name is not defined in the current scope. Check its "
	                "spelling or define it before use."},
	        {"E0301",
	                "an operator received incompatible operands. Check the "
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
	                "an index operation failed. Check the index and "
	                "indexed "
	                "value."},
	        {"E0600",
	                "execution failed. Read the message and source "
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
	bool quiet = false;
	bool dump_bytecode = false;
	bool check_only = false;
	bool trace = false;
	bool profile = false;
	FlWarnMode warnings = FL_WARN_DEFAULT;

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
			if (strcmp(a, "--quiet") == 0) {
				quiet = true;
				continue;
			}
			if (strcmp(a, "--dump-bytecode") == 0) {
				dump_bytecode = true;
				continue;
			}
			if (strcmp(a, "--check") == 0) {
				check_only = true;
				continue;
			}
			if (strcmp(a, "--trace") == 0) {
				trace = true;
				continue;
			}
			if (strcmp(a, "--profile") == 0) {
				profile = true;
				continue;
			}
			if (strncmp(a, "--warnings=", 11) == 0) {
				const char *value = a + 11;
				if (strcmp(value, "default") == 0)
					warnings = FL_WARN_DEFAULT;
				else if (strcmp(value, "none") == 0)
					warnings = FL_WARN_NONE;
				else if (strcmp(value, "all") == 0)
					warnings = FL_WARN_ALL;
				else {
					fprintf(stderr,
					        "error: unknown warning mode "
					        "'%s'\n"
					        "  expected default, none or "
					        "all\n",
					        value);
					free(kept);
					return 64;
				}
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

	FlRunMode mode = {
	        .check = check_only,
	        .dump_bytecode = dump_bytecode,
	        .trace = trace,
	        .profile = profile,
	};

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
			vm.warnings = warnings;
			vm.quiet = quiet;
			/* code has no file, so imports resolve against the
			 * working directory, which is the only thing they can
			 * sensibly mean */
			int code = compile_and_maybe_run(
			        &vm, argv[arg + 1], "<command line>", &mode);
			vm_free(&vm);
			return code;
		}
		if (strcmp(flag, "-") == 0) {
			/* the script comes from stdin. imports inside it
			 * resolve against the working directory, because
			 * stdin has no path of its own to be relative
			 * to. */
			VM vm;
			vm_init(&vm);
			vm_set_diagnostics(&vm, diag_format, diag_color);
			vm.warnings = warnings;
			vm.quiet = quiet;
			char *source = read_stdin();
			int code = compile_and_maybe_run(
			        &vm, source, "<stdin>", &mode);
			free(source);
			vm_free(&vm);
			return code;
		}
	}

	VM vm;
	vm_init(&vm);
	vm_set_diagnostics(&vm, diag_format, diag_color);
	vm.warnings = warnings;
	vm.quiet = quiet;

	int worst = 0;
	if (arg == argc) {
		worst = repl(&vm);
	} else {
		/* one path plus any number of arguments. the arguments belong
		 * to the script, not to flint, so they are not parsed here:
		 * `flint x.fl -v` runs x.fl with "-v" as an argument rather
		 * than printing the version and running nothing. that is what
		 * a script author expects, and it is what args() is for. */
		worst = run_file(&vm, argv[arg], &mode);
	}

	vm_free(&vm);
	sys_free_args();
	return worst;
}
