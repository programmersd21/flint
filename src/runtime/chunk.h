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

#endif /* FL_CHUNK_H */
