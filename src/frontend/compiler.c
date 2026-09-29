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
} LoopContext;

typedef struct {
	Token current;
	Token previous;
	bool had_error;
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
} CompilerState;

static CompilerState state;
static const FlSource *diag_source;
static FlDiagFormat diag_format = FL_DIAG_LEGACY;
static FlColorMode diag_color = FL_COLOR_AUTO;
static const char *diag_text;
static const char *diag_name;
static FlDiagSuggestion fixes[64];
static size_t fix_count;

size_t compiler_fix_count(void) { return fix_count; }

const FlDiagSuggestion *compiler_fix_at(size_t index)
{
	return index < fix_count ? &fixes[index] : NULL;
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
		if (strstr(message, "Unexpected character") != NULL)
			return "E0001";
		if (strstr(message, "scientific notation") != NULL)
			return "E0002";
		if (strstr(message, "string") != NULL)
			return "E0003";
	}
	if (strstr(message, "Expect ')'") != NULL ||
	        strstr(message, "Expect '}'") != NULL ||
	        strstr(message, "Expect ']'") != NULL)
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
		return;
	}
	FlDiagSuggestion suggestion;
	const FlDiagSuggestion *suggestions = NULL;
	size_t suggestion_count = 0;
	if (token->type == TOKEN_EOF &&
	        (strstr(message, "Expect ')'") != NULL ||
	                strstr(message, "Expect '}'") != NULL ||
	                strstr(message, "Expect ']'") != NULL)) {
		const char *replacement =
		        strstr(message, "Expect ')'") != NULL   ? ")"
		        : strstr(message, "Expect '}'") != NULL ? "}"
		                                                : "]";
		size_t insertion = (size_t)state.parser.previous.offset +
		                   (size_t)state.parser.previous.length;
		suggestion = (FlDiagSuggestion){
		        .span = fl_span(insertion, insertion),
		        .message = "add the missing delimiter",
		        .replacement = replacement,
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
	        .primary = fl_span(token->offset,
	                (size_t)token->offset + (size_t)token->length),
	        .has_primary = true,
	        .primary_label = token->type == TOKEN_EOF ? "expected here"
	                                                  : "unexpected token",
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
}

static void error(const char *message)
{
	error_at(&state.parser.previous, message);
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
	chunk_write(
	        state.vm, current_chunk(), byte, state.parser.previous.line);
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
		error("Too many constants in one chunk.");
		return 0; /* unreachable in practice: needs 16M constants */
	}
	return constant;
}

/*
 * Push a constant, picking the encoding. The 1-byte form covers 256
 * constants, which is every function anyone writes by hand, so the 3-byte
 * form only shows up in generated code.
 */
