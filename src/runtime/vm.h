/* SPDX-License-Identifier: MIT */
/*
 * The VM: bytecode interpreter, call frames, and GC roots.
 */
#ifndef FL_VM_H
#define FL_VM_H

#include "chunk.h"
#include "object.h"
#include "profile.h"
#include "table.h"
#include "value.h"
#include "../util/diagnostic.h"

typedef enum {
	INTERPRET_OK,
	INTERPRET_COMPILE_ERROR,
	INTERPRET_RUNTIME_ERROR
} InterpretResult;

/*
 * How much to say.
 *
 * FL_WARN_DEFAULT is the only mode that reports anything today: flint emits
 * no warnings at all yet, and a flag that pretends otherwise would be a lie
 * dressed as a feature. The modes exist so that the first real warning has
 * somewhere to go, and so a script that wants a quiet run can ask for one
 * without a second language.
 */
typedef enum {
	FL_WARN_NONE = 0, /* errors only */
	FL_WARN_DEFAULT, /* errors, and warnings once there are any */
	FL_WARN_ALL /* everything the compiler can produce */
} FlWarnMode;

/*
 * One entry per active call. slots points at the callee value on the value
 * stack, so argument 0 is slots[1] and local slot 0 is the function itself.
 * That single pointer is why the compiler can use one index for both.
 */
typedef struct {
	ObjClosure *closure;
	uint8_t *ip;
	Value *slots;
} CallFrame;

struct VM {
	CallFrame frames[FRAMES_MAX];
	int frame_count;

	/*
	 * Where this script started, so a failure can unwind to it and
	 * nothing further.
	 *
	 * Both fields, and the distinction matters. Unwinding to a frame
	 * index alone is not enough: the importing script's frame has a
	 * live call on its stack (the import_file callee and its path
	 * argument), and dropping those leaves call_value's
	 * `stack_top -= arg_count + 1` subtracting slots that are already
	 * gone, which walks off the bottom of the array. base_top is the
	 * exact stack position to return to, captured on entry.
	 *
	 * Set by vm_interpret() on entry, restored on exit, so it always
	 * describes the innermost active script.
	 */
	int base_frame;
	Value *base_top;
	const char *source_text;
	const char *source_name;
	FlWarnMode warnings;
	bool quiet; /* suppress the repl prompt */
	/* the repl asked for an expression statement's value to be left on
	 * the stack. it changes what OP_RETURN does at the base frame,
	 * where the value sits above the frame's closure rather than
	 * being the thing the return pops. */
	bool repl_leaves_value;
	/*
	 * Run the bytecode verifier before executing.
	 *
	 * On by default and not a debugging option: the JIT depends on it,
	 * and a build flag that a user can turn off does not make the JIT
	 * safer, it only makes it untestable. The flag exists so that
	 * --check can measure the cost, and so a future path that has
	 * already verified the same chunk can skip the repeat pass.
	 */
	bool verify;
	FlDiagFormat diag_format;
	FlColorMode diag_color;

	Value stack[STACK_MAX];
	Value *stack_top;

	/*
	 * The current module's globals.
	 *
	 * Every module gets its own Table rather than sharing one. That is
	 * the whole of 0.6.0's module semantics: two files that both define a
	 * private `scale` keep their own, and neither can read or clobber the
	 * other's.
	 *
	 * It used to be a single table for everything, which meant `export`
	 * could not hide anything and a bare name in one file silently
	 * overwrote the same name in another. Reproducing that was easy:
	 * two modules with a private helper of the same name, and whichever
	 * imported second won.
	 *
	 * `globals_envs` holds every module's table, and is marked by the
	 * collector; `globals` points at the one currently executing, so the
	 * bytecode does not change at all when a module is entered or left.
	 */
	Table *globals; /* the module being executed right now */

