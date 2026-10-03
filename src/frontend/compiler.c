/* SPDX-License-Identifier: MIT */
/*
 * Single-pass pratt parser and bytecode compiler.
 *
 * There is no AST. Tokens are pulled from the scanner one at a time and
 * bytecode is emitted straight into the chunk. Nothing is allocated per
 * node, so a function with a thousand expressions costs a thousand bytes of
 * code and nothing else.
 *
 * All parser state is in file-scope statics: one Scanner, one Parser, one
 * Compiler chain. A compiler is a stack of these, chained through
 * `enclosing`, and each one owns the locals and upvalues of one function.
 */
#include "compiler.h"
#include "chunk.h"
#include "common.h"
#include "diagnostic.h"
#include "memory.h"
#include "object.h"
#include "scanner.h"

/* the disassembler is called from end_compiler(), and only under
 * FL_DEBUG_PRINT_CODE. Including it unconditionally makes it an unused
 * include in a release build, so it goes where it is used. */
#ifdef FL_DEBUG_PRINT_CODE
#	include "debug.h"
#endif
#include "stdint.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/*
 * Precedence levels, loosest first. The parser takes a precedence and keeps
 * consuming infix operators whose precedence is at least that tight, which
 * is the whole algorithm. Adding an operator means adding a case to rules[]
 * and, if its level is new, a value here.
 */
typedef enum {
	PREC_NONE,
	PREC_ASSIGNMENT,
	PREC_COALESCE,
	PREC_OR,
	PREC_AND,
	PREC_EQUALITY,
	PREC_COMPARISON,
	PREC_RANGE,
	PREC_TERM,
	PREC_FACTOR,
	PREC_CAST,
	PREC_UNARY,
	PREC_CALL,
	PREC_PRIMARY
} Precedence;

/*
 * can_assign is passed down because `=` and the compound assignments bind
 * so loosely that a prefix rule has no way to tell whether it is on the left
 * of one. Without it, "f() = 1" would compile.
 */
typedef void (*ParseFn)(bool can_assign);

/* indexed by TokenType. see the rules table at the bottom. */
typedef struct {
	ParseFn prefix; /* at the start of an expression */
	ParseFn infix; /* after a complete expression */
	Precedence precedence;
} ParseRule;

/*
 * One stack slot. depth of -1 means declared but not yet initialized, which
 * is how "let a = a" is caught. is_captured means some inner function needs
 * this to outlive its scope, so it gets closed instead of popped.
 */
typedef struct {
	Token name;
	int depth;
	bool is_captured;
	bool is_const;
	/*
	 * Where the name was written, so a redeclaration in the same scope can
	 * point back at the first one. A copy rather than a pointer, because
	 * the token is a window into the source buffer and a pointer to it
	 * would dangle once the buffer went away. Four bytes, and the only
	 * thing that makes a duplicate-definition error show both halves.
	 */
	uint32_t name_offset;
} Local;

/* how to get one upvalue: from a local slot, or from our own upvalue. */
typedef struct {
	uint8_t index;
	bool is_local;
} CompilerUpvalue;

typedef enum { TYPE_FUNCTION, TYPE_SCRIPT } FunctionType;

typedef struct Compiler {
	struct Compiler *enclosing;
	ObjFunction *function;
	FunctionType type;

	/* slot 0 is the callee, so a local index doubles as a stack
	 * offset from frame->slots */
	Local locals[MAX_LOCALS];
	int local_count;

	CompilerUpvalue upvalues[MAX_UPVALUES];

	int scope_depth;

	/*
	 * How many try blocks are open in the function this compiler is
	 * building. Each one emitted an OP_TRY/OP_POP_HANDLER pair, and
	 * break/continue/return crossing one has to retire its handler
	 * first, because the handler stack is dynamic while those jumps
	 * are static.
	 */
	int try_depth;
} Compiler;

/*
 * One `for` or `while`, so break and continue can find somewhere to jump.
 * The jump offsets are collected here and patched once the loop body ends,
 * because at the point break is compiled the end is not known yet.
 */
typedef struct LoopContext {
	struct LoopContext *enclosing;
	int scope_depth;
	int start; /* offset of the loop head, for OP_LOOP */
	int continue_target; /* offset to jump to, or -1 if unknown */
	int break_jumps[256];
	int break_count;
	int continue_jumps[256];
	int continue_count;
	int try_depth; /* try blocks open when this loop began */
} LoopContext;

/* how many diagnostics before we stop and summarise instead. a file with
 * three mistakes should say so three times; a file that is wrong from the
 * first byte can produce thousands, and thousands is not information */
#define MAX_DIAGNOSTICS 20

typedef struct {
	Token current;
	Token previous;
	bool had_error;
	int error_count;
	bool suppressed; /* past MAX_DIAGNOSTICS: stop reporting */
	/* after one error, stop reporting until the parser resynchronizes */
	bool panic_mode;
} Parser;

/*
 * Everything one in-progress compilation owns.
 *
 * These were four separate file-scope globals. Bundling them is not tidiness:
 * they are four views of the same thing, and the reason they can be one
 * struct is that they are all saved and restored together.
 *
 * The scanner in scanner.c is still a global, and cannot be otherwise
 * without a much larger change. What that costs is reentrancy: a compile
 * inside a compile would clobber it. That cannot happen today, because
 * compiling and executing are separate phases and an outer compile has
 * always finished before any execution nests. compile() saving and
 * restoring this struct makes that an enforced property rather than an
 * argument in a comment, so the day import becomes eager the parser breaks
 * loudly instead of quietly.
 */
typedef struct {
	Parser parser; /* token position, error state */
	Compiler *current; /* innermost function being compiled */
	VM *vm; /* for allocation, and the GC's view */
	LoopContext *loop; /* innermost loop, for break and continue */

	/* repl echo: leave an expression statement's value on the stack
	 * rather than popping it, so `1 + 2` can answer. see
	 * expression_statement() */
	bool echo_repl_value;
	bool left_value_on_stack;

	/* how many try bodies this compile is inside. An import inside a
	 * try may legitimately end up unused: the module can throw before
	 * the binding is ever read, and reporting that as a dead import
	 * would be wrong. */
	int try_nesting;
} CompilerState;

static CompilerState state;
static const FlSource *diag_source;
static FlDiagFormat diag_format = FL_DIAG_LEGACY;
static FlColorMode diag_color = FL_COLOR_AUTO;
static const char *diag_text;
static const char *diag_name;
static FlDiagSuggestion fixes[64];
static size_t fix_count;

/*
 * True while compiling the declaration after an `export` keyword.
 *
 * File-scope like the parser state, for the same reason: it describes the
 * declaration being parsed, and there is exactly one declaration in flight.
 */
static bool exporting;

/* declared here because named_variable marks imports used long before the
 * definitions below. see import_mark_used for what it does. */
static void import_mark_used(const char *name, int length);

/*
 * Imports bound by this compilation, for the unused-import check.
 *
 * An import that is never read is dead code with a side effect -- it still
 * runs the module -- and dead code that runs things is the kind that
 * surprises people. So an import whose name is never referenced is a compile
 * error, not a warning: warnings are easy to ignore and this one is cheap to
 * fix, either by using the import or by deleting it.
 *
 * File-static like `fixes` above, reset at the start of every compile. The
 * names are malloc'd copies because the binding for a quoted import is
 * derived into a stack buffer that dies with import_declaration; comparing
 * raw bytes would read freed memory.
 *
 * `_` as an alias opts out explicitly, for the one legitimate case: importing
 * a module for its failure. A test that imports a module it knows will throw,
 * to check that the importer survives, cannot use the binding -- there is
 * nothing to use -- so it says so.
 */
typedef struct {
	char *name;
	int length;
	int line;
	uint32_t offset;
	bool used;
	/* inside a try body: the binding may legitimately go unused
	 * because the module threw before it could be used */
	bool in_try;
} ImportEntry;

#define FL_MAX_IMPORTS 64

static ImportEntry imports[FL_MAX_IMPORTS];
static size_t import_count;
static bool imports_full;

size_t compiler_fix_count(void) { return fix_count; }

const FlDiagSuggestion *compiler_fix_at(size_t index)
{
	return index < fix_count ? &fixes[index] : NULL;
}

void compiler_repl_echo(bool on) { state.echo_repl_value = on; }

bool compiler_repl_value(void)
{
	bool left = state.left_value_on_stack;
	state.left_value_on_stack = false;
	return left;
}

void compiler_set_diagnostics(
        const FlSource *source, FlDiagFormat format, FlColorMode color)
{
	diag_source = source;
	diag_format = format;
	diag_color = color;
}

static const char *diagnostic_code(const Token *token, const char *message)
{
	if (token->type == TOKEN_ERROR) {
		if (strstr(message, "unexpected character") != NULL)
			return "E0001";
		if (strstr(message, "scientific notation") != NULL)
			return "E0002";
		if (strstr(message, "string") != NULL)
			return "E0003";
	}
	if (strstr(message, "expect ')'") != NULL ||
	        strstr(message, "expect '}'") != NULL ||
	        strstr(message, "expect ']'") != NULL)
		return "E0102";
	return "E0100";
}

static Chunk *current_chunk(void) { return &state.current->function->chunk; }

/* error reporting */

/*
 * Report once, then set panic mode. One syntax error usually means the
 * state.parser is about to produce fifty more, all of them consequences. The flag
 * makes error_at() silent until synchronize() clears it.
 */
static void error_at(Token *token, const char *message)
{
	if (state.parser.panic_mode)
		return;
	state.parser.panic_mode = true;

	/* past the cap, stop talking. a file that is wrong from the first
	 * byte can produce thousands of diagnostics and thousands is not
	 * information, it is a wall. the summary at the end says how
	 * many were dropped. */
	if (state.parser.suppressed)
		return;
	if (state.parser.error_count >= MAX_DIAGNOSTICS) {
		state.parser.suppressed = true;
		fprintf(stderr,
		        "too many errors; stopping after %d. fix those and "
		        "run again to see more.\n",
		        MAX_DIAGNOSTICS);
		return;
	}
	if (diag_format == FL_DIAG_LEGACY) {
		fprintf(stderr, "[line %d] Error", token->line);
		if (token->type == TOKEN_EOF)
			fprintf(stderr, " at end");
		else if (token->type != TOKEN_ERROR)
			fprintf(stderr,
			        " at '%.*s'",
			        token->length,
			        token->start);
		fprintf(stderr, ": %s\n", message);
		state.parser.had_error = true;
		state.parser.error_count++;
		return;
	}
	FlDiagSuggestion suggestion;
	const FlDiagSuggestion *suggestions = NULL;
	size_t suggestion_count = 0;

	/*
	 * A missing delimiter is a problem with the token *before* it, not
	 * with whatever token the parser happened to be holding. The
	 * insertion point is the end of the previous token, which is where
	 * the delimiter belongs.
	 *
	 * Offering a fix used to be restricted to end of file, on the
	 * reasoning that a delimiter is missing "at the end". That was
	 * wrong: the common case is a missing brace in the middle of a
	 * function, and it got neither the right span nor a fix. This is a
	 * property of the *message*, not of where the parser ran out of
	 * input.
	 */
	const char *delimiter = NULL;
	if (strstr(message, "expect ')'") != NULL)
		delimiter = ")";
	else if (strstr(message, "expect '}'") != NULL)
		delimiter = "}";
	else if (strstr(message, "expect ']'") != NULL)
		delimiter = "]";

	size_t start = token->offset;
	size_t len = (size_t)token->length;
	const char *label =
	        token->type == TOKEN_EOF ? "expected here" : "unexpected token";

	if (delimiter != NULL) {
		Token *prev = &state.parser.previous;
		if (prev->type != TOKEN_EOF) {
			start = (size_t)prev->offset + (size_t)prev->length;
			len = 0;
			label = "expected here";
		}
		suggestion = (FlDiagSuggestion){
		        .span = fl_span(start, start + len),
		        .message = "add the missing delimiter",
		        .replacement = delimiter,
		        .applicability = FL_APPLICABILITY_MACHINE,
		};
		suggestions = &suggestion;
		suggestion_count = 1;
		if (fix_count < sizeof(fixes) / sizeof(fixes[0]))
			fixes[fix_count++] = suggestion;
	}
	FlDiagnostic diag = {
	        .severity = FL_DIAG_ERROR,
	        .code = diagnostic_code(token, message),
	        .message = message,
	        .primary = fl_span(start, start + len),
	        .has_primary = true,
	        .primary_label = label,
	        .suggestions = suggestions,
	        .suggestion_count = suggestion_count,
	};
	FlSource source;
	bool temporary = diag_source == NULL;
	if (temporary) {
		fl_source_init(&source, diag_name, diag_text);
		diag_source = &source;
	}
	fl_diag_emit(stderr, &diag, diag_source, diag_format, diag_color);
	if (temporary) {
		fl_source_free(&source);
		diag_source = NULL;
	}
	state.parser.had_error = true;
	state.parser.error_count++;
}

