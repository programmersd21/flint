/* SPDX-License-Identifier: MIT */
/*
 * The interpreter: one loop, one frame pointer, a switch.
 *
 * `frame` is cached in a local and reloaded after anything that can change
 * the current frame: a call, a return, or a native that reenters. Missing
 * one of those reloads is the classic bug in this design, so there are only
 * three places to get it right.
 */
#include "vm.h"
#include "chunk.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "diagnostic.h"
#include "profile.h"
#include "sys.h"
/* the disassembler is called from run(), and only under
 * FL_DEBUG_TRACE_EXECUTION. Same reasoning as the compiler: an include that
 * nothing references in a release build is noise the analyser has to be told
 * to ignore, and an #ifdef says it more honestly than a pragma does. */
#ifdef FL_DEBUG_TRACE_EXECUTION
#	include "debug.h"
#endif
#include "memory.h"
#include "native.h"
#include "object.h"
#include "stdint.h"
#include "table.h"
#include "value.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/*
 * Push. Anything that allocates must root the value here first, because the
 * allocation can collect and an unrooted object does not survive it.
 */
void vm_push(VM *vm, Value value)
{
	*vm->stack_top = value;
	vm->stack_top++;
}

Value vm_pop(VM *vm)
{
	vm->stack_top--;
	return *vm->stack_top;
}

/* 0 is the top of the stack. */
static Value peek(VM *vm, int distance) { return vm->stack_top[-1 - distance]; }

static void close_upvalues(VM *vm, Value *last);

/*
 * Discard everything this script added: the frames above base_frame, and the
 * value stack back down to base_top.
 *
 * Both, and neither is derivable from the other. The frame depth is what the
 * dispatch loop checks to know it is finished. The stack top is what the
 * caller of a nested interpret needs back so its own bookkeeping works.
 *
 * Unwinding to 0 instead of to the base is catastrophic when the failing
 * script is a module. `import` runs a second script inside the first, and
 * unwinding past the importer's frame leaves its run() holding a pointer to a
 * frame that no longer exists. The next dispatch reads it and the process
 * dies somewhere unrelated.
 *
 * Unwinding to the surviving frame's own `slots` is wrong in the other
 * direction. Those slots are where the callee sits, and an `import` has its
 * callee and path argument pushed above them, still owned by the caller,
 * which is about to subtract them from the stack top itself. Throwing them
 * away here first makes that subtraction run off the bottom of the array.
 * base_top is the exact position captured on entry, so it cannot be off by
 * the caller's operand count.
 */
static void unwind_to(VM *vm, int base_frame, Value *base_top)
{
	/*
	 * Close only the upvalues pointing into the slots being discarded.
	 * Closing the whole list would move every captured variable onto the
	 * heap, including the ones the surviving frames still hold, and a
	 * module failure would quietly detach the closures of the script that
	 * imported it.
	 *
	 * Order matters: the top has to be known before closing, because
	 * closing is what reads the slots.
	 */
	close_upvalues(vm, base_top);

	vm->stack_top = base_top;
	vm->frame_count = base_frame;
}

/*
 * Report and unwind. The trace is printed innermost first, and the line for
 * each frame is the line of the instruction *before* ip, because ip already
 * points past the opcode that failed.
 */
static int name_distance(const char *a, size_t alen, const char *b, size_t blen)
{
	if (alen > 31 || blen > 31 || alen > blen + 2 || blen > alen + 2)
		return 3;
	int prev[32];
	int next[32];
	for (size_t j = 0; j <= blen; j++)
		prev[j] = (int)j;
	for (size_t i = 1; i <= alen; i++) {
		next[0] = (int)i;
		int row_min = next[0];
		for (size_t j = 1; j <= blen; j++) {
			int cost = a[i - 1] == b[j - 1] ? 0 : 1;
			int del = prev[j] + 1;
			int ins = next[j - 1] + 1;
			int sub = prev[j - 1] + cost;
			int best = del < ins ? del : ins;
			next[j] = best < sub ? best : sub;
			if (next[j] < row_min)
				row_min = next[j];
		}
		if (row_min > 2)
			return 3;
		memcpy(prev, next, (blen + 1) * sizeof(int));
	}
	return prev[blen];
}

/*
 * Names a script might have meant that are not values.
 *
 * `print` is a keyword, not a global, so it is not in vm->globals and a
 * search over globals alone can never suggest it. `pritn("x")` is the single
 * most common typo in any language and it was reported with no help at all,
 * because the one name the user wanted was in a different table.
 *
 * Keywords and the builtins are candidates for the same reason, and the
 * search over both is bounded by construction: this list is fixed and the
 * globals table is whatever the program defined.
 */
static const char *const language_names[] = {
        "print",
        "len",
        "push",
        "pop",
        "str",
        "type",
        "input",
        "clock",
        "split",
        "join",
        "trim",
        "contains",
        "starts_with",
        "ends_with",
        "replace",
        "lower",
        "upper",
        "args",
        "env",
        "exit",
        "read_file",
        "write_file",
        "exec",
        "let",
        "const",
        "if",
        "else",
        "while",
        "for",
        "fn",
        "return",
        "break",
        "continue",
        "import",
        "export",
        "in",
        "and",
        "or",
        "not",
        "nil",
        "true",
        "false",
};

/*
 * The closest name to the needle, or NULL if nothing is close enough.
 *
 * Returns a static string from language_names, or a key borrowed from
 * vm->globals. Both outlive the call: the static one trivially, the globals
 * one because the table outlives the diagnostic.
 */
/*
 * The name of a module file that exists for this undefined name, or NULL.
 *
 * When a script reads a name it never defined, and a module file with the
 * matching shape exists, forgetting the import line is the likely story --
 * likelier than a typo, which is why the caller checks this before the fuzzy
 * match. Returns the name itself (borrowed from the caller) rather than a
 * copy, because the caller only needs it for one snprintf.
 */
static const char *unimported_module(const char *name, size_t length)
{
	if (length == 0 || length > 255)
		return NULL;
	char candidate[256];
	memcpy(candidate, name, length);
	candidate[length] = '\0';
	if (sys_module_file_exists(candidate))
		return name;
	return NULL;
}

static const char *similar_language_name(VM *vm, ObjString *needle)
{
	const char *best = NULL;
	int best_distance = 3;
	bool tied = false;

	for (size_t i = 0;
	        i < sizeof(language_names) / sizeof(language_names[0]);
	        i++) {
		const char *name = language_names[i];
		int distance = name_distance(needle->chars,
		        (size_t)needle->length,
		        name,
		        strlen(name));
		if (distance < best_distance) {
			best = name;
			best_distance = distance;
			tied = false;
		} else if (distance == best_distance) {
			tied = true;
		}
	}

	for (int i = 0; i < vm->globals->capacity; i++) {
		ObjString *key = vm->globals->entries[i].key;
		if (key == NULL)
			continue;
		if (best != NULL && strcmp(best, key->chars) == 0)
			continue; /* already the best, from the list above */
		int distance = name_distance(needle->chars,
		        (size_t)needle->length,
		        key->chars,
		        (size_t)key->length);
		if (distance < best_distance) {
			best = key->chars;
			best_distance = distance;
			tied = false;
		} else if (distance == best_distance) {
			tied = true;
		}
	}

	/* a tie means two candidates are equally close, and suggesting
	 * either is a coin flip. say nothing instead. */
	/*
	 * A name can be in both tables -- every builtin is a global *and* is
	 * listed above -- so the second pass can re-find the current best at
	 * the same distance and tie with itself. Skip anything already the
	 * best; a genuine tie between two *different* names is still a coin
	 * flip and correctly says nothing.
	 */
	return tied || best == NULL ? NULL : best;
}

static const char *undefined_name(const char *message)
{
	static const char prefix[] = "undefined variable '";
	if (strncmp(message, prefix, sizeof(prefix) - 1) != 0)
		return NULL;
	const char *start = message + sizeof(prefix) - 1;
	const char *end = strchr(start, '\'');
	if (end == NULL || end[1] != '.' || end[2] != '\0')
		return NULL;
	char *name = malloc((size_t)(end - start) + 1);
	if (name == NULL)
		return NULL;
	memcpy(name, start, (size_t)(end - start));
	name[end - start] = '\0';
	return name;
}

