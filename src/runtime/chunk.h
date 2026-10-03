/* SPDX-License-Identifier: MIT */
/*
 * The instruction set, and the byte buffer they live in.
 */
#ifndef FL_CHUNK_H
#define FL_CHUNK_H

#include "common.h"
#include "memory.h"
#include "value.h"

/* growable array of values. also used on its own by the compiler. */
typedef struct {
	int count;
	int capacity;
	Value *values;
} ValueArray;

void value_array_init(ValueArray *array);
void value_array_write(VM *vm, ValueArray *array, Value value);
void value_array_free(VM *vm, ValueArray *array);

/*
 * The vocabulary for `x as T`. Deliberately the same seven names type()
 * returns, so there is exactly one list of type names in the language and no
 * second spelling to learn.
 *
 * A cast is a check, not a conversion. There is nothing to convert to: there
 * is one numeric type, and every other type is already distinct at the value
 * level. `str()` is the conversion; `as` is the assertion.
 */
typedef enum {
	FL_TYPE_NUMBER = 0,
	FL_TYPE_STRING,
	FL_TYPE_BOOL,
	FL_TYPE_NIL,
	FL_TYPE_LIST,
	FL_TYPE_TABLE,
	FL_TYPE_FUNCTION
} FlType;

/*
 * Operand sizes, which the disassembler and the run loop both switch on:
 *   no operand : OP_NIL, OP_ADD, OP_RETURN, ...
 *   u8         : OP_GET_LOCAL, OP_CALL, OP_BUILD_LIST, ...
 *   u8 index   : OP_CONSTANT, OP_GET_GLOBAL, OP_GET_FIELD, ...
 *   u24 index  : OP_CONSTANT_LONG, once a chunk passes 256 constants
 *   u8 type    : OP_CAST
 *   u16 offset : OP_JUMP, OP_JUMP_IF_FALSE, OP_LOOP
 */
