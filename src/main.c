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
#include "fmt.h"
#include "pkg.h"
#include "ext.h"
#include "diagnostic.h"
#include "object.h"
#include "debug.h"
#include "profile.h"
#include "verify.h"
#include "version.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
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
/* the cli's color decision, shared by the banner and the usage text.
 * in AUTO it defers to the terminal: piped output stays plain, because
 * a test comparing output must never see escape codes it did not ask for. */
static FlColorMode g_cli_color = FL_COLOR_AUTO;

static bool cli_color(FILE *out)
{
	return g_cli_color == FL_COLOR_ALWAYS ||
	       (g_cli_color == FL_COLOR_AUTO && isatty(fileno(out)));
}

static void print_banner(VM *vm)
{
	if (vm->quiet)
		return;
	if (cli_color(stdout)) {
		printf("\x1b[1;36mflint\x1b[0m \x1b[1m%s\x1b[0m\n",
		        FLINT_VERSION);
		printf("\x1b[2ma small scripting language. type an "
		       "expression and press enter.\x1b[0m\n");
		printf("\x1b[33m:help\x1b[0m for what works here, "
		       "\x1b[33mctrl-d\x1b[0m to leave.\n\n");
	} else {
		printf("flint %s\n", FLINT_VERSION);
		printf("a small scripting language. type an expression and "
		       "press enter.\n");
		printf(":help for what works here, ctrl-d to leave.\n\n");
	}
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
	printf("  :history   what you typed this session and before\n");
	printf("  :clear     clear the screen\n");
	printf("  :load <f>  run a file in this session\n");
	printf("  :quit      leave, same as ctrl-d\n");
	printf("  !N        run history entry N again\n");
	printf("\n");
	printf("  each entry is its own script. state carries over through\n");
	printf("  globals, so `x = 1` then `x + 1` works. multi-line input\n");
	printf("  keeps reading with `... ` until the entry is complete.\n");
	printf("  history lives in ~/.flint_history when HOME names a\n");
	printf("  directory it can be written to; otherwise it is this\n");
	printf("  session only, and nothing complains about it.\n");
	printf("\n");
	printf("  try:\n");
	printf("    1 + 2\n");
	printf("    let xs = [1, 2, 3]\n");
	printf("    for x in xs { print(x * 2) }\n");
	printf("    split(\"a,b,c\", \",\")\n");
	printf("\n");
}

/*
 * Repl history. fgets gives no line editing, so this is not readline: it
 * is a record. Every executed entry is appended to ~/.flint_history when
 * that file can be written, and `:history` lists entries while `!N` runs
 * one again. Both degrade silently -- a repl that cannot write history is
 * still a repl, and saying so on every line would be worse than silence.
 */
#define REPL_HISTORY_MAX 500

typedef struct {
	char *entries[REPL_HISTORY_MAX];
	int count;
} ReplHistory;