void vm_runtime_error(VM *vm, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	va_list count_args;
	/*
	 * va_copy, because a va_list may be walked once.
	 *
	 * clang-analyzer-valist reports 'uninitialized value' on the
	 * vsnprintf below and has for years. The sequence is the one C99
	 * prescribes for using the arguments twice: va_start, then va_copy
	 * into a second list, use both, va_end both. gcc -Wformat=2 agrees
	 * that this is well formed.
	 *
	 * The annotation is here because the alternative -- restructuring to
	 * avoid the copy -- means either a fixed-size buffer with a truncation
	 * that can produce a wrong diagnostic, or formatting twice and
	 * hoping the two agree. Neither is better than one annotated line.
	 */
	/* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) */
	va_copy(count_args, args);
	/* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) */
	int length = vsnprintf(NULL, 0, format, count_args);
	va_end(count_args);
	char fallback[2048];
	char *message = fallback;
	if (length >= 0) {
		message = malloc((size_t)length + 1);
		if (message == NULL)
			message = fallback;
	}
	vsnprintf(message,
	        message == fallback ? sizeof(fallback) : (size_t)length + 1,
	        format,
	        args);
	va_end(args);
	if (vm->diag_format == FL_DIAG_LEGACY) {
		fprintf(stderr, "%s\n", message);
	} else {
		FlSource source;
		FlSource *source_ptr = NULL;
		FlSpan span = {0, 0};
		bool has_span = false;
		const char *code = "E0600";
		const char *primary_label = "runtime error";
		const char *missing_name = undefined_name(message);
		ObjString *missing =
		        missing_name != NULL
		                ? copy_string(vm,
		                          missing_name,
		                          (int)strlen(missing_name))
		                : NULL;
		const char *help[1];
		size_t help_count = 0;
		char help_text[128];
		if (missing != NULL) {
			code = "E0202";
			primary_label = "undefined name";
		} else if (strncmp(message,
		                   "operands must be",
		                   strlen("operands must be")) == 0) {
			code = "E0301";
			primary_label = "invalid operands";
		} else if (strncmp(message,
		                   "expected type '",
		                   strlen("expected type '")) == 0) {
			code = "E0302";
			primary_label = "type assertion failed";
		} else if (strstr(message, "index") != NULL) {
			code = "E0601";
			primary_label = "invalid index";
		} else if (strncmp(message, "expected ", 9) == 0) {
			code = "E0401";
			primary_label = "call failed";
		} else if (strstr(message, "module") != NULL ||
		           strstr(message, "import cycle") != NULL) {
			code = "E0501";
			primary_label = "module operation failed";
		}
		if (vm->source_text != NULL) {
			fl_source_init(
			        &source, vm->source_name, vm->source_text);
			source_ptr = &source;
			if (source.line_count != 0 &&
			        vm->frame_count > vm->base_frame) {
				CallFrame *frame =
				        &vm->frames[vm->frame_count - 1];
				ObjFunction *fn = frame->closure->function;
				size_t ip = (size_t)(frame->ip -
				                     fn->chunk.code - 1);
				size_t line = (size_t)fn->chunk.lines[ip];
				if (line > 0 && line <= source.line_count) {
					/*
					 * Point at the instruction, not the
					 * line. Each code byte records the
					 * source offset it came from, and
					 * the end of an instruction is the
					 * start offset of the next, so the
					 * span of the failing expression
					 * falls out of two array reads.
					 *
					 * Underlining the whole line was
					 * what made `print(xs[10])` show a
					 * caret across the entire call.
					 */
					if (fn->chunk.spans != NULL &&
					        ip < (size_t)fn->chunk.count) {
						span.start =
						        fn->chunk.spans[ip];
						span.end =
						        ip + 1 < (size_t)fn->chunk
						                                .count
						                ? fn->chunk.spans
						                          [ip + 1]
						                : span.start;
					} else {
						span.start =
						        (uint32_t)source
						                .line_starts[line -
						                             1];
					}
					if (span.end <= span.start) {
						/* a zero-width span: fall back
						 * to the rest of the line, which
						 * is what it was before, and is
						 * better than pointing at
						 * nothing */
						span.start =
						        (uint32_t)source
						                .line_starts[line -
						                             1];
					}
					if (span.end == span.start) {
						size_t end = span.start;
						while (end < source.length &&
						        source.text[end] !=
						                '\n')
							end++;
						span.end = (uint32_t)end;
					}
					/* never run past the end of the
					 * source, whatever the tables say */
					if (span.end > source.length)
						span.end =
						        (uint32_t)source.length;
					has_span = true;
					if (missing != NULL) {
						size_t name_len =
						        (size_t)missing->length;
						size_t at = span.start;
						while (at + name_len <=
						                span.end &&
						        memcmp(source.text + at,
						                missing->chars,
						                name_len) != 0)
							at++;
						if (at + name_len <= span.end) {
							span.start =
							        (uint32_t)at;
							span.end =
							        (uint32_t)(at +
							                   name_len);
						}
						/*
						 * A missing name that matches a
						 * standard library module is almost
						 * certainly a forgotten import, not a
						 * typo. Check this before the fuzzy
						 * match below: an exact hit on a real
						 * file beats a guess, and showing both
						 * ("did you mean X? did you forget
						 * to import Y?") is noise.
						 */
						const char *unimported =
						        unimported_module(
						                missing->chars,
						                name_len);
						if (unimported != NULL) {
							snprintf(help_text,
							        sizeof(help_text),
							        "did you "
							        "forget to "
							        "`import %s`?",
							        unimported);
							help[0] = help_text;
							help_count = 1;
						} else {
							const char *similar =
							        similar_language_name(
							                vm,
							                missing);
							if (similar != NULL) {
								snprintf(
								        help_text,
								        sizeof(help_text),
								        "did "
								        "you "
								        "mean "
								        "`%s`?",
								        similar);
								help[0] =
								        help_text;
								help_count = 1;
							}
						}
					}
				}
			}
		}
		FlDiagnostic diag = {
		        .severity = FL_DIAG_ERROR,
		        .code = code,
		        .message = message,
		        .primary = span,
		        .has_primary = has_span,
		        .primary_label = primary_label,
		        .help = help,
		        .help_count = help_count,
		};
		fl_diag_emit(stderr,
		        &diag,
		        source_ptr,
		        vm->diag_format,
		        vm->diag_color);
		if (source_ptr != NULL)
			fl_source_free(&source);
		free((void *)missing_name);
	}
	if (message != fallback)
		free(message);

	/*
	 * Only the frames this script owns. The importing script's frames
	 * are below base_frame and are not this error's business; printing
	 * them would attribute a module's failure to a line in the importer
	 * that ran long before it.
	 */
	for (int i = vm->frame_count - 1;
	        i >= vm->base_frame &&
	        (vm->diag_format == FL_DIAG_LEGACY ||
	                vm->diag_format == FL_DIAG_HUMAN);
	        i--) {
		CallFrame *frame = &vm->frames[i];
		ObjFunction *function = frame->closure->function;
		size_t instruction = frame->ip - function->chunk.code - 1;
		int line = function->chunk.lines[instruction];

		fprintf(stderr, "[line %d] in ", line);
		if (function->name == NULL)
			fprintf(stderr, "script\n");
		else
			fprintf(stderr, "%s()\n", function->name->chars);
	}

	/*
	 * Unwind to where this script started, and no further. The importing
	 * script's run() is still on the C stack and will resume as soon as
	 * this returns, holding a pointer to its own frame and expecting its
	 * stack exactly as it was.
	 */
	unwind_to(vm, vm->base_frame, vm->base_top);
}

/*
 * Turn a script value into a validated index into something `count` long.
 *
 * The naive version is `int i = (int)AS_NUMBER(v)`, and it is wrong in three
 * separate ways. A fractional index truncates, so x[1.5] silently reads
 * element 1 and there is no way for a script to notice. A value beyond the
 * range of int is undefined behaviour on the cast, and on x86-64 the value
 * that comes out is a plausible-looking negative number that then indexes
 * backwards. And `count + idx` for a negative idx can overflow on its own.
 *
 * So: check finiteness, check integrality with floor rather than with a
 * cast, and bound by `count` rather than by INT_MAX before converting. The
 * bound is what makes the negative branch safe, because |d| <= count means
 * count + idx cannot overflow, and count is an int to begin with.
 *
 * `what` is "list" or "string" and only appears in the message.
 */
bool vm_value_to_index(
        VM *vm, Value value, int count, int *out, const char *what)
{
	if (!IS_NUMBER(value)) {
		vm_runtime_error(vm, "%s index must be a number.", what);
		return false;
	}

	double d = AS_NUMBER(value);

	/* NaN and inf fail every comparison below, and converting either is
	 * undefined, so they have to go before the cast. */
	if (isnan(d) || isinf(d)) {
		vm_runtime_error(vm, "%s index must be a finite number.", what);
		return false;
	}

	/*
	 * floor, not a cast. This test is what rejects x[1.5]; a cast would
	 * answer the question we are trying to ask.
	 */
	if (d != floor(d)) {
		vm_runtime_error(vm, "%s index must be a whole number.", what);
		return false;
	}

	/* the bound, before the conversion. d is now known finite and
	 * integral, so the comparisons are exact. */
	if (d >= (double)count || d < -(double)count) {
		vm_runtime_error(vm,
		        "%s index %g out of bounds (len %d).",
		        what,
		        d,
		        count);
		return false;
	}

	/* |d| <= count <= INT_MAX, so this cannot overflow */
	int idx = (int)d;

	/* negative counts from the end. idx >= -count here, so the sum is
	 * within int as well. */
	if (idx < 0)
		idx = count + idx;

	*out = idx;
	return true;
}

/*
 * Register a native as a global. Both halves are pushed before the
 * table_set() because the name string and the native are each unrooted
 * until they are on the stack.
 */
void vm_define_native(VM *vm, const char *name, NativeFn function, int arity)
{
	vm_push(vm, STR_VAL(copy_string(vm, name, (int)strlen(name))));
	vm_push(vm, OBJ_VAL(new_native(vm, function, arity)));
	table_set(vm, vm->globals, AS_STRING(vm->stack[0]), vm->stack[1]);
	vm_pop(vm);
	vm_pop(vm);
}

