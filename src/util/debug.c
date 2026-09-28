/* SPDX-License-Identifier: MIT */
/*
 * Bytecode disassembler.
 *
 * Compiled in every configuration and called only from the debug build.
 * Each helper prints one instruction in the operand shape it handles and
 * returns the offset of the next one, which is what lets the loop below be
 * a plain for with an empty increment.
 */
#include "debug.h"
#include "chunk.h"
#include "object.h"
#include "stdint.h"
#include "value.h"

#include <stdio.h>
/* short form for disassembly. the real formatting is in the vm. */
static void print_value_brief(Value value)
{
	if (IS_NUMBER(value)) {
		double d = AS_NUMBER(value);
		/* integral values print as integers, same rule as print() */
		if (fl_double_is_printable_int(d))
			printf("%ld", fl_double_to_long(d));
		else
			printf("%g", d);
	} else if (IS_NIL(value)) {
		printf("nil");
	} else if (IS_TRUE(value)) {
		printf("true");
	} else if (IS_FALSE(value)) {
		printf("false");
	} else if (IS_OBJ(value)) {
		/* the address, because seeing that two constants are the
		 * same object is how you debug interning */
		printf("<obj %p>", AS_OBJ_PTR(value));
	} else {
		printf("<unknown>");
	}
}

/* no operand. one byte of code. */
static int simple_instruction(const char *name, int offset)
{
	printf("%s\n", name);
	return offset + 1;
}

/* one u8 constant index, plus the value it points at. */
static int constant_instruction(const char *name, Chunk *chunk, int offset)
{
	uint8_t idx = chunk->code[offset + 1];
	printf("%-20s %4d ; ", name, idx);
	/* a bad index is a compiler bug, not a user error, but printing
	 * garbage is worse than saying so */
	if (idx < chunk->constants.count)
		print_value_brief(chunk->constants.values[idx]);
	else
		printf("??? (out of range)");
	printf("\n");
	return offset + 2;
}

/*
 * 24-bit constant index, for chunks past 256 constants. A function big
 * enough to need this was almost certainly generated, which is a good
 * reason to look at it.
 */
static int constant_long_instruction(const char *name, Chunk *chunk, int offset)
{
	uint32_t idx = (uint32_t)chunk->code[offset + 1] << 16 |
	               (uint32_t)chunk->code[offset + 2] << 8 |
	               (uint32_t)chunk->code[offset + 3];
	printf("%-20s %4u ; ", name, idx);
	if (idx < (uint32_t)chunk->constants.count)
		print_value_brief(chunk->constants.values[idx]);
	else
		printf("??? (out of range)");
	printf("\n");
	return offset + 4;
}

/* one u8 operand: a local slot, an argument count, a call target. */
static int byte_instruction(const char *name, Chunk *chunk, int offset)
{
	uint8_t slot = chunk->code[offset + 1];
	printf("%-20s %4d\n", name, slot);
	(void)chunk;
	return offset + 2;
}

/*
 * A 16-bit jump offset plus the absolute target it points at. sign is +1 for
 * the forward jumps and -1 for OP_LOOP, which jumps backwards. Printing the
 * target is the whole point; an offset alone is not readable.
 */
static int jump_instruction(
        const char *name, int sign, Chunk *chunk, int offset)
{
	uint16_t jump = (uint16_t)((uint16_t)chunk->code[offset + 1] << 8 |
	                           (uint16_t)chunk->code[offset + 2]);
	int target = offset + 3 + sign * (int)jump;
	printf("%-20s %4d -> %d\n", name, jump, target);
	(void)chunk;
	return offset + 3;
}

/*
 * OP_CLOSURE is a constant index followed by two bytes per upvalue. Those
 * bytes are not decoded here: the count lives in the function object, which
 * this offset alone cannot reach.
 */
static int closure_instruction(Chunk *chunk, int offset)
{
	offset++;
	uint8_t fn_idx = chunk->code[offset++];
	printf("%-20s %4d ; ", "OP_CLOSURE", fn_idx);
	if (fn_idx < chunk->constants.count)
		print_value_brief(chunk->constants.values[fn_idx]);
	printf("\n");
	return offset;
}

void chunk_disassemble(Chunk *chunk, const char *name)
{
	printf("== %s ==\n", name);
	for (int offset = 0; offset < chunk->count;)
		offset = disassemble_instruction(chunk, offset);
}

