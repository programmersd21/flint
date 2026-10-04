# Flint 0.7 Audit Report

## 1. Executive Summary
Flint 0.7 is a dynamically typed programming language featuring a Pratt parser, direct bytecode compilation, a stack-based virtual machine with NaN-boxed values, a mark-and-sweep garbage collector, closures with upvalues, module environments, and a C11 core runtime.

This audit report documents the current state of Flint 0.7 prior to starting Phase 1 (Language Core) through Phase 9 of the 1.0.0 implementation.

## 2. Grammar and Syntax
- **Statements & Expressions**: Statements are separated/terminated by newlines or semicolons.
- **Variables**: `let` and `const` for binding.
- **Control Flow**: `if`, `while`, `for`, `match`, `try`/`catch`/`throw`.
- **Functions**: `fn name(params) { ... }`, anonymous functions, closures.
- **Modules**: `import`, `export`.
- **Literals**: Numbers (double precision float), strings, booleans, nil, lists `[...]`, tables `{k: v, ...}`.

## 3. Bytecode and Opcode List (0.7)
The VM executes bytecode instructions compiled from AST nodes:
- Constant loading (`OP_CONSTANT`)
- Nil, True, False (`OP_NIL`, `OP_TRUE`, `OP_FALSE`)
- Pop, Get/Set Local, Get/Set Global, Get/Set Upvalue
- Jump, Jump If False, Loop
- Call, Closure, Return
- List, Table, Get/Set Property/Index
- Arithmetic operators (`OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`, `OP_NEG`, etc.)
- Comparison operators (`OP_EQUAL`, `OP_GREATER`, `OP_LESS`, etc.)

## 4. Object System & GC Roots
- **Values**: NaN-boxed 64-bit values (`Value`).
- **Object Kinds**: `OBJ_STRING`, `OBJ_FUNCTION`, `OBJ_NATIVE`, `OBJ_CLOSURE`, `OBJ_UPVALUE`, `OBJ_LIST`, `OBJ_TABLE`, `OBJ_MODULE`, `OBJ_USERDATA`, `OBJ_ERROR`, `OBJ_RANGE`, `OBJ_BYTES`, `OBJ_ITERATOR`.
- **GC Roots**: VM stack, globals table, open upvalues linked list, compiler roots, active module registry.

## 5. Standard Library Modules (0.7)
Current modules include core math, strings, collections, fs, os, json, http, crypto, etc. (some stubs or partial implementations).

## 6. CLI Behavior & Exit Codes
- `flint [script]` runs a script.
- `flint --version` / `flint --help`.
- Exit codes:
  - 64: Usage error
  - 65: Compile/input error
  - 66: Missing input or module
  - 70: Runtime error

## 7. Known Bugs & Stale Documentation
- README lists 9 stdlib files while table lists 10.
- Two test failures in `tests/language/errors/closure_throw.fl` and `tests/language/errors/throw_gc_stress.fl` due to table literal syntax issues or test harness expectations.
- Stale 0.5/0.6 references in documentation.