void vm_init(VM *vm)
{
	/*
	 * Assigned directly rather than through reset_stack().
	 *
	 * reset_stack() unwinds, and unwinding closes open upvalues, which
	 * means reading vm->open_upvalues. At this point the VM is whatever
	 * was on the C stack, so that read walks a garbage pointer. It is not
	 * a subtle failure: it segfaults before main() gets a chance to run.
	 *
	 * There is also nothing to unwind yet. An empty VM has no frames and
	 * no upvalues, so writing the three fields is the whole job, and
	 * writing them is the one operation that cannot depend on the values
	 * already being sane.
	 */
	vm->stack_top = vm->stack;
	vm->frame_count = 0;
	vm->open_upvalues = NULL;
	vm->base_frame = 0;
	vm->source_text = NULL;
	vm->source_name = "<source>";
	vm->warnings = FL_WARN_DEFAULT;
	vm->repl_leaves_value = false;
	vm->diag_format = FL_DIAG_LEGACY;
	vm->diag_color = FL_COLOR_AUTO;
	vm->verify = true;

	vm->objects = NULL;
	vm->bytes_allocated = 0;
	/*
	 * The counters start at zero, which is the one time they are memset
	 * rather than assigned individually. vm_init has to clear the whole
	 * struct anyway -- it is inside the caller's frame, so it is whatever
	 * was there before -- and listing forty fields here to assign them one
	 * at a time is a list that has to be edited every time a counter is
	 * added.
	 */
	memset(&vm->counters, 0, sizeof(vm->counters));
	/*
	 * first collection after a megabyte, so startup does not collect.
	 * see FL_GC_FIRST_THRESHOLD in config.h for why this is a floor and
	 * not just a multiple of a live set that does not exist yet.
	 */
	vm->next_gc = FL_GC_FIRST_THRESHOLD;
	vm->gray_count = 0;
	vm->gray_capacity = 0;
	vm->gray_stack = NULL;

	/*
	 * globals points at the first module's table. The array is the
	 * storage; the pointer is the cursor into it, and a module push is a
	 * pointer increment. That is why the VM needs no other change to give
	 * every module its own environment: the bytecode says "global", and
	 * "global" means "whatever table this module is running in".
	 */
	vm->globals_count = 1;
	vm->globals_used = 1;
	vm->globals_capacity = 8;
	vm->globals_envs = ALLOCATE(vm, Table *, (size_t)vm->globals_capacity);
	/* NULL the whole array first. vm_free and the collector both walk to
	 * capacity and skip NULL slots, and an uninitialised slot is neither
	 * NULL nor a Table, so vm_free handed a pointer to table_free and ASan
	 * caught it the first time a program imported more than two modules. */
	for (int i = 0; i < vm->globals_capacity; i++)
		vm->globals_envs[i] = NULL;
	vm->globals_envs[0] = ALLOCATE(vm, Table, 1);
	table_init(vm->globals_envs[0]);
	vm->globals = vm->globals_envs[0];
	table_init(&vm->strings);
	table_init(&vm->modules);
	compiler_set_diagnostics(NULL, vm->diag_format, vm->diag_color);

	register_natives(vm);
}

void vm_set_diagnostics(VM *vm, FlDiagFormat format, FlColorMode color)
{
	vm->diag_format = format;
	vm->diag_color = color;
	compiler_set_diagnostics(NULL, format, color);
}

bool vm_pop_value(VM *vm, Value *out)
{
	if (vm->stack_top <= vm->stack)
		return false;
	vm->stack_top--;
	*out = *vm->stack_top;
	return true;
}

void vm_free(VM *vm)
{
	/*
	 * Objects first, then the tables.
	 *
	 * The reverse order looks harmless and is not. free_objects() calls
	 * free_object(), which calls fl_reallocate(), which can trigger a
	 * collection. That collection calls mark_roots(), which reads
	 * vm->globals. If the globals table has already been freed, the mark
	 * phase walks freed memory and the collector either faults or, worse,
	 * follows a pointer out of it.
	 *
	 * The general rule: freeing anything while a collector can still run
	 * requires the roots to still be readable, so the reachable structure
	 * has to outlive the objects that point into it. The strings table
	 * holds weak references to objects, which is why it has to go after
	 * the sweep and not before it -- the same ordering requirement,
	 * pointing the other way.
	 */
	free_objects(vm);
	/*
	 * Every module table, not just the current one. A finished module's
	 * bindings are still reachable through whatever imported it, and the
	 * collector needs to see all of them.
	 */
	/*
	 * Over capacity, not count.
	 *
	 * `globals_count` is the *nesting depth*: it goes up as an import
	 * starts and down as it finishes, so after the last import it is back
	 * to 1 no matter how many modules were loaded. The environments
	 * themselves are never released -- a closure may still point at one --
	 * so every slot ever used has to be freed, which means the whole
	 * allocated array. Walking only up to `count` freed the last one and
	 * leaked every other, which LeakSanitizer reported as a 48-byte leak
	 * for a three-module test.
	 */
	for (int i = 0; i < vm->globals_capacity; i++) {
		if (vm->globals_envs[i] == NULL)
			continue;
		table_free(vm, vm->globals_envs[i]);
		FREE(vm, Table, vm->globals_envs[i]);
	}
	FREE_ARRAY(vm, Table *, vm->globals_envs, (size_t)vm->globals_capacity);
	table_free(vm, &vm->strings);
	table_free(vm, &vm->modules);
}

/*
 * print() formatting.
 *
 * The only interesting part is the number case. Flint numbers are doubles,
 * but "print(4)" showing 4.000000 is not acceptable, so integral values are
 * printed as integers. Beyond that, try increasing precision until the text
 * round-trips: 15 digits covers most values, 17 covers all of them. Using 17
 * unconditionally would give every number a tail of noise digits.
 */
static void print_flint_value(Value value)
{
	if (IS_NUMBER(value)) {
		double d = AS_NUMBER(value);
		if (isnan(d)) {
			printf("nan\n");
			return;
		}
		if (isinf(d)) {
			if (d < 0)
				printf("-inf\n");
			else
				printf("inf\n");
			return;
		}
		/* exactly representable as an integer, and in range as a long.
		 * the range test has to happen inside the helper, before the
		 * cast: see the note on fl_double_is_printable_int. */
		if (fl_double_is_printable_int(d)) {
			/* the digit loop, not printf("%ld"). small integers
			 * are the common case and a libc format call is
			 * ~250ns for something this is twenty nanoseconds of. */
			char small[24];
			int n = fl_itoa(
			        fl_double_to_long(d), small, sizeof(small));
			if (n > 0) {
				printf("%s\n", small);
				return;
			}
			printf("%ld\n", fl_double_to_long(d));
			return;
		}
		char buf[64];
		double check;
		snprintf(buf, sizeof(buf), "%.15g", d);
		check = strtod(buf, NULL);
		if (check == d) {
			printf("%s\n", buf);
			return;
		}
		snprintf(buf, sizeof(buf), "%.16g", d);
		check = strtod(buf, NULL);
		if (check == d) {
			printf("%s\n", buf);
			return;
		}
		/* 17 significant digits is always enough for a double */
		snprintf(buf, sizeof(buf), "%.17g", d);
		printf("%s\n", buf);
	} else if (IS_BOOL(value)) {
		printf("%s\n", AS_BOOL(value) ? "true" : "false");
	} else if (IS_NIL(value)) {
		printf("nil\n");
	} else if (IS_OBJ(value)) {
		print_object(value);
		printf("\n");
	}
}

/* formats any value the way print() does, plus a newline. the repl needs it,
 * and duplicating the number formatting would be a second copy of the rule that
 * fixes a floating-to-integer cast. */
void vm_print_value(Value value) { print_flint_value(value); }

/*
 * Push a frame for a flint function. The callee and its arguments are already
 * on the stack, so slots points at the callee and argument 0 is slots[1].
 *
 * Returns false after reporting an error, so callers can just propagate.
 */
static bool call(VM *vm, ObjClosure *closure, int arg_count)
{
	if (arg_count != closure->function->arity) {
		vm_runtime_error(vm,
		        "expected %d arguments but got %d.",
		        closure->function->arity,
		        arg_count);
		return false;
	}

	if (vm->frame_count == FRAMES_MAX) {
		vm_runtime_error(vm, "stack overflow.");
		return false;
	}

	CallFrame *frame = &vm->frames[vm->frame_count++];
	frame->closure = closure;
	frame->ip = closure->function->chunk.code;
	frame->slots = vm->stack_top - arg_count - 1;
	return true;
}

/*
 * Call whatever is on the stack, argc slots down.
 *
 * A native runs right here instead of getting a frame: it is C code, it
 * cannot yield, and a frame would buy nothing. Its arguments are passed as a
 * pointer into the stack, and it is responsible for its own return value.
 * Note that this is the one path that can reenter run() through
 * vm_interpret(), which is how import works.
 */