static void repl_history_path(char *out, size_t size)
{
	const char *home = getenv("HOME");
	if (home == NULL || home[0] == '\0') {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%s/.flint_history", home);
}

static void repl_history_add(ReplHistory *history, const char *entry)
{
	if (history->count == REPL_HISTORY_MAX)
		return;
	size_t n = strlen(entry);
	char *copy = malloc(n + 1);
	if (copy == NULL)
		return;
	memcpy(copy, entry, n + 1);
	history->entries[history->count++] = copy;
}

static void repl_history_load(ReplHistory *history)
{
	char path[1024];
	repl_history_path(path, sizeof(path));
	if (path[0] == '\0')
		return;
	FILE *file = fopen(path, "r");
	if (file == NULL)
		return;
	char line[8192];
	while (history->count < REPL_HISTORY_MAX &&
	        fgets(line, sizeof(line), file) != NULL) {
		size_t n = strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';
		if (n > 0)
			repl_history_add(history, line);
	}
	fclose(file);
}

static void repl_history_save(const char *entry)
{
	char path[1024];
	repl_history_path(path, sizeof(path));
	if (path[0] == '\0')
		return;
	FILE *file = fopen(path, "a");
	if (file == NULL)
		return;
	fputs(entry, file);
	if (entry[strlen(entry)] != '\n')
		fputc('\n', file);
	fclose(file);
}

static void repl_history_list(const ReplHistory *history)
{
	int from = history->count > 20 ? history->count - 20 : 0;
	for (int i = from; i < history->count; i++)
		printf("  %d  %s\n", i + 1, history->entries[i]);
}

static void repl_history_free(ReplHistory *history)
{
	for (int i = 0; i < history->count; i++)
		free(history->entries[i]);
	history->count = 0;
}

/*
 * The repl. Returns the worst result any line produced, so that a session
 * which failed somewhere still exits non-zero. A script that fails exits 70;
 * a repl that swallowed that would report success for a session that clearly
 * did not, and a shell has no other way to know.
 */
static char *read_file_text(const char *path);
static int repl(VM *vm)
{
	int worst = 0;
	char line[1024];
	ReplHistory history = {0};
	repl_history_load(&history);
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
			else if (strncmp(typed, ":history", 8) == 0)
				repl_history_list(&history);
			else if (strncmp(typed, ":q", 2) == 0) {
				break;
			} else if (strncmp(typed, ":clear", 6) == 0 &&
			           (typed[6] == '\n' || typed[6] == '\r' ||
			                   typed[6] == ' ' ||
			                   typed[6] == '\0')) {
				/* scrollback clear, only where a
				 * terminal would see it. piped
				 * sessions get control bytes they
				 * cannot use, so they get
				 * silence. */
				if (isatty(STDOUT_FILENO))
					printf("\033[2J\033[H");
				fflush(stdout);
			} else if (strncmp(typed, ":load", 5) == 0 &&
			           (typed[5] == ' ' || typed[5] == '\t')) {
				const char *path = typed + 5;
				while (*path == ' ' || *path == '\t')
					path++;
				size_t n = strlen(path);
				while (n > 0 && (path[n - 1] == '\n' ||
				                        path[n - 1] == '\r' ||
				                        path[n - 1] == ' '))
					n--;
				char name[4096];
				if (n >= sizeof(name)) {
					printf("path too long\n");
					continue;
				}
				memcpy(name, path, n);
				name[n] = '\0';
				char *src = read_file_text(name);
				if (src == NULL)
					printf("cannot read '%s'\n", name);
				else {
					const char *saved = sys_source_dir();
					char *saved_copy =
					        malloc(strlen(saved) + 1);
					if (saved_copy != NULL)
						memcpy(saved_copy,
						        saved,
						        strlen(saved) + 1);
					sys_set_source_dir_for_file(name);
					InterpretResult r =
					        vm_interpret(vm, src);
					if (saved_copy != NULL) {
						sys_set_source_dir(saved_copy);
						free(saved_copy);
					}
					free(src);
					if (r == INTERPRET_RUNTIME_ERROR)
						worst = 70;
					else if (r == INTERPRET_COMPILE_ERROR)
						worst = 65;
				}
			} else
				printf("unknown command. try :help\n");
			continue;
		}

		/* `!N` runs history entry N again. the entry is echoed, so
		 * what runs is visible in the session, and it is not
		 * recorded twice: the original is the history. */
		if (*typed == '!') {
			long n = strtol(typed + 1, NULL, 10);
			if (n < 1 || n > history.count) {
				printf("no history entry %s", typed + 1);
				continue;
			}
			const char *entry = history.entries[n - 1];
			printf("%s\n", entry);
			memmove(line, entry, strlen(entry) + 1);
			goto run_line;
		}

		/* a recall is already history; only new input is recorded. */
		repl_history_add(&history, line);
		repl_history_save(line);
run_line:;

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
	repl_history_free(&history);
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
	bool stats; /* print static program stats, then run */
} FlRunMode;

