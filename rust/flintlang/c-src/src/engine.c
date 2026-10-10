/* SPDX-License-Identifier: MIT */
/*
 * The host side of the embedding API: one opaque engine per VM.
 *
 * This is deliberately the smallest API that is still useful: create,
 * run, free. Diagnostics go to stderr through the same printer the CLI
 * uses, and error reporting is the exit code, for the same reason --
 * one error model is the point, here as in fl_raise.
 *
 * What this does not do: exchange values with the host, call flint
 * functions by handle, or register host callbacks. Each of those needs a
 * lifetime story the module-side handles already have and the engine side
 * does not yet, and an engine API that hands out dangling values would be
 * worse than one that runs source and reports the code.
 */
#include "../include/flint.h"

#include "../src/core/value.h"
#include "../src/frontend/compiler.h"
#include "../src/runtime/object.h"
#include "../src/runtime/verify.h"
#include "../src/runtime/vm.h"

#include <stdio.h>
#include <stdlib.h>

struct FlEngine {
	VM vm;
};

FlEngine *fl_engine_new(void)
{
	FlEngine *engine = calloc(1, sizeof(FlEngine));
	if (engine == NULL)
		return NULL;
	vm_init(&engine->vm);
	return engine;
}

void fl_engine_free(FlEngine *engine)
{
	if (engine == NULL)
		return;
	vm_free(&engine->vm);
	free(engine);
}

int fl_engine_run(FlEngine *engine, const char *source, const char *name)
{
	if (engine == NULL || source == NULL)
		return 64;
	if (name == NULL)
		name = "<source>";
	ObjFunction *function = compile_named(&engine->vm, source, name);
	if (function == NULL)
		return 65;
	/* the verifier runs before anything executes, so malformed bytecode
	 * is reported rather than run -- the same order as the CLI. */
	FlVerifyError error;
	if (!fl_verify_function(function, &error)) {
		fprintf(stderr,
		        "internal error: malformed bytecode in %s at byte %d "
		        "(%s)\n",
		        function->name != NULL ? function->name->chars : name,
		        error.offset,
		        error.message != NULL ? error.message : "unknown");
		return 65;
	}
	InterpretResult result =
	        vm_interpret_function(&engine->vm, function, source, name);
	if (result == INTERPRET_COMPILE_ERROR)
		return 65;
	if (result == INTERPRET_RUNTIME_ERROR)
		return 70;
	return 0;
}