static void error(const char *message)
{
	error_at(&state.parser.previous, message);
}

/*
 * An error with one related location attached.
 *
 * A redeclaration is the case that needs it: saying "already a variable with
 * this name" and stopping there leaves the reader to go and find the other
 * one. With the second span attached the diagnostic can show both halves, and
 * the eye can go straight between them.
 *
 * The extra span is a window into the same source, so it stays valid for
 * exactly as long as the primary does.
 */
static void error_with_label(const char *message,
        size_t other_start,
        size_t other_end,
        const char *label)
{
	Token *token = &state.parser.previous;
	FlDiagLabel extra = {
	        .span = fl_span(other_start, other_end),
	        .label = label,
	};
	FlDiagnostic diag = {
	        .severity = FL_DIAG_ERROR,
	        .code = "E0201",
	        .message = message,
	        .primary = fl_span((size_t)token->offset,
	                (size_t)token->offset + (size_t)token->length),
	        .has_primary = true,
	        .primary_label = "defined again here",
	        .labels = &extra,
	        .label_count = 1,
	};

	FlSource source;
	bool temporary = diag_source == NULL;
	if (temporary) {
		fl_source_init(&source, diag_name, diag_text);
		diag_source = &source;
	}
	fl_diag_emit(stderr, &diag, diag_source, diag_format, diag_color);
	if (temporary) {
		fl_source_free(&source);
		diag_source = NULL;
	}
	state.parser.had_error = true;
	state.parser.error_count++;
}

static void error_at_current(const char *message)
{
	error_at(&state.parser.current, message);
}

/* scanner wrappers */

/*
 * Pull the next token into state.current, moving the old state.current to previous.
 *
 * Scanner errors are not returned to the caller: they are reported here and
 * then we scan again, so malformed source produces one message at the right
 * line instead of an error token the state.parser has to understand.
 */
static void advance(void)
{
	state.parser.previous = state.parser.current;
	for (;;) {
		state.parser.current = scan_token();
		if (state.parser.current.type != TOKEN_ERROR)
			break;
		error_at_current(state.parser.current.start);
	}
}

static void consume(TokenType type, const char *message)
{
	if (state.parser.current.type == type) {
		advance();
		return;
	}
	error_at_current(message);
}

static bool check(TokenType type) { return state.parser.current.type == type; }

/* consume if present. this is lookahead without a token buffer. */
static bool match(TokenType type)
{
	if (!check(type))
		return false;
	advance();
	return true;
}

/* bytecode emission */

/*
 * One byte, tagged with the line of the token that produced it. The operand
 * instructions write their operands with a separate chunk_write() call and
 * share this line number, so an error always blames the operator.
 */
static void emit_byte(uint8_t byte)
{
	chunk_write(state.vm,
	        current_chunk(),
	        byte,
	        state.parser.previous.line,
	        (uint32_t)state.parser.previous.offset);
}

static void emit_bytes(uint8_t byte1, uint8_t byte2)
{
	emit_byte(byte1);
	emit_byte(byte2);
}

/* nil; return. used to close a function and to implement a bare `return`. */
static void emit_return(void)
{
	emit_byte(OP_NIL);
	emit_byte(OP_RETURN);
}

/*
 * Add a value to the constant pool and return its index.
 *
 * The value is pushed across the call because this is where a freshly
 * interned string first becomes reachable. Before chunk_add_constant() runs,
 * an object lives only in the weak intern table, so a collection would sweep
 * it. chunk_add_constant() can allocate, and allocating can collect. Push
 * first, write second.
 *
 * Every path that interns a string goes through here: identifier_constant(),
 * string() and import_declaration(). Rooting it anywhere else would be one
 * more place to forget.
 */
static int make_constant(Value value)
{
	vm_push(state.vm, value);
	int constant = chunk_add_constant(state.vm, current_chunk(), value);
	vm_pop(state.vm);
	if (constant < 0) {
		error("too many constants in one chunk.");
		return 0; /* unreachable in practice: needs 16M constants */
	}
	return constant;
}

static void emit_indexed(uint8_t short_op, uint8_t long_op, int index)
{
	if (index < 256) {
		emit_bytes(short_op, (uint8_t)index);
	} else {
		emit_byte(long_op);
		emit_byte((uint8_t)((index >> 16) & 0xff));
		emit_byte((uint8_t)((index >> 8) & 0xff));
		emit_byte((uint8_t)(index & 0xff));
	}
}

static void emit_constant(Value value)
{
	emit_indexed(OP_CONSTANT, OP_CONSTANT_LONG, make_constant(value));
}

/*
 * A forward jump whose target is not known yet. Two placeholder bytes go out
 * and patch_jump() fills them in. Returns the offset of the placeholder so
 * the caller can patch it later.
 */
static int emit_jump(uint8_t instruction)
{
	emit_byte(instruction);
	emit_byte(0xff);
	emit_byte(0xff);
	return current_chunk()->count - 2;
}

/* fill in a forward jump. target is measured from the end of the jump. */
static void patch_jump(int offset)
{
	int jump = current_chunk()->count - offset - 2;
	if (jump > UINT16_MAX)
		error("too much code to jump over.");
	current_chunk()->code[offset] = (jump >> 8) & 0xff;
	current_chunk()->code[offset + 1] = jump & 0xff;
}

/*
 * A backward jump, closing a loop. Unlike a forward jump this one is emitted
 * at the point where the target is already known, so there is no patch step.
 */
static void emit_loop(int loop_start)
{
	emit_byte(OP_LOOP);
	int offset = current_chunk()->count - loop_start + 2;
	if (offset > UINT16_MAX)
		error("loop body too large.");
	emit_byte((offset >> 8) & 0xff);
	emit_byte(offset & 0xff);
}

/* scope management */

/*
 * Push a compiler onto the chain. The new compiler becomes state.current, and
 * `enclosing` is how resolve_upvalue() walks outward.
 */
static void init_compiler(Compiler *compiler, FunctionType type)
{
	compiler->enclosing = state.current;
	compiler->function = NULL;
	compiler->type = type;
	compiler->local_count = 0;
	compiler->scope_depth = 0;
	compiler->try_depth = 0;
	compiler->function = new_function(state.vm);
	state.current = compiler;

	/* the top-level script has no name */
	if (type != TYPE_SCRIPT)
		state.current->function->name = copy_string(state.vm,
		        state.parser.previous.start,
		        state.parser.previous.length);

	/* slot 0 is the callee. unnamed, and never resolved, because the
	 * index lines up with the call frame's base. */
	Local *local = &state.current->locals[state.current->local_count++];
	local->depth = 0;
	local->is_captured = false;
	local->is_const = false;
	local->name.start = "";
	local->name.length = 0;
	local->name_offset = 0;
	state.current->function->chunk.local_count = state.current->local_count;
}

/* Pop the compiler, emit the implicit return, and hand back the function. */
static ObjFunction *end_compiler(void)
{
	emit_return();
	ObjFunction *function = state.current->function;

	/* dumping half-compiled code after an error is just noise */
#ifdef FL_DEBUG_PRINT_CODE
	if (!state.parser.had_error) {
		chunk_disassemble(current_chunk(),
		        function->name != NULL ? function->name->chars
			                       : "<script>");
	}
#endif

	state.current = state.current->enclosing;
	return function;
}

static void begin_scope(void) { state.current->scope_depth++; }

/*
 * Leave a scope. Every local declared in it is still on the stack and has to
 * be removed. Captured ones get OP_CLOSE_UPVALUE, which moves the value onto
 * the heap first; a plain OP_POP would leave a captured upvalue pointing at
 * a slot the next call is free to reuse.
 */
static void end_scope(void)
{
	state.current->scope_depth--;

	while (state.current->local_count > 0 &&
	        state.current->locals[state.current->local_count - 1].depth >
	                state.current->scope_depth) {
		if (state.current->locals[state.current->local_count - 1]
		                .is_captured)
			emit_byte(OP_CLOSE_UPVALUE);
		else
			emit_byte(OP_POP);
		state.current->local_count--;
	}
}

/* identifier helpers */

/* intern a name into the constant pool. names are interned, so equality on
 * them is a pointer compare all the way down. */
static int identifier_constant(Token *name)
{
	return make_constant(
	        STR_VAL(copy_string(state.vm, name->start, name->length)));
}

/*
 * The same, for a name the parser has no token for.
 *
 * An import's binding name is sometimes computed rather than read -- the last
 * path component of "lib/geometry.fl" is "geometry", and there is no token for
 * it. This exists so that computed name can go through the same interning as
 * every other identifier, which is what makes field access on it a pointer
 * compare like any other.
 */
static int identifier_constant_from(const char *text, int length)
{
	return make_constant(STR_VAL(copy_string(state.vm, text, length)));
}

static bool identifiers_equal(Token *a, Token *b)
{
	return a->length == b->length &&
	       memcmp(a->start, b->start, a->length) == 0;
}

/*
 * A token that matches a known word, for comparing a parsed identifier against
 * a fixed set of names. The word is a string literal, so its `start` is not
 * the source buffer and the memcmp in identifiers_equal is not valid on it.
 * This wraps one with a computed length so the same compare works for both.
 */
static Token token_string(const char *text)
{
	Token token;
	token.type = TOKEN_IDENTIFIER;
	token.start = text;
	token.length = (int)strlen(text);
	token.line = state.parser.previous.line;
	token.newline_before = false;
	return token;
}

/* compare a parsed identifier against a known word. */
static bool identifier_is(const char *word)
{
	Token known = token_string(word);
	return identifiers_equal(&state.parser.previous, &known);
}

/*
 * Find a local by name, innermost first. Shadowing works for free: the
 * innermost match is the one you meant.
 *
 * depth of -1 is a variable whose initializer has not been compiled yet. If
 * we find that one, the source is reading a variable in its own
 * initializer and the stack slot does not exist.
 */
static int resolve_local(Compiler *compiler, Token *name)
{
	for (int i = compiler->local_count - 1; i >= 0; i--) {
		Local *local = &compiler->locals[i];
		if (identifiers_equal(name, &local->name)) {
			if (local->depth == -1)
				error("can't read local variable in its own "
				      "initializer.");
			return i;
		}
	}
	return -1;
}

/* record an upvalue, deduplicating: two captures of the same slot share one. */
static int add_upvalue(Compiler *compiler, uint8_t index, bool is_local)
{
	int upvalue_count = compiler->function->upvalue_count;

	for (int i = 0; i < upvalue_count; i++) {
		CompilerUpvalue *upvalue = &compiler->upvalues[i];
		if (upvalue->index == index && upvalue->is_local == is_local)
			return i;
	}

	if (upvalue_count == MAX_UPVALUES) {
		error("too many closure variables in function.");
		return 0;
	}

	compiler->upvalues[upvalue_count].is_local = is_local;
	compiler->upvalues[upvalue_count].index = index;
	return compiler->function->upvalue_count++;
}

/*
 * Find a name in an enclosing scope, which is what makes a closure a
 * closure. Walks outward one compiler at a time:
 *
 *   1. a local in the immediate parent: mark it captured and record a
 *      capture of that slot
 *   2. otherwise, whatever the parent itself captured: record a capture of
 *      the parent's upvalue
 *   3. otherwise it is not a local at all, and the caller falls back to a
 *      global
 *
 * Step 2 is the recursive case, and it is why upvalues nest. The marking in
 * step 1 is what makes end_scope() emit OP_CLOSE_UPVALUE later.
 */
static int resolve_upvalue(Compiler *compiler, Token *name)
{
	if (compiler->enclosing == NULL)
		return -1;

	int local = resolve_local(compiler->enclosing, name);
	if (local != -1) {
		compiler->enclosing->locals[local].is_captured = true;
		return add_upvalue(compiler, (uint8_t)local, true);
	}

	int upvalue = resolve_upvalue(compiler->enclosing, name);
	if (upvalue != -1)
		return add_upvalue(compiler, (uint8_t)upvalue, false);

	return -1;
}

