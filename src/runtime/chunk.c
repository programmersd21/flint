/* SPDX-License-Identifier: MIT */
/*
 * Chunk and constant pool: growable byte arrays.
 *
 * Nothing clever here. code and lines grow in lockstep so that a line lookup
 * is a single index, and the constant pool is a plain value array.
 */
#include "chunk.h"
#include "common.h"
#include "memory.h"
#include "stdint.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
void value_array_init(ValueArray *array)
{
	array->count = 0;
	array->capacity = 0;
	array->values = NULL;
}

void value_array_write(VM *vm, ValueArray *array, Value value)
{
	if (array->count + 1 > array->capacity) {
		int old_capacity = array->capacity;
		array->capacity = GROW_CAPACITY(old_capacity);
		array->values = GROW_ARRAY(vm,
		        Value,
		        array->values,
		        old_capacity,
		        array->capacity);
	}
	array->values[array->count] = value;
	array->count++;
}

void value_array_free(VM *vm, ValueArray *array)
{
	FREE_ARRAY(vm, Value, array->values, array->capacity);
	value_array_init(array);
}

void chunk_init(Chunk *chunk)
{
	chunk->count = 0;
	chunk->capacity = 0;
	chunk->code = NULL;
	chunk->lines = NULL;
	chunk->spans = NULL;
	chunk->local_count = 0;
	value_array_init(&chunk->constants);
}

/*
 * One byte of code, one int of line. The line is the caller's line, which
 * is parser.previous.line at the point of the call, so an error blames the
 * operator and not the operand that followed it.
 */
void chunk_write(VM *vm, Chunk *chunk, uint8_t byte, int line, uint32_t offset)
{
	if (chunk->count + 1 > chunk->capacity) {
		int old_capacity = chunk->capacity;
		chunk->capacity = GROW_CAPACITY(old_capacity);
		chunk->code = GROW_ARRAY(vm,
		        uint8_t,
		        chunk->code,
		        old_capacity,
		        chunk->capacity);
		chunk->lines = GROW_ARRAY(
		        vm, int, chunk->lines, old_capacity, chunk->capacity);
		chunk->spans = GROW_ARRAY(vm,
		        uint32_t,
		        chunk->spans,
		        old_capacity,
		        chunk->capacity);
	}
	chunk->code[chunk->count] = byte;
	chunk->lines[chunk->count] = line;
	chunk->spans[chunk->count] = offset;
	chunk->count++;
}

void chunk_free(VM *vm, Chunk *chunk)
{
	FREE_ARRAY(vm, uint8_t, chunk->code, chunk->capacity);
	FREE_ARRAY(vm, int, chunk->lines, chunk->capacity);
	FREE_ARRAY(vm, uint32_t, chunk->spans, chunk->capacity);
	value_array_free(vm, &chunk->constants);
	chunk_init(chunk);
}

/*
 * Returns the index of the new constant, or -1 when the pool is full. The
 * caller is expected to report that as a compile error rather than to
 * recover, since there is no way to shrink a pool that is already full.
 */
int chunk_add_constant(VM *vm, Chunk *chunk, Value value)
{
	if (chunk->constants.count >= MAX_CONSTANTS) {
		fprintf(stderr, "Too many constants in one chunk.\n");
		return -1;
	}
	value_array_write(vm, &chunk->constants, value);
	return chunk->constants.count - 1;
}

bool chunk_opcode_valid(uint8_t opcode)
{
	/* OP_JUMP_IF_NOT_NIL is the last member.
	 * Update this when adding opcodes -- and note that getting it wrong
	 * makes every opcode above the true end read as invalid, which is
	 * what happened once already. */
	return opcode <= OP_JUMP_IF_NOT_NIL;
}

int chunk_instruction_size(uint8_t opcode)
{
	switch (opcode) {
	/* one byte: no operand */
	case OP_NIL:
	case OP_TRUE:
	case OP_FALSE:
	case OP_POP:
	case OP_EQUAL:
	case OP_NOT_EQUAL:
	case OP_GREATER:
	case OP_GREATER_EQUAL:
	case OP_LESS:
	case OP_LESS_EQUAL:
	case OP_ADD:
	case OP_SUBTRACT:
	case OP_MULTIPLY:
	case OP_DIVIDE:
	case OP_MODULO:
	case OP_NOT:
	case OP_NEGATE:
	case OP_PRINT:
	case OP_LIST_LEN:
	case OP_TABLE_COUNT:
	case OP_TABLE_KEY:
	case OP_TABLE_VALUE:
	/* indexing is a bare opcode: the operands are already on the stack */
	case OP_GET_INDEX:
	case OP_SET_INDEX:
	case OP_CLOSE_UPVALUE:
	case OP_RETURN:
	case OP_BUILD_TABLE:
	case OP_IMPORT:
	case OP_EXPORT:
		return 1;

	/* two bytes: one u8 operand */
	case OP_CONSTANT:
	case OP_GET_LOCAL:
	case OP_SET_LOCAL:
	case OP_GET_UPVALUE:
	case OP_SET_UPVALUE:
	case OP_CALL:
	case OP_CAST:
	case OP_BUILD_LIST:
	case OP_GET_GLOBAL:
	case OP_DEFINE_GLOBAL:
	case OP_DEFINE_GLOBAL_CONST:
	case OP_SET_GLOBAL:
	case OP_GET_FIELD:
	case OP_SET_FIELD:
	case OP_SET_FIELD_TOP:
		return 2;

	/* three bytes: a u16 jump offset */
	case OP_JUMP:
	case OP_JUMP_IF_FALSE:
	case OP_JUMP_IF_NOT_NIL:
	case OP_LOOP:
		return 3;

	/*
	 * Four bytes: a 24-bit constant index, or a u8 index for the short
	 * forms that have one.
	 */
	case OP_CONSTANT_LONG:
	case OP_GET_GLOBAL_LONG:
	case OP_DEFINE_GLOBAL_LONG:
	case OP_DEFINE_GLOBAL_CONST_LONG:
	case OP_SET_GLOBAL_LONG:
	case OP_GET_FIELD_LONG:
	case OP_SET_FIELD_LONG:
	case OP_SET_FIELD_TOP_LONG:
	/*
	 * Four is the minimum. The two extra bytes are one (is_local, index)
	 * pair per upvalue the closure captures, so the real width depends on
	 * the constant this names and cannot be known without the pool.
	 *
	 * Every caller in this codebase only needs "is this a fixed width, and
	 * what is the minimum", and all of them handle OP_CLOSURE separately:
	 * the verifier recurses into the named function, and the disassembler
	 * walks the pairs itself. Returning 0 instead would make the minimum
	 * unanswerable, and a 0 means "malformed" everywhere else here.
	 */
	case OP_CLOSURE:
	case OP_CLOSURE_LONG:
		return 4;

	/* specialized numeric ops: one byte, no operand */
	case OP_ADD_NUM:
	case OP_SUB_NUM:
	case OP_MUL_NUM:
	case OP_DIV_NUM:
	case OP_MOD_NUM:
	case OP_LT_NUM:
	case OP_LE_NUM:
	case OP_GT_NUM:
	case OP_GE_NUM:
	case OP_EQ_NUM:
	case OP_NEQ_NUM:
	case OP_DEFINE_GLOBAL_EXPORT:
	case OP_DEFINE_GLOBAL_CONST_EXPORT:
		return 2;
	case OP_DEFINE_GLOBAL_EXPORT_LONG:
	case OP_DEFINE_GLOBAL_CONST_EXPORT_LONG:
		return 4;
	case OP_NEG_NUM:
		return 1;
	}
	return 0;
}
