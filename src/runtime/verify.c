/* SPDX-License-Identifier: MIT */
/*
 * Bytecode verification.
 *
 * Two questions this answers, and it is worth being clear about why they are
 * worth a pass at all.
 *
 * 1. The compiler produces well-formed bytecode. That is an assumption, and
 *    assumptions about memory offsets are the expensive kind to hold. A bug in
 *    a jump patch or a constant index produces bytecode that reads out of
 *    bounds: a crash with no useful message rather than a diagnostic.
 *
 * 2. The JIT makes far more assumptions than the interpreter does. The
 *    interpreter re-reads the opcode and its operand from the code array on
 *    every execution, so a wrong operand is a wrong answer, or a bounds error
 *    it happens to survive. Compiled code bakes the operand into a machine
 *    instruction and reads it once, which turns "usually correct" into "correct
 *    if the input was valid". That is a much larger gap to close.
 *
 * So this is not belt-and-braces. It is the precondition for the second half of
 * the runtime.
 *
 * What it checks, and why each of those is here:
 *
 *   opcode is a member of the enum      the run loop would index a jump table
 *   operand width is known              the walk steps over it
 *   instruction does not run past the end
 *   constant index is in range          the run loop would read the pool
 *   local slot is below the function's own slot count
 *   upvalue slot is below the function's own upvalue count
 *   cast type tag is a real FlType
 *   jump target is inside the code
 *   jump target is an instruction boundary
 *   nested functions, recursively
 *
 * What it does not check: stack depth. That is a deliberate omission with a
 * reason, and it is worth stating rather than rediscovering.
 *
 * A depth check needs the control-flow graph. OP_JUMP_IF_FALSE leaves its
 * condition on the stack for the branch to pop, so the two edges out of every if
 * arrive at different depths, and every merge point has to reconcile them. That
 * is a depth per basic block plus a merge rule -- most of a real verifier.
 *
 * A linear walk can pick one edge and check the other optimistically, and then
 * it rejects the compiler's own output on ordinary programs. A check that fails
 * on valid bytecode is worse than no check: it is not a safety net, it is a new
 * source of false crashes on code that works. That is not a hypothetical; an
 * earlier version of this file did exactly that and spent an afternoon being
 * wrong about it.
 *
 * The checks above are the ones that stop a malformed chunk from reading out of
 * bounds, which is what the JIT actually needs. A chunk that genuinely
 * underflows the stack is caught by the interpreter, which executes every
 * instruction itself and was going to misbehave anyway.
 */
#include "verify.h"
#include "chunk.h"
#include "object.h"
#include "value.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(FlVerifyError *error, int offset, const char *message)
{
	error->offset = offset;
	error->message = message;
}

/* Does this opcode's operand name a constant? */
static bool operand_is_constant_index(uint8_t opcode)
{
	switch (opcode) {
	case OP_CONSTANT:
	case OP_CLOSURE:
	case OP_GET_GLOBAL:
	case OP_DEFINE_GLOBAL:
	case OP_DEFINE_GLOBAL_CONST:
	case OP_SET_GLOBAL:
	case OP_GET_FIELD:
	case OP_SET_FIELD:
	case OP_SET_FIELD_TOP:
	case OP_CONSTANT_LONG:
	case OP_CLOSURE_LONG:
	case OP_GET_GLOBAL_LONG:
	case OP_DEFINE_GLOBAL_LONG:
	case OP_DEFINE_GLOBAL_CONST_LONG:
	case OP_SET_GLOBAL_LONG:
	case OP_GET_FIELD_LONG:
	case OP_SET_FIELD_LONG:
	case OP_SET_FIELD_TOP_LONG:
	case OP_DEFINE_GLOBAL_EXPORT:
	case OP_DEFINE_GLOBAL_EXPORT_LONG:
	case OP_DEFINE_GLOBAL_CONST_EXPORT:
	case OP_DEFINE_GLOBAL_CONST_EXPORT_LONG:
		return true;
	default:
		return false;
	}
}

/* Is this one of the wide (24-bit operand) forms? */
static bool opcode_is_long_form(uint8_t opcode)
{
	switch (opcode) {
	case OP_CONSTANT_LONG:
	case OP_CLOSURE_LONG:
	case OP_GET_GLOBAL_LONG:
	case OP_DEFINE_GLOBAL_LONG:
	case OP_DEFINE_GLOBAL_CONST_LONG:
	case OP_SET_GLOBAL_LONG:
	case OP_GET_FIELD_LONG:
	case OP_SET_FIELD_LONG:
	case OP_SET_FIELD_TOP_LONG:
	case OP_DEFINE_GLOBAL_EXPORT_LONG:
	case OP_DEFINE_GLOBAL_CONST_EXPORT_LONG:
		return true;
	default:
		return false;
	}
}