typedef enum {
	OP_CONSTANT,
	OP_CONSTANT_LONG,
	OP_NIL,
	OP_TRUE,
	OP_FALSE,
	OP_POP,
	OP_GET_LOCAL,
	OP_SET_LOCAL,
	OP_GET_GLOBAL,
	OP_DEFINE_GLOBAL,
	/*
	 * `const` at the top level. Same as OP_DEFINE_GLOBAL except the
	 * entry it creates is marked const, so every later write to that name
	 * is refused. A separate opcode rather than a third operand byte,
	 * so the encoding of the common `let` does not grow to serve a case
	 * almost no script uses.
	 */
	OP_DEFINE_GLOBAL_CONST,
	OP_SET_GLOBAL,
	OP_GET_UPVALUE,
	OP_SET_UPVALUE,
	OP_EQUAL,
	OP_NOT_EQUAL,
	/*
	 * Check the value's type and pass it through unchanged, or fail.
	 * Operands: u8 FlType.
	 *
	 * The value is left alone when the check passes. `as` is not a
	 * conversion, so there is no opcode here that rewrites a value, and
	 * nothing to get wrong: either the type matches or the script
	 * stops.
	 */
	OP_CAST,
	OP_GREATER,
	OP_GREATER_EQUAL,
	OP_LESS,
	OP_LESS_EQUAL,
	OP_ADD,
	OP_SUBTRACT,
	OP_MULTIPLY,
	OP_DIVIDE,
	OP_MODULO,
	OP_NOT,
	OP_NEGATE,
	OP_PRINT,
	OP_JUMP,
	OP_JUMP_IF_FALSE,
	OP_LOOP,
	OP_CALL,
	OP_CLOSURE,
	OP_CLOSE_UPVALUE,
	OP_RETURN,
	/*
	 * len(list) without going through the builtin.
	 *
	 * The for-in loop the compiler emits asks for the list length on every
	 * iteration, and it used to do that by loading the global `len`,
	 * pushing the list, and making a real call into C. One call, one
	 * global hash lookup and one frame's worth of arity checking per loop
	 * iteration, to read an integer that is already in the object's
	 * header.
	 *
	 * This is the reason the opcode exists. It is not a peephole: it
	 * removes a call that only the compiler emits, and it is the single
	 * largest interpreter win in list iteration.
	 *
	 * Operands: none. Stack: [list] -> [number].
	 */
	OP_LIST_LEN,
	/*
	 * The three table-iteration primitives, emitted only by `for k in t`
	 * and `for k, v in t`.
	 *
	 * A table is insertion-ordered parallel arrays, so position `i` always
	 * means the i-th inserted entry. The loop holds an index in a hidden
	 * local and reads through these, exactly as list iteration holds an
	 * index and reads through OP_GET_INDEX -- same shape, same reasoning.
	 *
	 * All three take no operands:
	 *   OP_TABLE_COUNT  [table] -> [number]
	 *   OP_TABLE_KEY    [table][index] -> [key-string]
	 *   OP_TABLE_VALUE  [table][index] -> [value]
	 *
	 * Bounds are checked with the same whole-number validation as list
	 * indexing, so a fractional or out-of-range index fails the same way
	 * in both.
	 */
	OP_TABLE_COUNT,
	OP_TABLE_KEY,
	OP_TABLE_VALUE,
	OP_BUILD_LIST,
	OP_BUILD_TABLE,
	/*
	 * Set a field on the table *below* the value, leaving the table in
	 * place. Operands: u8 key index. Stack: [table][value] -> [table].
	 *
	 * This is what a table literal is built from. The alternative was to
	 * park the table in a hidden local between pairs, which only works
	 * when the table is the topmost thing on the stack -- and a literal
	 * used as a call argument is not, because the callee is already
	 * there. That is what made `type({a:1})` set a field on the callee.
	 */
	OP_SET_FIELD_TOP,
	OP_GET_INDEX,
	OP_SET_INDEX,
	OP_GET_FIELD,
	OP_SET_FIELD,
	OP_IMPORT,
	OP_EXPORT,
	OP_GET_GLOBAL_LONG,
	OP_DEFINE_GLOBAL_LONG,
	OP_DEFINE_GLOBAL_CONST_LONG,
	OP_SET_GLOBAL_LONG,
	OP_GET_FIELD_LONG,
	OP_SET_FIELD_LONG,
	OP_SET_FIELD_TOP_LONG,
	OP_CLOSURE_LONG,
	/*
	 * Specialized numeric opcodes. These are the same as their generic
	 * counterparts (OP_ADD, OP_SUBTRACT, etc.) but skip the type check:
	 * the compiler emits them when both operands are provably numbers at
	 * compile time (constant folding, or typed local inference). The
	 * interpreter dispatches to a two-instruction sequence: no tag test,
	 * no branch, just unbox-operate-rebox.
	 *
	 * The verifier checks that both operands exist on the stack; it
	 * cannot check that they are numbers. A misprediction here produces
	 * a wrong number, not a crash, because AS_NUMBER on a non-number
	 * returns whatever bits are in the value. The compiler only emits
	 * these when it is sure, so in practice the guarantee holds.
	 *
	 * All take no operands (1 byte).
	 */
	OP_ADD_NUM,
	OP_SUB_NUM,
	OP_MUL_NUM,
	OP_DIV_NUM,
	OP_MOD_NUM,
	OP_LT_NUM,
	OP_LE_NUM,
	OP_GT_NUM,
	OP_GE_NUM,
	OP_EQ_NUM,
	OP_NEQ_NUM,
	OP_NEG_NUM,
	/*
	 * The `export` forms of the two global definitions.
	 *
	 * Identical to their unmarked counterparts except that the binding is
	 * flagged exported, which the module loader reads when it builds the
	 * table the importer receives.
	 *
	 * Separate opcodes rather than a flag in an operand byte because the
	 * operand is a constant index and stealing its high bit would mean
	 * every global access in every program carries a flag it does not use.
	 * The ordinary `let` stays two bytes.
	 */
	OP_DEFINE_GLOBAL_EXPORT,
	OP_DEFINE_GLOBAL_EXPORT_LONG,
	OP_DEFINE_GLOBAL_CONST_EXPORT,
	OP_DEFINE_GLOBAL_CONST_EXPORT_LONG,
	/*
	 * Jump when the top of stack is not nil, peeking rather than popping.
	 *
	 * Emitted only by `??`, which needs "keep the value and skip the
	 * fallback" in one jump: if the left side is not nil it stays and the
	 * right side never runs; if it is nil the jump falls through to a POP
	 * and the right side takes its place. Either path leaves one value.
	 */
	OP_JUMP_IF_NOT_NIL,
	/*
	 * Recoverable errors. See vm.h for the handler model.
	 *
	 * OP_TRY, u16: push a catch handler on the VM's handler stack and
	 * continue. The offset is the jump past the try body to the catch
	 * block, measured from the end of the instruction -- the same
	 * encoding as OP_JUMP, so the compiler's emit_jump()/patch_jump()
	 * emit it, and the verifier's jump-target check covers it.
	 *
	 * OP_POP_HANDLER: the try body finished normally; drop the handler.
	 *
	 * OP_THROW: pop the value above and raise it.
	 */
	OP_TRY,
	OP_POP_HANDLER,
	OP_THROW,
} OpCode;

