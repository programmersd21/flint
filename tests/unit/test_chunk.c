/*
 * test_chunk.c -- hand-assembles bytecode and disassembles it.
 *
 * The point is to exercise the disassembler against every operand shape
 * without needing a compiler to produce them:
 *
 *   no operand : OP_ADD, OP_NEGATE, ...
 *   u8         : OP_GET_LOCAL, OP_CALL, OP_BUILD_LIST, ...
 *   u8 index   : OP_CONSTANT, OP_GET_GLOBAL, ...
 *   u24 index  : OP_CONSTANT_LONG
 *   u16 offset : OP_JUMP, OP_LOOP
 *
 * The disassembler decides the next offset from the opcode, so a wrong
 * length shows up as garbage in the output rather than as a crash. Read the
 * output.
 *
 * A NULL VM means the allocator skips collection, which is what makes this
 * usable as a standalone binary.
 */
#include "../../src/core/memory.h"
#include "../../src/runtime/chunk.h"
#include "../../src/util/debug.h"

#include <stdio.h>

int main(void)
{
	Chunk chunk;
	chunk_init(&chunk);

	/* 1 + 2, the smallest interesting program */
	int c0 = chunk_add_constant(NULL, &chunk, NUMBER_VAL(1.5));
	chunk_write(NULL, &chunk, OP_CONSTANT, 1, 0);
	chunk_write(NULL, &chunk, (uint8_t)c0, 1, 0);

	int c1 = chunk_add_constant(NULL, &chunk, NUMBER_VAL(2.5));
	chunk_write(NULL, &chunk, OP_CONSTANT, 1, 0);
	chunk_write(NULL, &chunk, (uint8_t)c1, 1, 0);

	chunk_write(NULL, &chunk, OP_ADD, 1, 0);

	/* line 2 */
	chunk_write(NULL, &chunk, OP_NEGATE, 2, 0);

	/* line 3: the three immediate values */
	chunk_write(NULL, &chunk, OP_NIL, 3, 0);
	chunk_write(NULL, &chunk, OP_TRUE, 3, 0);
	chunk_write(NULL, &chunk, OP_FALSE, 3, 0);

	/* line 4 */
	chunk_write(NULL, &chunk, OP_NOT, 4, 0);

	/* line 5: forward jump of 10 bytes */
	chunk_write(NULL, &chunk, OP_JUMP, 5, 0);
	chunk_write(NULL, &chunk, 0x00, 5, 0);
	chunk_write(NULL, &chunk, 0x0A, 5, 0);

	/* line 6: backward jump of 5 bytes */
	chunk_write(NULL, &chunk, OP_LOOP, 6, 0);
	chunk_write(NULL, &chunk, 0x00, 6, 0);
	chunk_write(NULL, &chunk, 0x05, 6, 0);

	/* line 7: local slot access */
	chunk_write(NULL, &chunk, OP_GET_LOCAL, 7, 0);
	chunk_write(NULL, &chunk, 3, 7, 0);

	chunk_write(NULL, &chunk, OP_SET_LOCAL, 7, 0);
	chunk_write(NULL, &chunk, 3, 7, 0);

	/*
     * line 8: the 24-bit constant form. The compiler only emits it past
     * 256 constants, so fill the pool to get there.
     */
	for (int i = chunk.constants.count; i < 260; i++)
		chunk_add_constant(NULL, &chunk, NUMBER_VAL((double)i));

	int c260 = chunk_add_constant(NULL, &chunk, NUMBER_VAL(260.0));
	chunk_write(NULL, &chunk, OP_CONSTANT_LONG, 8, 0);
	chunk_write(NULL, &chunk, (uint8_t)((c260 >> 16) & 0xFF), 8, 0);
	chunk_write(NULL, &chunk, (uint8_t)((c260 >> 8) & 0xFF), 8, 0);
	chunk_write(NULL, &chunk, (uint8_t)(c260 & 0xFF), 8, 0);

	chunk_write(NULL, &chunk, OP_RETURN, 9, 0);

	/* line 10: the comparison and equality opcodes */
	chunk_write(NULL, &chunk, OP_EQUAL, 10, 0);
	chunk_write(NULL, &chunk, OP_NOT_EQUAL, 10, 0);
	chunk_write(NULL, &chunk, OP_LESS, 10, 0);
	chunk_write(NULL, &chunk, OP_LESS_EQUAL, 10, 0);
	chunk_write(NULL, &chunk, OP_GREATER, 10, 0);
	chunk_write(NULL, &chunk, OP_GREATER_EQUAL, 10, 0);

	/* line 11 */
	chunk_write(NULL, &chunk, OP_SUBTRACT, 11, 0);
	chunk_write(NULL, &chunk, OP_MULTIPLY, 11, 0);
	chunk_write(NULL, &chunk, OP_DIVIDE, 11, 0);
	chunk_write(NULL, &chunk, OP_MODULO, 11, 0);

	/* line 12 */
	chunk_write(NULL, &chunk, OP_POP, 12, 0);
	chunk_write(NULL, &chunk, OP_PRINT, 12, 0);

	/* line 13: call with 3 arguments */
	chunk_write(NULL, &chunk, OP_CALL, 13, 0);
	chunk_write(NULL, &chunk, 3, 13, 0);

	/* line 14 */
	chunk_write(NULL, &chunk, OP_CLOSE_UPVALUE, 14, 0);

	/* line 15: container construction */
	chunk_write(NULL, &chunk, OP_BUILD_LIST, 15, 0);
	chunk_write(NULL, &chunk, 4, 15, 0);
	chunk_write(NULL, &chunk, OP_BUILD_TABLE, 15, 0);
	chunk_write(NULL, &chunk, 2, 15, 0);

	/* line 16: indexing */
	chunk_write(NULL, &chunk, OP_GET_INDEX, 16, 0);
	chunk_write(NULL, &chunk, OP_SET_INDEX, 16, 0);

	printf("--- Disassembly output ---\n");
	chunk_disassemble(&chunk, "test_chunk");
	printf("--- End ---\n");

	printf("Constants: %d\n", chunk.constants.count);

	chunk_free(NULL, &chunk);
	printf("test_chunk: PASSED\n");
	return 0;
}