static bool call_value(VM *vm, Value callee, int arg_count)
{
	vm->counters.calls++;
	if (IS_OBJ(callee)) {
		switch (OBJ_TYPE(callee)) {
		case OBJ_CLOSURE: {
			ObjClosure *closure = AS_CLOSURE(callee);
			/*
			 * Hotness lives on the function, not the closure. Two
			 * closures over one function are the same code and would
			 * be compiled twice, which is the same waste as compiling
			 * a loop twice.
			 */
			closure->function->call_count++;
			return call(vm, closure, arg_count);
		}
		case OBJ_NATIVE: {
			ObjNative *native = AS_NATIVE(callee);
			vm->counters.primitives++;
			/* arity -1 is variadic and skips the check */
			if (native->arity != -1 && arg_count != native->arity) {
				vm_runtime_error(vm,
				        "expected %d arguments but got %d.",
				        native->arity,
				        arg_count);
				return false;
			}
			Value result = native->function(
			        vm, arg_count, vm->stack_top - arg_count);
			/* drop callee and args, then leave the result */
			vm->stack_top -= arg_count + 1;
			vm_push(vm, result);
			return true;
		}
		default:
			break;
		}
	}
	vm_runtime_error(vm, "can only call functions.");
	return false;
}

/*
 * Capture a local into an upvalue, reusing an existing one if the same slot
 * is captured twice. Two closures sharing a variable must share the upvalue,
 * or writes through one would be invisible to the other.
 *
 * The open list is kept sorted by descending stack address, which lets
 * close_upvalues() stop as soon as it passes the last slot that died.
 */
static ObjUpvalue *capture_upvalue(VM *vm, Value *local)
{
	ObjUpvalue *prev = NULL;
	ObjUpvalue *upvalue = vm->open_upvalues;

	while (upvalue != NULL && upvalue->location > local) {
		prev = upvalue;
		upvalue = upvalue->next;
	}

	if (upvalue != NULL && upvalue->location == local)
		return upvalue;

	ObjUpvalue *created = new_upvalue(vm, local);
	created->next = upvalue;

	if (prev == NULL)
		vm->open_upvalues = created;
	else
		prev->next = created;

	return created;
}

/*
 * Move every open upvalue at or above `last` into its own storage. Called
 * when a frame or a block goes away, because those stack slots are about to
 * be reused by another call. Without this an escaped closure would read
 * whatever the next function put there.
 */
static void close_upvalues(VM *vm, Value *last)
{
	while (vm->open_upvalues != NULL &&
	        vm->open_upvalues->location >= last) {
		ObjUpvalue *upvalue = vm->open_upvalues;
		upvalue->closed = *upvalue->location;
		upvalue->location = &upvalue->closed;
		vm->open_upvalues = upvalue->next;
	}
}

/* b .. a -> "ab". the result is interned, so a repeated concat is cheap. */
/* returns false if the result would be too long to build; the error is
 * already reported in that case */
static bool concatenate(VM *vm)
{
	ObjString *b = AS_STRING(peek(vm, 0));
	ObjString *a = AS_STRING(peek(vm, 1));

	/*
	 * The addition is checked because it can be done twice over.
	 *
	 * `s = (s + "x") + (s + "y")` doubles the length every iteration, so
	 * it is an easy way for a script to reach this from a short line of
	 * code. Without the check a->length + b->length overflows int, and
	 * the negative result is converted to a size_t for the allocator and
	 * becomes an enormous request. That is undefined behaviour in the
	 * addition itself, not just an allocation that fails.
	 *
	 * The operands are lengths of live strings, so anything near INT_MAX
	 * is already a program that cannot finish. Refusing is honest; the
	 * alternative is undefined behaviour and a confusing crash.
	 */
	if (a->length > INT_MAX - b->length) {
		vm_runtime_error(vm, "string is too long to concatenate.");
		return false;
	}

	/*
	 * The result is not interned. It used to be, on the theory that a
	 * repeated concatenation would then be free, and the theory is wrong in
	 * both halves: a repeated concatenation is a loop that builds a *new*
	 * string each time, so the repeats are not the same bytes and the
	 * interning never hits; and the cost is paid every time whether or not
	 * anything looks the result up again.
	 *
	 * Both operands are rooted across the allocation inside
	 * concat_strings(), so this pops them after the result exists rather
	 * than before it.
	 */
	ObjString *result = concat_strings(vm, a, b);
	vm_pop(vm);
	vm_pop(vm);
	vm_push(vm, STR_VAL(result));
	return true;
}

/*
 * The dispatch loop. Computed goto would be faster and less readable; a
 * switch is fine until profiling says otherwise.
 *
 * This is a parameter rather than a field, and deliberately so. `import`
 * calls vm_interpret() from inside a running script, so this function gets
 * reentered with the importing script's frames still live below. Each
 * invocation must return to the depth it started at, and a shared field
 * would be overwritten by the nested call before the outer one finished.
 * vm->base_frame exists for a different job -- telling a runtime error how
 * far down it may unwind -- and it is saved and restored for the same reason.
 *
 * Returning only at frame_count == 0 does not work here. The module's
 * OP_RETURN drops the count by one, the module's run() falls through to the
 * next instruction, and that instruction belongs to the importing script. It
 * runs a second time, with a `frame` pointer that is already stale, and then
 * the outer run() resumes and unwinds the same stack again. The crash lands
 * in whatever opcode happens to read the wrecked stack, which is why this
 * presents as a bug in print_object().
 */
