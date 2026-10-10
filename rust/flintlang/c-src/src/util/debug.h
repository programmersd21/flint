/* SPDX-License-Identifier: MIT */
/*
 * The bytecode disassembler.
 *
 * Built in every configuration, called only from the debug build. When the
 * VM does something surprising, look here before you blame the dispatch loop.
 */
#ifndef FL_DEBUG_H
#define FL_DEBUG_H

#include "chunk.h"

void chunk_disassemble(Chunk *chunk, const char *name);

/* prints one instruction and returns the offset of the next one. */
int disassemble_instruction(Chunk *chunk, int offset);

#endif /* FL_DEBUG_H */
