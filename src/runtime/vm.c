/* SPDX-License-Identifier: MIT */
/*
 * The interpreter: one loop, one frame pointer, a switch.
 *
 * `frame` is cached in a local and reloaded after anything that can change
 * the current frame: a call, a return, or a native that reenters. Missing
 * one of those reloads is the classic bug in this design, so there are only
 * three places to get it right.
 */
#include "vm.h"
#include "chunk.h"
#include "common.h"
#include "compiler.h"
/* the disassembler is called from run(), and only under
 * FL_DEBUG_TRACE_EXECUTION. Same reasoning as the compiler: an include that
 * nothing references in a release build is noise the analyser has to be told
 * to ignore, and an #ifdef says it more honestly than a pragma does. */
#ifdef FL_DEBUG_TRACE_EXECUTION
#	include "debug.h"
#endif
#include "memory.h"
#include "native.h"
#include "object.h"
#include "stdint.h"
#include "table.h"
#include "value.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/*
 * Push. Anything that allocates must root the value here first, because the
 * allocation can collect and an unrooted object does not survive it.
 */
void vm_push(VM *vm, Value value)
{
	*vm->stack_top = value;
	vm->stack_top++;
}

Value vm_pop(VM *vm)
{
	vm->stack_top--;
	return *vm->stack_top;
}

/* 0 is the top of the stack. */
static Value peek(VM *vm, int distance) { return vm->stack_top[-1 - distance]; }

static void close_upvalues(VM *vm, Value *last);

/*
 * Discard everything this script pushed: the frames above vm->base_frame, and
 * the stack above vm->base_top.
 *
 * Both, and the second one is not optional. Recovering the stack top from the
 * frame index is wrong, because a frame's base is where its *callee* sits, not
 * where the current statement began. An `import` at the top of a script has the
 * import_file callee and its path argument sitting above the frame base, and
 * call_value is about to subtract them itself. Unwinding to the frame base
 * throws them away first, so the subtraction runs off the bottom of the array
 * and writes there. That is silent corruption when it lands in the VM struct
 * and a segfault when it does not.
 *
 * So vm_interpret() records both on entry: the frame count to keep, and the
 * exact stack position to return to. A failure restores both, and the caller
 * finds its stack exactly as it left it.
 */
/*
 * Discard everything this script added: the frames above base_frame, and the
 * value stack back down to base_top.
 *
 * Both, and neither is derivable from the other. The frame depth is what the
 * dispatch loop checks to know it is finished. The stack top is what the
 * caller of a nested interpret needs back so its own bookkeeping works.
 *
 * Unwinding to 0 instead of to the base is catastrophic when the failing
 * script is a module. `import` runs a second script inside the first, and
 * unwinding past the importer's frame leaves its run() holding a pointer to a
 * frame that no longer exists. The next dispatch reads it and the process
 * dies somewhere unrelated.
 *
 * Unwinding to the surviving frame's own `slots` is wrong in the other
 * direction. Those slots are where the callee sits, and an `import` has its
 * callee and path argument pushed above them, still owned by the caller,
 * which is about to subtract them from the stack top itself. Throwing them
 * away here first makes that subtraction run off the bottom of the array.
 * base_top is the exact position captured on entry, so it cannot be off by
 * the caller's operand count.
 */
static void unwind_to(VM *vm, int base_frame, Value *base_top)
{
	/*
	 * Close only the upvalues pointing into the slots being discarded.
	 * Closing the whole list would move every captured variable onto the
	 * heap, including the ones the surviving frames still hold, and a
	 * module failure would quietly detach the closures of the script that
	 * imported it.
	 *
	 * Order matters: the top has to be known before closing, because
	 * closing is what reads the slots.
	 */
	close_upvalues(vm, base_top);

	vm->stack_top = base_top;
	vm->frame_count = base_frame;
}

/*
 * Report and unwind. The trace is printed innermost first, and the line for
 * each frame is the line of the instruction *before* ip, because ip already
 * points past the opcode that failed.
 */
void vm_runtime_error(VM *vm, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputs("\n", stderr);

	/*
	 * Only the frames this script owns. The importing script's frames
	 * are below base_frame and are not this error's business; printing
	 * them would attribute a module's failure to a line in the importer
	 * that ran long before it.
	 */
	for (int i = vm->frame_count - 1; i >= vm->base_frame; i--) {
		CallFrame *frame = &vm->frames[i];
		ObjFunction *function = frame->closure->function;
		size_t instruction = frame->ip - function->chunk.code - 1;
		int line = function->chunk.lines[instruction];

		fprintf(stderr, "[line %d] in ", line);
		if (function->name == NULL)
			fprintf(stderr, "script\n");
		else
			fprintf(stderr, "%s()\n", function->name->chars);
	}

	/*
	 * Unwind to where this script started, and no further. The importing
	 * script's run() is still on the C stack and will resume as soon as
	 * this returns, holding a pointer to its own frame and expecting its
	 * stack exactly as it was.
	 */
	unwind_to(vm, vm->base_frame, vm->base_top);
}

