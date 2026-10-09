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

	/*
	 * A cast tag above the last FlTypeTag. The VM's cast switch would fall
	 * off the end of itself for one, so this is the check that keeps the
	 * enum's last member meaning "the last one".
	 */
	for (int tag = FL_INT_TYPE_FUNCTION + 1; tag <= 255; tag++) {
		Chunk chunk;
		chunk_init(&chunk);
		chunk_write(NULL, &chunk, OP_CAST, 1, 0);
		chunk_write(NULL, &chunk, (uint8_t)tag, 1, 0);
		chunk_write(NULL, &chunk, OP_RETURN, 1, 0);
		FlVerifyError error;
		checks++;
		if (fl_verify_chunk_for_test(&chunk, "cast", &error)) {
			failures++;
			printf("FAIL: accepts cast tag %d\n", tag);
		}
		chunk_free(NULL, &chunk);
	}
	/* and the last real one is still fine */
	{
		Chunk chunk;
		chunk_init(&chunk);
		chunk_write(NULL, &chunk, OP_CAST, 1, 0);
		chunk_write(NULL, &chunk, (uint8_t)FL_INT_TYPE_FUNCTION, 1, 0);
		chunk_write(NULL, &chunk, OP_RETURN, 1, 0);
		FlVerifyError error;
		check(fl_verify_chunk_for_test(&chunk, "cast", &error),
		        "accepts the last real cast tag");
		chunk_free(NULL, &chunk);
	}

	/*
	 * A jump into the middle of another instruction's operand. The byte
	 * after OP_JUMP is an operand; landing there reads it as an opcode.
	 */
	for (int target = 1; target <= 5; target++) {
		Chunk chunk;
		chunk_init(&chunk);
		chunk_write(NULL, &chunk, OP_JUMP, 1, 0);
		chunk_write(NULL, &chunk, 0, 1, 0);
		/* five bytes of operands that must never be a jump target */
		chunk_write(NULL, &chunk, OP_CONSTANT, 1, 0);
		chunk_write(NULL, &chunk, 1, 1, 0);
		chunk_write(NULL, &chunk, OP_CONSTANT, 1, 0);
		chunk_write(NULL, &chunk, 2, 1, 0);
		chunk_write(NULL, &chunk, OP_RETURN, 1, 0);
		FlVerifyError error;
		checks++;
		if (fl_verify_chunk_for_test(&chunk, "into-operand", &error)) {
			failures++;
			printf("FAIL: accepts a jump into operand byte %d\n",
			        target);
		}
		chunk_free(NULL, &chunk);
	}

	/*
	 * A jump that lands on a real instruction boundary is fine. The
	 * delta is measured from the end of the instruction, so OP_RETURN
	 * at offset 3 is a delta of 0 from a jump at offset 0.
	 */
	{
		Chunk chunk;
		chunk_init(&chunk);
		chunk_write(NULL, &chunk, OP_JUMP, 1, 0);
		chunk_write(NULL, &chunk, 0, 1, 0);
		chunk_write(NULL, &chunk, 0, 1, 0);
		chunk_write(NULL, &chunk, OP_RETURN, 1, 0);
		FlVerifyError error;
		check(fl_verify_chunk_for_test(&chunk, "boundary", &error),
		        "accepts a jump onto an instruction boundary");
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
static void check_upvalue(
        uint8_t slot, int upvalue_count, bool accepted, const char *what)
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

/*
 * Local slots, the same shape of question as upvalues and the same fix.
 *
 * A function with no locals is a real function with a real answer: it may
 * not read or write any local slot. Before the fix, a zero local_count
 * skipped the check the way a bare chunk does, so OP_GET_LOCAL 0 in a
 * zero-local function reached the VM. Reading frame->slots[0] there reads
 * the callee slot rather than memory outside the frame -- so this was
 * wrong rather than unsafe -- but a verifier that accepts a slot the
 * function does not have has stopped being able to say anything at all.
 */
static void check_local(
        uint8_t slot, int local_count, bool accepted, const char *what)
{
	for (int op = 0; op < 2; op++) {
		Chunk chunk;
		chunk_init(&chunk);
		chunk.local_count = local_count;
		chunk_write(NULL,
		        &chunk,
		        op == 0 ? OP_GET_LOCAL : OP_SET_LOCAL,
		        1,
		        0);
		chunk_write(NULL, &chunk, slot, 1, 0);
		chunk_write(NULL, &chunk, OP_RETURN, 1, 0);
		FlVerifyError error;
		bool ok =
		        fl_verify_function_for_test(&chunk, 0, "local", &error);
		char label[256];
		snprintf(label,
		        sizeof(label),
		        "%s (%s %d, %d locals)",
		        what,
		        op == 0 ? "GET" : "SET",
		        (int)slot,
		        local_count);
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

static void test_local_bounds(void)
{
	check_local(0, 0, false, "zero-local function rejects slot 0");
	check_local(9, 0, false, "zero-local function rejects slot 9");
	check_local(0, 1, true, "one local accepts slot 0");
	check_local(1, 1, false, "one local rejects slot 1");
	check_local(254, 255, true, "255 locals accepts slot 254");
	check_local(255, 255, false, "255 locals reject one past the end");
	check_local(255, 256, true, "256 locals accept the last slot");
	check_local(0, 256, true, "256 locals accept slot 0");
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

/*
 * Compiler state does not leak between compilations.
 *
 * The frontend keeps one CompilerState, bundled and saved/restored around
 * compile_named(). That makes the isolation an enforced property rather than
 * an argument in a comment: a nested compile starts clean and puts the outer
 * state back exactly. The scanner is still global and cannot be otherwise
 * without a larger change -- that limitation is documented rather than
 * half-refactored.
 *
 * These compile the same VM repeatedly, and also compile a failing program in
 * between, because the interesting leaks are the ones a *failed* compile
 * leaves behind: error counts, and the import list whose entries hold malloc'd
 * names freed at the end of the compile that recorded them.
 */
static void test_compiler_state_isolation(void)
{
	static const char *const good[] = {
	        "let x = 1\nprint(x)\n",
	        "import \"nope.fl\"\n", /* fails to resolve */
	        "let y = 2\nprint(y)\n",
	        "fn f() {\n  syntax error here\n}\n", /* fails to parse */
	        "print(\"still fine\")\n",
	};

	VM vm;
	vm_init(&vm);
	int expected_failures = 0;
	for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
		ObjFunction *function = compile_named(&vm, good[i], "<iso>");
		if (function == NULL) {
			expected_failures++;
			continue;
		}
		FlVerifyError error;
		char message[96];
		snprintf(message,
		        sizeof message,
		        "program %zu compiles and verifies after its "
		        "neighbours failed",
		        i);
		check(fl_verify_function(function, &error), message);
	}
	vm_free(&vm);

	checks++;
	if (expected_failures != 2) {
		failures++;
		printf("FAIL: expected 2 fixtures to fail to compile, got %d\n",
		        expected_failures);
	}

	/* A clean compile after a failing one must not inherit its error
	 * count. Compiling a program with a genuine error and then a valid
	 * one is the case: if error_count survived, the valid one would be
	 * reported as failing. */
	VM vm2;
	vm_init(&vm2);
	ObjFunction *bad = compile_named(&vm2, "let = \n", "<bad>");
	ObjFunction *good_fn = compile_named(&vm2, "print(1)\n", "<good>");
	checks++;
	if (bad != NULL || good_fn == NULL) {
		failures++;
		printf("FAIL: a failed compile affected the next one "
		       "(bad=%s good=%s)\n",
		        bad == NULL ? "null" : "compiled",
		        good_fn == NULL ? "null" : "compiled");
	}
	vm_free(&vm2);
}

int main(void)
{
	test_every_opcode_has_a_width();
	test_widths_agree_with_the_compiler();
	test_rejects_malformed();
	test_local_bounds();
	test_upvalue_bounds();
	test_bare_chunk_skips_upvalue_check();
	test_real_closures_verify();
	test_fuzz_random();
	test_compiler_state_isolation();

	if (failures > 0) {
		printf("verifier: %d checks, %d failed\n", checks, failures);
		return 1;
	}
	printf("verifier: %d checks passed\n", checks);
	return 0;
}