/* take a stack slot for a new local. depth -1: not initialized yet. */
static void add_local(Token name, bool is_const)
{
	if (state.current->local_count == MAX_LOCALS) {
		error("too many local variables in function.");
		return;
	}

	Local *local = &state.current->locals[state.current->local_count++];
	local->name = name;
	local->name_offset = (uint32_t)name.offset;
	local->depth = -1;
	local->is_captured = false;
	local->is_const = is_const;

	/*
	 * Published on the chunk as each local is added, so the verifier can
	 * check an OP_GET_LOCAL operand against something real. An operand is a
	 * byte, so it can never exceed MAX_LOCALS and a check against that limit
	 * catches nothing; only the function's own peak slot count means
	 * anything.
	 *
	 * A peak, and only ever raised. Slots are reused: end_scope() drops
	 * local_count back so a sibling scope can have the same slots, which
	 * means the current count goes down and up across a function. Assigning
	 * the current count here would lower the recorded peak when a later
	 * scope happens to be shallower than an earlier one, and then every slot
	 * index the earlier scope used would look out of range to the verifier.
	 * That is not hypothetical: `for x in xs` followed by `for i in 0..n`
	 * at the same level did exactly that.
	 */
	if (state.current->local_count >
	        state.current->function->chunk.local_count)
		state.current->function->chunk.local_count =
		        state.current->local_count;
}

/*
 * Reject a redeclaration in the same scope. The scan stops at the first
 * local from an outer scope, since shadowing that one is fine.
 */
static void declare_variable(bool is_const)
{
	/* at depth 0 there are no stack slots, so no shadowing to check */
	if (state.current->scope_depth == 0)
		return;

	Token *name = &state.parser.previous;
	for (int i = state.current->local_count - 1; i >= 0; i--) {
		Local *local = &state.current->locals[i];
		if (local->depth != -1 &&
		        local->depth < state.current->scope_depth)
			break;
		if (identifiers_equal(name, &local->name)) {
			/* format the name into the message first. a
			 * variadic call cannot interleave printf's own
			 * arguments with positional ones, and reading it
			 * that way is a very easy bug to ship. */
			char message[160];
			snprintf(message,
			        sizeof(message),
			        "variable `%.*s` is already defined in "
			        "this scope",
			        name->length,
			        name->start);
			state.parser.panic_mode = false;
			error_with_label(message,
			        (size_t)local->name_offset,
			        (size_t)local->name_offset +
			                (size_t)local->name.length,
			        "first defined here");
			break;
		}
	}
	add_local(*name, is_const);
}

/* the initializer has been compiled, so the slot exists now */
static void mark_initialized(void)
{
	if (state.current->scope_depth == 0)
		return;
	state.current->locals[state.current->local_count - 1].depth =
	        state.current->scope_depth;
}

/*
 * Read a name and reserve its slot. Returns a constant index, or 0 for a
 * local, which the caller only uses when this is a global. Globals skip
 * add_local() entirely, which is why the scope check is here rather than in
 * the caller.
 */
static int parse_variable(const char *message, bool is_const)
{
	consume(TOKEN_IDENTIFIER, message);

	declare_variable(is_const);
	if (state.current->scope_depth > 0)
		return 0;

	return identifier_constant(&state.parser.previous);
}

/* the initializer is on the stack: store it, in a slot or in globals. */
/*
 * Store the initializer that is sitting on the stack, in a local slot or in
 * the global table.
 *
 * is_const reaches the globals table as a flag on the entry, so the binding
 * is constant from the moment it is created rather than only from the moment
 * the compiler next looks at it. That is what lets a const be written in one
 * file and assigned in another.
 */
static void define_variable(int global, bool is_const)
{
	if (state.current->scope_depth > 0) {
		mark_initialized();
		return;
	}

	/*
	 * `export let x` at the top level. The flag rides along in the opcode's
	 * spare operand byte rather than in a second opcode, because the
	 * ordinary case -- an unexported global -- must not grow to serve a
	 * case most modules do not use.
	 */
	if (exporting) {
		if (is_const) {
			emit_indexed(OP_DEFINE_GLOBAL_CONST_EXPORT,
			        OP_DEFINE_GLOBAL_CONST_EXPORT_LONG,
			        global);
		} else {
			emit_indexed(OP_DEFINE_GLOBAL_EXPORT,
			        OP_DEFINE_GLOBAL_EXPORT_LONG,
			        global);
		}
		return;
	}

	if (is_const) {
		/* a separate opcode rather than a third operand byte, so the
		 * encoding of the common `let` does not grow to pay for a
		 * case almost no script uses */
		emit_indexed(OP_DEFINE_GLOBAL_CONST,
		        OP_DEFINE_GLOBAL_CONST_LONG,
		        global);
		return;
	}

	emit_indexed(OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_LONG, global);
}

/* forward declarations. the expression and statement parsers call each other
 * freely, which C does not allow without these. */

static void expression(void);
static void statement(void);
static void declaration(void);
static void let_destructure(bool is_const);
static void fn_declaration(void);
static void anonymous_function(bool can_assign);
static void block(void);
static ParseRule *get_rule(TokenType type);
static void parse_precedence(Precedence precedence);

/* expression parsers */

/*
 * Pratt's algorithm, and the core of the compiler. Parse a prefix, then
 * keep consuming infix operators that bind at least as tightly as the
 * state.current precedence. That is the whole thing: a table lookup and a loop,
 * which is why this language has no operator precedence table written out
 * by hand and no shift/reduce conflicts.
 */
static void parse_precedence(Precedence precedence)
{
	advance();
	ParseFn prefix_rule = get_rule(state.parser.previous.type)->prefix;
	if (prefix_rule == NULL) {
		error("expect expression.");
		return;
	}

	/*
	 * Assignment is right-associative and looser than everything, so
	 * can_assign is only true at the outermost level. Nested calls do
	 * not get it, which is what rejects "f() = 1".
	 */
	bool can_assign = precedence <= PREC_ASSIGNMENT;
	prefix_rule(can_assign);

	while (precedence <= get_rule(state.parser.current.type)->precedence) {
		advance();
		ParseFn infix_rule =
		        get_rule(state.parser.previous.type)->infix;
		infix_rule(can_assign);
	}

	/*
	 * Having just parsed a full expression with no assignment in it, an
	 * `=` here is a syntax error. Catching it here gives a decent
	 * message instead of failing somewhere in the statement state.parser.
	 */
	if (can_assign &&
	        (match(TOKEN_EQUAL) || match(TOKEN_PLUS_EQUAL) ||
	                match(TOKEN_MINUS_EQUAL) || match(TOKEN_STAR_EQUAL) ||
	                match(TOKEN_SLASH_EQUAL)))
		error("invalid assignment target.");
}

static void number(bool can_assign)
{
	(void)can_assign;
	/*
	 * strtod, not a hand-written state.parser. The scanner has already
	 * validated the shape, and strtod is required to be correct by
	 * the standard, which is worth more than the microseconds.
	 */
	double value = strtod(state.parser.previous.start, NULL);
	emit_constant(NUMBER_VAL(value));
}

static void literal(bool can_assign)
{
	(void)can_assign;
	switch (state.parser.previous.type) {
	case TOKEN_FALSE:
		emit_byte(OP_FALSE);
		break;
	case TOKEN_NIL:
		emit_byte(OP_NIL);
		break;
	case TOKEN_TRUE:
		emit_byte(OP_TRUE);
		break;
	default:
		return;
	}
}

/*
 * Unescape a string literal into a fresh buffer, then intern it.
 *
 * Escape handling lives here rather than in the scanner because interning
 * allocates, and the compiler is the layer that owns allocation. Unknown
 * escapes keep the character, so "\q" is a q rather than an error.
 */
static void string(bool can_assign)
{
	(void)can_assign;
	const char *src = state.parser.previous.start + 1; /* skip the quote */
	int len = state.parser.previous.length - 2; /* and the far quote */

	/* the decoded form is never longer than the source */
	char *chars = (char *)malloc(len + 1);
	int out = 0;
	for (int i = 0; i < len; i++) {
		if (src[i] == '\\' && i + 1 < len) {
			i++;
			switch (src[i]) {
			case 'n':
				chars[out++] = '\n';
				break;
			case 't':
				chars[out++] = '\t';
				break;
			case 'r':
				chars[out++] = '\r';
				break;
			case '\\':
				chars[out++] = '\\';
				break;
			case '"':
				chars[out++] = '"';
				break;
			case '0':
				chars[out++] = '\0';
				break;
			default:
				chars[out++] = src[i];
				break;
			}
		} else {
			chars[out++] = src[i];
		}
	}

	ObjString *str = copy_string(state.vm, chars, out);
	free(chars);
	emit_constant(STR_VAL(str));
}

static void emit_variable_op(uint8_t op, int arg)
{
	if (op == OP_GET_GLOBAL)
		emit_indexed(OP_GET_GLOBAL, OP_GET_GLOBAL_LONG, arg);
	else if (op == OP_SET_GLOBAL)
		emit_indexed(OP_SET_GLOBAL, OP_SET_GLOBAL_LONG, arg);
	else
		emit_bytes(op, (uint8_t)arg);
}

/*
 * An identifier that is not being declared. Resolves to a local, an
 * upvalue, or a global, in that order, and picks the matching get and set
 * opcodes. The three cases are identical apart from the opcode, so they are
 * computed once and used twice.
 */
static void named_variable(Token name, bool can_assign)
{
	uint8_t get_op, set_op;
	int arg = resolve_local(state.current, &name);
	if (arg != -1) {
		get_op = OP_GET_LOCAL;
		set_op = OP_SET_LOCAL;
	} else if ((arg = resolve_upvalue(state.current, &name)) != -1) {
		get_op = OP_GET_UPVALUE;
		set_op = OP_SET_UPVALUE;
	} else {
		arg = identifier_constant(&name);
		get_op = OP_GET_GLOBAL;
		set_op = OP_SET_GLOBAL;
		/*
		 * A global read is a use of whatever binding it resolves to,
		 * imports included. Locals and upvalues resolve first above,
		 * so reaching here means the name really is global -- a local
		 * shadowing an import does not mark it used, which is correct:
		 * the import is still dead.
		 */
		import_mark_used(name.start, name.length);
	}

	if (can_assign && match(TOKEN_EQUAL)) {
		/* const is enforced here, at compile time, and only for
		 * locals. a global const is not protected: OP_SET_GLOBAL
		 * has no way to know. that is a bug. */
		if (get_op == OP_GET_LOCAL &&
		        state.current->locals[arg].is_const) {
			error("can't assign to constant.");
			return;
		}
		expression();
		emit_variable_op(set_op, arg);
	} else if (can_assign &&
	           (match(TOKEN_PLUS_EQUAL) || match(TOKEN_MINUS_EQUAL) ||
	                   match(TOKEN_STAR_EQUAL) ||
	                   match(TOKEN_SLASH_EQUAL))) {
		if (get_op == OP_GET_LOCAL &&
		        state.current->locals[arg].is_const) {
			error("can't assign to constant.");
			return;
		}
		TokenType op = state.parser.previous.type;
		/* read, modify, write. no in-place bytecode for this, so the
		 * value is loaded, combined and stored. */
		emit_variable_op(get_op, arg);
		expression();
		switch (op) {
		case TOKEN_PLUS_EQUAL:
			emit_byte(OP_ADD);
			break;
		case TOKEN_MINUS_EQUAL:
			emit_byte(OP_SUBTRACT);
			break;
		case TOKEN_STAR_EQUAL:
			emit_byte(OP_MULTIPLY);
			break;
		case TOKEN_SLASH_EQUAL:
			emit_byte(OP_DIVIDE);
			break;
		default:
			break;
		}
		emit_variable_op(set_op, arg);
	} else {
		emit_variable_op(get_op, arg);
	}
}

static void variable(bool can_assign)
{
	named_variable(state.parser.previous, can_assign);
}

static void grouping(bool can_assign)
{
	(void)can_assign;
	expression();
	consume(TOKEN_RIGHT_PAREN, "expect ')' after expression.");
}

/* a prefix operator: parse tighter, then emit. postfix is below. */
static void unary(bool can_assign)
{
	(void)can_assign;
	TokenType op = state.parser.previous.type;
	parse_precedence(PREC_UNARY);

	switch (op) {
	case TOKEN_NOT:
	case TOKEN_BANG:
		emit_byte(OP_NOT);
		break;
	case TOKEN_MINUS:
		emit_byte(OP_NEGATE);
		break;
	default:
		return;
	}
}

/*
 * An infix operator. Parse one level tighter than the operator's own
 * precedence, which is what makes the operators left-associative, then emit.
 *
 * Do not fold the comparisons together. NaN is unordered, so a <= b is not
 * !(a > b) and the test suite checks exactly that.
 */
