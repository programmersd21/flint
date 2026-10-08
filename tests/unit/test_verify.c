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

/* xorshift32, seeded fixed so a failure reproduces everywhere. */
static unsigned long next_random(unsigned long state)
{
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return state;
}

/*
 * Random bytecode must be rejected, never trusted, and never crash.
 *
 * The invariant under test is the one the whole runtime rests on: a
 * malformed chunk produces a diagnostic, not undefined behaviour. There
 * is no corpus of known-bad bytecode to replay here -- the point is that
 * nobody has thought of the next shape yet -- so this generates bytes
 * itself: random opcodes, random operands, random truncations, over a
 * deterministic seed so a failure is reproducible from the seed alone.
 *
 * A verifier that crashes shows up as a signal, not a failed check, which
 * is exactly what the caller above notices.
 */
static void test_fuzz_random(void)
{
	/* xorshift32: deterministic, no rand() differences between
	 * platforms, and one line. */
	unsigned long state = 0x9E3779B9UL;
	for (int iteration = 0; iteration < 4000; iteration++) {
		Chunk chunk;
		chunk_init(&chunk);
		int length = 1 + (int)(state = next_random(state)) % 24;
		for (int i = 0; i < length; i++) {
			state = next_random(state);
			chunk_write(
			        NULL, &chunk, (uint8_t)(state & 0xFF), 1, 0);
		}
		FlVerifyError error;
		/* the return value is deliberately unused: both answers
		 * are acceptable, and only the absence of a crash and a
		 * leak is the thing being asserted. */
		(void)fl_verify_chunk_for_test(&chunk, "fuzz", &error);
		chunk_free(NULL, &chunk);
		checks++;
	}

	/* structured shapes an unstructured sweep rarely reaches: jump
	 * operands that land on operand bytes, and truncated jump
	 * instructions. */
	for (int target = 0; target < 12; target++) {
		Chunk chunk;
		chunk_init(&chunk);
		chunk_write(NULL, &chunk, OP_JUMP, 1, 0);
		chunk_write(NULL, &chunk, (uint8_t)target, 1, 0);
		chunk_write(NULL, &chunk, (uint8_t)0, 1, 0);
		FlVerifyError error;
		(void)fl_verify_chunk_for_test(&chunk, "jump-shape", &error);
		chunk_free(NULL, &chunk);
		checks++;
	}

	/* every single-byte opcode followed by a truncated operand, which is
	 * what a chunk cut off mid-instruction looks like. */
	for (int opcode = 0; opcode <= OP_THROW; opcode++) {
		if (chunk_instruction_size((uint8_t)opcode) <= 1)
			continue;
		Chunk chunk;
		chunk_init(&chunk);
		chunk_write(NULL, &chunk, (uint8_t)opcode, 1, 0);
		FlVerifyError error;
		check(!fl_verify_chunk_for_test(&chunk, "cut", &error),
		        "rejects an opcode cut off mid-instruction");
		chunk_free(NULL, &chunk);
	}
}

/*
 * Upvalue bounds, at every boundary that has ever mattered.
 *
 * The bug: the check read `upvalue_count > 0 && operand >= (uint8_t)
 * upvalue_count`, which is wrong at both ends. A function capturing nothing
 * skipped the check entirely and any operand sailed through to a VM read
 * outside the (empty) upvalue array. And 256 upvalues narrowed to a byte is
 * zero, so a function at the maximum rejected *every* upvalue access
 * including the valid ones -- a verifier refusing correct code.
 *
 * Neither end is reachable from Flint source, so these build the bytecode by
 * hand. That is the point: the compiler being correct about these cases is
 * what the verifier is being tested against.
 */
static void check_upvalue(uint8_t slot,
        int upvalue_count,
        bool accepted,
        const char *what)
{
	for (int op = 0; op < 2; op++) {
		Chunk chunk;
		chunk_init(&chunk);
		chunk_write(NULL,
		        &chunk,
		        op == 0 ? OP_GET_UPVALUE : OP_SET_UPVALUE,
		        1,
		        0);
		chunk_write(NULL, &chunk, slot, 1, 0);
		chunk_write(NULL, &chunk, OP_RETURN, 1, 0);
		FlVerifyError error;
		bool ok = fl_verify_function_for_test(
		        &chunk, upvalue_count, "upvalue", &error);
		char label[256];
		snprintf(label,
		        sizeof(label),
		        "%s (%s %d, %d upvalues)",
		        what,
		        op == 0 ? "GET" : "SET",
		        (int)slot,
		        upvalue_count);
		checks++;
		if (ok != accepted) {
			failures++;
			printf("FAIL: %s: %s\n",
			       label,
			       ok ? "accepted" : "rejected");
		}
		chunk_free(NULL, &chunk);
	}
}