static void emit_constant(Value value)
{
	int constant = make_constant(value);
	if (constant < 256) {
		emit_bytes(OP_CONSTANT, (uint8_t)constant);
	} else {
		emit_byte(OP_CONSTANT_LONG);
		emit_byte((uint8_t)((constant >> 16) & 0xff));
		emit_byte((uint8_t)((constant >> 8) & 0xff));
		emit_byte((uint8_t)(constant & 0xff));
	}
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
		error("Too much code to jump over.");
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
		error("Loop body too large.");
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
static uint8_t identifier_constant(Token *name)
{
	return make_constant(
	        OBJ_VAL(copy_string(state.vm, name->start, name->length)));
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
				error("Can't read local variable in its own "
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
		error("Too many closure variables in function.");
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
		error("Too many local variables in function.");
		return;
	}

	Local *local = &state.current->locals[state.current->local_count++];
	local->name = name;
	local->depth = -1;
	local->is_captured = false;
	local->is_const = is_const;
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
		if (identifiers_equal(name, &local->name))
			error("Already a variable with this name in this "
			      "scope.");
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
static uint8_t parse_variable(const char *message, bool is_const)
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
static void define_variable(uint8_t global, bool is_const)
{
	if (state.current->scope_depth > 0) {
		mark_initialized();
		return;
	}

	if (is_const) {
		/* a separate opcode rather than a third operand byte, so the
		 * encoding of the common `let` does not grow to pay for a
		 * case almost no script uses */
		emit_bytes(OP_DEFINE_GLOBAL_CONST, global);
		return;
	}

	emit_bytes(OP_DEFINE_GLOBAL, global);
}

/* forward declarations. the expression and statement parsers call each other
 * freely, which C does not allow without these. */

static void expression(void);
static void statement(void);
static void declaration(void);
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
		error("Expect expression.");
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
		error("Invalid assignment target.");
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
	emit_constant(OBJ_VAL(str));
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
	}

	if (can_assign && match(TOKEN_EQUAL)) {
		/* const is enforced here, at compile time, and only for
		 * locals. a global const is not protected: OP_SET_GLOBAL
		 * has no way to know. that is a bug. */
		if (get_op == OP_GET_LOCAL &&
		        state.current->locals[arg].is_const) {
			error("Can't assign to constant.");
			return;
		}
		expression();
		emit_bytes(set_op, (uint8_t)arg);
	} else if (can_assign &&
	           (match(TOKEN_PLUS_EQUAL) || match(TOKEN_MINUS_EQUAL) ||
	                   match(TOKEN_STAR_EQUAL) ||
	                   match(TOKEN_SLASH_EQUAL))) {
		if (get_op == OP_GET_LOCAL &&
		        state.current->locals[arg].is_const) {
			error("Can't assign to constant.");
			return;
		}
		TokenType op = state.parser.previous.type;
		/* read, modify, write. no in-place bytecode for this, so the
		 * value is loaded, combined and stored. */
		emit_bytes(get_op, (uint8_t)arg);
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
		emit_bytes(set_op, (uint8_t)arg);
	} else {
		emit_bytes(get_op, (uint8_t)arg);
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
	consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
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

	consume(TOKEN_IDENTIFIER, "Expect a type name after 'as'.");

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
		error("Unknown type name after 'as'. Expected one of: "
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
				error("Can't have more than 255 arguments.");
			argc++;
		} while (match(TOKEN_COMMA));
	}
	consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
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
		} while (match(TOKEN_COMMA));
	}
	consume(TOKEN_RIGHT_BRACKET, "Expect ']' after list.");
	if (count > 255)
		error("Can't have more than 255 elements in a list literal.");
	emit_bytes(OP_BUILD_LIST, (uint8_t)count);
}

/* a[i], and a[i] = v. the value form is an lvalue like any other. */
static void subscript(bool can_assign)
{
	expression();
	consume(TOKEN_RIGHT_BRACKET, "Expect ']' after index.");

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
	consume(TOKEN_IDENTIFIER, "Expect field name after '.'.");
	uint8_t name = identifier_constant(&state.parser.previous);

	if (can_assign && match(TOKEN_EQUAL)) {
		expression();
		emit_bytes(OP_SET_FIELD, name);
	} else {
		emit_bytes(OP_GET_FIELD, name);
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
			        "Expect key name in table literal.");
			uint8_t name =
			        identifier_constant(&state.parser.previous);
			consume(TOKEN_COLON,
			        "Expect ':' after key in table literal.");

			/*
			 * The value is pushed on top of the table, and
			 * OP_SET_FIELD_TOP consumes it and leaves the table
			 * where it is. So each pair leaves the stack exactly
			 * as it found it, and the table never has to be
			 * stashed anywhere to survive to the next pair.
			 */
			expression();
			emit_bytes(OP_SET_FIELD_TOP, name);
		} while (match(TOKEN_COMMA));
	}
	consume(TOKEN_RIGHT_BRACE, "Expect '}' after table literal.");
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
        [TOKEN_FN] = {NULL, NULL, PREC_NONE},
        [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
        [TOKEN_IF] = {NULL, NULL, PREC_NONE},
        [TOKEN_IMPORT] = {NULL, NULL, PREC_NONE},
        [TOKEN_IN] = {NULL, NULL, PREC_NONE},
        [TOKEN_LET] = {NULL, NULL, PREC_NONE},
        [TOKEN_NIL] = {literal, NULL, PREC_NONE},
        [TOKEN_NOT] = {unary, NULL, PREC_NONE},
        [TOKEN_OR] = {NULL, or_, PREC_OR},
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
	error_at_current("Expect ';' or newline after statement.");
}