/*
 * Register a native as a global. Both halves are pushed before the
 * table_set() because the name string and the native are each unrooted
 * until they are on the stack.
 */
void vm_define_native(VM *vm, const char *name, NativeFn function, int arity)
{
	vm_push(vm, OBJ_VAL(copy_string(vm, name, (int)strlen(name))));
	vm_push(vm, OBJ_VAL(new_native(vm, function, arity)));
	table_set(
	        vm, &vm->globals, AS_STRING(vm->stack[0]), vm->stack[1], false);
	vm_pop(vm);
	vm_pop(vm);
}

void vm_init(VM *vm)
{
	/*
	 * Assigned directly rather than through reset_stack().
	 *
	 * reset_stack() unwinds, and unwinding closes open upvalues, which
	 * means reading vm->open_upvalues. At this point the VM is whatever
	 * was on the C stack, so that read walks a garbage pointer. It is not
	 * a subtle failure: it segfaults before main() gets a chance to run.
	 *
	 * There is also nothing to unwind yet. An empty VM has no frames and
	 * no upvalues, so writing the three fields is the whole job, and
	 * writing them is the one operation that cannot depend on the values
	 * already being sane.
	 */
	vm->stack_top = vm->stack;
	vm->frame_count = 0;
	vm->open_upvalues = NULL;
	vm->base_frame = 0;

	vm->objects = NULL;
	vm->bytes_allocated = 0;
	/* first collection after a megabyte, so startup does not collect.
	 * the multiply is size_t so it is done in the type it is stored
	 * in, rather than in int and widening afterwards. */
	vm->next_gc = (size_t)1024 * 1024;
	vm->gray_count = 0;
	vm->gray_capacity = 0;
	vm->gray_stack = NULL;

	table_init(&vm->globals);
	table_init(&vm->strings);

	register_natives(vm);
}

void vm_free(VM *vm)
{
	table_free(vm, &vm->globals);
	table_free(vm, &vm->strings);
	free_objects(vm);
}

/*
 * print() formatting.
 *
 * The only interesting part is the number case. Flint numbers are doubles,
 * but "print(4)" showing 4.000000 is not acceptable, so integral values are
 * printed as integers. Beyond that, try increasing precision until the text
 * round-trips: 15 digits covers most values, 17 covers all of them. Using 17
 * unconditionally would give every number a tail of noise digits.
 */
static void print_flint_value(Value value)
{
	if (IS_NUMBER(value)) {
		double d = AS_NUMBER(value);
		if (isnan(d)) {
			printf("nan\n");
			return;
		}
		if (isinf(d)) {
			if (d < 0)
				printf("-inf\n");
			else
				printf("inf\n");
			return;
		}
		/* exactly representable as an integer, and in range as a long.
		 * the range test has to happen inside the helper, before the
		 * cast: see the note on fl_double_is_printable_int. */
		if (fl_double_is_printable_int(d)) {
			printf("%ld\n", fl_double_to_long(d));
			return;
		}
		char buf[64];
		double check;
		snprintf(buf, sizeof(buf), "%.15g", d);
		check = strtod(buf, NULL);
		if (check == d) {
			printf("%s\n", buf);
			return;
		}
		snprintf(buf, sizeof(buf), "%.16g", d);
		check = strtod(buf, NULL);
		if (check == d) {
			printf("%s\n", buf);
			return;
		}
		/* 17 significant digits is always enough for a double */
		snprintf(buf, sizeof(buf), "%.17g", d);
		printf("%s\n", buf);
	} else if (IS_BOOL(value)) {
		printf("%s\n", AS_BOOL(value) ? "true" : "false");
	} else if (IS_NIL(value)) {
		printf("nil\n");
	} else if (IS_OBJ(value)) {
		print_object(value);
		printf("\n");
	}
}

/*
 * Push a frame for a flint function. The callee and its arguments are already
 * on the stack, so slots points at the callee and argument 0 is slots[1].
 *
 * Returns false after reporting an error, so callers can just propagate.
 */
static bool call(VM *vm, ObjClosure *closure, int arg_count)
{
	if (arg_count != closure->function->arity) {
		vm_runtime_error(vm,
		        "Expected %d arguments but got %d.",
		        closure->function->arity,
		        arg_count);
		return false;
	}

	if (vm->frame_count == FRAMES_MAX) {
		vm_runtime_error(vm, "Stack overflow.");
		return false;
	}

	CallFrame *frame = &vm->frames[vm->frame_count++];
	frame->closure = closure;
	frame->ip = closure->function->chunk.code;
	frame->slots = vm->stack_top - arg_count - 1;
	return true;
}