static InterpretResult run(VM *vm, int base_frame)
{
	CallFrame *frame = &vm->frames[vm->frame_count - 1];

/* operand reads. each advances ip past the operand. */
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT()                                                           \
	(frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT()                                                        \
	(frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define READ_U24()                                                             \
	(frame->ip += 3,                                                       \
	        (uint32_t)((frame->ip[-3] << 16) | (frame->ip[-2] << 8) |      \
	                   frame->ip[-1]))
#define READ_CONSTANT_LONG()                                                   \
	(frame->closure->function->chunk.constants.values[READ_U24()])
#define READ_STRING_AS(long_op)                                                \
	AS_STRING(instruction == (long_op) ? READ_CONSTANT_LONG()              \
	                                   : READ_CONSTANT())

/*
 * Arithmetic and comparison on numbers, shared by six opcodes. Pops right
 * then left, so the operand order is a op b. BINARY_OP takes the Value
 * constructor as an argument, which is how one macro produces both NUMBER_VAL
 * results and BOOL_VAL results.
 */
#define BINARY_OP(value_type, op)                                              \
	do {                                                                   \
		if (!IS_NUMBER(peek(vm, 0)) || !IS_NUMBER(peek(vm, 1))) {      \
			vm_runtime_error(vm, "operands must be numbers.");     \
			return INTERPRET_RUNTIME_ERROR;                        \
		}                                                              \
		double b = AS_NUMBER(vm_pop(vm));                              \
		double a = AS_NUMBER(vm_pop(vm));                              \
		vm_push(vm, value_type(a op b));                               \
	} while (false)

	for (;;) {
#ifdef FL_DEBUG_TRACE_EXECUTION
		/*
		 * Dump the stack and the next instruction, every instruction.
		 *
		 * print_value, not print_object: a stack slot holds whatever
		 * the script last computed, and most of the time that is a
		 * number. print_object switches on OBJ_TYPE(), which reads a
		 * pointer out of the value, so the tracer used to segfault on
		 * the first integer in any script at all.
		 */
		printf("          ");
		for (Value *slot = vm->stack; slot < vm->stack_top; slot++) {
			printf("[ ");
			print_value(*slot);
			printf(" ]");
		}
		printf("\n");
		disassemble_instruction(&frame->closure->function->chunk,
		        (int)(frame->ip -
		                frame->closure->function->chunk.code));
#endif

		uint8_t instruction = READ_BYTE();
		/*
		 * No `break` inside a loop in any handler below. The
		 * computed-goto build rewrites every break in this switch
		 * into a dispatch jump, so a break meant for a `for` skips
		 * the code after the loop instead. Early exit from a loop
		 * is `goto` to a handler-local label (field_done,
		 * index_found, ...), which survives the transform because
		 * the transform only touches break.
		 */
		switch (instruction) {
		case OP_CONSTANT: {
			Value constant = READ_CONSTANT();
			vm_push(vm, constant);
			break;
		}
		case OP_CONSTANT_LONG: {
			/* 24-bit index, for chunks with more than 256 constants */
			uint32_t b0 = READ_BYTE();
			uint32_t b1 = READ_BYTE();
			uint32_t b2 = READ_BYTE();
			uint32_t idx = (b0 << 16) | (b1 << 8) | b2;
			vm_push(vm,
			        frame->closure->function->chunk.constants
			                .values[idx]);
			break;
		}
		case OP_NIL:
			vm_push(vm, NIL_VAL);
			break;
		case OP_TRUE:
			vm_push(vm, TRUE_VAL);
			break;
		case OP_FALSE:
			vm_push(vm, FALSE_VAL);
			break;
		case OP_POP:
			vm_pop(vm);
			break;

		case OP_GET_LOCAL: {
			uint8_t slot = READ_BYTE();
			vm_push(vm, frame->slots[slot]);
			break;
		}
		case OP_SET_LOCAL: {
			/* leaves the value on the stack: an assignment is an
			 * expression, and the caller may need the result */
			uint8_t slot = READ_BYTE();
			frame->slots[slot] = peek(vm, 0);
			break;
		}
		case OP_GET_GLOBAL:
		case OP_GET_GLOBAL_LONG: {
			ObjString *name = READ_STRING_AS(OP_GET_GLOBAL_LONG);
			Value value;
			/*
			 * The module's own table first, then the builtins.
			 *
			 * A module does not contain `print` or `len` -- it is
			 * not the module that owns them -- so a plain lookup
			 * would make every module unable to call anything. The
			 * fallback keeps them reachable without copying them
			 * into every module, which would let a module shadow a
			 * builtin for every other module too.
			 */
			/*
			 * Resolve against the frame's module, not whichever
			 * module is currently running.
			 *
			 * A closure carries the environment its body was written
			 * in, so calling it later -- after its module's import
			 * returned and another module has loaded -- still sees
			 * its own private names. Without this a module could
			 * not see its own `scale` the moment anything else
			 * loaded, which is the bug per-module environments exist
			 * to remove.
			 *
			 * The closure's module is NULL for the top-level script
			 * and for anything defined outside a module, and then
			 * this is the running environment, as before.
			 */
			Table *env = frame->closure->module != NULL
			                     ? frame->closure->module
			                     : vm->globals;
			if (!table_get_with_fallback(
			            env, vm->globals_envs[0], name, &value)) {
				vm_runtime_error(vm,
				        "undefined variable '%s'.",
				        name->chars);
				return INTERPRET_RUNTIME_ERROR;
			}
			vm_push(vm, value);
			break;
		}
		case OP_DEFINE_GLOBAL:
		case OP_DEFINE_GLOBAL_LONG: {
			/* let at top level. overwrites, unlike assignment. */
			ObjString *name = READ_STRING_AS(OP_DEFINE_GLOBAL_LONG);

			/*
			 * `let` on a name that is already const. Redeclaration
			 * is normally allowed and overwrites, but doing it to
			 * a const would quietly downgrade the binding: the
			 * entry keeps its const flag, so the very next write
			 * to the "new" x is rejected, which is the opposite of
			 * what the source now says. Refuse it here instead.
			 *
			 * This has to be a runtime check. The name may have
			 * been made const by a module this file imported, and
			 * imports run at runtime, so the compiler cannot know.
			 */
			Table *env = frame->closure->module != NULL
			                     ? frame->closure->module
			                     : vm->globals;
			if (table_is_const(env, name)) {
				vm_pop(vm);
				vm_runtime_error(vm,
				        "cannot redefine constant '%s'.",
				        name->chars);
				return INTERPRET_RUNTIME_ERROR;
			}

			table_set(vm, env, name, peek(vm, 0));
			vm_pop(vm);
			break;
		}
		case OP_DEFINE_GLOBAL_CONST:
		case OP_DEFINE_GLOBAL_CONST_LONG: {
			/*
			 * `const` at the top level. The flag goes on the
			 * binding rather than into the bytecode, so the name
			 * is read-only from here on no matter which file or
			 * function tries to write it next.
			 *
			 * Redefining an existing const is refused, with one
			 * exception. Importing a module re-executes its top
			 * level, so the same `const LIMIT = 100` runs twice
			 * with the same value. That is not a contradiction
			 * and failing on it would make a second import of any
			 * module that exports a const an error. Only a
			 * *different* value is refused, because then one of
			 * the two declarations is a lie about what the name
			 * holds forever.
			 *
			 * The value is on the stack across the call, so it is
			 * rooted if the table grows and collection happens.
			 */
			ObjString *name =
			        READ_STRING_AS(OP_DEFINE_GLOBAL_CONST_LONG);
			if (table_is_const(vm->globals, name)) {
				Value existing;
				table_get(vm->globals, name, &existing);
				if (values_equal(existing, peek(vm, 0))) {
					vm_pop(vm);
					break;
				}
				vm_pop(vm);
				vm_runtime_error(vm,
				        "cannot redefine constant '%s'.",
				        name->chars);
				return INTERPRET_RUNTIME_ERROR;
			}
			table_define_const(vm, vm->globals, name, peek(vm, 0));
			vm_pop(vm);
			break;
		}
		/*
		 * The `export` definitions. Identical to the plain forms except
		 * that the binding is flagged, which the module loader reads when
		 * it builds the table the importer gets.
		 *
		 * A `let` that is not marked is the module's own business and
		 * never leaves it -- that is the difference from v0.5.0, where
		 * every top-level name in every file was visible everywhere.
		 */
		case OP_DEFINE_GLOBAL_EXPORT:
		case OP_DEFINE_GLOBAL_EXPORT_LONG: {
			ObjString *name =
			        READ_STRING_AS(OP_DEFINE_GLOBAL_EXPORT_LONG);
			table_define_exported(vm,
			        frame->closure->module != NULL
			                ? frame->closure->module
					: vm->globals,
			        name,
			        peek(vm, 0),
			        false);
			vm_pop(vm);
			break;
		}
		case OP_DEFINE_GLOBAL_CONST_EXPORT:
		case OP_DEFINE_GLOBAL_CONST_EXPORT_LONG: {
			ObjString *name = READ_STRING_AS(
			        OP_DEFINE_GLOBAL_CONST_EXPORT_LONG);
			if (table_is_const(vm->globals, name)) {
				/* the same rule as an unexported const: re-running
				 * the same declaration is fine, a different value
				 * is not. importing a module re-executes its top
				 * level, so the first case has to be allowed. */
				Value existing;
				table_get(vm->globals, name, &existing);
				if (values_equal(existing, peek(vm, 0))) {
					vm_pop(vm);
					break;
				}
				vm_pop(vm);
				vm_runtime_error(vm,
				        "cannot redefine constant '%s'.",
				        name->chars);
				return INTERPRET_RUNTIME_ERROR;
			}
			table_define_exported(
			        vm, vm->globals, name, peek(vm, 0), true);
			vm_pop(vm);
			break;
		}
		case OP_SET_GLOBAL:
		case OP_SET_GLOBAL_LONG: {
			ObjString *name = READ_STRING_AS(OP_SET_GLOBAL_LONG);

			/*
			 * The const check, which is the whole reason this fix
			 * exists. A local const is caught at compile time, but
			 * a global can be written from another file, from a
			 * function the compiler never saw being defined, or
			 * through a loop. Only the table that holds the binding
			 * knows for sure, so the check has to happen here.
			 *
			 * Checked before table_set, because table_set on an
			 * unknown name would create it and the error below
			 * would report "undefined" for a name that exists.
			 */
			if (table_is_const(vm->globals, name)) {
				vm_runtime_error(vm,
				        "cannot assign to constant '%s'.",
				        name->chars);
				return INTERPRET_RUNTIME_ERROR;
			}

			if (table_set(vm, vm->globals, name, peek(vm, 0))) {
				/* assigning to something that was not declared
				 * just created it. undo that and complain. */
				table_delete(vm->globals, name);
				vm_runtime_error(vm,
				        "undefined variable '%s'.",
				        name->chars);
				return INTERPRET_RUNTIME_ERROR;
			}
			break;
		}
		case OP_GET_UPVALUE: {
			uint8_t slot = READ_BYTE();
			vm_push(vm, *frame->closure->upvalues[slot]->location);
			break;
		}
		case OP_SET_UPVALUE: {
			/* writes straight into the shared cell, which is what
			 * makes capture by reference work */
			uint8_t slot = READ_BYTE();
			*frame->closure->upvalues[slot]->location = peek(vm, 0);
			break;
		}
		case OP_EQUAL: {
			Value b = vm_pop(vm);
			Value a = vm_pop(vm);
			vm_push(vm, BOOL_VAL(values_equal(a, b)));
			break;
		}
		case OP_NOT_EQUAL: {
			Value b = vm_pop(vm);
			Value a = vm_pop(vm);
			vm_push(vm, BOOL_VAL(!values_equal(a, b)));
			break;
		}
		case OP_CAST: {
			/*
			 * `x as T`. Peek, check, and leave the value exactly
			 * where it was. This is an assertion, not a
			 * conversion, so there is nothing to pop and nothing
			 * to push: the stack is untouched on both the
			 * success and the failure path.
			 *
			 * The whole value is the error message. Without a
			 * cast, a table where a string was expected
			 * surfaces three functions later as a nil, or worse
			 * as a number that happens to be zero. With one,
			 * the failure names the expression that was wrong.
			 */
			FlType want = (FlType)READ_BYTE();
			Value value = peek(vm, 0);

			if (!value_has_type(value, want)) {
				/*
				 * Suggest the conversion when one exists.
				 *
				 * This confusion is common enough to have a
				 * shape: `as` asserts a type and never converts,
				 * so a number asserted as a string (or the reverse)
				 * fails here. Both directions have a function --
				 * str() and num() -- and naming it turns "your
				 * program is wrong" into "here is the line that
				 * makes it right". Pairs with no conversion stay
				 * silent instead of guessing.
				 */
				const char *hint = NULL;
				if (want == FL_TYPE_STRING && IS_NUMBER(value))
					hint = "use str() to convert a number "
					       "to a string.";
				else if (want == FL_TYPE_NUMBER &&
				         IS_STRING(value))
					hint = "use num() to convert a string "
					       "to a number.";
				if (hint != NULL) {
					vm_runtime_error(vm,
					        "expected type '%s' but got "
					        "'%s'. %s",
					        flint_type_name_of(want),
					        flint_type_name(value),
					        hint);
				} else {
					vm_runtime_error(vm,
					        "expected type '%s' but got "
					        "'%s'.",
					        flint_type_name_of(want),
					        flint_type_name(value));
				}
				return INTERPRET_RUNTIME_ERROR;
			}
			break;
		}
		case OP_GREATER:
			BINARY_OP(BOOL_VAL, >);
			break;
		case OP_GREATER_EQUAL:
			BINARY_OP(BOOL_VAL, >=);
			break;
		case OP_LESS:
			BINARY_OP(BOOL_VAL, <);
			break;
		case OP_LESS_EQUAL:
			BINARY_OP(BOOL_VAL, <=);
			break;

		case OP_ADD: {
			/* the one overloaded operator: number or string */
			if (IS_STRING(peek(vm, 0)) && IS_STRING(peek(vm, 1))) {
				/*
				 * concatenate reports its own failure and
				 * returns. The check here is not
				 * belt-and-braces: without it the
				 * interpreter carries on into the number
				 * branch and reports "operands must be two
				 * numbers or two strings" about two strings,
				 * which is both wrong and unhelpful.
				 */
				if (!concatenate(vm))
					return INTERPRET_RUNTIME_ERROR;
			} else if (IS_NUMBER(peek(vm, 0)) &&
			           IS_NUMBER(peek(vm, 1))) {
				double b = AS_NUMBER(vm_pop(vm));
				double a = AS_NUMBER(vm_pop(vm));
				vm_push(vm, NUMBER_VAL(a + b));
			} else {
				vm_runtime_error(vm,
				        "operands must be two numbers or two "
				        "strings.");
				return INTERPRET_RUNTIME_ERROR;
			}
			break;
		}
		case OP_SUBTRACT:
			BINARY_OP(NUMBER_VAL, -);
			break;
		case OP_MULTIPLY:
			BINARY_OP(NUMBER_VAL, *);
			break;
		case OP_DIVIDE:
			BINARY_OP(NUMBER_VAL, /);
			break;
		case OP_MODULO: {
			/* not BINARY_OP: fmod takes doubles, not a Value. */
			if (!IS_NUMBER(peek(vm, 0)) ||
			        !IS_NUMBER(peek(vm, 1))) {
				vm_runtime_error(
				        vm, "operands must be numbers.");
				return INTERPRET_RUNTIME_ERROR;
			}
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, NUMBER_VAL(fmod(a, b)));
			break;
		}
		case OP_NOT:
			vm_push(vm, BOOL_VAL(IS_FALSY(vm_pop(vm))));
			break;
		case OP_NEGATE: {
			if (!IS_NUMBER(peek(vm, 0))) {
				vm_runtime_error(
				        vm, "operand must be a number.");
				return INTERPRET_RUNTIME_ERROR;
			}
			vm_push(vm, NUMBER_VAL(-AS_NUMBER(vm_pop(vm))));
			break;
		}
		case OP_PRINT: {
			print_flint_value(vm_pop(vm));
			break;
		}
		case OP_JUMP: {
			/* signed 16-bit forward offset, from the byte after it */
			uint16_t offset = READ_SHORT();
			frame->ip += offset;
			break;
		}
		case OP_JUMP_IF_FALSE: {
			/* peeks, does not pop: the false value has to survive
			 * the jump for the matching pop in the then-branch */
			uint16_t offset = READ_SHORT();
			if (IS_FALSY(peek(vm, 0)))
				frame->ip += offset;
			break;
		}
		case OP_JUMP_IF_NOT_NIL: {
			/* peeks, does not pop: a non-nil left side stays for
			 * the end of the `??`, and a nil one falls through to
			 * the POP that clears it for the right side. */
			uint16_t offset = READ_SHORT();
			if (!IS_NIL(peek(vm, 0)))
				frame->ip += offset;
			break;
		}
		case OP_LOOP: {
			/* signed 16-bit backward offset, negated on the way in */
			/*
			 * signed 16-bit backward offset, negated on the way in
			 *
			 * This is the only backward branch in the instruction set,
			 * which makes it the loop counter. Counting it here rather
			 * than trying to recognize loop shapes in the bytecode is
			 * one increment, and it is exactly right: a loop is
			 * whatever this instruction is in.
			 */
			uint16_t offset = READ_SHORT();
			frame->ip -= offset;
			vm->counters.backedges++;
			break;
		}
		case OP_CALL: {
			int arg_count = READ_BYTE();
			if (!call_value(vm, peek(vm, arg_count), arg_count))
				return INTERPRET_RUNTIME_ERROR;

			/*
			 * A native can report a runtime error without
			 * call_value() seeing it: len() on a table, pop() on
			 * an empty list, and so on all print a message and
			 * unwind the stack, then return nil as if nothing had
			 * happened. By the time we get here the stack may be
			 * gone and frame_count may be zero, so reloading
			 * `frame` would read frames[-1].
			 *
			 * The test is the same condition OP_RETURN uses: if we
			 * are no longer inside this run()'s frames, the
			 * script is over and so are we.
			 */
			if (vm->frame_count <= base_frame)
				return INTERPRET_RUNTIME_ERROR;

			/* the frame changed: either a new frame, or a native
			 * that returned and popped one */
			frame = &vm->frames[vm->frame_count - 1];
			break;
		}
		case OP_CLOSURE:
		case OP_CLOSURE_LONG: {
			ObjFunction *function =
			        AS_FUNCTION(instruction == OP_CLOSURE_LONG
			                            ? READ_CONSTANT_LONG()
			                            : READ_CONSTANT());

			/* push before the upvalues are filled in, so a
			 * collection triggered below sees a rooted closure */
			ObjClosure *closure = new_closure(vm, function);
			vm_push(vm, OBJ_VAL(closure));

			/* two bytes per upvalue: is_local, then index. the
			 * compiler worked this out by walking outward. */
			for (int i = 0; i < closure->upvalue_count; i++) {
				uint8_t is_local = READ_BYTE();
				uint8_t index = READ_BYTE();
				if (is_local)
					closure->upvalues[i] = capture_upvalue(
					        vm, frame->slots + index);
				else
					closure->upvalues[i] =
					        frame->closure->upvalues[index];
			}
			break;
		}
		case OP_CLOSE_UPVALUE: {
			/* a captured local going out of scope at the end of a
			 * block. the value stays on the stack for the pop. */
			close_upvalues(vm, vm->stack_top - 1);
			vm_pop(vm);
			break;
		}
		case OP_RETURN: {
			/* save the result before unwinding: closing upvalues
			 * moves stack values into the heap */
			Value result = vm_pop(vm);
			close_upvalues(vm, frame->slots);
			vm->frame_count--;
			if (vm->frame_count == base_frame) {
				/* the frame this run() started with, and there
				 * is no caller inside this run() to return to.
				 *
				 * the closure sits at frame->slots, and the
				 * result was just popped from above it. when a
				 * repl asked for the result to be left on the
				 * stack, frame->slots is the wrong thing to pop,
				 * so the value goes back and the stack is left
				 * as the caller expects. */
				if (vm->repl_leaves_value) {
					vm_push(vm, result);
				} else {
					vm_pop(vm);
				}
				return INTERPRET_OK;
			}

			/* drop everything the frame owned, callee included,
			 * then leave the result where the callee was */
			vm->stack_top = frame->slots;
			vm_push(vm, result);
			vm->counters.returns++;
			frame = &vm->frames[vm->frame_count - 1];
			break;
		}
		case OP_LIST_LEN: {
			/*
			 * The length, in place like a cast.
			 *
			 * This replaces a call to the `len` builtin, and it has
			 * to accept exactly what `len` accepted. It initially
			 * accepted only lists, which broke `for c in s` over a
			 * string -- documented, and it worked, because `len`
			 * takes both. Accepting a list only would have been a
			 * silent language change made by a performance
			 * shortcut, which is the one kind of change that is never
			 * acceptable.
			 *
			 * The error is `len`'s error for the same reason.
			 */
			Value target = peek(vm, 0);
			int length;
			if (IS_LIST(target)) {
				length = AS_LIST(target)->count;
			} else if (IS_STRING(target)) {
				length = AS_STRING(target)->length;
			} else {
				vm_runtime_error(vm,
				        "argument to len() must be a string or "
				        "list.");
				return INTERPRET_RUNTIME_ERROR;
			}
			vm->stack_top[-1] = NUMBER_VAL((double)length);
			break;
		}
		case OP_TABLE_COUNT: {
			/*
			 * The entry count, in place like OP_LIST_LEN.
			 *
			 * The loop condition re-reads this every iteration, so
			 * entries appended inside the body are visited and the
			 * loop cannot overrun a table that shrank: the bound is
			 * always current. That is a deliberate semantic, not an
			 * accident -- see the for-in documentation for what
			 * mutation during iteration means.
			 */
			Value target = peek(vm, 0);
			if (!IS_FLINT_TABLE(target)) {
				vm_runtime_error(vm,
				        "argument to len() must be a table.");
				return INTERPRET_RUNTIME_ERROR;
			}
			vm->stack_top[-1] = NUMBER_VAL(
			        (double)AS_FLINT_TABLE(target)->count);
			break;
		}
		case OP_TABLE_KEY:
		case OP_TABLE_VALUE: {
			/*
			 * The key or value at a numeric position.
			 *
			 * Tables are insertion-ordered, so position `i` always
			 * means the i-th inserted entry. The index goes through
			 * the same whole-number validation as list indexing --
			 * fractional, negative-out-of-range and non-numeric
			 * indices fail the same way in both -- because reading
			 * and writing through a bad index are the same bug
			 * wherever the container lives.
			 *
			 * A position past the end can only happen if the table
			 * shrank mid-loop. That is a runtime error rather than
			 * nil, because silently yielding nothing for an entry
			 * that was there a moment ago would hide the mutation
			 * that removed it.
			 */
			Value index_val = vm_pop(vm);
			Value target = vm_pop(vm);
			if (!IS_FLINT_TABLE(target)) {
				vm_runtime_error(vm,
				        "can only index tables by position "
				        "during iteration.");
				return INTERPRET_RUNTIME_ERROR;
			}
			ObjTable *t = AS_FLINT_TABLE(target);
			int idx;
			if (!vm_value_to_index(
			            vm, index_val, t->count, &idx, "table"))
				return INTERPRET_RUNTIME_ERROR;
			if (instruction == OP_TABLE_KEY)
				vm_push(vm, STR_VAL(t->keys[idx]));
			else
				vm_push(vm, t->values[idx]);
			break;
		}
		case OP_BUILD_LIST: {
			/* the elements are on the stack above the placeholder
			 * that was just pushed for the list itself */
			uint8_t count = READ_BYTE();
			ObjList *list = new_list(vm);
			vm_push(vm, OBJ_VAL(list));

			if (count > 0) {
				/* exact allocation: the literal cannot grow
				 * later except through push() */
				list->items = ALLOCATE(vm, Value, count);
				list->capacity = count;
				list->count = count;

				/* copy in reverse, since the stack is top-down */
				for (int i = count - 1; i >= 0; i--)
					list->items[i] =
					        vm->stack_top[-2 -
					                      (count - 1 - i)];

				/* drop the elements, keep the list */
				vm->stack_top -= count;
				vm->stack_top[-1] = OBJ_VAL(list);
			}
			break;
		}
		case OP_BUILD_TABLE: {
			/*
			 * Only the empty table. The compiler fills it with
			 * OP_SET_FIELD, and does it through a hidden local
			 * so each pair re-pushes the table.
			 */
			ObjTable *table = new_flint_table(vm);
			vm_push(vm, OBJ_VAL(table));
			break;
		}
		case OP_GET_INDEX: {
			Value index_val = vm_pop(vm);
			Value target = vm_pop(vm);

			if (IS_LIST(target)) {
				ObjList *list = AS_LIST(target);
				int idx;
				if (!vm_value_to_index(vm,
				            index_val,
				            list->count,
				            &idx,
				            "list"))
					return INTERPRET_RUNTIME_ERROR;
				vm_push(vm, list->items[idx]);
			} else if (IS_STRING(target)) {
				/* indexing a string yields a one-byte string, not a
				 * number */
				ObjString *str = AS_STRING(target);
				int idx;
				if (!vm_value_to_index(vm,
				            index_val,
				            str->length,
				            &idx,
				            "string"))
					return INTERPRET_RUNTIME_ERROR;
				/* the byte goes into a C local before copy_string(),
				 * which can collect. `str` is rooted and will not be
				 * freed, but reading through a pointer the collector
				 * just walked past is a habit not worth forming. */
				char c[2] = {str->chars[idx], '\0'};
				vm_push(vm, STR_VAL(copy_string(vm, c, 1)));
			} else if (IS_FLINT_TABLE(target)) {
				/*
				 * Table subscript with a computed key: `t[k]`
				 * where k is a value rather than a literal name.
				 *
				 * Field access (`t.name`) only ever sees interned
				 * constants, but a computed key is a runtime
				 * string and may never have been interned -- so
				 * this compares by content, not by pointer. The
				 * field opcodes below do the same, for the same
				 * reason: two equal strings must find each other
				 * however they were made.
				 *
				 * A missing key is nil, exactly as with field
				 * access. A non-string key is an error rather
				 * than a nil, because silently accepting `t[42]`
				 * would hide a bug in the key expression.
				 */
				ObjTable *t = AS_FLINT_TABLE(target);
				if (!IS_STRING(index_val)) {
					vm_runtime_error(vm,
					        "table index must be a "
					        "string.");
					return INTERPRET_RUNTIME_ERROR;
				}
				ObjString *key = AS_STRING(index_val);
				Value found = NIL_VAL;
				for (int i = 0; i < t->count; i++) {
					if (fl_strings_equal(t->keys[i], key)) {
						found = t->values[i];
						goto index_found;
					}
				}
index_found:;
				vm_push(vm, found);
			} else {
				vm_runtime_error(vm,
				        "can only index lists, strings and "
				        "tables.");
				return INTERPRET_RUNTIME_ERROR;
			}
			break;
		}
		case OP_SET_INDEX: {
			Value val = vm_pop(vm);
			Value index_val = vm_pop(vm);
			Value target = vm_pop(vm);

			/* assignment target must already exist. there is no
			 * append syntax; use push(). */
			if (IS_FLINT_TABLE(target)) {
				/*
				 * Table subscript assignment: `t[k] = v`.
				 *
				 * Content comparison, as in OP_GET_INDEX above:
				 * a computed key must find the entry a literal
				 * key created. A missing key appends, which is
				 * how a table grows through subscript; field
				 * assignment (`t.name = v`) does the same.
				 */
				ObjTable *t = AS_FLINT_TABLE(target);
				if (!IS_STRING(index_val)) {
					vm_runtime_error(vm,
					        "table index must be a "
					        "string.");
					return INTERPRET_RUNTIME_ERROR;
				}
				ObjString *key = AS_STRING(index_val);
				bool found = false;
				for (int i = 0; i < t->count; i++) {
					if (fl_strings_equal(t->keys[i], key)) {
						t->values[i] = val;
						found = true;
						goto set_index_found;
					}
				}
set_index_found:;
				if (!found) {
					if (t->capacity < t->count + 1) {
						int old_cap = t->capacity;
						t->capacity =
						        GROW_CAPACITY(old_cap);
						t->keys = GROW_ARRAY(vm,
						        ObjString *,
						        t->keys,
						        old_cap,
						        t->capacity);
						t->values = GROW_ARRAY(vm,
						        Value,
						        t->values,
						        old_cap,
						        t->capacity);
					}
					t->keys[t->count] = key;
					t->values[t->count] = val;
					t->count++;
				}
				vm_push(vm,
				        val); /* assignment yields the value */
				break;
			}
			if (!IS_LIST(target)) {
				vm_runtime_error(vm,
				        "can only index-assign to lists and "
				        "tables.");
				return INTERPRET_RUNTIME_ERROR;
			}
			ObjList *list = AS_LIST(target);
			int idx;
			/* same validation as OP_GET_INDEX. reading and writing
			 * through a fractional or overflowing index are the same
			 * bug, and one of them being checked is worse than
			 * neither. */
			if (!vm_value_to_index(
			            vm, index_val, list->count, &idx, "list"))
				return INTERPRET_RUNTIME_ERROR;
			list->items[idx] = val;
			vm_push(vm, val); /* assignment yields the value */
			break;
		}
		case OP_GET_FIELD:
		case OP_GET_FIELD_LONG: {
			ObjString *name = READ_STRING_AS(OP_GET_FIELD_LONG);
			Value target = vm_pop(vm);
			if (IS_FLINT_TABLE(target)) {
				/*
				 * Linear scan. Tables are parallel arrays in
				 * insertion order, and scripts do not build
				 * tables big enough for this to hurt. A hash
				 * table per flint table would be the fix if
				 * that ever changed.
				 */
				ObjTable *t = AS_FLINT_TABLE(target);
				for (int i = 0; i < t->count; i++) {
					/*
					 * Content, not pointer, comparison.
					 * Field names from constants are interned,
					 * but a key stored by subscript may never
					 * have been, and two equal strings must
					 * find each other however they were made.
					 */
					if (fl_strings_equal(
					            t->keys[i], name)) {
						vm_push(vm, t->values[i]);
						goto field_done;
					}
				}
				/* missing key is nil, not an error */
				vm_push(vm, NIL_VAL);
field_done:;
			} else {
				vm_runtime_error(
				        vm, "only tables have fields.");
				return INTERPRET_RUNTIME_ERROR;
			}
			break;
		}
		case OP_SET_FIELD:
		case OP_SET_FIELD_LONG: {
			ObjString *name = READ_STRING_AS(OP_SET_FIELD_LONG);
			Value val = vm_pop(vm);
			Value target = vm_pop(vm);
			if (IS_FLINT_TABLE(target)) {
				ObjTable *t = AS_FLINT_TABLE(target);
				for (int i = 0; i < t->count; i++) {
					/* content comparison, as in OP_GET_FIELD:
					 * a computed key must overwrite the entry
					 * a literal key created, not duplicate it. */
					if (fl_strings_equal(
					            t->keys[i], name)) {
						t->values[i] = val;
						vm_push(vm, val);
						goto field_set_done;
					}
				}

				/* new key. grow by doubling, both arrays at
				 * once so they stay parallel. */
				if (t->capacity < t->count + 1) {
					int old_cap = t->capacity;
					t->capacity = GROW_CAPACITY(old_cap);
					t->keys = GROW_ARRAY(vm,
					        ObjString *,
					        t->keys,
					        old_cap,
					        t->capacity);
					t->values = GROW_ARRAY(vm,
					        Value,
					        t->values,
					        old_cap,
					        t->capacity);
				}
				t->keys[t->count] = name;
				t->values[t->count] = val;
				t->count++;
				vm_push(vm, val);
field_set_done:;
			} else {
				vm_runtime_error(
				        vm, "only tables have fields.");
				return INTERPRET_RUNTIME_ERROR;
			}
			break;
		}
		case OP_SET_FIELD_TOP:
		case OP_SET_FIELD_TOP_LONG: {
			/*
			 * The table literal's opcode. The table is on the
			 * stack, the value is on top of it, and the table has
			 * to survive for the next pair -- so the value is
			 * popped and the table is read in place and left
			 * alone.
			 *
			 * Keeping the table on the stack is what makes a literal
			 * work as a call argument. Parking it in a local
			 * instead meant the literal assumed it was the topmost
			 * value, and it is not: in type({a:1}) the callee is
			 * already there, so the literal set a field on the
			 * function.
			 */
			ObjString *name = READ_STRING_AS(OP_SET_FIELD_TOP_LONG);
			Value val = vm_pop(vm);
			Value target = vm->stack_top[-1];

			if (!IS_FLINT_TABLE(target)) {
				vm_runtime_error(
				        vm, "only tables have fields.");
				return INTERPRET_RUNTIME_ERROR;
			}

			ObjTable *t = AS_FLINT_TABLE(target);

			/*
			 * find or append. `found` is what makes a key repeated
			 * inside one literal overwrite instead of adding a second
			 * entry under the same name, which a lookup would then
			 * never reach.
			 */
			bool found = false;
			for (int i = 0; i < t->count; i++) {
				if (fl_strings_equal(t->keys[i], name)) {
					t->values[i] = val;
					found = true;
					goto build_table_found;
				}
			}
build_table_found:;
			if (!found) {
				/* grow both arrays together, they must stay parallel */
				if (t->capacity < t->count + 1) {
					int old_cap = t->capacity;
					t->capacity = GROW_CAPACITY(old_cap);
					t->keys = GROW_ARRAY(vm,
					        ObjString *,
					        t->keys,
					        old_cap,
					        t->capacity);
					t->values = GROW_ARRAY(vm,
					        Value,
					        t->values,
					        old_cap,
					        t->capacity);
				}
				t->keys[t->count] = name;
				t->values[t->count] = val;
				t->count++;
			}
			break;
		}
		case OP_IMPORT:
		case OP_EXPORT:
			/*
			 * Never emitted. The compiler desugars import into a
			 * call to the import_file native, and export is a
			 * compile-time marker that leaves the ordinary
			 * definition in place. Kept so the enum and the
			 * disassembler agree on the opcode list.
			 */
			break;

		/*
		 * Specialized numeric ops: no type check, straight arithmetic.
		 * The compiler emits these when both operands are provably
		 * numbers (e.g. literal + literal, or typed locals). The
		 * savings is the tag test and branch that OP_ADD etc. pay.
		 */
		case OP_ADD_NUM: {
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, NUMBER_VAL(a + b));
			break;
		}
		case OP_SUB_NUM: {
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, NUMBER_VAL(a - b));
			break;
		}
		case OP_MUL_NUM: {
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, NUMBER_VAL(a * b));
			break;
		}
		case OP_DIV_NUM: {
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, NUMBER_VAL(a / b));
			break;
		}
		case OP_MOD_NUM: {
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, NUMBER_VAL(fmod(a, b)));
			break;
		}
		case OP_LT_NUM: {
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, BOOL_VAL(a < b));
			break;
		}
		case OP_LE_NUM: {
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, BOOL_VAL(a <= b));
			break;
		}
		case OP_GT_NUM: {
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, BOOL_VAL(a > b));
			break;
		}
		case OP_GE_NUM: {
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, BOOL_VAL(a >= b));
			break;
		}
		case OP_EQ_NUM: {
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, BOOL_VAL(a == b));
			break;
		}
		case OP_NEQ_NUM: {
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, BOOL_VAL(a != b));
			break;
		}
		case OP_NEG_NUM: {
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, NUMBER_VAL(-a));
			break;
		}
		}
	}

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef READ_U24
#undef READ_CONSTANT_LONG
#undef READ_STRING_AS
#undef BINARY_OP
}

