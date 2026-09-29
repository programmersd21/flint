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