typedef struct {
	int count;
	int capacity;
	uint8_t *code;
	/*
	 * One int per bytecode byte. Four bytes of line table for every byte
	 * of code is a terrible cache footprint. A run-length table would fix
	 * it and would also be a source of bugs nobody has time for yet.
	 */
	int *lines;
	/*
	 * Source byte offset of the start of each instruction, parallel to
	 * `code`. Only the start is stored: the end of one instruction is
	 * the start of the next, so the array doubles as the end table and
	 * no span needs two entries.
	 *
	 * This is what lets a runtime error underline the expression that
	 * failed instead of the whole line. Without it the VM knows an
	 * instruction failed but not which words in the source produced it.
	 *
	 * It costs four bytes per byte of code, the same as the line table
	 * beside it, and is never read on a successful run: the VM only
	 * touches it while unwinding an error.
	 */
	uint32_t *spans;
	ValueArray constants;

	/*
	 * How many local slots this function's frame has, slot 0 being the
	 * callee.
	 *
	 * The compiler has always known this and used it to assign indices; it
	 * just threw the number away, which left nobody able to check that an
	 * OP_GET_LOCAL operand names a slot that exists. Both the verifier and
	 * the JIT need it -- a JIT wants to size a frame once rather than
	 * scanning for the highest slot -- and both were re-deriving it badly.
	 *
	 * Set by the compiler as locals are added. A chunk built by hand in a
	 * test may leave it 0, in which case the verifier skips the local-range
	 * check rather than failing it.
	 */
	int local_count;
} Chunk;

void chunk_init(Chunk *chunk);

/*
 * Append one byte.
 *
 * line is the source line, for the stack trace. offset is the source byte
 * offset the instruction starts at, so a runtime error can underline the
 * expression rather than the whole line. Both are recorded per byte, which
 * makes the end of an instruction the start offset of the next one.
 */
void chunk_write(VM *vm, Chunk *chunk, uint8_t byte, int line, uint32_t offset);
void chunk_free(VM *vm, Chunk *chunk);

/* returns the new index, or -1 if the pool is full. */
int chunk_add_constant(VM *vm, Chunk *chunk, Value value);

/*
 * Operand width of one instruction, in bytes including the opcode.
 *
 * This is the single place that knows the instruction encoding. The compiler
 * emits through emit_indexed() and friends, the disassembler switches on the
 * same numbers, and the verifier below steps over instructions using this.
 * Three independent copies of "how wide is OP_CALL" is three chances to
 * disagree, and a disagreement between the emitter and the verifier is a
 * buffer read out of bounds.
 *
 * Returns 0 for an opcode that is not in the enum, which is what a malformed
 * chunk contains. Callers treat 0 as "this is not a valid instruction", which
 * is precisely the verdict the verifier is reaching.
 */
int chunk_instruction_size(uint8_t opcode);

/* true if `opcode` is a member of the enum. */
bool chunk_opcode_valid(uint8_t opcode);

#endif /* FL_CHUNK_H */