/* Read an instruction's constant operand, either width. */
static int constant_operand(const uint8_t *code, bool wide)
{
	if (!wide)
		return (int)code[1];
	return (int)((code[1] << 16) | (code[2] << 8) | code[3]);
}

/*
 * The width of an OP_CLOSURE, which chunk_instruction_size() cannot know.
 *
 * The opcode is followed by a constant index and then one (is_local, index)
 * byte pair per upvalue the closure captures. The pair count lives in the
 * ObjFunction the constant names, so the width is a property of the constant
 * pool rather than of the opcode.
 *
 * Both passes below need this, and getting it wrong in either is the same bug: a
 * walk that steps over the wrong number of bytes desynchronizes and then
 * reports every instruction after the first closure as malformed.
 */
static int closure_width(const Chunk *chunk, int offset, bool wide)
{
	int base = wide ? 4 : 2;
	int index = constant_operand(chunk->code + offset, wide);

	if (index < 0 || index >= chunk->constants.count)
		return base; /* the operand check reports this properly */
	Value constant = chunk->constants.values[index];
	if (!IS_FUNCTION(constant))
		return base;
	return base + 2 * AS_FUNCTION(constant)->upvalue_count;
}

/*
 * The core, over a bare Chunk.
 *
 * `upvalue_count` is the enclosing function's upvalue count, or 0 for a chunk
 * with no function around it (the unit tests). It bounds the OP_GET_UPVALUE
 * operand; 0 disables that check, which is correct for a bare chunk because
 * there is nothing for the operand to mean.
 */