	/*
	 * Every module's environment, kept for as long as the VM lives.
	 *
	 * A growable array of heap Tables rather than a fixed stack of them,
	 * and that is the whole point. A closure remembers the environment it
	 * was created in, and it may be called long after that module's
	 * import returned -- possibly after another module loaded. If
	 * environments were a fixed array whose slots got reused, the second
	 * module would land on the first module's slot and the first
	 * module's closures would silently start reading the second
	 * module's private names. That is not a subtle bug, it is two modules
	 * with a private name each quietly calling each other's.
	 *
	 * So: allocated, never reused, freed only at vm_free. A program that
	 * imports N modules pays for N tables, which is the honest cost of N
	 * modules existing.
	 */
	Table **globals_envs;
	int globals_count;  /* import nesting depth */
	int globals_used;   /* slots ever taken; only ever grows */
	int globals_capacity;

	Table strings; /* weak. the intern table. */

	/*
	 * Modules already loaded, keyed by resolved path.
	 *
	 * The value is a marker, not data: TRUE means loaded, NIL means
	 * currently being loaded. That distinction is what detects a cycle
	 * without a second table, and it is the whole reason the value
	 * exists rather than being a plain set.
	 *
	 * A module runs once per VM. Importing it again is a no-op, which
	 * means a library's top-level side effects happen once no matter
	 * how many files pull it in, and a mutual import between two files
	 * is an error naming the file already in flight rather than a
	 * stack overflow.
	 */
	Table modules;

	ObjUpvalue *open_upvalues; /* sorted by descending stack address */
	Obj *objects; /* every live object, for sweeping */

	size_t bytes_allocated;
	size_t next_gc;

	/*
	 * Counters, and the one switch that decides whether anything writes
	 * to them. `profile` is what --profile sets; `counters` is always
	 * there because the collector needs the byte total anyway and
	 * splitting the two would mean two sources for one number.
	 *
	 * The counters are incremented unconditionally. A branch on
	 * vm->profile around every increment would be a load and a
	 * predictable test on every allocation in the program, which is
	 * exactly the kind of tax a release build should not pay for a mode
	 * that is off. Writing a counter is a load and a store to a struct
	 * the VM already owns, so it is cheaper than the branch would be.
	 */
	FlCounters counters;

	/* explicit gray stack. see the tracing note in memory.c. */
	int gray_count;
	int gray_capacity;
	Obj **gray_stack;
};

void vm_init(VM *vm);
void vm_free(VM *vm);

/*
 * Pop and return the top of the value stack, or false if it is empty.
 *
 * This exists for the repl. It hands out a raw Value, which the caller must
 * treat as unrooted: any allocation before it is pushed again could collect it.
 * The repl prints and does nothing else, which is safe. Anything more should
 * push it straight back.
 */
bool vm_pop_value(VM *vm, Value *out);

/* format any value the way print() does, plus a newline. for the repl. */
void vm_print_value(Value value);
void vm_set_diagnostics(VM *vm, FlDiagFormat format, FlColorMode color);

/* compile and run. the entry point for the repl, files and modules alike. */
InterpretResult vm_interpret(VM *vm, const char *source);
InterpretResult vm_interpret_named(
        VM *vm, const char *source, const char *name);

/*
 * Run a function that has already been compiled.
 *
 * Separate from vm_interpret_named() so that a caller which needed the
 * ObjFunction for something else -- a bytecode dump, a verifier report, a
 * bytecode cache -- does not have to compile it twice. The function is rooted
 * by this caller's own frame, so it must stay alive across the call.
 */
InterpretResult vm_interpret_function(
        VM *vm, ObjFunction *function, const char *source, const char *name);

/*
 * Stack primitives. These are also the GC roots, so an object must be on the
 * stack before any allocation that could trigger a collection.
 */
void vm_push(VM *vm, Value value);
Value vm_pop(VM *vm);

/* prints the message and a stack trace, then unwinds the stack. */
void vm_runtime_error(VM *vm, const char *format, ...);

void vm_define_native(VM *vm, const char *name, NativeFn function, int arity);

#endif /* FL_VM_H */