static void binary(bool can_assign)
{
	(void)can_assign;
	TokenType op = state.parser.previous.type;
	ParseRule *rule = get_rule(op);
	parse_precedence((Precedence)(rule->precedence + 1));

	switch (op) {
	case TOKEN_BANG_EQUAL:
		emit_byte(OP_NOT_EQUAL);
		break;
	case TOKEN_EQUAL_EQUAL:
		emit_byte(OP_EQUAL);
		break;
	case TOKEN_GREATER:
		emit_byte(OP_GREATER);
		break;
	case TOKEN_GREATER_EQUAL:
		emit_byte(OP_GREATER_EQUAL);
		break;
	case TOKEN_LESS:
		emit_byte(OP_LESS);
		break;
	case TOKEN_LESS_EQUAL:
		emit_byte(OP_LESS_EQUAL);
		break;
	case TOKEN_PLUS:
		emit_byte(OP_ADD);
		break;
	case TOKEN_MINUS:
		emit_byte(OP_SUBTRACT);
		break;
	case TOKEN_STAR:
		emit_byte(OP_MULTIPLY);
		break;
	case TOKEN_SLASH:
		emit_byte(OP_DIVIDE);
		break;
	case TOKEN_PERCENT:
		emit_byte(OP_MODULO);
		break;
	default:
		return;
	}
}

/*
 * `a and b`: evaluate a, jump to the end if falsy, otherwise drop a and
 * evaluate b. The value of the expression is b in that case, and the
 * compiler must not know which branch ran.
 */
static void and_(bool can_assign)
{
	(void)can_assign;
	int end_jump = emit_jump(OP_JUMP_IF_FALSE);
	emit_byte(OP_POP);
	parse_precedence(PREC_AND);
	patch_jump(end_jump);
}

/* `a or b`: the mirror. on the short path a is truthy, so it is the result
 * and must be preserved by jumping around the evaluation of b. */
static void or_(bool can_assign)
{
	(void)can_assign;
	int else_jump = emit_jump(OP_JUMP_IF_FALSE);
	int end_jump = emit_jump(OP_JUMP);
	patch_jump(else_jump);
	emit_byte(OP_POP);
	parse_precedence(PREC_OR);
	patch_jump(end_jump);
}
/*
 * `a ?? b`: b when a is nil, a otherwise, with b evaluated only if needed.
 *
 * One jump, not two. The value is already on top of the stack: if it is not
 * nil, jump past the fallback and keep it; if it is nil, fall through, pop
 * it, and evaluate the right side in its place. Either path leaves exactly
 * one value, which is what makes this compose: `a ?? b ?? c` nests without
 * stack bookkeeping.
 *
 * Right-associative, like assignment: the right side parses at the same
 * level, so `a ?? b ?? c` is `a ?? (b ?? c)`. Left would evaluate the middle
 * before knowing whether the left needs it, which defeats the short circuit
 * this exists for.
 *
 * Only nil triggers the fallback. False, 0 and "" are all values that stay,
 * which is the entire difference from `or` and the reason both exist.
 */
static void coalesce(bool can_assign)
{
	(void)can_assign;
	int end_jump = emit_jump(OP_JUMP_IF_NOT_NIL);
	emit_byte(OP_POP);
	parse_precedence(PREC_COALESCE);
	patch_jump(end_jump);
}

/*
 * `expr as T` -- a checked type assertion.
 *
 * The value is checked and left alone. There is nothing to convert: flint has
 * one numeric type, and every other type is already distinguished at the value
 * level, so a conversion operator would have to invent a policy for every
 * pair of types and then document the pairs it got wrong. `str()` is the
 * conversion. `as` is the assertion, and its whole value is that a mismatch is
 * a named error at the line you wrote rather than a nil three functions later.
 *
 * The type name is resolved here, at compile time, so a typo is a compile
 * error instead of a runtime one. The names are exactly the seven type()
 * returns, which is the only list of type names in the language.
 */
static void as_(bool can_assign)
{
	(void)can_assign;

	/*
	 * `nil` is a keyword, so it cannot arrive here as an identifier the
	 * way the other six type names do. Take it as its own case rather
	 * than special-casing the scanner, which would make `nil` stop
	 * being a value in every other position.
	 */
	if (match(TOKEN_NIL)) {
		emit_bytes(OP_CAST, (uint8_t)FL_TYPE_NIL);
		return;
	}

	consume(TOKEN_IDENTIFIER, "expect a type name after 'as'.");

	int tag = -1;
	if (identifier_is("number"))
		tag = FL_TYPE_NUMBER;
	else if (identifier_is("string"))
		tag = FL_TYPE_STRING;
	else if (identifier_is("bool"))
		tag = FL_TYPE_BOOL;
	else if (identifier_is("nil"))
		tag = FL_TYPE_NIL;
	else if (identifier_is("list"))
		tag = FL_TYPE_LIST;
	else if (identifier_is("table"))
		tag = FL_TYPE_TABLE;
	else if (identifier_is("function"))
		tag = FL_TYPE_FUNCTION;

	if (tag < 0) {
		error("unknown type name after 'as'. Expected one of: "
		      "number, string, bool, nil, list, table, function.");
		return;
	}

	emit_bytes(OP_CAST, (uint8_t)tag);
}

/* arguments, as a count. 255 is the byte limit on the call opcode. */
static uint8_t argument_list(void)
{
	uint8_t argc = 0;
	if (!check(TOKEN_RIGHT_PAREN)) {
		do {
			expression();
			if (argc == 255)
				error("can't have more than 255 arguments.");
			argc++;
		} while (match(TOKEN_COMMA));
	}
	consume(TOKEN_RIGHT_PAREN, "expect ')' after arguments.");
	return argc;
}

static void call(bool can_assign)
{
	(void)can_assign;
	uint8_t argc = argument_list();
	emit_bytes(OP_CALL, argc);
}

/*
 * A list literal. Every element is left on the stack, and OP_BUILD_LIST takes
 * a count and consumes exactly that many. The count is a byte, so 255 is
 * the limit.
 */
static void list_literal(bool can_assign)
{
	(void)can_assign;
	int count = 0;
	if (!check(TOKEN_RIGHT_BRACKET)) {
		do {
			expression();
			count++;
			/*
			 * A trailing comma is allowed: `[1, 2,]` is a
			 * two-element list. A literal written across
			 * lines almost always ends in a comma, and
			 * rejecting that made the shape people actually
			 * type the one shape that did not parse. The
			 * `]` is what ends the list either way, so the
			 * loop condition is the comma *and* not the
			 * closer.
			 */
		} while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_BRACKET));
	}
	consume(TOKEN_RIGHT_BRACKET, "expect ']' after list.");
	if (count > 255)
		error("can't have more than 255 elements in a list literal.");
	emit_bytes(OP_BUILD_LIST, (uint8_t)count);
}

/* a[i], and a[i] = v. the value form is an lvalue like any other. */
static void subscript(bool can_assign)
{
	expression();
	consume(TOKEN_RIGHT_BRACKET, "expect ']' after index.");

	if (can_assign && match(TOKEN_EQUAL)) {
		expression();
		emit_byte(OP_SET_INDEX);
	} else {
		emit_byte(OP_GET_INDEX);
	}
}

/* t.name and t.name = v. the name is a bare identifier, so there is no
 * string literal to lex here. */
static void dot(bool can_assign)
{
	consume(TOKEN_IDENTIFIER, "expect field name after '.'.");
	int name = identifier_constant(&state.parser.previous);

	if (can_assign && match(TOKEN_EQUAL)) {
		expression();
		emit_indexed(OP_SET_FIELD, OP_SET_FIELD_LONG, name);
	} else {
		emit_indexed(OP_GET_FIELD, OP_GET_FIELD_LONG, name);
	}
}

/*
 * A table literal: an empty table, then one OP_SET_FIELD_TOP per pair.
 *
 * The hard part is that the field opcode has to leave the table on the
 * stack, because the next pair needs it again. The original version parked
 * the table in a hidden local and reloaded it between pairs, which only works
 * while the table is the topmost thing on the stack. It is not, whenever the
 * literal is an argument: in `type({a:1})` the callee is pushed before the
 * argument, so the hidden local landed on the callee and the literal set a
 * field on the function instead of on the table.
 *
 * OP_SET_FIELD_TOP reads the table from below the value and leaves it there,
 * so the table is never taken off the stack at all. No slot, no hidden scope,
 * and it works in any expression position.
 */
static void table_literal(bool can_assign)
{
	(void)can_assign;
	emit_byte(OP_BUILD_TABLE);

	if (!check(TOKEN_RIGHT_BRACE)) {
		do {
			consume(TOKEN_IDENTIFIER,
			        "expect key name in table literal.");
			int name = identifier_constant(&state.parser.previous);
			consume(TOKEN_COLON,
			        "expect ':' after key in table literal.");

			/*
			 * The value is pushed on top of the table, and
			 * OP_SET_FIELD_TOP consumes it and leaves the table
			 * where it is. So each pair leaves the stack exactly
			 * as it found it, and the table never has to be
			 * stashed anywhere to survive to the next pair.
			 */
			expression();
			emit_indexed(
			        OP_SET_FIELD_TOP, OP_SET_FIELD_TOP_LONG, name);
			/* trailing comma, for the same reason as a list:
			 * see list_literal() */
		} while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_BRACE));
	}
	consume(TOKEN_RIGHT_BRACE, "expect '}' after table literal.");
}

/*
 * The operator table, indexed by TokenType. Designated initializers, so a new
 * token type defaults to no rules and shows up here as a hole rather than
 * silently becoming a keyword.
 *
 * A prefix rule parses something that starts an expression; an infix rule
 * parses one that follows a complete expression. An operator with only an
 * infix rule, like `+`, cannot start an expression.
 */