static void test_upvalue_bounds(void)
{
	/*
	 * A function that captures nothing may not read or write any
	 * upvalue. Before the fix this was the whole check skipped, so
	 * OP_GET_UPVALUE 0 reached the VM with nothing behind it.
	 */
	check_upvalue(0, 0, false, "zero-upvalue function rejects slot 0");
	check_upvalue(7, 0, false, "zero-upvalue function rejects slot 7");
	check_upvalue(255, 0, false, "zero-upvalue function rejects slot 255");

	/* one upvalue: exactly one slot is real. */
	check_upvalue(0, 1, true, "one upvalue accepts slot 0");
	check_upvalue(1, 1, false, "one upvalue rejects slot 1");
	check_upvalue(255, 1, false, "one upvalue rejects slot 255");

	/* 255 upvalues: the last real slot is 254. */
	check_upvalue(253, 255, true, "255 upvalues accepts slot 253");
	check_upvalue(254, 255, true, "255 upvalues accepts its last slot");
	check_upvalue(255, 255, false, "255 upvalues rejects one past the end");

	/*
	 * 256 upvalues, MAX_UPVALUES and the widest a byte operand can
	 * reach. Slot 255 is valid, and the narrowing that made this
	 * unusable turned it into zero and rejected the lot.
	 */
	check_upvalue(255, 256, true, "256 upvalues accepts slot 255");
	check_upvalue(254, 256, true, "256 upvalues accepts slot 254");
	check_upvalue(0, 256, true, "256 upvalues accepts slot 0");
}

static void test_bare_chunk_skips_upvalue_check(void)
{
	/*
	 * The bare-chunk form is not a function with zero upvalues: there
	 * is no function, so there is no count and nothing for the operand
	 * to mean. The unit tests build malformed chunks this way, and
	 * they have to keep working.
	 */
	Chunk chunk;
	chunk_init(&chunk);
	chunk_write(NULL, &chunk, OP_GET_UPVALUE, 1, 0);
	chunk_write(NULL, &chunk, 200, 1, 0);
	chunk_write(NULL, &chunk, OP_RETURN, 1, 0);
	FlVerifyError error;
	check(fl_verify_chunk_for_test(&chunk, "bare", &error),
	        "a bare chunk has no upvalue count to check against");
	chunk_free(NULL, &chunk);
}

/*
 * Real closure bytecode still verifies.
 *
 * The complement of the bounds tests: a verifier that rejects valid
 * bytecode is as broken as one that accepts invalid bytecode, and the
 * compiler is the only authority on what valid looks like. This compiles
 * nested captures and checks the result is accepted.
 */
static void test_real_closures_verify(void)
{
	static const char *const sources[] = {
	        "let x = 1\nfn outer() {\n  let y = 2\n  fn inner() {\n"
	        "    return x + y\n  }\n  return inner()\n}\n",
	        "fn a() {\n  let v = 1\n  fn b() {\n    fn c() {\n"
	        "      return v\n    }\n    return c()\n  }\n  return b()\n"
	        "}\n",
	        "fn no_capture() {\n  return 42\n}\n",
	};
	for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
		VM vm;
		vm_init(&vm);
		bool ok;
		ObjFunction *function =
		        compile_and_verify(&vm, sources[i], &ok);
		char message[128];
		snprintf(message,
		        sizeof message,
		        "real closures verify: %.48s",
		        sources[i]);
		check(ok && function != NULL, message);
		vm_free(&vm);
	}
}

int main(void)
{
	test_every_opcode_has_a_width();
	test_widths_agree_with_the_compiler();
	test_rejects_malformed();
	test_upvalue_bounds();
	test_bare_chunk_skips_upvalue_check();
	test_real_closures_verify();
	test_fuzz_random();

	if (failures > 0) {
		printf("verifier: %d checks, %d failed\n", checks, failures);
		return 1;
	}
	printf("verifier: %d checks passed\n", checks);
	return 0;
}