/* statement parsing */

static void block(void)
{
	while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF))
		declaration();
	consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
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

static void print_statement(void)
{
	consume(TOKEN_LEFT_PAREN, "Expect '(' after 'print'.");
	expression();
	emit_byte(OP_PRINT);
	/* print(a, b, c) is not a tuple, it is three prints. one opcode per
	 * argument, so there is no temporary to worry about */
	while (match(TOKEN_COMMA)) {
		expression();
		emit_byte(OP_PRINT);
	}
	consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
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

	consume(TOKEN_LEFT_BRACE, "Expect '{' after if condition.");
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
			consume(TOKEN_LEFT_BRACE, "Expect '{' after else.");
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
	loop.scope_depth = state.current->scope_depth;
	loop.start = current_chunk()->count;
	loop.continue_target = loop.start; /* re-test the condition */
	loop.break_count = 0;
	loop.continue_count = 0;
	state.loop = &loop;

	expression();
	int exit_jump = emit_jump(OP_JUMP_IF_FALSE);
	emit_byte(OP_POP);

	consume(TOKEN_LEFT_BRACE, "Expect '{' after while condition.");
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

	consume(TOKEN_IDENTIFIER, "Expect variable name after 'for'.");
	Token var_name = state.parser.previous;
	consume(TOKEN_IN, "Expect 'in' after for variable.");

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
			/* stack: [start] then [start][end] */
			expression();

			/* the user's variable takes the start slot, and a
			 * hidden local takes the end. */
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

			LoopContext loop;
			loop.enclosing = state.loop;
			loop.scope_depth = state.current->scope_depth;
			loop.start = current_chunk()->count;
			/* the increment is emitted below, so no single offset
			 * is known yet; continue waits for a patch */
			loop.continue_target = -1;
			loop.break_count = 0;
			loop.continue_count = 0;
			state.loop = &loop;

			int var_slot = state.current->local_count - 2;
			int end_slot = state.current->local_count - 1;

			/* condition: var < end */
			emit_bytes(OP_GET_LOCAL, (uint8_t)var_slot);
			emit_bytes(OP_GET_LOCAL, (uint8_t)end_slot);
			emit_byte(OP_LESS);

			int exit_jump = emit_jump(OP_JUMP_IF_FALSE);
			emit_byte(OP_POP);

			consume(TOKEN_LEFT_BRACE,
			        "Expect '{' after for range.");
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
		loop.scope_depth = state.current->scope_depth;
		loop.start = current_chunk()->count;
		loop.continue_target = -1;
		loop.break_count = 0;
		loop.continue_count = 0;
		state.loop = &loop;

		/*
		 * Condition: idx < len(list).
		 *
		 * There is no opcode for a list length, so the compiler
		 * calls the len() native: push the global, push the list,
		 * OP_CALL 1. That works, and it does mean for-in-over-a-list
		 * calls a C function on every iteration. Emitting a real
		 * OP_LIST_LEN would be a one-line VM change and a
		 * two-line compiler change, if anyone ever profiles it.
		 */
		emit_bytes(OP_GET_LOCAL, (uint8_t)idx_slot);

		Token len_tok = {TOKEN_IDENTIFIER,
		        "len",
		        3,
		        var_name.line,
		        false,
		        var_name.offset};
		uint8_t len_const = identifier_constant(&len_tok);
		emit_bytes(OP_GET_GLOBAL, len_const);
		emit_bytes(OP_GET_LOCAL, (uint8_t)list_slot);
		emit_bytes(OP_CALL, 1);

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
		        "Expect '{' after for-in expression.");
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
		error("Can't use 'break' outside of a loop.");
		return;
	}

	emit_close_upvalues_to(state.loop->scope_depth);

	/* the offsets are patched once the loop body is complete */
	if (state.loop->break_count >= 256) {
		error("Too many break statements in loop.");
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
		error("Can't use 'continue' outside of a loop.");
		return;
	}

	emit_close_upvalues_to(state.loop->scope_depth);
	if (state.loop->continue_target == -1) {
		if (state.loop->continue_count >= 256) {
			error("Too many continue statements in loop.");
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
		error("Can't return from top-level code.");

	/* `return` with nothing after it is return nil */
	if (check(TOKEN_SEMICOLON) || check(TOKEN_RIGHT_BRACE) ||
	        state.parser.current.newline_before || check(TOKEN_EOF)) {
		emit_return();
	} else {
		expression();
		emit_byte(OP_RETURN);
	}
	consume_terminator();
}

/* an expression statement. the result is discarded, so it is popped. */
static void expression_statement(void)
{
	expression();
	consume_terminator();
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
static void fn_declaration(void)
{
	uint8_t global = parse_variable("Expect function name.", false);
	mark_initialized();

	Compiler compiler;
	init_compiler(&compiler, TYPE_FUNCTION);
	begin_scope();

	consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
	if (!check(TOKEN_RIGHT_PAREN)) {
		do {
			/* one stack slot per parameter, in order */
			state.current->function->arity++;
			if (state.current->function->arity > 255)
				error_at_current(
				        "Can't have more than 255 parameters.");
			uint8_t param =
			        parse_variable("Expect parameter name.", false);
			define_variable(param, false);
		} while (match(TOKEN_COMMA));
	}
	consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
	consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
	block();

	ObjFunction *function = end_compiler();
	int constant = make_constant(OBJ_VAL(function));
	emit_bytes(OP_CLOSURE, (uint8_t)constant);

	/* the upvalue descriptors, in the order OP_CLOSURE expects them */
	for (int i = 0; i < function->upvalue_count; i++) {
		emit_byte(compiler.upvalues[i].is_local ? 1 : 0);
		emit_byte(compiler.upvalues[i].index);
	}

	define_variable(global, false);
}

/* let, with or without an initializer. no initializer means nil. */
static void let_declaration(void)
{
	uint8_t global = parse_variable("Expect variable name.", false);

	if (match(TOKEN_EQUAL))
		expression();
	else
		emit_byte(OP_NIL);

	consume_terminator();
	define_variable(global, false);
}

/* const. the initializer is mandatory: a const with no value has nothing to
 * be constant about. */
static void const_declaration(void)
{
	uint8_t global = parse_variable("Expect variable name.", true);

	consume(TOKEN_EQUAL, "Expect '=' after const name.");
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
static void import_declaration(void)
{
	consume(TOKEN_STRING, "Expect module file path string after 'import'.");
	Token path_token = state.parser.previous;
	consume_terminator();

	Token import_fn = {TOKEN_IDENTIFIER,
	        "import_file",
	        11,
	        path_token.line,
	        false,
	        path_token.offset};
	uint8_t fn_const = identifier_constant(&import_fn);
	emit_bytes(OP_GET_GLOBAL, fn_const);

	const char *src = path_token.start + 1;
	int len = path_token.length - 2;
	ObjString *str = copy_string(state.vm, src, len);
	emit_constant(OBJ_VAL(str));

	emit_bytes(OP_CALL, 1);
	emit_byte(OP_POP);
}

/*
 * export fn / let / const
 *
 * There is no module namespace. The declaration is compiled exactly as if
 * the export keyword were not there and lands in the shared globals table,
 * which is why `export` can simply parse the declaration and step aside.
 */
static void export_declaration(void)
{
	if (match(TOKEN_FN)) {
		fn_declaration();
	} else if (match(TOKEN_LET)) {
		let_declaration();
	} else if (match(TOKEN_CONST)) {
		const_declaration();
	} else {
		error("Expect 'fn', 'let', or 'const' after 'export'.");
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

	/* a fresh compile does not inherit the previous one's error state */
	state.parser.had_error = false;
	state.parser.panic_mode = false;
	state.loop = NULL;
	state.current = NULL;

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
	state = saved;

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
