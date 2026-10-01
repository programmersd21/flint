/* SPDX-License-Identifier: MIT */
/*
 * The verifier, and the instruction-width table it depends on.
 *
 * Both exist because the JIT needs to trust the bytecode. Both were also, at
 * one point, wrong in ways that only showed up as "the verifier rejected the
 * compiler's own output", which is an unhelpful way to find a bug.
 *
 * The tests here are therefore mostly about the width table agreeing with what
 * the compiler actually emits, and about the verifier accepting every valid
 * chunk in the test suite. A verifier that fails valid bytecode is worse than
 * no verifier, so the "accepts" tests are the important ones.
 */
#include "chunk.h"
#include "compiler.h"
#include "memory.h"
#include "object.h"
#include "value.h"
#include "verify.h"
#include "vm.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void check(bool condition, const char *what)
{
	checks++;
	if (!condition) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

/* every opcode must have a known width, or the verifier cannot step over it */
static void test_every_opcode_has_a_width(void)
{
	for (int opcode = 0; opcode <= OP_CLOSURE_LONG; opcode++) {
		int size = chunk_instruction_size((uint8_t)opcode);
		if (size <= 0) {
			char message[96];
			snprintf(message,
			        sizeof message,
			        "opcode %d has no width",
			        opcode);
			check(false, message);
		}
	}
}

/*
 * Compile a program and verify it.
 *
 * Returns the top-level function, or NULL if it failed to compile. The VM is
 * left for the caller to free.
 */
static ObjFunction *compile_and_verify(VM *vm, const char *source, bool *ok)
{
	ObjFunction *function = compile_named(vm, source, "<test>");
	if (function == NULL) {
		*ok = false;
		return NULL;
	}
	FlVerifyError error;
	*ok = fl_verify_function(function, &error);
	return function;
}

/*
 * The table must agree with the compiler.
 *
 * Each program below is a single chunk whose instructions the width table has
 * to step over correctly. If any width is wrong the walk desynchronizes and
 * reports either a nonsense offset or a spurious failure, which is exactly the
 * bug this file exists to prevent.
 */
static void test_widths_agree_with_the_compiler(void)
{
	static const char *const programs[] = {
	        /* every fixed-width operand shape */
	        "let x = 1\nx = x + 1\n",
	        "let x = 1\nlet y = 2\nprint(x + y)\n",
	        "let t = {}\nt.a = 1\nprint(t.a)\n",
	        "let t = {a: 1, b: 2}\nprint(t.a + t.b)\n",
	        "let xs = [1, 2, 3]\nprint(xs[0])\n",
	        "let xs = [1, 2, 3]\nxs[1] = 9\n",
	        "let xs = []\nfor x in xs { print(x) }\n",
	        "for i in 0..10 { print(i) }\n",
	        "if 1 > 2 { print(1) } else { print(2) }\n",
	        "let i = 0\nwhile i < 3 { i += 1 }\nprint(i)\n",
	        "fn f(x) { return x }\nprint(f(1) as number)\n",
	        "fn f() { return nil }\nprint(type(f()))\n",
	        /* a closure, which is the one variable-width instruction */
	        "fn outer() {\n"
	        "    let n = 0\n"
	        "    fn inner() {\n"
	        "        n += 1\n"
	        "        return n\n"
	        "    }\n"
	        "    return inner\n"
	        "}\n"
	        "print(outer()())\n",
	        /* closures capturing several upvalues */
	        "fn outer() {\n"
	        "    let a = 1\n"
	        "    let b = 2\n"
	        "    fn one() { return a }\n"
	        "    fn two() { return b }\n"
	        "    return [one, two]\n"
	        "}\n",
	        /* the for-range and for-in shapes together, which is where the
	         * local-slot peak used to be recorded wrongly */
	        "for n in [1, 2] { print(n) }\n"
	        "for n in 0..2 { print(n) }\n",
	        "for n in 0..2 { print(n) }\n"
	        "for n in [1, 2] { print(n) }\n",
	};

	VM vm;
	for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
		vm_init(&vm);
		bool ok;
		ObjFunction *function =
		        compile_and_verify(&vm, programs[i], &ok);
		char message[128];
		snprintf(message,
		        sizeof message,
		        "program %zu verifies: %.48s",
		        i,
		        programs[i]);
		check(ok && function != NULL, message);
		vm_free(&vm);
	}
}

/*
 * A chunk is malformed in ways a hand-built one can express and the compiler
 * cannot produce. These are the checks the JIT depends on.
 */
static void test_rejects_malformed(void)
{
	/* an unknown opcode: 0xFE is above the last member */
	{
		Chunk chunk;
		chunk_init(&chunk);
		chunk_write(NULL, &chunk, 0xFE, 1, 0);
		FlVerifyError error;
		check(!fl_verify_chunk_for_test(&chunk, "bad-opcode", &error),
		        "rejects an unknown opcode");
		chunk_free(NULL, &chunk);
	}

	/* a constant index past the end of the pool */
	{
		Chunk chunk;
		chunk_init(&chunk);
		chunk_write(NULL, &chunk, OP_CONSTANT, 1, 0);
		chunk_write(NULL,
		        &chunk,
		        (uint8_t)200,
		        1,
		        0); /* no such constant */
		FlVerifyError error;
		check(!fl_verify_chunk_for_test(&chunk, "bad-const", &error),
		        "rejects an out-of-range constant index");
		chunk_free(NULL, &chunk);
	}

	/* a forward jump past the end of the code */
	{
		Chunk chunk;
		chunk_init(&chunk);
		chunk_write(NULL, &chunk, OP_JUMP, 1, 0);
		chunk_write(NULL, &chunk, 0x7F, 1, 0);
		chunk_write(NULL, &chunk, 0xFF, 1, 0);
		FlVerifyError error;
		check(!fl_verify_chunk_for_test(&chunk, "bad-jump", &error),
		        "rejects a jump outside the code");
		chunk_free(NULL, &chunk);
	}

	/* an instruction that runs off the end: OP_CONSTANT with no operand */
	{
		Chunk chunk;
		chunk_init(&chunk);
		chunk_write(NULL, &chunk, OP_CONSTANT, 1, 0);
		FlVerifyError error;
		check(!fl_verify_chunk_for_test(&chunk, "truncated", &error),
		        "rejects a truncated instruction");
		chunk_free(NULL, &chunk);
	}
}

int main(void)
{
	test_every_opcode_has_a_width();
	test_widths_agree_with_the_compiler();
	test_rejects_malformed();

	if (failures > 0) {
		printf("verifier: %d checks, %d failed\n", checks, failures);
		return 1;
	}
	printf("verifier: %d checks passed\n", checks);
	return 0;
}