/* one instruction: offset, source line, mnemonic, operands. */
int disassemble_instruction(Chunk *chunk, int offset)
{
	printf("%04d ", offset);

	/* repeat the line only when it changes. every byte of code has a
	 * line number, so printing them all is unreadable */
	if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1])
		printf("   | ");
	else
		printf("%4d ", chunk->lines[offset]);

	/*
	 * A new opcode here means adding the case to the compiler's rules
	 * table too. An unknown opcode prints and skips one byte, which
	 * loses sync with the operands, but the alternative is a crash.
	 */
	uint8_t instruction = chunk->code[offset];
	switch (instruction) {
	case OP_CONSTANT:
		return constant_instruction("OP_CONSTANT", chunk, offset);
	case OP_CONSTANT_LONG:
		return constant_long_instruction(
		        "OP_CONSTANT_LONG", chunk, offset);
	case OP_NIL:
		return simple_instruction("OP_NIL", offset);
	case OP_TRUE:
		return simple_instruction("OP_TRUE", offset);
	case OP_FALSE:
		return simple_instruction("OP_FALSE", offset);
	case OP_POP:
		return simple_instruction("OP_POP", offset);
	case OP_GET_LOCAL:
		return byte_instruction("OP_GET_LOCAL", chunk, offset);
	case OP_SET_LOCAL:
		return byte_instruction("OP_SET_LOCAL", chunk, offset);
	case OP_GET_GLOBAL:
		return constant_instruction("OP_GET_GLOBAL", chunk, offset);
	case OP_DEFINE_GLOBAL:
		return constant_instruction("OP_DEFINE_GLOBAL", chunk, offset);
	case OP_DEFINE_GLOBAL_CONST:
		return constant_instruction(
		        "OP_DEFINE_GLOBAL_CONST", chunk, offset);
	case OP_SET_GLOBAL:
		return constant_instruction("OP_SET_GLOBAL", chunk, offset);
	case OP_GET_UPVALUE:
		return byte_instruction("OP_GET_UPVALUE", chunk, offset);
	case OP_SET_UPVALUE:
		return byte_instruction("OP_SET_UPVALUE", chunk, offset);
	case OP_EQUAL:
		return simple_instruction("OP_EQUAL", offset);
	case OP_NOT_EQUAL:
		return simple_instruction("OP_NOT_EQUAL", offset);
	case OP_CAST: {
		/* the operand is a type tag, not a constant index, so it
		 * gets its own printer rather than reusing byte_instruction
		 * and leaving the reader to guess what the number means */
		uint8_t tag = chunk->code[offset + 1];
		printf("%-20s %4s\n",
		        "OP_CAST",
		        flint_type_name_of((FlType)tag));
		return offset + 2;
	}
	case OP_GREATER:
		return simple_instruction("OP_GREATER", offset);
	case OP_GREATER_EQUAL:
		return simple_instruction("OP_GREATER_EQUAL", offset);
	case OP_LESS:
		return simple_instruction("OP_LESS", offset);
	case OP_LESS_EQUAL:
		return simple_instruction("OP_LESS_EQUAL", offset);
	case OP_ADD:
		return simple_instruction("OP_ADD", offset);
	case OP_SUBTRACT:
		return simple_instruction("OP_SUBTRACT", offset);
	case OP_MULTIPLY:
		return simple_instruction("OP_MULTIPLY", offset);
	case OP_DIVIDE:
		return simple_instruction("OP_DIVIDE", offset);
	case OP_MODULO:
		return simple_instruction("OP_MODULO", offset);
	case OP_NOT:
		return simple_instruction("OP_NOT", offset);
	case OP_NEGATE:
		return simple_instruction("OP_NEGATE", offset);
	case OP_PRINT:
		return simple_instruction("OP_PRINT", offset);
	case OP_JUMP:
		return jump_instruction("OP_JUMP", 1, chunk, offset);
	case OP_JUMP_IF_FALSE:
		return jump_instruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
	case OP_LOOP:
		return jump_instruction("OP_LOOP", -1, chunk, offset);
	case OP_CALL:
		return byte_instruction("OP_CALL", chunk, offset);
	case OP_CLOSURE:
		return closure_instruction(chunk, offset);
	case OP_CLOSE_UPVALUE:
		return simple_instruction("OP_CLOSE_UPVALUE", offset);
	case OP_RETURN:
		return simple_instruction("OP_RETURN", offset);
	case OP_BUILD_LIST:
		return byte_instruction("OP_BUILD_LIST", chunk, offset);
	/*
	 * OP_BUILD_TABLE takes no operand. The compiler emits the bare
	 * opcode and then fills the table with OP_SET_FIELD pairs, so
	 * printing it as a u8-operand instruction desynchronized the whole
	 * rest of the dump. That matters more than a wrong length: a
	 * desynchronized disassembly sends you looking at the wrong
	 * bytecode entirely, which is how a table-literal bug ends up
	 * looking like a GC bug.
	 */
	case OP_BUILD_TABLE:
		return simple_instruction("OP_BUILD_TABLE", offset);
	case OP_GET_INDEX:
		return simple_instruction("OP_GET_INDEX", offset);
	case OP_SET_INDEX:
		return simple_instruction("OP_SET_INDEX", offset);
	case OP_GET_FIELD:
		return constant_instruction("OP_GET_FIELD", chunk, offset);
	case OP_SET_FIELD:
		return constant_instruction("OP_SET_FIELD", chunk, offset);
	case OP_SET_FIELD_TOP:
		return constant_instruction("OP_SET_FIELD_TOP", chunk, offset);
	case OP_IMPORT:
		return constant_instruction("OP_IMPORT", chunk, offset);
	case OP_EXPORT:
		return constant_instruction("OP_EXPORT", chunk, offset);
	default:
		printf("Unknown opcode %d\n", instruction);
		return offset + 1;
	}
}
