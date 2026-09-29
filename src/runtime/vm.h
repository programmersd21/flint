/* SPDX-License-Identifier: MIT */
/*
 * The VM: bytecode interpreter, call frames, and GC roots.
 */
#ifndef FL_VM_H
#define FL_VM_H

#include "chunk.h"
#include "object.h"
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
	FlDiagFormat diag_format;
	FlColorMode diag_color;

	Value stack[STACK_MAX];
	Value *stack_top;

	Table globals; /* name -> value, for top-level variables */
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
 * Stack primitives. These are also the GC roots, so an object must be on the
 * stack before any allocation that could trigger a collection.
 */
void vm_push(VM *vm, Value value);
Value vm_pop(VM *vm);

/* prints the message and a stack trace, then unwinds the stack. */
void vm_runtime_error(VM *vm, const char *format, ...);

void vm_define_native(VM *vm, const char *name, NativeFn function, int arity);

#endif /* FL_VM_H */