/* static stats over a compiled function and everything nested in it.
 * --profile asks what the run did; --stats asks what the program is:
 * how many functions, how many bytes of bytecode, how many constants.
 * to stderr, so stdout stays the program's. */
typedef struct {
	int functions;
	int bytes;
	int constants;
} FlStats;

static void fl_stats_walk(ObjFunction *function, FlStats *stats)
{
	stats->functions++;
	stats->bytes += function->chunk.count;
	stats->constants += function->chunk.constants.count;
	for (int i = 0; i < function->chunk.constants.count; i++) {
		Value constant = function->chunk.constants.values[i];
		if (IS_FUNCTION(constant))
			fl_stats_walk(AS_FUNCTION(constant), stats);
	}
}

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
	bool stats = mode->stats;
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

	if (stats) {
		FlStats st = {0, 0, 0};
		fl_stats_walk(function, &st);
		fprintf(stderr, "functions: %d\n", st.functions);
		fprintf(stderr, "bytecode: %d bytes\n", st.bytes);
		fprintf(stderr, "constants: %d\n", st.constants);
	}

	if (check_only)
		return 0;

	uint64_t started = profile ? fl_now_ns() : 0;
	/* the wall clock is reported from the counters, so it lands in the
	 * same report as everything else instead of being printed beside it */
	if (profile)
		vm->counters.wall_ns = started;
	InterpretResult result =
	        vm_interpret_function(vm, function, source, name);
	if (profile) {
		vm->counters.wall_ns = fl_now_ns() - started;
		fprintf(stderr, "\n");
		fl_profile_report(stderr, &vm->counters);
	}

	if (result == INTERPRET_COMPILE_ERROR)
		return 65;
	if (result == INTERPRET_RUNTIME_ERROR)
		return 70;
	return 0;
}