static ParseRule rules[] = {
        [TOKEN_LEFT_PAREN] = {grouping, call, PREC_CALL},
        [TOKEN_RIGHT_PAREN] = {NULL, NULL, PREC_NONE},
        [TOKEN_LEFT_BRACE] = {table_literal, NULL, PREC_NONE},
        [TOKEN_RIGHT_BRACE] = {NULL, NULL, PREC_NONE},
        [TOKEN_LEFT_BRACKET] = {list_literal, subscript, PREC_CALL},
        [TOKEN_RIGHT_BRACKET] = {NULL, NULL, PREC_NONE},
        [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},
        [TOKEN_DOT] = {NULL, dot, PREC_CALL},
        [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},
        [TOKEN_COLON] = {NULL, NULL, PREC_NONE},
        [TOKEN_PLUS] = {NULL, binary, PREC_TERM},
        [TOKEN_MINUS] = {unary, binary, PREC_TERM},
        [TOKEN_STAR] = {NULL, binary, PREC_FACTOR},
        [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},
        [TOKEN_PERCENT] = {NULL, binary, PREC_FACTOR},
        [TOKEN_BANG] = {unary, NULL, PREC_NONE},
        [TOKEN_BANG_EQUAL] = {NULL, binary, PREC_EQUALITY},
        [TOKEN_EQUAL] = {NULL, NULL, PREC_NONE},
        [TOKEN_EQUAL_EQUAL] = {NULL, binary, PREC_EQUALITY},
        [TOKEN_GREATER] = {NULL, binary, PREC_COMPARISON},
        [TOKEN_GREATER_EQUAL] = {NULL, binary, PREC_COMPARISON},
        [TOKEN_LESS] = {NULL, binary, PREC_COMPARISON},
        [TOKEN_LESS_EQUAL] = {NULL, binary, PREC_COMPARISON},
        [TOKEN_DOT_DOT] = {NULL, NULL, PREC_NONE},
        [TOKEN_PLUS_EQUAL] = {NULL, NULL, PREC_NONE},
        [TOKEN_MINUS_EQUAL] = {NULL, NULL, PREC_NONE},
        [TOKEN_STAR_EQUAL] = {NULL, NULL, PREC_NONE},
        [TOKEN_SLASH_EQUAL] = {NULL, NULL, PREC_NONE},
        [TOKEN_IDENTIFIER] = {variable, NULL, PREC_NONE},
        [TOKEN_STRING] = {string, NULL, PREC_NONE},
        [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
        [TOKEN_AND] = {NULL, and_, PREC_AND},
        [TOKEN_AS] = {NULL, as_, PREC_CAST},
        [TOKEN_BREAK] = {NULL, NULL, PREC_NONE},
        [TOKEN_CONST] = {NULL, NULL, PREC_NONE},
        [TOKEN_CONTINUE] = {NULL, NULL, PREC_NONE},
        [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
        [TOKEN_EXPORT] = {NULL, NULL, PREC_NONE},
        [TOKEN_FALSE] = {literal, NULL, PREC_NONE},
        [TOKEN_FN] = {anonymous_function, NULL, PREC_NONE},
        [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
        [TOKEN_IF] = {NULL, NULL, PREC_NONE},
        [TOKEN_IMPORT] = {NULL, NULL, PREC_NONE},
        [TOKEN_IN] = {NULL, NULL, PREC_NONE},
        [TOKEN_LET] = {NULL, NULL, PREC_NONE},
        [TOKEN_NIL] = {literal, NULL, PREC_NONE},
        [TOKEN_NOT] = {unary, NULL, PREC_NONE},
        [TOKEN_OR] = {NULL, or_, PREC_OR},
        [TOKEN_QUESTION_QUESTION] = {NULL, coalesce, PREC_COALESCE},
        [TOKEN_PRINT] = {NULL, NULL, PREC_NONE},
        [TOKEN_RETURN] = {NULL, NULL, PREC_NONE},
        [TOKEN_TRUE] = {literal, NULL, PREC_NONE},
        [TOKEN_WHILE] = {NULL, NULL, PREC_NONE},
        [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
        [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
};

static ParseRule *get_rule(TokenType type) { return &rules[type]; }

static void expression(void) { parse_precedence(PREC_ASSIGNMENT); }

/* statement termination */

/*
 * A statement ends at a semicolon, a newline, a closing brace or EOF. The
 * newline case is why the scanner tracks newline_before rather than treating
 * newlines as plain whitespace.
 */
static void consume_terminator(void)
{
	if (match(TOKEN_SEMICOLON))
		return;
	if (state.parser.current.newline_before)
		return;
	if (check(TOKEN_EOF) || check(TOKEN_RIGHT_BRACE))
		return;
	error_at_current("expect ';' or newline after statement.");
}

/* statement parsing */

static void block(void)
{
	while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF))
		declaration();
	consume(TOKEN_RIGHT_BRACE, "expect '}' after block.");
}

/*
 * Close everything above a scope depth without ending the scope. break and
 * continue jump out of blocks rather than falling out of them, so they have
 * to retire the locals in between by hand. Same opcodes as end_scope().
 */
static void emit_close_upvalues_to(int depth)
{
	for (int i = state.current->local_count - 1; i >= 0; i--) {
		Local *local = &state.current->locals[i];
		if (local->depth <= depth)
			break;
		if (local->is_captured)
			emit_byte(OP_CLOSE_UPVALUE);
		else
			emit_byte(OP_POP);
	}
}

/*
 * try { ... } catch err { ... }
 *
 * OP_TRY pushes a handler and the catch block doubles as the jump target
 * for errors. OP_POP_HANDLER retires the handler when the body completes
 * normally. The catch clause binds the error value as an ordinary local:
 * when the VM unwinds to the handler it pushes the error value at exactly
 * the stack depth the try block began, which is where the catch body's
 * first local belongs. No binding, and an OP_POP discards it instead.
 */
static void try_statement(void)
{
	consume(TOKEN_LEFT_BRACE, "expect '{' after try.");

	int jump = emit_jump(OP_TRY);
	state.current->try_depth++;
	state.try_nesting++;
	begin_scope();
	block();
	state.try_nesting--;
	end_scope();
	state.current->try_depth--;
	emit_byte(OP_POP_HANDLER);
	int skip = emit_jump(OP_JUMP);

	patch_jump(jump);

	if (!match(TOKEN_CATCH)) {
		error("expect 'catch' after try block.");
		return;
	}
	begin_scope();
	bool have_binding = match(TOKEN_IDENTIFIER);
	Token name = state.parser.previous;
	if (!have_binding)
		emit_byte(OP_POP);
	else {
		add_local(name, false);
		mark_initialized();
	}
	consume(TOKEN_LEFT_BRACE, "expect '{' after catch.");
	block();
	end_scope();
	patch_jump(skip);
}

/* throw expr: raise the value to the innermost handler, or the top. */
static void throw_statement(void)
{
	expression();
	emit_byte(OP_THROW);
	consume_terminator();
}

static void print_statement(void)
{
	consume(TOKEN_LEFT_PAREN, "expect '(' after 'print'.");
	expression();
	emit_byte(OP_PRINT);
	/* print(a, b, c) is not a tuple, it is three prints. one opcode per
	 * argument, so there is no temporary to worry about */
	while (match(TOKEN_COMMA)) {
		expression();
		emit_byte(OP_PRINT);
	}
	consume(TOKEN_RIGHT_PAREN, "expect ')' after arguments.");
	consume_terminator();
}

/*
 * The jump sequence here is the standard one, and the pops are the part
 * people get wrong: OP_JUMP_IF_FALSE peeks rather than pops, so exactly one
 * POP is needed on each path, and the else's POP comes after the then-jump
 * is patched.
 */
static void if_statement(void)
{
	expression();
	int then_jump = emit_jump(OP_JUMP_IF_FALSE);
	emit_byte(OP_POP); /* the condition, on the then path */

	consume(TOKEN_LEFT_BRACE, "expect '{' after if condition.");
	begin_scope();
	block();
	end_scope();

	int else_jump = emit_jump(OP_JUMP);
	patch_jump(then_jump);
	emit_byte(OP_POP); /* the condition, on the else path */

	if (match(TOKEN_ELSE)) {
		if (match(TOKEN_IF)) {
			if_statement(); /* else if, recursively */
		} else {
			consume(TOKEN_LEFT_BRACE, "expect '{' after else.");
			begin_scope();
			block();
			end_scope();
		}
	}
	patch_jump(else_jump);
}

/*
 * while cond { body }
 *
 * The loop head is recorded before the condition, because continue can only
 * be patched at the end and has to jump somewhere. A while loop's continue
 * target is the condition, so it can be a back-edge.
 */
static void while_statement(void)
{
	LoopContext loop;
	loop.enclosing = state.loop;
	loop.try_depth = state.current->try_depth;
	loop.scope_depth = state.current->scope_depth;
	loop.start = current_chunk()->count;
	loop.continue_target = loop.start; /* re-test the condition */
	loop.break_count = 0;
	loop.continue_count = 0;
	state.loop = &loop;

	expression();
	int exit_jump = emit_jump(OP_JUMP_IF_FALSE);
	emit_byte(OP_POP);

	consume(TOKEN_LEFT_BRACE, "expect '{' after while condition.");
	begin_scope();
	block();
	end_scope();

	/* continue jumps land here, on the increment side of the back edge */
	for (int i = 0; i < loop.continue_count; i++)
		patch_jump(loop.continue_jumps[i]);

	emit_loop(loop.start);

	patch_jump(exit_jump);
	emit_byte(OP_POP);

	for (int i = 0; i < loop.break_count; i++)
		patch_jump(loop.break_jumps[i]);

	state.loop = loop.enclosing;
}

/*
 * for x in a..b { body }   or   for x in list { body }
 *
 * Both forms desugar to hidden locals and an index-style loop; there is no
 * range opcode. The range form needs one hidden local for the limit, the
 * list form needs two, for the list and the position.
 */
static void for_statement(void)
{
	begin_scope();

	consume(TOKEN_IDENTIFIER, "expect variable name after 'for'.");
	Token var_name = state.parser.previous;

	/*
	 * `for k, v in table` iterates entries. The comma is what selects
	 * table iteration rather than list iteration: a single variable
	 * ranges, lists and strings, and two variables always mean table
	 * pairs. That keeps one `for` with two shapes instead of two loops,
	 * and it means the compiler never has to guess a container's type --
	 * the syntax already said.
	 */
	bool pair_mode = false;
	Token val_name = var_name;
	if (match(TOKEN_COMMA)) {
		consume(TOKEN_IDENTIFIER,
		        "expect value name after ',' in for loop.");
		val_name = state.parser.previous;
		pair_mode = true;
	}
	consume(TOKEN_IN, "expect 'in' after for variable.");

	/*
	 * Anything that can start an expression. Asking the rules table beats
	 * listing the tokens by hand: the old version of this guard allowed
	 * only a number, an identifier and '(', so `for x in [1, 2]` was a
	 * syntax error while `let l = [1, 2]; for x in l` worked. A hand list
	 * has to be updated every time a prefix rule is added, and forgetting
	 * is a syntax error in one construct and nowhere else.
	 */
	if (get_rule(state.parser.current.type)->prefix != NULL) {
		expression();

		/* a range if a ".." followed the first expression */
		if (match(TOKEN_DOT_DOT)) {
			/*
			 * A pair loop over a range is meaningless -- ranges
			 * yield one value per step, and there is no key to
			 * pair it with. Say so here rather than emitting a
			 * loop that misbehaves.
			 */
			if (pair_mode) {
				error("cannot iterate a range with two "
				      "variables; ranges yield one value.");
				return;
			}
			/* stack: [start] then [start][end], then
			 * [start][end][step] when a step was written */
			expression();

			/*
			 * `a..b..s` steps the range. The step defaults to 1
			 * and its sign decides both the comparison and the
			 * direction, so the loop below branches on the sign
			 * rather than being compiled twice: a step is usually a
			 * literal, but it can be a variable, and two copies
			 * of this loop would have to agree about everything
			 * else.
			 */
			bool has_step = match(TOKEN_DOT_DOT);
			if (has_step)
				expression();

			/* the user's variable takes the start slot, and hidden
			 * locals take the end and the step. */
			add_local(var_name, false);
			mark_initialized();
			Token hidden = {TOKEN_IDENTIFIER,
			        " end",
			        4,
			        var_name.line,
			        false,
			        var_name.offset};
			add_local(hidden, false);
			mark_initialized();

			/* the step is on the stack only when it was written;
			 * otherwise the constant one goes here. */
			if (!has_step)
				emit_constant(NUMBER_VAL(1));
			Token hidden_step = {TOKEN_IDENTIFIER,
			        " step",
			        5,
			        var_name.line,
			        false,
			        var_name.offset};
			add_local(hidden_step, false);
			mark_initialized();

			LoopContext loop;
			loop.enclosing = state.loop;
			loop.try_depth = state.current->try_depth;
			loop.scope_depth = state.current->scope_depth;
			loop.start = current_chunk()->count;
			/* the increment is emitted below, so no single offset
			 * is known yet; continue waits for a patch */
			loop.continue_target = -1;
			loop.break_count = 0;
			loop.continue_count = 0;
			state.loop = &loop;

			int var_slot = state.current->local_count - 3;
			int end_slot = state.current->local_count - 2;
			int step_slot = state.current->local_count - 1;

			/*
			 * condition: step >= 0 ? var < end : var > end
			 *
			 * The sign is tested once per iteration rather than
			 * once per loop, because the step can be a variable
			 * and only its value at loop entry decides which
			 * direction the whole loop runs in. Hoisting that test
			 * out of the loop is what keeps this one loop instead
			 * of two, and a zero step is refused at run time
			 * rather than looping forever.
			 */
			emit_bytes(OP_GET_LOCAL, (uint8_t)step_slot);
			emit_constant(NUMBER_VAL(0));
			emit_byte(OP_LESS_EQUAL);

			int up_jump = emit_jump(OP_JUMP_IF_FALSE);
			emit_byte(OP_POP);

			/* ascending */
			emit_bytes(OP_GET_LOCAL, (uint8_t)var_slot);
			emit_bytes(OP_GET_LOCAL, (uint8_t)end_slot);
			emit_byte(OP_LESS);
			int ascending_exit = emit_jump(OP_JUMP_IF_FALSE);
			emit_byte(OP_POP);
			int to_body = emit_jump(OP_JUMP);

			patch_jump(up_jump);
			emit_byte(OP_POP); /* the step >= 0 test */

			/* descending */
			emit_bytes(OP_GET_LOCAL, (uint8_t)var_slot);
			emit_bytes(OP_GET_LOCAL, (uint8_t)end_slot);
			emit_byte(OP_GREATER);
			int exit_jump = emit_jump(OP_JUMP_IF_FALSE);
			emit_byte(OP_POP);

			patch_jump(to_body);

			consume(TOKEN_LEFT_BRACE,
			        "expect '{' after for range.");
			begin_scope();
			block();
			end_scope();

			for (int i = 0; i < loop.continue_count; i++)
				patch_jump(loop.continue_jumps[i]);

			/* increment: var = var + 1 */
			emit_bytes(OP_GET_LOCAL, (uint8_t)var_slot);
			emit_constant(NUMBER_VAL(1));
			emit_byte(OP_ADD);
			emit_bytes(OP_SET_LOCAL, (uint8_t)var_slot);
			emit_byte(OP_POP);

			emit_loop(loop.start);
			patch_jump(exit_jump);
			emit_byte(OP_POP);

			for (int i = 0; i < loop.break_count; i++)
				patch_jump(loop.break_jumps[i]);

			state.loop = loop.enclosing;
			end_scope();
			return;
		}

		/*
		 * Table iteration. The table expression is already on the
		 * stack, exactly as with lists, and the shape below mirrors
		 * the list loop on purpose: hidden table, hidden index, two
		 * user variables filled per iteration. Reading the two side by
		 * side should show the same loop with different loads.
		 *
		 * The count is re-read every iteration through OP_TABLE_COUNT,
		 * so entries appended in the body are visited. Entries removed
		 * shift everything after them down by position, which the
		 * documentation states plainly rather than preventing: a loop
		 * that mutates its own table is the author's responsibility,
		 * and the behaviour is positional rather than surprising.
		 */
		if (pair_mode) {
			Token hidden_table = {TOKEN_IDENTIFIER,
			        " table",
			        6,
			        var_name.line,
			        false,
			        var_name.offset};
			add_local(hidden_table, false);
			mark_initialized();

			emit_constant(NUMBER_VAL(0));
			Token hidden_tidx = {TOKEN_IDENTIFIER,
			        " tidx",
			        5,
			        var_name.line,
			        false,
			        var_name.offset};
			add_local(hidden_tidx, false);
			mark_initialized();

			/* key and value both start nil, filled per iteration */
			emit_byte(OP_NIL);
			add_local(var_name, false);
			mark_initialized();
			emit_byte(OP_NIL);
			add_local(val_name, false);
			mark_initialized();

			int table_slot = state.current->local_count - 4;
			int tidx_slot = state.current->local_count - 3;
			int key_slot = state.current->local_count - 2;
			int val_slot = state.current->local_count - 1;

			LoopContext loop;
			loop.enclosing = state.loop;
			loop.try_depth = state.current->try_depth;
			loop.scope_depth = state.current->scope_depth;
			loop.start = current_chunk()->count;
			loop.continue_target = -1;
			loop.break_count = 0;
			loop.continue_count = 0;
			state.loop = &loop;

			/* condition: idx < table_count(table) */
			emit_bytes(OP_GET_LOCAL, (uint8_t)tidx_slot);
			emit_bytes(OP_GET_LOCAL, (uint8_t)table_slot);
			emit_byte(OP_TABLE_COUNT);
			emit_byte(OP_LESS);

			int exit_jump = emit_jump(OP_JUMP_IF_FALSE);
			emit_byte(OP_POP);

			/* key = table_key(table, idx) */
			emit_bytes(OP_GET_LOCAL, (uint8_t)table_slot);
			emit_bytes(OP_GET_LOCAL, (uint8_t)tidx_slot);
			emit_byte(OP_TABLE_KEY);
			emit_bytes(OP_SET_LOCAL, (uint8_t)key_slot);
			emit_byte(OP_POP);

			/* value = table_value(table, idx) */
			emit_bytes(OP_GET_LOCAL, (uint8_t)table_slot);
			emit_bytes(OP_GET_LOCAL, (uint8_t)tidx_slot);
			emit_byte(OP_TABLE_VALUE);
			emit_bytes(OP_SET_LOCAL, (uint8_t)val_slot);
			emit_byte(OP_POP);

			consume(TOKEN_LEFT_BRACE,
			        "expect '{' after for-in expression.");
			begin_scope();
			block();
			end_scope();

			for (int i = 0; i < loop.continue_count; i++)
				patch_jump(loop.continue_jumps[i]);

			/* idx = idx + 1 */
			emit_bytes(OP_GET_LOCAL, (uint8_t)tidx_slot);
			emit_constant(NUMBER_VAL(1));
			emit_byte(OP_ADD);
			emit_bytes(OP_SET_LOCAL, (uint8_t)tidx_slot);
			emit_byte(OP_POP);

			emit_loop(loop.start);
			patch_jump(exit_jump);
			emit_byte(OP_POP);

			for (int i = 0; i < loop.break_count; i++)
				patch_jump(loop.break_jumps[i]);

			state.loop = loop.enclosing;
			end_scope();
			return;
		}

		/* List iteration. The list expression is already on the
		 * stack, so it becomes the first hidden local. */
		Token hidden_list = {TOKEN_IDENTIFIER,
		        " list",
		        5,
		        var_name.line,
		        false,
		        var_name.offset};
		add_local(hidden_list, false);
		mark_initialized();

		/* the index starts at zero */
		emit_constant(NUMBER_VAL(0));
		Token hidden_idx = {TOKEN_IDENTIFIER,
		        " idx",
		        4,
		        var_name.line,
		        false,
		        var_name.offset};
		add_local(hidden_idx, false);
		mark_initialized();

		/* the user's variable starts as nil and is filled in by the
		 * first statement of the loop body */
		emit_byte(OP_NIL);
		add_local(var_name, false);
		mark_initialized();

		int list_slot = state.current->local_count - 3;
		int idx_slot = state.current->local_count - 2;
		int var_slot = state.current->local_count - 1;

		LoopContext loop;
		loop.enclosing = state.loop;
		loop.try_depth = state.current->try_depth;
		loop.scope_depth = state.current->scope_depth;
		loop.start = current_chunk()->count;
		loop.continue_target = -1;
		loop.break_count = 0;
		loop.continue_count = 0;
		state.loop = &loop;

		/*
		 * Condition: idx < len(list).
		 *
		 * OP_LIST_LEN reads the count out of the list header. It used
		 * to load the `len` global, push the list, and make a real call
		 * into C on every single iteration -- a global hash lookup, an
		 * arity check and a native frame, to read an int that is
		 * already in the object.
		 */
		emit_bytes(OP_GET_LOCAL, (uint8_t)idx_slot);
		emit_bytes(OP_GET_LOCAL, (uint8_t)list_slot);
		emit_byte(OP_LIST_LEN);

		emit_byte(OP_LESS);

		int exit_jump = emit_jump(OP_JUMP_IF_FALSE);
		emit_byte(OP_POP);

		/* var = list[idx] */
		emit_bytes(OP_GET_LOCAL, (uint8_t)list_slot);
		emit_bytes(OP_GET_LOCAL, (uint8_t)idx_slot);
		emit_byte(OP_GET_INDEX);
		emit_bytes(OP_SET_LOCAL, (uint8_t)var_slot);
		emit_byte(OP_POP);

		consume(TOKEN_LEFT_BRACE,
		        "expect '{' after for-in expression.");
		begin_scope();
		block();
		end_scope();

		for (int i = 0; i < loop.continue_count; i++)
			patch_jump(loop.continue_jumps[i]);

		/* idx = idx + 1 */
		emit_bytes(OP_GET_LOCAL, (uint8_t)idx_slot);
		emit_constant(NUMBER_VAL(1));
		emit_byte(OP_ADD);
		emit_bytes(OP_SET_LOCAL, (uint8_t)idx_slot);
		emit_byte(OP_POP);

		emit_loop(loop.start);
		patch_jump(exit_jump);
		emit_byte(OP_POP);

		for (int i = 0; i < loop.break_count; i++)
			patch_jump(loop.break_jumps[i]);

		state.loop = loop.enclosing;
	}

	end_scope();
}

/* break: retire the locals of the blocks being left, then jump out. */
static void break_statement(void)
{
	if (state.loop == NULL) {
		error("can't use 'break' outside of a loop.");
		return;
	}

	emit_close_upvalues_to(state.loop->scope_depth);

	/*
	 * break jumps to the loop's end, so every try block opened
	 * inside the loop since it began has to retire its handler.
	 * Exactly the trys lexically between this break and the loop.
	 */
	for (int i = state.current->try_depth - state.loop->try_depth; i > 0;
	        i--)
		emit_byte(OP_POP_HANDLER);

	/* the offsets are patched once the loop body is complete */
	if (state.loop->break_count >= 256) {
		error("too many break statements in loop.");
		return;
	}
	state.loop->break_jumps[state.loop->break_count++] = emit_jump(OP_JUMP);
	consume_terminator();
}

/*
 * continue: same cleanup, different destination. A while loop can jump
 * straight back to the condition, since it is already known. A for loop
 * cannot, because the increment has not been emitted yet, so it gets a
 * forward jump patched to the same place after the fact.
 */
static void continue_statement(void)
{
	if (state.loop == NULL) {
		error("can't use 'continue' outside of a loop.");
		return;
	}

	emit_close_upvalues_to(state.loop->scope_depth);

	/* retire the handlers of trys between here and the loop head */
	for (int i = state.current->try_depth - state.loop->try_depth; i > 0;
	        i--)
		emit_byte(OP_POP_HANDLER);

	if (state.loop->continue_target == -1) {
		if (state.loop->continue_count >= 256) {
			error("too many continue statements in loop.");
			return;
		}
		state.loop->continue_jumps[state.loop->continue_count++] =
		        emit_jump(OP_JUMP);
	} else {
		emit_loop(state.loop->continue_target);
	}
	consume_terminator();
}

static void return_statement(void)
{
	/* the top level is a function too, but it has no caller */
	if (state.current->type == TYPE_SCRIPT)
		error("can't return from top-level code.");

	/*
	 * The return value is computed first, then the handlers are
	 * retired. The other order looks equivalent and is not: an error
	 * raised while computing the value has to reach a handler in this
	 * function, and a handler already popped is a handler that is
	 * gone. `try { return [][1] } catch e {}` has to catch.
	 */
	if (check(TOKEN_SEMICOLON) || check(TOKEN_RIGHT_BRACE) ||
	        state.parser.current.newline_before || check(TOKEN_EOF)) {
		for (int i = 0; i < state.current->try_depth; i++)
			emit_byte(OP_POP_HANDLER);
		emit_return();
	} else {
		expression();
		for (int i = 0; i < state.current->try_depth; i++)
			emit_byte(OP_POP_HANDLER);
		emit_byte(OP_RETURN);
	}
	consume_terminator();
}

/* an expression statement. the result is discarded, so it is popped. */
/*
 * An expression used as a statement.
 *
 * In a script the value is thrown away, which is what OP_POP is for. At the
 * repl it is not thrown away: a person who types `1 + 2` is asking what the
 * answer is, and a repl that says nothing is a calculator with the screen
 * off. So in echo mode the POP is left off and the value is left on the
 * stack for the caller to print.
 *
 * The flag is checked against the script's own function, not "depth 0",
 * because a nested function inside the repl input is a statement, not
 * something to echo.
 */
static void expression_statement(void)
{
	expression();
	consume_terminator();
	if (state.echo_repl_value && state.current->type == TYPE_SCRIPT) {
		state.left_value_on_stack = true;
		/* return it rather than discarding it and falling out
		 * through emit_return, which would push nil on top and
		 * the repl would print nil every time. */
		emit_byte(OP_RETURN);
		return;
	}
	emit_byte(OP_POP);
}

static void statement(void)
{
	/* a chain of matches rather than a switch, because every branch has
	 * to call a function and the common cases are all keywords */
	if (match(TOKEN_PRINT)) {
		print_statement();
	} else if (match(TOKEN_IF)) {
		if_statement();
	} else if (match(TOKEN_WHILE)) {
		while_statement();
	} else if (match(TOKEN_FOR)) {
		for_statement();
	} else if (match(TOKEN_BREAK)) {
		break_statement();
	} else if (match(TOKEN_CONTINUE)) {
		continue_statement();
	} else if (match(TOKEN_RETURN)) {
		return_statement();
	} else if (match(TOKEN_TRY)) {
		try_statement();
	} else if (match(TOKEN_THROW)) {
		throw_statement();
	} else if (match(TOKEN_LEFT_BRACE)) {
		/* a bare block is a scope */
		begin_scope();
		block();
		end_scope();
	} else {
		expression_statement();
	}
}

/* declarations */

/*
 * fn name(a, b) { ... }
 *
 * The inner function is compiled by pushing a new compiler, which is the
 * only recursion in the grammar besides expressions. After the body, the
 * function object goes in the constant pool and OP_CLOSURE builds a closure
 * for it at runtime.
 *
 * The two bytes per upvalue after OP_CLOSURE tell the VM where to find each
 * captured variable: 1 for a local slot in this frame, 0 for an upvalue of
 * this closure. That is exactly what resolve_upvalue() recorded.
 */
/*
 * The parameter list and body of a function, for both spellings:
 * `fn name(a, b) { ... }` and the anonymous expression `fn(a, b) { ... }`.
 *
 * The compiler is on the stack when this returns, and the caller's
 * `end_compiler()` pops it. `compiler` is where the upvalue descriptors
 * land, and the caller emits them right after OP_CLOSURE.
 */
static void fn_signature(Compiler *compiler, Token name)
{
	init_compiler(compiler, TYPE_FUNCTION);
	compiler->function->name =
	        copy_string(state.vm, name.start, name.length);
	begin_scope();

	consume(TOKEN_LEFT_PAREN, "expect '(' after function name.");
	if (!check(TOKEN_RIGHT_PAREN)) {
		do {
			/* one stack slot per parameter, in order */
			state.current->function->arity++;
			if (state.current->function->arity > 255)
				error_at_current(
				        "can't have more than 255 parameters.");
			int param =
			        parse_variable("expect parameter name.", false);
			define_variable(param, false);
		} while (match(TOKEN_COMMA));
	}
	consume(TOKEN_RIGHT_PAREN, "expect ')' after parameters.");
	consume(TOKEN_LEFT_BRACE, "expect '{' before function body.");
	block();
}

static void fn_declaration(void)
{
	int global = parse_variable("expect function name.", false);
	mark_initialized();

	Compiler compiler;
	/* the name token the function was declared with, for the trace */
	Token name = state.parser.previous;
	fn_signature(&compiler, name);

	ObjFunction *function = end_compiler();
	int constant = make_constant(OBJ_VAL(function));
	emit_indexed(OP_CLOSURE, OP_CLOSURE_LONG, constant);

	/* the upvalue descriptors, in the order OP_CLOSURE expects them */
	for (int i = 0; i < function->upvalue_count; i++) {
		emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
		emit_byte(compiler.upvalues[i].index);
	}

	define_variable(global, false);
}

/*
 * fn(a, b) { ... } as an expression, so a function can be a value:
 * passed to another function, returned from one, or stored in a list.
 *
 * The value is the closure itself, on the stack, exactly where an
 * expression is expected. `<anonymous>` is the name a stack trace shows:
 * there is no name in the source to show, and an empty one would print
 * as a bare `()`, which reads like a mistake.
 */
static void anonymous_function(bool can_assign)
{
	(void)can_assign;
	Token anon = token_string("<anonymous>");

	Compiler compiler;
	fn_signature(&compiler, anon);

	ObjFunction *function = end_compiler();
	int constant = make_constant(OBJ_VAL(function));
	emit_indexed(OP_CLOSURE, OP_CLOSURE_LONG, constant);
	for (int i = 0; i < function->upvalue_count; i++) {
		emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
		emit_byte(compiler.upvalues[i].index);
	}
}

/* let, with or without an initializer. no initializer means nil. */
static void let_declaration(void)
{
	/*
	 * Table destructuring: `let {host, port} = config`.
	 *
	 * Flat names only -- no nesting, no defaults, no renaming. Each name
	 * becomes an ordinary binding by the ordinary rules: missing keys read
	 * nil (as with any field access), duplicates are a redeclaration
	 * error, and const works the same way through const_declaration.
	 * Anything fancier is a second declaration system, and this one stays
	 * small on purpose.
	 */
	if (check(TOKEN_LEFT_BRACE)) {
		let_destructure(false);
		return;
	}

	int global = parse_variable("expect variable name.", false);

	if (match(TOKEN_EQUAL))
		expression();
	else
		emit_byte(OP_NIL);

	consume_terminator();
	define_variable(global, false);
}

/*
 * Shared by let and const destructuring. is_const threads through exactly
 * as it does for a plain declaration.
 */
static void let_destructure(bool is_const)
{
	consume(TOKEN_LEFT_BRACE, "expect '{' after 'let'.");

	/*
	 * Parse the names first, into tokens, because the expression comes
	 * after them in source order. Token is a window into the source
	 * buffer, which outlives compilation, so storing them is safe.
	 */
	Token names[MAX_LOCALS];
	int name_count = 0;
	for (;;) {
		consume(TOKEN_IDENTIFIER, "expect variable name in '{...}'.");
		if (name_count >= MAX_LOCALS) {
			error("too many names in destructuring.");
			return;
		}
		names[name_count++] = state.parser.previous;
		if (!match(TOKEN_COMMA))
			break;
	}
	consume(TOKEN_RIGHT_BRACE, "expect '}' after destructured names.");
	consume(TOKEN_EQUAL, "expect '=' after destructured names.");

	/*
	 * Locals need a value in every slot before the working code runs.
	 *
	 * A local slot is stack memory, and anything pushed afterwards lands
	 * on the lowest free position -- which is a user slot if the user
	 * slots sit above the stack top with nothing in them. for-in avoids
	 * this by pushing a NIL per hidden slot first, and this does the
	 * same: one NIL per name, each immediately claimed, so every slot
	 * holds a real value below a top that only moves up from here.
	 *
	 * Globals need none of this: they live in a table, not on the
	 * stack, so there is nothing to overlap.
	 */
	bool local = state.current->scope_depth > 0;
	/*
	 * Remember where the user slots start: they are declared next, in
	 * order, so name i lives at first_slot + i. The hidden table slot
	 * comes after them, which puts every user slot below every working
	 * value for the rest of the statement.
	 */
	int first_slot = state.current->local_count;
	if (local) {
		for (int i = 0; i < name_count; i++) {
			emit_byte(OP_NIL);
			state.parser.previous = names[i];
			declare_variable(is_const);
			mark_initialized();
		}
	}

	expression();
	consume_terminator();

	/*
	 * The table now sits on top of the stack, so it becomes a hidden
	 * local -- the same shape for-in uses for its list. Everything after
	 * this reads through it, and the top stays above it for the rest of
	 * the statement, so no working value ever lands on a live slot.
	 */
	Token hidden = {TOKEN_IDENTIFIER,
	        " table",
	        6,
	        names[0].line,
	        false,
	        names[0].offset};
	add_local(hidden, false);
	mark_initialized();
	int table_slot = state.current->local_count - 1;

	/*
	 * One load-and-bind per name. declare_variable runs with
	 * parser.previous temporarily set for the globals case, because it
	 * reads the name from there; locals were already declared above.
	 */
	Token saved = state.parser.previous;
	for (int i = 0; i < name_count; i++) {
		int constant = -1;
		int slot = -1;
		if (!local) {
			state.parser.previous = names[i];
			declare_variable(is_const);
			constant = identifier_constant(&state.parser.previous);
		} else {
			/*
			 * Slots were assigned in order above, starting at
			 * first_slot: name i lives at first_slot + i, all
			 * below the hidden table slot. Recomputing beats
			 * re-resolving, because resolve_local would find the
			 * name but could not distinguish it from an outer
			 * binding of the same spelling.
			 */
			slot = first_slot + i;
		}

		/* load table.field, leaving the value on top */
		emit_bytes(OP_GET_LOCAL, (uint8_t)table_slot);
		emit_indexed(OP_GET_FIELD,
		        OP_GET_FIELD_LONG,
		        identifier_constant(&names[i]));

		/* bind it, by the same rules as a plain declaration */
		if (!local) {
			if (exporting) {
				if (is_const) {
					emit_indexed(
					        OP_DEFINE_GLOBAL_CONST_EXPORT,
					        OP_DEFINE_GLOBAL_CONST_EXPORT_LONG,
					        constant);
				} else {
					emit_indexed(OP_DEFINE_GLOBAL_EXPORT,
					        OP_DEFINE_GLOBAL_EXPORT_LONG,
					        constant);
				}
			} else if (is_const) {
				emit_indexed(OP_DEFINE_GLOBAL_CONST,
				        OP_DEFINE_GLOBAL_CONST_LONG,
				        constant);
			} else {
				emit_indexed(OP_DEFINE_GLOBAL,
				        OP_DEFINE_GLOBAL_LONG,
				        constant);
			}
		} else {
			emit_bytes(OP_SET_LOCAL, (uint8_t)slot);
			emit_byte(OP_POP);
		}
	}
	state.parser.previous = saved;
}

/* const. the initializer is mandatory: a const with no value has nothing to
 * be constant about. */
static void const_declaration(void)
{
	if (check(TOKEN_LEFT_BRACE)) {
		let_destructure(true);
		return;
	}

	int global = parse_variable("expect variable name.", true);

	consume(TOKEN_EQUAL, "expect '=' after const name.");
	expression();
	consume_terminator();

	/* this is the call that marks a global binding const. A local one
	 * needs no instruction: the compiler already knows its own locals,
	 * and checks them in named_variable(). */
	define_variable(global, true);
}

/*
 * Resynchronize after an error: throw away tokens until something that can
 * start a statement. Without this one bad expression produces a cascade of
 * errors for every token that follows it.
 */
static void synchronize(void)
{
	state.parser.panic_mode = false;

	while (state.parser.current.type != TOKEN_EOF) {
		if (state.parser.previous.type == TOKEN_SEMICOLON)
			return;
		switch (state.parser.current.type) {
		case TOKEN_FN:
		case TOKEN_LET:
		case TOKEN_CONST:
		case TOKEN_IF:
		case TOKEN_WHILE:
		case TOKEN_FOR:
		case TOKEN_RETURN:
		case TOKEN_PRINT:
		case TOKEN_IMPORT:
		case TOKEN_EXPORT:
		case TOKEN_BREAK:
		case TOKEN_CONTINUE:
			return;
		default:;
		}
		advance();
	}
}

/*
 * import "path.fl"
 *
 * There is no import opcode. This desugars to a call to the import_file
 * native: push the global, push the path as a constant, OP_CALL 1, pop the
 * result. The path is a raw string constant because escapes have already
 * been processed and a file path has no use for them.
 */
/*
 * `import "foo.fl"` and `import math` are the same statement with two
 * spellings, because they are the same thing: ask the module loader for a
 * source file. The loader decides whether the name is a relative path or a
 * library.
 *
 * Keeping the distinction in one place matters. The alternative is a second
 * import form for packages, and then the language has two module systems that
 * have to be kept in agreement about what an import means. The package manager
 * will sit in front of import_file(), not beside it.
 */

/*
 * Record an import binding for the unused-import check.
 *
 * Called with the name about to be bound and the path token that names the
 * file, so the eventual error can point at the import line rather than at the
 * end of the file. A `_` alias is not recorded: it is the explicit opt-out
 * for imports kept for their failure, and checking it would defeat the
 * purpose of writing it.
 */
static void import_record(const char *name, int length, const Token *path)
{
	if (import_count >= FL_MAX_IMPORTS) {
		imports_full = true;
		return;
	}
	char *copy = malloc((size_t)length + 1);
	if (copy == NULL)
		return;
	memcpy(copy, name, (size_t)length);
	copy[length] = '\0';
	imports[import_count].name = copy;
	imports[import_count].length = length;
	imports[import_count].line = path->line;
	imports[import_count].offset = path->offset;
	imports[import_count].used = false;
	imports[import_count].in_try = state.try_nesting > 0;
	import_count++;
}

/*
 * Mark every import binding with this name as used.
 *
 * All matching entries, not just the first: importing the same module twice
 * under one name and then using it is the documented re-import idiom, and
 * flagging the first of the two would punish a program for doing what the
 * manual says to do.
 */
static void import_mark_used(const char *name, int length)
{
	for (size_t i = 0; i < import_count; i++) {
		if (imports[i].length == length &&
		        memcmp(imports[i].name, name, (size_t)length) == 0)
			imports[i].used = true;
	}
}

/*
 * Report every import that was never referenced.
 *
 * Runs at the end of compilation, before the result is returned, so the
 * errors carry the import's own line rather than pointing nowhere. Skipped
 * when the file already failed: a broken program produces enough diagnostics
 * without one more, and an unused import in code that does not compile is
 * the least of its problems.
 *
 * Skipped in the REPL, where each submission compiles separately: `import
 * math` on one line and `math.floor(2)` on the next is the normal way to
 * work interactively, and flagging the first line would make the REPL
 * unusable for exactly the exploration it exists for.
 */
static void import_check_unused(void)
{
	if (state.parser.had_error || state.echo_repl_value)
		return;
	if (imports_full)
		return;
	/*
	 * Collapse by name first: if any import of a name is used, all of
	 * them are. Re-importing the same module under one name is
	 * idempotent -- every binding holds the same table -- so asking
	 * whether each individual statement's result was read would flag
	 * re-imports that the manual explicitly blesses. What matters is
	 * whether the module was ever touched, not which import line the
	 * reference happens to follow in source order.
	 */
	for (size_t i = 0; i < import_count; i++) {
		if (!imports[i].used)
			continue;
		for (size_t j = 0; j < import_count; j++) {
			if (imports[j].length == imports[i].length &&
			        memcmp(imports[j].name,
			                imports[i].name,
			                (size_t)imports[i].length) == 0)
				imports[j].used = true;
		}
	}
	for (size_t i = 0; i < import_count; i++) {
		if (imports[i].used || imports[i].in_try)
			continue;
		Token at = {
		        TOKEN_IDENTIFIER,
		        imports[i].name,
		        imports[i].length,
		        imports[i].line,
		        false,
		        imports[i].offset,
		};
		char message[128];
		snprintf(message,
		        sizeof(message),
		        "imported '%s' but never used. remove the import, or "
		        "use it.",
		        imports[i].name);
		error_at(&at, message);
		/*
		 * Each unused import is an independent fact, not a cascade
		 * from the previous one. synchronize() clears panic_mode
		 * between declarations for the same reason; without this,
		 * only the first of several dead imports would be reported.
		 */
		state.parser.panic_mode = false;
	}
}

static void import_declaration(void)
{
	const char *src;
	int len;
	Token path_token;
	bool quoted;

	if (match(TOKEN_STRING)) {
		path_token = state.parser.previous;
		quoted = true;
		/* the quotes are at both ends; the path is what is between */
		src = path_token.start + 1;
		len = path_token.length - 2;
	} else if (match(TOKEN_IDENTIFIER)) {
		path_token = state.parser.previous;
		quoted = false;
		src = path_token.start;
		len = path_token.length;
	} else {
		error("expect a module path or a library name after 'import'.");
		return;
	}

	/*
	 * `import "x.fl" as name` binds the module's exports to `name`.
	 *
	 * Without it, the name is the last path component with the .fl
	 * dropped, or the bare library name. That default is what every
	 * existing script already spells, so it keeps working; `as` exists for
	 * the cases where the derived name is wrong or unreadable --
	 *
	 *     import "lib/geometry/circle.fl" as geometry
	 *     import "../shared/util.fl" as util
	 *
	 * or simply to say what a file is for when its filename is not that.
	 */
	Token alias;
	bool has_alias = false;
	if (match(TOKEN_AS)) {
		alias = state.parser.previous;
		if (match(TOKEN_IDENTIFIER)) {
			Token name = state.parser.previous;
			/* copy the token out; the scanner's buffer moves on */
			alias.start = name.start;
			alias.length = name.length;
			alias.line = name.line;
			alias.offset = name.offset;
			has_alias = true;
		} else {
			error("expect a name after 'as'.");
		}
	}
	consume_terminator();

	Token import_fn = {TOKEN_IDENTIFIER,
	        "import_file",
	        11,
	        path_token.line,
	        false,
	        path_token.offset};
	int fn_const = identifier_constant(&import_fn);

	/*
	 * Call it: [import_file][path], then OP_CALL 1.
	 *
	 * OP_CALL reads the callee at peek(arg_count) -- one slot *below* the
	 * arguments -- so the callee goes under its argument, not over it.
	 * Getting this backwards compiles cleanly and calls the path string,
	 * which fails with "can only call functions" several frames from the
	 * mistake.
	 */
	emit_indexed(OP_GET_GLOBAL, OP_GET_GLOBAL_LONG, fn_const);
	ObjString *str = copy_string(state.vm, src, len);
	emit_constant(STR_VAL(str));
	emit_bytes(OP_CALL, 1);

	/*
	 * Bind the returned table.
	 *
	 * This is the line that did not exist before. In v0.5.0 the result was
	 * popped and discarded, and a library import worked because the loader
	 * had separately poked a name into the *importer's* table -- which is
	 * why a module could define things in its importer, and why two
	 * modules could clobber each other's globals.
	 *
	 * Now the module's exports arrive as a table and are bound here, in
	 * this module, under one name. `import geometry` then means
	 * `geometry.area(5)` through ordinary field access.
	 */
	const char *bind_name;
	int bind_len;
	char derived[64];

	if (has_alias) {
		bind_name = alias.start;
		bind_len = alias.length;
	} else if (quoted) {
		/*
		 * "lib/geometry.fl" binds as `geometry`: the last component,
		 * without the extension. Trailing slashes are ignored so
		 * "lib/geometry/" is not a syntax error for a path nobody
		 * would write on purpose.
		 */
		const char *start = src;
		const char *end = src + len;
		while (end > start && end[-1] == '/')
			end--;
		const char *slash = end;
		while (slash > start && slash[-1] != '/')
			slash--;
		/*
		 * Drop the extension. `stop` walks back from the end of the
		 * component to its last '.', but only when that leaves a name
		 * behind -- ".fl" must not reduce to nothing, and "a.b.fl" is
		 * `a.b` rather than `a`.
		 */
		const char *stop = end;
		while (stop > slash + 1 && stop[-1] != '.')
			stop--;
		/* back off the dot itself. the loop above stops with `stop`
		 * one past the '.', because the test reads stop[-1] before
		 * the decrement, so leaving it there yields "geometry." */
		if (stop < end && stop[-1] == '.')
			stop--;
		size_t n = (size_t)(stop - slash);
		if (n >= sizeof(derived))
			n = sizeof(derived) - 1;
		memcpy(derived, slash, n);
		derived[n] = '\0';
		if (n == 0) {
			error("cannot derive a name from this import path. "
			      "use 'as'.");
			emit_byte(OP_POP);
			return;
		}
		bind_name = derived;
		bind_len = (int)n;
	} else {
		bind_name = src;
		bind_len = len;
	}

	/*
	 * OP_DEFINE_GLOBAL takes the name as an operand and defines whatever
	 * is on top of the stack, which right now is the import's result
	 * table. Nothing else needs pushing: pushing the name as well would
	 * make it the value being defined, and `math` would be the string
	 * "math".
	 */
	emit_indexed(OP_DEFINE_GLOBAL,
	        OP_DEFINE_GLOBAL_LONG,
	        identifier_constant_from(bind_name, bind_len));

	/*
	 * Record the binding for the unused-import check, unless it is the
	 * explicit opt-out. `import "x.fl" as _` means "run this for its
	 * failure" -- a test importing a module it knows will throw -- and
	 * there is nothing to use, so checking it would defeat the purpose
	 * of writing it.
	 */
	if (!(bind_len == 1 && bind_name[0] == '_'))
		import_record(bind_name, bind_len, &path_token);
}

/*
 * export fn / let / const
 *
 * `export` is a flag, not a namespace. The declaration is compiled exactly as
 * if the keyword were not there -- it lands in this module's own globals, like
 * any other top-level name -- and the name is additionally marked so the
 * loader will put it in the table the importer receives.
 *
 * That is the smallest thing that makes `export` mean something. The previous
 * implementation parsed the declaration and stepped aside, and the "exports"
 * were whatever the module had added to the shared global table, discovered by
 * diffing that table across the run. Diffing cannot tell a helper from an
 * export: both are just a name that appeared. So `export` marked intent and
 * nothing else, and a module's private function was as visible as its public
 * one.
 *
 * Marking the binding at compile time makes the distinction the module
 * actually wrote down.
 */
static void export_declaration(void)
{
	if (match(TOKEN_FN)) {
		exporting = true;
		fn_declaration();
		exporting = false;
	} else if (match(TOKEN_LET)) {
		exporting = true;
		let_declaration();
		exporting = false;
	} else if (match(TOKEN_CONST)) {
		exporting = true;
		const_declaration();
		exporting = false;
	} else {
		error("expect 'fn', 'let', or 'const' after 'export'.");
	}
}

static void declaration(void)
{
	if (match(TOKEN_IMPORT))
		import_declaration();
	else if (match(TOKEN_EXPORT))
		export_declaration();
	else if (match(TOKEN_FN))
		fn_declaration();
	else if (match(TOKEN_LET))
		let_declaration();
	else if (match(TOKEN_CONST))
		const_declaration();
	else
		statement();

	/* one error per statement, then resynchronize */
	if (state.parser.panic_mode)
		synchronize();
}

/* entry point */

/*
 * Compile a whole source string into a top-level function.
 *
 * Returns NULL if anything failed, and the caller must not run or free the
 * result in that case. state.vm is saved rather than restored: compile()
 * can be reentered by import, and the reentrant call shares the same VM.
 */
ObjFunction *compile(VM *vm, const char *source)
{
	return compile_named(vm, source, "<source>");
}

ObjFunction *compile_named(VM *vm, const char *source, const char *name)
{
	fix_count = 0;
	/*
	 * Own the compiler state for the duration of this compile.
	 *
	 * The save and the restore are the whole point of bundling the four
	 * globals into a struct. A nested compile -- an import that compiles
	 * while this one is still on the stack -- now starts from a clean
	 * slate and puts the outer state back exactly as it was on the way
	 * out, instead of inheriting a half-finished token position and a
	 * dangling compiler chain.
	 *
	 * A nested compile cannot actually happen today, because compiling
	 * and executing are separate phases and `import` runs at execution
	 * time. That is an argument, not a guarantee, and this makes it a
	 * guarantee. The cost is a struct copy on entry and exit, which is
	 * a few hundred bytes and about forty instructions.
	 */
	CompilerState saved = state;
	/* echo mode is set by the repl before it calls in, and it is a
	 * mode rather than per-compile state, so it has to survive the
	 * save/restore below. left_value_on_stack is the per-compile
	 * result and deliberately does not. */
	bool echo_mode = state.echo_repl_value;

	/* a fresh compile does not inherit the previous one's error state */
	state.parser.had_error = false;
	state.parser.error_count = 0;
	state.parser.suppressed = false;
	state.left_value_on_stack = false;
	state.parser.panic_mode = false;
	state.loop = NULL;
	state.current = NULL;

	/*
	 * Nor its import list. The entries hold malloc'd names freed at the
	 * end of the compile that recorded them; without this, a second
	 * compilation would report the first one's imports -- or read freed
	 * memory, if the first compile's cleanup already ran.
	 */
	for (size_t i = 0; i < import_count; i++)
		free(imports[i].name);
	import_count = 0;
	imports_full = false;

	state.vm = vm;
	scanner_init(source);
	diag_text = source;
	diag_name = name;

	Compiler compiler;
	init_compiler(&compiler, TYPE_SCRIPT);

	advance();

	while (!match(TOKEN_EOF))
		declaration();

	/*
	 * Unused imports are reported here, after every declaration has had
	 * its chance to reference them, and before the error summary below so
	 * the count includes them. See import_check_unused for why the REPL
	 * is exempt.
	 */
	import_check_unused();
	for (size_t i = 0; i < import_count; i++)
		free(imports[i].name);
	import_count = 0;

	/*
	 * The summary. One line, and only when it is not obvious: if the
	 * file did not compile, say how many things were wrong with it.
	 * A reader who fixed the first error and ran again deserves to
	 * know whether that was the whole job.
	 */
	if (state.parser.had_error) {
		int n = state.parser.error_count;
		fprintf(stderr,
		        "\nerror: could not compile due to %d error%s\n",
		        n,
		        n == 1 ? "" : "s");
	}

	/*
	 * end_compiler() pops the chain, which leaves state.current NULL. That
	 * is what tells the collector there is nothing in flight, so it must
	 * happen before the function is returned: after this point the only
	 * reference to `function` is the caller's, and the caller has not
	 * rooted it yet.
	 */
	ObjFunction *function = end_compiler();
	bool failed = state.parser.had_error;

	/*
	 * restore before returning. note that the returned ObjFunction is
	 * already a live heap object, so dropping the compiler chain does
	 * not lose it: the caller roots it on the next line.
	 */
	bool left_value = state.left_value_on_stack;
	state = saved;
	state.echo_repl_value = echo_mode;
	state.left_value_on_stack = left_value;

	return failed ? NULL : function;
}

/*
 * Mark every function on the compiler chain. Called from mark_roots().
 * A function being compiled is reachable from nothing else: it is not on the
 * value stack and not in any table, so without this a collection in the
 * middle of a compile would free it.
 *
 * mark_object(), not a bare is_marked = true. Setting the bit by hand keeps
 * the function alive but skips the gray stack, so nothing the function
 * references gets marked: its constant pool, and every string in it, is swept
 * while the function that uses them survives. That is a use-after-free one
 * allocation later, in a completely unrelated place, which is a miserable
 * bug to track down.
 */
void compiler_mark_roots(VM *vm)
{
	/*
	 * `state.current`, not something the VM holds. `state.current` is the innermost
	 * compiler, so walking its enclosing chain visits every function
	 * currently being built, including the ones inside a nested `fn`.
	 * A pointer cached in the VM would go stale the moment a nested
	 * function pushed a frame of its own, and the function being
	 * compiled right now would be invisible to the collector.
	 *
	 * Outside a compile `state.current` is NULL and this is a no-op, which is
	 * why it does not need a flag to say whether a compile is in
	 * progress.
	 */
	for (Compiler *compiler = state.current; compiler != NULL;
	        compiler = compiler->enclosing) {
		if (compiler->function != NULL)
			mark_object(vm, (Obj *)compiler->function);
	}
}