/*
 * Call whatever is on the stack, argc slots down.
 *
 * A native runs right here instead of getting a frame: it is C code, it
 * cannot yield, and a frame would buy nothing. Its arguments are passed as a
 * pointer into the stack, and it is responsible for its own return value.
 * Note that this is the one path that can reenter run() through
 * vm_interpret(), which is how import works.
 */
static bool call_value(VM *vm, Value callee, int arg_count)
{
	if (IS_OBJ(callee)) {
		switch (OBJ_TYPE(callee)) {
		case OBJ_CLOSURE:
			return call(vm, AS_CLOSURE(callee), arg_count);
		case OBJ_NATIVE: {
			ObjNative *native = AS_NATIVE(callee);
			/* arity -1 is variadic and skips the check */
			if (native->arity != -1 && arg_count != native->arity) {
				vm_runtime_error(vm,
				        "Expected %d arguments but got %d.",
				        native->arity,
				        arg_count);
				return false;
			}
			Value result = native->function(
			        vm, arg_count, vm->stack_top - arg_count);
			/* drop callee and args, then leave the result */
			vm->stack_top -= arg_count + 1;
			vm_push(vm, result);
			return true;
		}
		default:
			break;
		}
	}
	vm_runtime_error(vm, "Can only call functions.");
	return false;
}

/*
 * Capture a local into an upvalue, reusing an existing one if the same slot
 * is captured twice. Two closures sharing a variable must share the upvalue,
 * or writes through one would be invisible to the other.
 *
 * The open list is kept sorted by descending stack address, which lets
 * close_upvalues() stop as soon as it passes the last slot that died.
 */
static ObjUpvalue *capture_upvalue(VM *vm, Value *local)
{
	ObjUpvalue *prev = NULL;
	ObjUpvalue *upvalue = vm->open_upvalues;

	while (upvalue != NULL && upvalue->location > local) {
		prev = upvalue;
		upvalue = upvalue->next;
	}

	if (upvalue != NULL && upvalue->location == local)
		return upvalue;

	ObjUpvalue *created = new_upvalue(vm, local);
	created->next = upvalue;

	if (prev == NULL)
		vm->open_upvalues = created;
	else
		prev->next = created;

	return created;
}

/*
 * Move every open upvalue at or above `last` into its own storage. Called
 * when a frame or a block goes away, because those stack slots are about to
 * be reused by another call. Without this an escaped closure would read
 * whatever the next function put there.
 */
static void close_upvalues(VM *vm, Value *last)
{
	while (vm->open_upvalues != NULL &&
	        vm->open_upvalues->location >= last) {
		ObjUpvalue *upvalue = vm->open_upvalues;
		upvalue->closed = *upvalue->location;
		upvalue->location = &upvalue->closed;
		vm->open_upvalues = upvalue->next;
	}
}

/* b .. a -> "ab". the result is interned, so a repeated concat is cheap. */
static void concatenate(VM *vm)
{
	ObjString *b = AS_STRING(peek(vm, 0));
	ObjString *a = AS_STRING(peek(vm, 1));

	int length = a->length + b->length;
	char *chars = ALLOCATE(vm, char, length + 1);
	memcpy(chars, a->chars, a->length);
	memcpy(chars + a->length, b->chars, b->length);
	chars[length] = '\0';

	/* take_string frees chars, and both operands are still rooted. */
	ObjString *result = take_string(vm, chars, length);
	vm_pop(vm);
	vm_pop(vm);
	vm_push(vm, OBJ_VAL(result));
}

/*
 * The dispatch loop. Computed goto would be faster and less readable; a
 * switch is fine until profiling says otherwise.
 *
 * This is a parameter rather than a field, and deliberately so. `import`
 * calls vm_interpret() from inside a running script, so this function gets
 * reentered with the importing script's frames still live below. Each
 * invocation must return to the depth it started at, and a shared field
 * would be overwritten by the nested call before the outer one finished.
 * vm->base_frame exists for a different job -- telling a runtime error how
 * far down it may unwind -- and it is saved and restored for the same reason.
 *
 * Returning only at frame_count == 0 does not work here. The module's
 * OP_RETURN drops the count by one, the module's run() falls through to the
 * next instruction, and that instruction belongs to the importing script. It
 * runs a second time, with a `frame` pointer that is already stale, and then
 * the outer run() resumes and unwinds the same stack again. The crash lands
 * in whatever opcode happens to read the wrecked stack, which is why this
 * presents as a bug in print_object().
 */