/* slurp a whole file. the caller frees the result. */
/* a read_file for the repl: no exit on failure, the session lives on. */
static char *read_file_text(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	if (fseek(file, 0L, SEEK_END) != 0) {
		fclose(file);
		return NULL;
	}
	long length = ftell(file);
	if (length < 0) {
		fclose(file);
		return NULL;
	}
	if ((unsigned long)length >= (unsigned long)-1) {
		fclose(file);
		return NULL;
	}
	size_t size = (size_t)length;
	if (fseek(file, 0L, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	char *text = malloc(size + 1);
	if (text == NULL) {
		fclose(file);
		return NULL;
	}
	size_t got = fread(text, 1, size, file);
	fclose(file);
	if (got != size) {
		free(text);
		return NULL;
	}
	/*
	 * In bounds: the allocation above is `size + 1`. The analyzer
	 * cannot tie `size` back to it across the ftell/fseek sequence,
	 * which is the same complaint it makes about the identical
	 * read_file() below, where the same annotation is already in
	 * place.
	 */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	text[size] = '\0';
	return text;
}

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

static void cli_section(FILE *stream, const char *title)
{
	if (cli_color(stream))
		fprintf(stream, "\x1b[1;35m%s\x1b[0m\n", title);
	else
		fprintf(stream, "%s\n", title);
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
	cli_section(stream, "Options:");
	fprintf(stream, "\n");
	fprintf(stream, "  -h, --help              show this help and exit\n");
	fprintf(stream,
	        "  -v, --version           show the version and exit\n");
	fprintf(stream, "  -e <code>               execute code and exit\n");
	fprintf(stream, "  -                       read a script from stdin\n");
	fprintf(stream,
	        "  sync                    update the installed standard "
	        "library\n");
	fprintf(stream,
	        "  test                    run every *_test.fl under tests/\n");
	fprintf(stream,
	        "  fmt [--check] FILES     canonical layout for sources\n");
	fprintf(stream, "  pkg install|add|update|list\n");
	fprintf(stream,
	        "                         dependencies in flint.toml\n");
	fprintf(stream,
	        "  test --filter P         only tests whose name has P\n");
	fprintf(stream, "\n");
	cli_section(stream, "Diagnostics:");
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
	cli_section(stream, "Inspection:");
	fprintf(stream, "\n");
	fprintf(stream,
	        "  --check                compile and verify, do not run\n");
	fprintf(stream,
	        "  --dump-bytecode        print the bytecode before running\n");
	fprintf(stream,
	        "  --profile              print runtime counters after "
	        "the run\n");
	fprintf(stream,
	        "  --stats                print functions, bytecode bytes "
	        "and\n");
	fprintf(stream,
	        "                         constants, then run as normal\n");
	fprintf(stream, "  --trace                print every instruction\n");
	fprintf(stream, "\n");
	cli_section(stream, "Output:");
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
	        {"E0201",
	                "a name is declared twice in one scope. Remove one of "
	                "the declarations, or rename one of them."},
	        {"E0202",
	                "A name is not defined in the current scope. Check its "
	                "spelling or define it before use."},
	        {"E0203",
	                "an import is never read. Remove it, or use the name "
	                "it "
	                "binds. Aliasing to _ opts out when the import is only "
	                "wanted for its side effect."},
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

/*
 * `flint sync`
 *
 * Downloads the standard library from the project's github repository and
 * installs it where `make install` puts it. The point is that a stdlib is
 * data, not code: `make install` copies whatever happened to be in lib/ at
 * build time, and a user who installed a binary a week ago has no way to get
 * the library that came with this week's fix. Sync is that way.
 *
 * Four rules, in order of how much they matter:
 *
 *   1. Nothing is installed until it has been compiled. A download goes to a
 *      temporary name, is compiled in place, and is only then renamed over
 *      the old file. A truncated transfer or a 404 page cannot break a
 *      working install, which is the whole reason the step exists.
 *   2. An identical file is left alone. Sync is run on a whim, often, and a
 *      rewrite that changes nothing but the mtime is how people learn to
 *      distrust a command.
 *   3. The downloader is not flint. https needs TLS, TLS needs a dependency,
 *      and this language's whole pitch is that it has none. curl(1) or
 *      wget(1) already do it, keep their certificates current, and are two
 *      processes that have been doing this for twenty years.
 *   4. The module list is compiled in, on purpose. A library that needs a
 *      new builtin is unusable to an older binary, so installing one would
 *      leave a half-upgraded install that fails at import time instead of
 *      failing here, where the message can be honest. Bump flint itself.
 */

/* the standard library, as shipped. keep in step with the lib directory. */
static const char *const sync_modules[] = {
        "collections",
        "fs",
        "json",
        "math",
        "os",
        "path",
        "process",
        "random",
        "time",
};

/* where the sources live. a mirror works: --url or FLINT_STDLIB_URL. */
#define SYNC_DEFAULT_URL                                                       \
	"https://raw.githubusercontent.com/programmersd21/flint"
#define SYNC_DEFAULT_REF "main"

static void print_sync_usage(FILE *stream)
{
	fprintf(stream, "Usage: flint sync [options]\n");
	fprintf(stream, "\n");
	fprintf(stream,
	        "Download the standard library and install it into the\n");
	fprintf(stream, "directory `make install` uses.\n");
	fprintf(stream, "\n");
	cli_section(stream, "Options:");
	fprintf(stream, "\n");
	fprintf(stream,
	        "  --dry-run        print what would be fetched, write "
	        "nothing\n");
	fprintf(stream,
	        "  --ref=<ref>      git ref to take. default: %s\n",
	        SYNC_DEFAULT_REF);
	fprintf(stream,
	        "  --url=<base>     base URL. default: %s\n",
	        SYNC_DEFAULT_URL);
	fprintf(stream, "  -h, --help       show this help and exit\n");
	fprintf(stream, "\n");
	fprintf(stream, "Environment:\n");
	fprintf(stream, "\n");
	fprintf(stream,
	        "  FLINT_STDLIB      install here instead of "
	        "$HOME/.flint/stdlib\n");
	fprintf(stream, "  FLINT_STDLIB_URL  default for --url\n");
	fprintf(stream, "  FLINT_STDLIB_REF  default for --ref\n");
	fprintf(stream, "\n");
	fprintf(stream,
	        "Each file is compiled before it is installed, and one that\n");
	fprintf(stream,
	        "does not compile is left out. Exit status is 0 when every\n");
	fprintf(stream, "module installed, 69 when any failed, 73 when the\n");
	fprintf(stream, "directory could not be created.\n");
}

/*
 * Two files, byte for byte, without reading either into memory whole. A
 * stdlib module is a few kilobytes, but this is a general helper and a
 * general helper that mallocs a file size is a general helper that can fail
 * on a file size.
 */
static bool files_are_identical(const char *a, const char *b)
{
	FILE *fa = fopen(a, "rb");
	if (fa == NULL)
		return false;
	FILE *fb = fopen(b, "rb");
	if (fb == NULL) {
		fclose(fa);
		return false;
	}

	/* fgetc rather than block reads: the byte-wise compare has no
	 * indeterminate buffer states, which a fread-based loop always
	 * seems to trip the analyzer on. the files are a few kilobytes
	 * each, so the cost is theoretical. */
	int ca = 0;
	int cb = 0;
	bool same = true;
	for (;;) {
		ca = fgetc(fa);
		cb = fgetc(fb);
		if (ca == EOF && cb == EOF)
			break;
		if (ca == EOF || cb == EOF || ca != cb) {
			same = false;
			break;
		}
	}
	fclose(fa);
	fclose(fb);
	return same;
}

/*
 * flint test -- the built-in test runner.
 *
 * Discovery is deliberately narrow: files named *_test.fl under tests/,
 * sorted, each run in its own VM. A test file that fails reports the
 * failure and the runner moves on, so one broken test does not hide the
 * rest; the exit code is non-zero if any failed.
 *
 * There is no assertion library. A test file uses assert(), which is a
 * normal builtin and raises a normal runtime error, so a failing test needs
 * no special machinery to report itself and the runner treats a non-zero
 * exit exactly as it treats any other script.
 */
static int compare_names(const void *a, const void *b)
{
	/* qsort hands back pointers to the array's elements, which are
	 * themselves char *: each is one dereference, not two */
	const char *const *x = a;
	const char *const *y = b;
	return strcmp(*x, *y);
}

/* collect *_test.fl under dir into out, growing *count. */
static void collect_tests(
        const char *dir, const char *sub, char ***out, int *count)
{
	char path[1024];
	if (sub != NULL)
		snprintf(path, sizeof(path), "%s/%s", dir, sub);
	else
		snprintf(path, sizeof(path), "%s", dir);

	DIR *d = opendir(path);
	if (d == NULL)
		return;
	struct dirent *entry;
	while ((entry = readdir(d)) != NULL) {
		size_t n = strlen(entry->d_name);
		/* "_test.fl" is eight characters: an off-by-one here finds
		 * nothing at all and reports an empty suite */
		if (n < 8 || strcmp(entry->d_name + n - 8, "_test.fl") != 0)
			continue;
		char child[1024];
		int written = snprintf(
		        child, sizeof(child), "%s/%s", path, entry->d_name);
		if (written < 0 || (size_t)written >= sizeof(child))
			continue;
		struct stat st;
		if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
			/* walk it and keep going: a sibling test file in the
			 * parent must not be skipped because a subdirectory
			 * happened to be read first */
			collect_tests(dir, child, out, count);
			continue;
		}
		/* grow by exactly one each time: the index and the number of
		 * entries are the same number, and letting them drift is how
		 * a collected path gets overwritten before it is read */
		char **grown =
		        realloc(*out, sizeof(char *) * (size_t)(*count + 1));
		if (grown == NULL)
			break;
		*out = grown;
		(*out)[*count] = strdup(child);
		(*count)++;
	}
	closedir(d);
}

static int run_test_suite(const char *filter)
{
	char **tests = NULL;
	int cap = 0;

	collect_tests("tests", NULL, &tests, &cap);
	if (cap == 0) {
		fprintf(stderr, "no tests found under tests/\n");
		fprintf(stderr, "a test file is named *_test.fl\n");
		return 65;
	}
	qsort(tests, (size_t)cap, sizeof(char *), compare_names);

	int ran = 0;
	int failed = 0;
	for (int i = 0; i < cap; i++) {
		const char *path = tests[i];
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		if (filter != NULL && strstr(base, filter) == NULL)
			continue;

		/* one VM per test, so a test that defines a global or
		 * imports a module cannot leak into the next one */
		VM vm;
		vm_init(&vm);
		vm_set_diagnostics(&vm, FL_DIAG_SHORT, FL_COLOR_NEVER);
		vm.quiet = true;

		char *source = read_file(path);
		if (source == NULL) {
			fprintf(stderr, "FAIL %s: cannot read\n", base);
			failed++;
			vm_free(&vm);
			continue;
		}
		/* compile_and_maybe_run dereferences mode, so a test run
		 * needs a real one rather than a null to mean "defaults" */
		FlRunMode test_mode = {0};
		int rc = compile_and_maybe_run(&vm, source, path, &test_mode);
		free(source);
		vm_free(&vm);

		ran++;
		if (rc != 0) {
			fprintf(stderr, "FAIL %s\n", base);
			failed++;
		} else {
			printf("ok   %s\n", base);
			fflush(stdout);
		}
		free(tests[i]);
	}
	free(tests);

	if (failed == 0) {
		printf("\n%d test file(s), all passed\n", ran);
		return 0;
	}
	printf("\n%d test file(s), %d failed\n", ran, failed);
	return 70;
}

static int sync_stdlib(int argc, char **argv, int first)
{
	const char *url_base = getenv("FLINT_STDLIB_URL");
	const char *ref = getenv("FLINT_STDLIB_REF");
	bool dry_run = false;

	if (url_base == NULL || url_base[0] == '\0')
		url_base = SYNC_DEFAULT_URL;
	if (ref == NULL || ref[0] == '\0')
		ref = SYNC_DEFAULT_REF;

	/* options are flint's own here, so they are read here rather than in
	 * the pass in main(): that pass runs before it knows what the command
	 * is, and a script's arguments must stay the script's. */
	for (int i = first; i < argc; i++) {
		const char *opt = argv[i];
		if (strcmp(opt, "--dry-run") == 0) {
			dry_run = true;
		} else if (strcmp(opt, "-h") == 0 ||
		           strcmp(opt, "--help") == 0) {
			print_sync_usage(stdout);
			return 0;
		} else if (strncmp(opt, "--ref=", 6) == 0) {
			ref = opt + 6;
		} else if (strncmp(opt, "--url=", 6) == 0) {
			url_base = opt + 6;
		} else {
			fprintf(stderr,
			        "flint sync: unknown option '%s'\n"
			        "  try 'flint sync --help'\n",
			        opt);
			return 64;
		}
	}
	if (ref[0] == '\0' || url_base[0] == '\0') {
		fprintf(stderr,
		        "flint sync: --ref and --url both need a value\n");
		return 64;
	}

	const char *dir = sys_stdlib_install_dir();
	if (dir == NULL) {
		fprintf(stderr,
		        "flint sync: cannot tell where to install. Set "
		        "FLINT_STDLIB\n  to the directory, or set HOME.\n");
		return 73;
	}

	if (!dry_run && !sys_make_dirs(dir)) {
		fprintf(stderr,
		        "flint sync: cannot create '%s': %s\n",
		        dir,
		        strerror(errno));
		return 73;
	}

	printf("syncing the standard library from %s at %s\ninto %s\n",
	        url_base,
	        ref,
	        dir);
	size_t count = sizeof(sync_modules) / sizeof(sync_modules[0]);
	if (dry_run)
		printf("%zu modules, nothing will be written\n", count);

	int updated = 0;
	int unchanged = 0;
	int failed = 0;

	/* one VM for the verification compiles. it is created here rather
	 * than per file because each compile leaves globals behind and the
	 * collector is not what this code should be testing. */
	VM vm;
	vm_init(&vm);
	vm_set_diagnostics(&vm, FL_DIAG_SHORT, FL_COLOR_NEVER);
	FlRunMode check = {.check = true};

	for (size_t i = 0; i < count; i++) {
		const char *name = sync_modules[i];

		/* the URL is built into a buffer rather than assembled by
		 * hand each time, and every length is checked, because a
		 * --url that is 4000 characters long must be an error and
		 * not a truncated request for the wrong file. */
		size_t namelen = strlen(name);
		size_t baselen = strlen(url_base);
		size_t reflen = strlen(ref);
		if (baselen + reflen + namelen + 12 > 1024) {
			fprintf(stderr,
			        "flint sync: the url is too long for '%s'\n",
			        name);
			failed++;
			continue;
		}
		char url[1024];
		snprintf(url,
		        sizeof(url),
		        "%s/%s/lib/%s.fl",
		        url_base,
		        ref,
		        name);

		if (namelen + 16 > 1024) {
			fprintf(stderr,
			        "flint sync: the name of '%s' is too long\n",
			        name);
			failed++;
			continue;
		}
		char final_path[1024];
		char temp_path[1024];
		snprintf(final_path, sizeof(final_path), "%s/%s.fl", dir, name);
		/* the temporary name is dotted, so an interrupted sync leaves
		 * something that `ls` hides and `import` never sees */
		snprintf(temp_path,
		        sizeof(temp_path),
		        "%s/.%s.fl.new",
		        dir,
		        name);

		if (dry_run) {
			printf("would fetch %s -> %s\n", url, final_path);
			continue;
		}

		if (sys_fetch_url(url, temp_path) != 0) {
			fprintf(stderr,
			        "flint sync: %s: could not download %s\n",
			        name,
			        url);
			(void)unlink(temp_path);
			failed++;
			continue;
		}

		/*
		 * Compile before install. `flint --check` is exactly this
		 * check, which is why it is the same code path rather than a
		 * second parser nobody keeps in step.
		 */
		if (run_file(&vm, temp_path, &check) != 0) {
			fprintf(stderr,
			        "flint sync: %s: downloaded file does not "
			        "compile.\n  left the installed copy alone; "
			        "this is usually\n  a stdlib newer than this "
			        "flint. Try --ref=v0.7.0.\n",
			        name);
			(void)unlink(temp_path);
			failed++;
			continue;
		}

		if (files_are_identical(temp_path, final_path)) {
			printf("  %-12s unchanged\n", name);
			(void)unlink(temp_path);
			unchanged++;
			continue;
		}

		if (rename(temp_path, final_path) != 0) {
			fprintf(stderr,
			        "flint sync: %s: cannot install: %s\n",
			        name,
			        strerror(errno));
			(void)unlink(temp_path);
			failed++;
			continue;
		}
		printf("  %-12s updated\n", name);
		updated++;
	}

	vm_free(&vm);

	if (dry_run) {
		printf("dry run: nothing was written\n");
		return 0;
	}

	printf("%d updated, %d unchanged, %d failed\n",
	        updated,
	        unchanged,
	        failed);
	if (failed > 0) {
		fprintf(stderr,
		        "the installed library may now be a mixture of "
		        "versions.\nrun it again when the network is "
		        "behaving.\n");
		return 69;
	}
	return 0;
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
	bool stats = false;
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
			if (strcmp(a, "--stats") == 0) {
				stats = true;
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

	g_cli_color = diag_color;
	FlRunMode mode = {
	        .check = check_only,
	        .dump_bytecode = dump_bytecode,
	        .trace = trace,
	        .profile = profile,
	        .stats = stats,
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
			if (arg + 1 < argc &&
			        (strcmp(argv[arg + 1], "--verbose") == 0 ||
			                strcmp(argv[arg + 1], "-v") == 0)) {
				printf("  language version: 0.12.0\n");
				printf("  runtime version: 0.12.0\n");
				printf("  package format version: 0.12.0\n");
				printf("  bytecode version: 0.12.0\n");
				printf("  native ABI version: 1\n");
				printf("  lockfile version: 0.12.0\n");
			}
			return 0;
		}
		/*
		 * `flint sync`. Only when no such file exists, because a
		 * script is allowed to be called anything at all, and a
		 * subcommand must not take a name away from a script the user
		 * already has. The rule is "an existing file wins", so
		 * `flint sync` runs ./sync where such a file exists, and
		 * installs the library where it does not.
		 */
		if (strcmp(flag, "sync") == 0 && access(flag, F_OK) != 0) {
			return sync_stdlib(argc, argv, arg + 1);
		}
		/*
		 * `flint test`. Same rule as sync: only when no such file
		 * exists, so a script named `test` still runs.
		 */
		/*
		 * `flint fmt`. Same rule as sync and test: only when no
		 * such file exists, so a script named `fmt` still runs.
		 */
		if (strcmp(flag, "fmt") == 0 && access(flag, F_OK) != 0) {
			/* --check is flint's own flag, so the option scan
			 * already ate it wherever it appeared and set
			 * check_only. for fmt that *is* the check mode:
			 * `flint fmt --check f` lists without rewriting. */
			return flint_fmt(argc, argv, arg + 1, check_only);
		}
		/*
		 * `flint pkg`. Same rule as sync, test and fmt: only when
		 * no such file exists, so a script named `pkg` still runs.
		 */
		if (strcmp(flag, "pkg") == 0 && access(flag, F_OK) != 0) {
			return flint_pkg(argc, argv, arg + 1);
		}
		/*
		 * `flint native <lib> <module>` -- load a compiled module and
		 * make its functions callable for this run. A test and a
		 * development tool rather than something a script depends on:
		 * the loader keeps the library for the life of the process,
		 * so nothing here claims unloading.
		 */
		if (strcmp(flag, "native") == 0 && access(flag, F_OK) != 0) {
			if (arg + 2 >= argc) {
				fprintf(stderr,
				        "flint native: needs a library path "
				        "and a "
				        "module name\n");
				return 64;
			}
			VM vm;
			vm_init(&vm);
			vm.quiet = quiet;
			char error[512] = {0};
			if (!fl_ext_load_native(&vm,
			            argv[arg + 1],
			            argv[arg + 2],
			            error,
			            sizeof(error))) {
				fprintf(stderr, "flint native: %s\n", error);
				vm_free(&vm);
				return 65;
			}
			int code = 0;
			if (arg + 3 < argc)
				code = run_file(&vm, argv[arg + 3], &mode);
			vm_free(&vm);
			return code;
		}
		if (strcmp(flag, "test") == 0 && access(flag, F_OK) != 0) {
			const char *filter = NULL;
			for (int i = arg + 1; i < argc; i++) {
				if (strcmp(argv[i], "--filter") == 0) {
					if (i + 1 >= argc) {
						fprintf(stderr,
						        "--filter requires a "
						        "pattern\n");
						return 64;
					}
					filter = argv[i + 1];
					i++;
				} else {
					fprintf(stderr,
					        "flint test: unknown option "
					        "'%s'\n",
					        argv[i]);
					return 64;
				}
			}
			return run_test_suite(filter);
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
