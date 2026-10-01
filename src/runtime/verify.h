/* SPDX-License-Identifier: MIT */
/*
 * Bytecode verification: the header, and nothing else.
 */
#ifndef FL_VERIFY_H
#define FL_VERIFY_H

#include "object.h"

/*
 * Why a chunk was rejected. message is a static string, not allocated, so
 * there is nothing to free; offset is a byte index into the chunk, which the
 * caller turns into a line using chunk.lines.
 */
typedef struct {
	int offset;
	const char *message;
} FlVerifyError;

/*
 * Check one function's chunk, and recursively every function it closes over.
 *
 * Takes the ObjFunction rather than the Chunk because two of the checks are
 * about things only the function knows: the number of locals, which bounds an
 * OP_GET_LOCAL operand, and the number of upvalues, which bounds an
 * OP_GET_UPVALUE one. Both limits are MAX_LOCALS and MAX_UPVALUES, both are
 * 256, and both operands are single bytes -- so a check against the limits
 * alone cannot fail and catches nothing. Only the function's own counts make
 * these checks mean anything.
 *
 * Called before the bytecode runs, and therefore before anything downstream of
 * it -- the interpreter, and eventually the JIT -- is entitled to assume that
 * every opcode is real, every operand is in range, and every jump lands on an
 * instruction boundary inside this function.
 *
 * Returns true when the chunk is well formed, false otherwise, with error
 * filled in either way. `name` is used only for diagnostics.
 */
bool fl_verify_function(const ObjFunction *function, FlVerifyError *error);

/*
 * Verify a bare Chunk, with no enclosing function.
 *
 * This is the form the unit tests use, because the malformed cases they check
 * cannot be produced by the compiler: there is no way to ask it for an unknown
 * opcode. The two upvalue-bearing checks have nothing to check without a
 * function, and skip themselves when the count is zero.
 */
bool fl_verify_chunk_for_test(
        const Chunk *chunk, const char *name, FlVerifyError *error);

#endif /* FL_VERIFY_H */