/*
 * Compile and run. This is the entry point for the repl, for files, and for
 * modules, since import_file_native() calls straight back into here.
 */
InterpretResult vm_interpret(VM *vm, const char *source)
{
	return vm_interpret_named(vm, source, "<source>");
}

InterpretResult vm_interpret_named(VM *vm, const char *source, const char *name)
{
	ObjFunction *function = compile_named(vm, source, name);
	if (function == NULL)
		return INTERPRET_COMPILE_ERROR;
	return vm_interpret_function(vm, function, source, name);
}

/*
 * Run an already-compiled function.
 *
 * Everything about the entry sequence lives here: base frame, base stack top,
 * verification, and the closure-and-call that starts execution. The stack
 * unwinding and restoration are the same in every case, which is why
 * vm_interpret_named() is now a two-line wrapper.
 */
InterpretResult vm_interpret_function(
        VM *vm, ObjFunction *function, const char *source, const char *name)
{
	const char *saved_text = vm->source_text;
	const char *saved_name = vm->source_name;
	/*
	 * Publish the source for the whole run. vm_runtime_error() reads
	 * vm->source_text to place a span and a caret, so this has to be set
	 * here rather than only around the compile -- which is what
	 * vm_interpret_named() used to do, back when compiling and running were
	 * one function. Splitting them left it unset, and the symptom was
	 * runtime errors with a code and a message and no span, which reads as
	 * "there is no location" rather than "the caller forgot".
	 */
	vm->source_text = source;
	vm->source_name = name;
	/*
	 * Reentrant, in two independent ways.
	 *
	 * `import` calls this from inside a running script, and the repl
	 * calls it once per line. The compiler's parser state is file-scope
	 * static, so a nested compile would clobber an outer one -- that is
	 * safe today only because compiling and executing are separate
	 * phases, and an outer compile has always finished by the time any
	 * execution nests. That is an invariant nothing enforces, which is
	 * the kind that survives until someone makes import eager.
	 *
	 * The frame depth is the part that cannot be assumed, and the reason
	 * this used to crash on every import. See run().
	 */
	int saved_base = vm->base_frame;
	int base_frame = vm->frame_count;

	/*
	 * The stack top on entry, before this script pushes anything, and the
	 * frame count it started at. Both are restored on the way out so the
	 * caller's own bookkeeping is correct, and both are published in the
	 * VM so a failure inside this script can unwind to exactly here.
	 *
	 * Restoring stack_top explicitly is not belt and braces. On the error
	 * path vm_runtime_error() has already unwound to these values, so run()
	 * returns from a state that is already consistent -- which is exactly
	 * why the restore can be a single unconditional assignment instead of
	 * two paths that each restore a different subset.
	 */
	Value *base_top = vm->stack_top;

	vm->base_frame = base_frame;
	vm->base_top = base_top;

	/*
	 * The function is rooted on the stack across the new_closure() call,
	 * which can collect. After the closure exists the function is
	 * reachable through it, so the raw function can come off.
	 */
	vm_push(vm, OBJ_VAL(function));
	ObjClosure *closure = new_closure(vm, function);
	vm_pop(vm);
	vm_push(vm, OBJ_VAL(closure));

	/*
	 * call() failing means arity mismatch or FRAMES_MAX, and it has
	 * already reported. Returning unchecked used to enter the dispatch
	 * loop with no frame at all, which reads frames[-1].
	 */
	if (!call(vm, closure, 0)) {
		unwind_to(vm, base_frame, base_top);
		vm->base_frame = saved_base;
		vm->source_text = saved_text;
		vm->source_name = saved_name;
		return INTERPRET_RUNTIME_ERROR;
	}

	InterpretResult result = run(vm, base_frame);

	/*
	 * Hand the caller's stack back exactly as we found it. On the success
	 * path run() has already returned to this depth and popped this
	 * script's own closure, so this is a no-op. On the error path it is
	 * the whole fix: the handler unwound to base_top, and this restores
	 * the same position for the caller that is about to do its own
	 * arithmetic on it.
	 */
	/*
	 * The repl is the one case where the stack is deliberately not
	 * restored: it asked for the value of the last expression and
	 * OP_RETURN left it above the frame's closure. handing back
	 * base_top here would throw away the answer the repl is about
	 * to print, and a repl that throws away the answer is the
	 * thing this whole path exists to avoid.
	 */
	if (!vm->repl_leaves_value)
		vm->stack_top = base_top;
	vm->frame_count = base_frame;

	/*
	 * The caller's own base, so a failure on its side unwinds to its own
	 * start and not to this script's.
	 */
	vm->base_frame = saved_base;
	vm->source_text = saved_text;
	vm->source_name = saved_name;

	return result;
}