static InterpretResult run(VM *vm, int base_frame)
{
	CallFrame *frame = &vm->frames[vm->frame_count - 1];

/* operand reads. each advances ip past the operand. */
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT()                                                           \
	(frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT()                                                        \
	(frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())

/*
 * Arithmetic and comparison on numbers, shared by six opcodes. Pops right
 * then left, so the operand order is a op b. BINARY_OP takes the Value
 * constructor as an argument, which is how one macro produces both NUMBER_VAL
 * results and BOOL_VAL results.
 */
#define BINARY_OP(value_type, op)                                              \
	do {                                                                   \
		if (!IS_NUMBER(peek(vm, 0)) || !IS_NUMBER(peek(vm, 1))) {      \
			vm_runtime_error(vm, "Operands must be numbers.");     \
			return INTERPRET_RUNTIME_ERROR;                        \
		}                                                              \
		double b = AS_NUMBER(vm_pop(vm));                              \
		double a = AS_NUMBER(vm_pop(vm));                              \
		vm_push(vm, value_type(a op b));                               \
	} while (false)

	for (;;) {
#ifdef FL_DEBUG_TRACE_EXECUTION
		/*
		 * Dump the stack and the next instruction, every instruction.
		 *
		 * print_value, not print_object: a stack slot holds whatever
		 * the script last computed, and most of the time that is a
		 * number. print_object switches on OBJ_TYPE(), which reads a
		 * pointer out of the value, so the tracer used to segfault on
		 * the first integer in any script at all.
		 */
		printf("          ");
		for (Value *slot = vm->stack; slot < vm->stack_top; slot++) {
			printf("[ ");
			print_value(*slot);
			printf(" ]");
		}
		printf("\n");
		disassemble_instruction(&frame->closure->function->chunk,
		        (int)(frame->ip -
		                frame->closure->function->chunk.code));
#endif

		switch (READ_BYTE()) {
		case OP_CONSTANT: {
			Value constant = READ_CONSTANT();
			vm_push(vm, constant);
			break;
		}
		case OP_CONSTANT_LONG: {
			/* 24-bit index, for chunks with more than 256 constants */
			uint32_t b0 = READ_BYTE();
			uint32_t b1 = READ_BYTE();
			uint32_t b2 = READ_BYTE();
			uint32_t idx = (b0 << 16) | (b1 << 8) | b2;
			vm_push(vm,
			        frame->closure->function->chunk.constants
			                .values[idx]);
			break;
		}
		case OP_NIL:
			vm_push(vm, NIL_VAL);
			break;
		case OP_TRUE:
			vm_push(vm, TRUE_VAL);
			break;
		case OP_FALSE:
			vm_push(vm, FALSE_VAL);
			break;
		case OP_POP:
			vm_pop(vm);
			break;

		case OP_GET_LOCAL: {
			uint8_t slot = READ_BYTE();
			vm_push(vm, frame->slots[slot]);
			break;
		}
		case OP_SET_LOCAL: {
			/* leaves the value on the stack: an assignment is an
			 * expression, and the caller may need the result */
			uint8_t slot = READ_BYTE();
			frame->slots[slot] = peek(vm, 0);
			break;
		}
		case OP_GET_GLOBAL: {
			ObjString *name = READ_STRING();
			Value value;
			if (!table_get(&vm->globals, name, &value)) {
				vm_runtime_error(vm,
				        "Undefined variable '%s'.",
				        name->chars);
				return INTERPRET_RUNTIME_ERROR;
			}
			vm_push(vm, value);
			break;
		}
		case OP_DEFINE_GLOBAL: {
			/* let at top level. overwrites, unlike assignment. */
			ObjString *name = READ_STRING();

			/*
			 * `let` on a name that is already const. Redeclaration
			 * is normally allowed and overwrites, but doing it to
			 * a const would quietly downgrade the binding: the
			 * entry keeps its const flag, so the very next write
			 * to the "new" x is rejected, which is the opposite of
			 * what the source now says. Refuse it here instead.
			 *
			 * This has to be a runtime check. The name may have
			 * been made const by a module this file imported, and
			 * imports run at runtime, so the compiler cannot know.
			 */
			if (table_is_const(&vm->globals, name)) {
				vm_pop(vm);
				vm_runtime_error(vm,
				        "Cannot redefine constant '%s'.",
				        name->chars);
				return INTERPRET_RUNTIME_ERROR;
			}

			table_set(vm, &vm->globals, name, peek(vm, 0), false);
			vm_pop(vm);
			break;
		}
		case OP_DEFINE_GLOBAL_CONST: {
			/*
			 * `const` at the top level. The flag goes on the
			 * binding, not into the bytecode, so the name is
			 * read-only from here on no matter which file or
			 * function tries to write it next.
			 */
			ObjString *name = READ_STRING();
			table_set(vm, &vm->globals, name, peek(vm, 0), true);
			vm_pop(vm);
			break;
		}
		case OP_SET_GLOBAL: {
			ObjString *name = READ_STRING();

			/*
			 * The const check, which is the whole reason this fix
			 * exists. A local const is caught at compile time, but
			 * a global can be written from another file, from a
			 * function the compiler never saw being defined, or
			 * through a loop. Only the table that holds the binding
			 * knows for sure, so the check has to happen here.
			 *
			 * Checked before table_set, because table_set on an
			 * unknown name would create it and the error below
			 * would report "undefined" for a name that exists.
			 */
			if (table_is_const(&vm->globals, name)) {
				vm_runtime_error(vm,
				        "Cannot assign to constant '%s'.",
				        name->chars);
				return INTERPRET_RUNTIME_ERROR;
			}

			if (table_set(vm,
			            &vm->globals,
			            name,
			            peek(vm, 0),
			            false)) {
				/* assigning to something that was not declared
				 * just created it. undo that and complain. */
				table_delete(&vm->globals, name);
				vm_runtime_error(vm,
				        "Undefined variable '%s'.",
				        name->chars);
				return INTERPRET_RUNTIME_ERROR;
			}
			break;
		}
		case OP_GET_UPVALUE: {
			uint8_t slot = READ_BYTE();
			vm_push(vm, *frame->closure->upvalues[slot]->location);
			break;
		}
		case OP_SET_UPVALUE: {
			/* writes straight into the shared cell, which is what
			 * makes capture by reference work */
			uint8_t slot = READ_BYTE();
			*frame->closure->upvalues[slot]->location = peek(vm, 0);
			break;
		}
		case OP_EQUAL: {
			Value b = vm_pop(vm);
			Value a = vm_pop(vm);
			vm_push(vm, BOOL_VAL(values_equal(a, b)));
			break;
		}
		case OP_NOT_EQUAL: {
			Value b = vm_pop(vm);
			Value a = vm_pop(vm);
			vm_push(vm, BOOL_VAL(!values_equal(a, b)));
			break;
		}
		case OP_CAST: {
			/*
			 * `x as T`. Peek, check, and leave the value exactly
			 * where it was. This is an assertion, not a
			 * conversion, so there is nothing to pop and nothing
			 * to push: the stack is untouched on both the
			 * success and the failure path.
			 *
			 * The whole value is the error message. Without a
			 * cast, a table where a string was expected
			 * surfaces three functions later as a nil, or worse
			 * as a number that happens to be zero. With one,
			 * the failure names the expression that was wrong.
			 */
			FlType want = (FlType)READ_BYTE();
			Value value = peek(vm, 0);

			if (!value_has_type(value, want)) {
				vm_runtime_error(vm,
				        "Expected type '%s' but got '%s'.",
				        flint_type_name_of(want),
				        flint_type_name(value));
				return INTERPRET_RUNTIME_ERROR;
			}
			break;
		}
		case OP_GREATER:
			BINARY_OP(BOOL_VAL, >);
			break;
		case OP_GREATER_EQUAL:
			BINARY_OP(BOOL_VAL, >=);
			break;
		case OP_LESS:
			BINARY_OP(BOOL_VAL, <);
			break;
		case OP_LESS_EQUAL:
			BINARY_OP(BOOL_VAL, <=);
			break;

		case OP_ADD: {
			/* the one overloaded operator: number or string */
			if (IS_STRING(peek(vm, 0)) && IS_STRING(peek(vm, 1))) {
				concatenate(vm);
			} else if (IS_NUMBER(peek(vm, 0)) &&
			           IS_NUMBER(peek(vm, 1))) {
				double b = AS_NUMBER(vm_pop(vm));
				double a = AS_NUMBER(vm_pop(vm));
				vm_push(vm, NUMBER_VAL(a + b));
			} else {
				vm_runtime_error(vm,
				        "Operands must be two numbers or two "
				        "strings.");
				return INTERPRET_RUNTIME_ERROR;
			}
			break;
		}
		case OP_SUBTRACT:
			BINARY_OP(NUMBER_VAL, -);
			break;
		case OP_MULTIPLY:
			BINARY_OP(NUMBER_VAL, *);
			break;
		case OP_DIVIDE:
			BINARY_OP(NUMBER_VAL, /);
			break;
		case OP_MODULO: {
			/* not BINARY_OP: fmod takes doubles, not a Value. */
			if (!IS_NUMBER(peek(vm, 0)) ||
			        !IS_NUMBER(peek(vm, 1))) {
				vm_runtime_error(
				        vm, "Operands must be numbers.");
				return INTERPRET_RUNTIME_ERROR;
			}
			double b = AS_NUMBER(vm_pop(vm));
			double a = AS_NUMBER(vm_pop(vm));
			vm_push(vm, NUMBER_VAL(fmod(a, b)));
			break;
		}
		case OP_NOT:
			vm_push(vm, BOOL_VAL(IS_FALSY(vm_pop(vm))));
			break;
		case OP_NEGATE: {
			if (!IS_NUMBER(peek(vm, 0))) {
				vm_runtime_error(
				        vm, "Operand must be a number.");
				return INTERPRET_RUNTIME_ERROR;
			}
			vm_push(vm, NUMBER_VAL(-AS_NUMBER(vm_pop(vm))));
			break;
		}
		case OP_PRINT: {
			print_flint_value(vm_pop(vm));
			break;
		}
		case OP_JUMP: {
			/* signed 16-bit forward offset, from the byte after it */
			uint16_t offset = READ_SHORT();
			frame->ip += offset;
			break;
		}
		case OP_JUMP_IF_FALSE: {
			/* peeks, does not pop: the false value has to survive
			 * the jump for the matching pop in the then-branch */
			uint16_t offset = READ_SHORT();
			if (IS_FALSY(peek(vm, 0)))
				frame->ip += offset;
			break;
		}
		case OP_LOOP: {
			/* signed 16-bit backward offset, negated on the way in */
			uint16_t offset = READ_SHORT();
			frame->ip -= offset;
			break;
		}
		case OP_CALL: {
			int arg_count = READ_BYTE();
			if (!call_value(vm, peek(vm, arg_count), arg_count))
				return INTERPRET_RUNTIME_ERROR;

			/*
			 * A native can report a runtime error without
			 * call_value() seeing it: len() on a table, pop() on
			 * an empty list, and so on all print a message and
			 * unwind the stack, then return nil as if nothing had
			 * happened. By the time we get here the stack may be
			 * gone and frame_count may be zero, so reloading
			 * `frame` would read frames[-1].
			 *
			 * The test is the same condition OP_RETURN uses: if we
			 * are no longer inside this run()'s frames, the
			 * script is over and so are we.
			 */
			if (vm->frame_count <= base_frame)
				return INTERPRET_RUNTIME_ERROR;

			/* the frame changed: either a new frame, or a native
			 * that returned and popped one */
			frame = &vm->frames[vm->frame_count - 1];
			break;
		}
		case OP_CLOSURE: {
			ObjFunction *function = AS_FUNCTION(READ_CONSTANT());

			/* push before the upvalues are filled in, so a
			 * collection triggered below sees a rooted closure */
			ObjClosure *closure = new_closure(vm, function);
			vm_push(vm, OBJ_VAL(closure));

			/* two bytes per upvalue: is_local, then index. the
			 * compiler worked this out by walking outward. */
			for (int i = 0; i < closure->upvalue_count; i++) {
				uint8_t is_local = READ_BYTE();
				uint8_t index = READ_BYTE();
				if (is_local)
					closure->upvalues[i] = capture_upvalue(
					        vm, frame->slots + index);
				else
					closure->upvalues[i] =
					        frame->closure->upvalues[index];
			}
			break;
		}
		case OP_CLOSE_UPVALUE: {
			/* a captured local going out of scope at the end of a
			 * block. the value stays on the stack for the pop. */
			close_upvalues(vm, vm->stack_top - 1);
			vm_pop(vm);
			break;
		}
		case OP_RETURN: {
			/* save the result before unwinding: closing upvalues
			 * moves stack values into the heap */
			Value result = vm_pop(vm);
			close_upvalues(vm, frame->slots);
			vm->frame_count--;
			if (vm->frame_count == base_frame) {
				/* the frame this run() started with. pop its
				 * closure and stop: there is no caller inside this
				 * run() to return to, even though the VM may
				 * still hold frames from an outer script. */
				vm_pop(vm);
				return INTERPRET_OK;
			}

			/* drop everything the frame owned, callee included,
			 * then leave the result where the callee was */
			vm->stack_top = frame->slots;
			vm_push(vm, result);
			frame = &vm->frames[vm->frame_count - 1];
			break;
		}
		case OP_BUILD_LIST: {
			/* the elements are on the stack above the placeholder
			 * that was just pushed for the list itself */
			uint8_t count = READ_BYTE();
			ObjList *list = new_list(vm);
			vm_push(vm, OBJ_VAL(list));

			if (count > 0) {
				/* exact allocation: the literal cannot grow
				 * later except through push() */
				list->items = ALLOCATE(vm, Value, count);
				list->capacity = count;
				list->count = count;

				/* copy in reverse, since the stack is top-down */
				for (int i = count - 1; i >= 0; i--)
					list->items[i] =
					        vm->stack_top[-2 -
					                      (count - 1 - i)];

				/* drop the elements, keep the list */
				vm->stack_top -= count;
				vm->stack_top[-1] = OBJ_VAL(list);
			}
			break;
		}
		case OP_BUILD_TABLE: {
			/*
			 * Only the empty table. The compiler fills it with
			 * OP_SET_FIELD, and does it through a hidden local
			 * so each pair re-pushes the table.
			 */
			ObjTable *table = new_flint_table(vm);
			vm_push(vm, OBJ_VAL(table));
			break;
		}
		case OP_GET_INDEX: {
			Value index_val = vm_pop(vm);
			Value target = vm_pop(vm);

			if (IS_LIST(target)) {
				if (!IS_NUMBER(index_val)) {
					vm_runtime_error(vm,
					        "List index must be a number.");
					return INTERPRET_RUNTIME_ERROR;
				}
				ObjList *list = AS_LIST(target);
				int idx = (int)AS_NUMBER(index_val);
				/* negative counts from the end */
				if (idx < 0)
					idx = list->count + idx;
				if (idx < 0 || idx >= list->count) {
					vm_runtime_error(vm,
					        "List index %d out of bounds "
					        "(len %d).",
					        idx,
					        list->count);
					return INTERPRET_RUNTIME_ERROR;
				}
				vm_push(vm, list->items[idx]);
			} else if (IS_STRING(target)) {
				/* indexing a string yields a one-byte string,
				 * not a number */
				if (!IS_NUMBER(index_val)) {
					vm_runtime_error(vm,
					        "String index must be a "
					        "number.");
					return INTERPRET_RUNTIME_ERROR;
				}
				ObjString *str = AS_STRING(target);
				int idx = (int)AS_NUMBER(index_val);
				if (idx < 0)
					idx = str->length + idx;
				if (idx < 0 || idx >= str->length) {
					vm_runtime_error(vm,
					        "String index %d out of "
					        "bounds.",
					        idx);
					return INTERPRET_RUNTIME_ERROR;
				}
				char c[2] = {str->chars[idx], '\0'};
				vm_push(vm, OBJ_VAL(copy_string(vm, c, 1)));
			} else {
				vm_runtime_error(vm,
				        "Can only index lists and strings.");
				return INTERPRET_RUNTIME_ERROR;
			}
			break;
		}
		case OP_SET_INDEX: {
			Value val = vm_pop(vm);
			Value index_val = vm_pop(vm);
			Value target = vm_pop(vm);

			/* assignment target must already exist. there is no
			 * append syntax; use push(). */
			if (!IS_LIST(target)) {
				vm_runtime_error(
				        vm, "Can only index-assign to lists.");
				return INTERPRET_RUNTIME_ERROR;
			}
			if (!IS_NUMBER(index_val)) {
				vm_runtime_error(
				        vm, "List index must be a number.");
				return INTERPRET_RUNTIME_ERROR;
			}
			ObjList *list = AS_LIST(target);
			int idx = (int)AS_NUMBER(index_val);
			if (idx < 0)
				idx = list->count + idx;
			if (idx < 0 || idx >= list->count) {
				vm_runtime_error(vm,
				        "List index %d out of bounds.",
				        idx);
				return INTERPRET_RUNTIME_ERROR;
			}
			list->items[idx] = val;
			vm_push(vm, val); /* assignment yields the value */
			break;
		}
		case OP_GET_FIELD: {
			ObjString *name = READ_STRING();
			Value target = vm_pop(vm);
			if (IS_FLINT_TABLE(target)) {
				/*
				 * Linear scan. Tables are parallel arrays in
				 * insertion order, and scripts do not build
				 * tables big enough for this to hurt. A hash
				 * table per flint table would be the fix if
				 * that ever changed.
				 */
				ObjTable *t = AS_FLINT_TABLE(target);
				for (int i = 0; i < t->count; i++) {
					/* keys are interned, so pointer compare */
					if (t->keys[i] == name) {
						vm_push(vm, t->values[i]);
						goto field_done;
					}
				}
				/* missing key is nil, not an error */
				vm_push(vm, NIL_VAL);
field_done:;
			} else {
				vm_runtime_error(
				        vm, "Only tables have fields.");
				return INTERPRET_RUNTIME_ERROR;
			}
			break;
		}
		case OP_SET_FIELD: {
			ObjString *name = READ_STRING();
			Value val = vm_pop(vm);
			Value target = vm_pop(vm);
			if (IS_FLINT_TABLE(target)) {
				ObjTable *t = AS_FLINT_TABLE(target);
				for (int i = 0; i < t->count; i++) {
					if (t->keys[i] == name) {
						t->values[i] = val;
						vm_push(vm, val);
						goto field_set_done;
					}
				}

				/* new key. grow by doubling, both arrays at
				 * once so they stay parallel. */
				if (t->capacity < t->count + 1) {
					int old_cap = t->capacity;
					t->capacity = GROW_CAPACITY(old_cap);
					t->keys = GROW_ARRAY(vm,
					        ObjString *,
					        t->keys,
					        old_cap,
					        t->capacity);
					t->values = GROW_ARRAY(vm,
					        Value,
					        t->values,
					        old_cap,
					        t->capacity);
				}
				t->keys[t->count] = name;
				t->values[t->count] = val;
				t->count++;
				vm_push(vm, val);
field_set_done:;
			} else {
				vm_runtime_error(
				        vm, "Only tables have fields.");
				return INTERPRET_RUNTIME_ERROR;
			}
			break;
		}
		case OP_SET_FIELD_TOP: {
			/*
			 * The table literal's opcode. The table is on the
			 * stack, the value is on top of it, and the table has
			 * to survive for the next pair -- so the value is
			 * popped and the table is read in place and left
			 * alone.
			 *
			 * Keeping the table on the stack is what makes a literal
			 * work as a call argument. Parking it in a local
			 * instead meant the literal assumed it was the topmost
			 * value, and it is not: in type({a:1}) the callee is
			 * already there, so the literal set a field on the
			 * function.
			 */
			ObjString *name = READ_STRING();
			Value val = vm_pop(vm);
			Value target = vm->stack_top[-1];

			if (!IS_FLINT_TABLE(target)) {
				vm_runtime_error(
				        vm, "Only tables have fields.");
				return INTERPRET_RUNTIME_ERROR;
			}

			ObjTable *t = AS_FLINT_TABLE(target);

			/*
			 * find or append. `found` is what makes a key repeated
			 * inside one literal overwrite instead of adding a second
			 * entry under the same name, which a lookup would then
			 * never reach.
			 */
			bool found = false;
			for (int i = 0; i < t->count; i++) {
				if (t->keys[i] == name) {
					t->values[i] = val;
					found = true;
					break;
				}
			}

			if (!found) {
				/* grow both arrays together, they must stay parallel */
				if (t->capacity < t->count + 1) {
					int old_cap = t->capacity;
					t->capacity = GROW_CAPACITY(old_cap);
					t->keys = GROW_ARRAY(vm,
					        ObjString *,
					        t->keys,
					        old_cap,
					        t->capacity);
					t->values = GROW_ARRAY(vm,
					        Value,
					        t->values,
					        old_cap,
					        t->capacity);
				}
				t->keys[t->count] = name;
				t->values[t->count] = val;
				t->count++;
			}
			break;
		}
		case OP_IMPORT:
		case OP_EXPORT:
			/*
			 * Never emitted. The compiler desugars import into a
			 * call to the import_file native, and export is a
			 * compile-time marker that leaves the ordinary
			 * definition in place. Kept so the enum and the
			 * disassembler agree on the opcode list.
			 */
			break;
		}
	}

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

/*
 * Compile and run. This is the entry point for the repl, for files, and for
 * modules, since import_file_native() calls straight back into here.
 */
InterpretResult vm_interpret(VM *vm, const char *source)
{
	/*
	 * Reentrant, in two independent ways.
	 *
	 * `import` calls this from inside a running script, and the repl
	 * calls it once per line. The compiler's parser state is file-scope
	 * static, so a nested compile would clobber an outer one -- that is
	 * safe today only because compiling and executing are separate
	 * phases, and an outer compile has always finished by the time any
	 * execution nests. That is an invariant nothing enforces, which is
	 * the kind that survives until someone makes import eager.
	 *
	 * The frame depth is the part that cannot be assumed, and the reason
	 * this used to crash on every import. See run().
	 */
	int saved_base = vm->base_frame;
	int base_frame = vm->frame_count;

	/*
	 * The stack top on entry, before this script pushes anything, and the
	 * frame count it started at. Both are restored on the way out so the
	 * caller's own bookkeeping is correct, and both are published in the
	 * VM so a failure inside this script can unwind to exactly here.
	 *
	 * Restoring stack_top explicitly is not belt and braces. On the error
	 * path vm_runtime_error() has already unwound to these values, so run()
	 * returns from a state that is already consistent -- which is exactly
	 * why the restore can be a single unconditional assignment instead of
	 * two paths that each restore a different subset.
	 */
	Value *base_top = vm->stack_top;

	vm->base_frame = base_frame;
	vm->base_top = base_top;

	ObjFunction *function = compile(vm, source);
	if (function == NULL) {
		vm->base_frame = saved_base;
		return INTERPRET_COMPILE_ERROR;
	}

	/*
	 * The function is rooted on the stack across the new_closure() call,
	 * which can collect. After the closure exists the function is
	 * reachable through it, so the raw function can come off.
	 */
	vm_push(vm, OBJ_VAL(function));
	ObjClosure *closure = new_closure(vm, function);
	vm_pop(vm);
	vm_push(vm, OBJ_VAL(closure));

	/*
	 * call() failing means arity mismatch or FRAMES_MAX, and it has
	 * already reported. Returning unchecked used to enter the dispatch
	 * loop with no frame at all, which reads frames[-1].
	 */
	if (!call(vm, closure, 0)) {
		unwind_to(vm, base_frame, base_top);
		vm->base_frame = saved_base;
		return INTERPRET_RUNTIME_ERROR;
	}

	InterpretResult result = run(vm, base_frame);

	/*
	 * Hand the caller's stack back exactly as we found it. On the success
	 * path run() has already returned to this depth and popped this
	 * script's own closure, so this is a no-op. On the error path it is
	 * the whole fix: the handler unwound to base_top, and this restores
	 * the same position for the caller that is about to do its own
	 * arithmetic on it.
	 */
	vm->stack_top = base_top;
	vm->frame_count = base_frame;

	/*
	 * The caller's own base, so a failure on its side unwinds to its own
	 * start and not to this script's.
	 */
	vm->base_frame = saved_base;

	return result;
}