static bool verify_chunk(
        const Chunk *chunk, int upvalue_count, FlVerifyError *error)
{
	memset(error, 0, sizeof(*error));

	/*
	 * Zeroed, because the choice of which error to report at the end reads
	 * this message. An uninitialised stack struct holds whatever was there
	 * before, and "some pointer that was on this stack earlier" is a
	 * segfault inside the diagnostic -- on the one path whose whole job is
	 * to explain a compiler bug without crashing.
	 */
	FlVerifyError nested_error;
	memset(&nested_error, 0, sizeof(nested_error));

	/*
	 * An empty chunk is not an error. The compiler emits one for a function
	 * that exists only to be called, and a malformed-chunk check that
	 * rejected it would be rejecting valid input.
	 */
	if (chunk->count == 0)
		return true;

	/*
	 * code, lines and spans are written in lockstep by chunk_write(), so
	 * they are always the same length. The check costs nothing and turns a
	 * mismatch into a diagnostic instead of an out-of-bounds read in the
	 * error path that walks them.
	 */
	if (chunk->lines != NULL && chunk->capacity > 0 &&
	        (int)chunk->capacity < chunk->count) {
		fail(error, 0, "line table is shorter than the code");
		return false;
	}

	/*
	 * is_boundary[i] is true for the first byte of an instruction.
	 *
	 * Filled by a pass of its own, before the main walk, and that ordering
	 * is the whole reason it is separate. A forward jump names a byte that
	 * has not been reached yet, so "have I walked past here" and "does an
	 * instruction start here" cannot be the same question in a single
	 * left-to-right pass: the honest answer for a legal forward jump target
	 * is not yet, which is indistinguishable from a jump into an operand.
	 *
	 * Marking boundaries first costs one extra walk over bytes that are
	 * already in cache and removes the ambiguity entirely.
	 */
	uint8_t *is_boundary =
	        (uint8_t *)calloc((size_t)chunk->count, sizeof(uint8_t));
	if (is_boundary == NULL) {
		fail(error, 0, "out of memory verifying bytecode");
		return false;
	}

	for (int i = 0; i < chunk->count;) {
		uint8_t opcode = chunk->code[i];
		int size = chunk_opcode_valid(opcode)
		                   ? chunk_instruction_size(opcode)
		                   : 0;

		/*
		 * An unknown opcode ends this pass. The main walk reports it with
		 * better context; stopping here just leaves is_boundary unfilled
		 * past that point, and the walk never consults a boundary it has
		 * not already accepted the instruction for.
		 */
		if (size == 0)
			break;
		if (opcode == OP_CLOSURE || opcode == OP_CLOSURE_LONG)
			size = closure_width(
			        chunk, i, opcode == OP_CLOSURE_LONG);
		if (i + size > chunk->count)
			break;

		is_boundary[i] = 1;
		i += size;
	}

	const uint8_t *code = chunk->code;
	int offset = 0;
	bool ok = true;

	while (offset < chunk->count) {
		uint8_t opcode = code[offset];

		if (!chunk_opcode_valid(opcode)) {
			fail(error, offset, "unknown opcode in bytecode");
			ok = false;
			break;
		}

		int size = chunk_instruction_size(opcode);
		if (size == 0) {
			fail(error, offset, "opcode has no defined width");
			ok = false;
			break;
		}
		if (opcode == OP_CLOSURE || opcode == OP_CLOSURE_LONG)
			size = closure_width(
			        chunk, offset, opcode == OP_CLOSURE_LONG);
		if (offset + size > chunk->count) {
			fail(error,
			        offset,
			        "instruction runs past the end of the code");
			ok = false;
			break;
		}

		if (operand_is_constant_index(opcode)) {
			bool wide = opcode_is_long_form(opcode);
			int index = constant_operand(code + offset, wide);

			if (index < 0 || index >= chunk->constants.count) {
				fail(error,
				        offset,
				        "constant index is out of range");
				ok = false;
				break;
			}

			/*
			 * A closure's constant must be an ObjFunction, and its own
			 * chunk has to verify too. Recursing here rather than in the
			 * caller means every path into the bytecode -- --check, the
			 * interpreter, the JIT -- gets nested functions checked by
			 * the same code.
			 */
			if (opcode == OP_CLOSURE || opcode == OP_CLOSURE_LONG) {
				Value constant = chunk->constants.values[index];
				if (!IS_FUNCTION(constant)) {
					fail(error,
					        offset,
					        "closure operand is not a "
					        "function");
					ok = false;
					break;
				}
				if (!fl_verify_function(AS_FUNCTION(constant),
				            &nested_error)) {
					ok = false;
					break;
				}
			}
		}

		/*
		 * Against the function's own counts, not against the MAX_*
		 * limits. The operand is a byte, so it can never exceed a limit of
		 * 256 and a check against that catches nothing. What it can do is
		 * name a slot this function does not have, which reads whatever
		 * the caller's frame happens to put there.
		 *
		 * A chunk with local_count of 0 was built by hand rather than by
		 * the compiler, so the check is skipped rather than failed; see
		 * Chunk.local_count.
		 */
		if (opcode == OP_GET_LOCAL || opcode == OP_SET_LOCAL) {
			if (chunk->local_count > 0 &&
			        (int)code[offset + 1] >= chunk->local_count) {
				fail(error,
				        offset,
				        "local slot is out of range");
				ok = false;
				break;
			}
		}
		if (opcode == OP_GET_UPVALUE || opcode == OP_SET_UPVALUE) {
			if (upvalue_count > 0 &&
			        code[offset + 1] >= (uint8_t)upvalue_count) {
				fail(error,
				        offset,
				        "upvalue slot is out of range");
				ok = false;
				break;
			}
		}

		if (opcode == OP_CAST &&
		        (int)code[offset + 1] > FL_TYPE_FUNCTION) {
			fail(error, offset, "cast type tag is out of range");
			ok = false;
			break;
		}

		if (opcode == OP_JUMP || opcode == OP_JUMP_IF_FALSE ||
		        opcode == OP_LOOP) {
			int delta = (int)((code[offset + 1] << 8) |
			                  code[offset + 2]);
			/*
			 * Measured from the end of the instruction, for both
			 * directions. That is where ip is when the VM applies the
			 * delta: READ_SHORT advances ip past the operand, so the
			 * forward case adds and the backward case subtracts from
			 * offset + 3.
			 *
			 * A loop is emitted as `jump_offset = here - (loop_start +
			 * 3)`, so target = here + 3 - offset is loop_start. The
			 * same formula as the disassembler, deliberately: two
			 * implementations of the same encoding drift apart, and this
			 * is the third time that has happened here.
			 */
			int target = opcode == OP_LOOP ? offset + 3 - delta
			                               : offset + 3 + delta;
			if (target < 0 || target >= chunk->count) {
				fail(error,
				        offset,
				        "jump target is outside the code");
				ok = false;
				break;
			}
			/*
			 * Landing on an instruction boundary. Without this, a jump
			 * into the middle of a two-byte operand reads that operand
			 * byte as an opcode -- a bug that can survive a long time
			 * before it crashes, and one the JIT cannot survive at all.
			 */
			if (!is_boundary[target]) {
				fail(error,
				        offset,
				        "jump target is not an instruction "
				        "boundary");
				ok = false;
				break;
			}
		}

		offset += size;
	}

	free(is_boundary);

	/*
	 * A nested failure replaces the outer one. The outer offset points at
	 * an OP_CLOSURE, which is a perfectly valid instruction, and reporting
	 * it would send the reader to the wrong line for a bug in a function
	 * they may not even have open.
	 */
	if (!ok && nested_error.message != NULL)
		*error = nested_error;
	return ok;
}
bool fl_verify_function(const ObjFunction *function, FlVerifyError *error)
{
	return verify_chunk(&function->chunk, function->upvalue_count, error);
}

bool fl_verify_chunk_for_test(
        const Chunk *chunk, const char *name, FlVerifyError *error)
{
	(void)name;
	/* 0 upvalues: the upvalue check compares against this and skips
	 * itself, which is what a bare chunk wants. */
	return verify_chunk(chunk, 0, error);
}
