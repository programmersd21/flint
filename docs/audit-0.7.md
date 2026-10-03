# Flint 0.7 Audit Report

## 1. Executive Summary
Flint 0.7 is a dynamically typed programming language featuring a Pratt parser, direct bytecode compilation, a stack-based virtual machine with NaN-boxed values, a mark-and-sweep garbage collector, closures with upvalues, module environments, and a C11 core runtime.

This audit report documents the current state of Flint 0.7 prior to starting Phase 1 (Language Core) through Phase 9 of the 1.0.0 implementation.

**Status as of audit**: All tests passing. Clean build with -Wpedantic -Werror. Code formatted with clang-format.

## 2. Grammar and Syntax (from SPEC.md)

### Lexical
- Keywords: `and`, `as`, `break`, `const`, `continue`, `else`, `export`, `false`, `fn`, `for`, `if`, `import`, `in`, `let`, `nil`, `not`, `or`, `print`, `return`, `true`, `while`, `try`, `catch`, `throw`
- Literals: numbers (decimal and exponent notation), strings (double-quoted with escapes), booleans (`true`, `false`), `nil`
- Operators: `+ - * / % ! = == != < <= > >= += -= *= /= .. : . , ;`
- Comments: `#` starts a comment to end of line

### Statements (SPEC grammar)
```
program        = declaration* EOF ;
declaration    = fnDecl | letDecl | constDecl | importDecl | exportDecl | statement ;

letDecl        = "let" IDENTIFIER ( "=" expression )? terminator ;
constDecl      = "const" IDENTIFIER "=" expression terminator ;
fnDecl         = "fn" IDENTIFIER "(" parameters? ")" "{" block "}" ;
fnExpr         = "fn" "(" parameters? ")" "{" block "}" ;
importDecl     = "import" STRING terminator ;
exportDecl     = "export" ( fnDecl | letDecl | constDecl ) ;

statement      = exprStmt | printStmt | ifStmt | whileStmt | forStmt
               | breakStmt | continueStmt | returnStmt | tryStmt | throwStmt
               | "{" block "}" ;

printStmt      = "print" "(" arguments ")" terminator ;
ifStmt         = "if" expression block ( "else" ( ifStmt | block ) )? ;
whileStmt      = "while" expression block ;
forStmt        = "for" IDENTIFIER "in" expression block ;
returnStmt     = "return" expression? terminator ;
breakStmt      = "break" terminator ;
continueStmt   = "continue" terminator ;
throwStmt      = "throw" expression terminator ;
tryStmt        = "try" block "catch" ( IDENTIFIER )? block ;

terminator     = ";" | newline | "}" | EOF ;
```

### Expression Precedence (loosest to tightest)
1. assignment: `=`, `+=`, `-=`, `*=`, `/=`
2. nil-coalescing: `??`
3. `or`
4. `and`
5. equality: `==`, `!=`
6. comparison: `<`, `<=`, `>`, `>=`
7. range: `..`
8. term: `+`, `-`
9. factor: `*`, `/`, `%`
10. type assertion: `as`
11. unary: `!`, `not`, `-`
12. call, subscript, field: `()`, `[]`, `.`
13. primary: literals, identifiers, grouping, list, table, function literal

## 3. Bytecode and Opcode List (0.7)
```
OP_CONSTANT, OP_CONSTANT_LONG
OP_NIL, OP_TRUE, OP_FALSE, OP_POP
OP_GET_LOCAL, OP_SET_LOCAL
OP_GET_GLOBAL, OP_DEFINE_GLOBAL, OP_DEFINE_GLOBAL_CONST, OP_SET_GLOBAL
OP_GET_UPVALUE, OP_SET_UPVALUE
OP_EQUAL, OP_NOT_EQUAL, OP_GREATER, OP_GREATER_EQUAL, OP_LESS, OP_LESS_EQUAL
OP_ADD, OP_SUBTRACT, OP_MULTIPLY, OP_DIVIDE, OP_MODULO
OP_NOT, OP_NEGATE
OP_PRINT
OP_JUMP, OP_JUMP_IF_FALSE, OP_LOOP
OP_CALL, OP_CLOSURE, OP_CLOSE_UPVALUE, OP_RETURN
OP_LIST_LEN, OP_TABLE_COUNT, OP_TABLE_KEY, OP_TABLE_VALUE
OP_BUILD_LIST, OP_BUILD_TABLE
```

Operand sizes: no operand, u8, u8 index, u24 index, u8 type, u16 offset.

## 4. Object System & GC Roots

### Object Kinds (from object.h)
- `OBJ_STRING` - interned or runtime strings
- `OBJ_FUNCTION` - compiled functions with bytecode
- `OBJ_NATIVE` - C functions exposed to Flint
- `OBJ_CLOSURE` - closures with upvalue references
- `OBJ_UPVALUE` - captured variables (open when frame live, closed when frame returns)
- `OBJ_LIST` - dynamic arrays
- `OBJ_TABLE` - insertion-ordered hash tables

### Value Representation (from value.h)
- NaN-boxed 64-bit `Value`
- Top 13 bits all set → boxed (tag + payload)
- Tags: `FL_TAG_NIL`, `FL_TAG_FALSE`, `FL_TAG_TRUE`, `FL_TAG_OBJ`, `FL_TAG_STR`
- Payload: 48 bits for pointers

### GC Roots (from memory.c)
- VM stack values up to `stack_top`
- Function closures in each call frame
- Open upvalues linked list
- Top-level module globals (all modules' environments)
- Pending error value
- Functions being compiled (compiler roots)

## 5. Standard Library Modules (0.7)
10 modules in `lib/`:
- `math.fl` - math functions and constants (pi, e, tau, sqrt2, ln2, ln10, abs, sqrt, exp, log, trig, hyperbolic, rounding, float helpers)
- `random.fl` - xorshift64* PRNG (seed, float, range, int, choice, chance, shuffle)
- `time.fl` - clocks and time (now, clock_ms, str, now_str, sleep, measure)
- `fs.fl` - files and directories (exists, isdir, mkdir, remove, listdir, read, write, append)
- `path.fl` - path manipulation (join, basename, dirname, ext, stem, isabs, has_ext)
- `collections.fl` - list helpers (sum, max, min, avg, reverse, index_of, contains, unique, take, drop)
- `json.fl` - parsing and serialization (parse, stringify, pretty)
- `os.fl` - machine info and env (name, arch, pathsep, getcwd, chdir, getenv, setenv, unsetenv, homedir, tmpdir, pid)
- `process.fl` - spawn programs (run, run_opts)
- `http.fl` - HTTP client (request, get, post, put, delete, get_json)

All modules are plain Flint code with native primitives for heavy lifting.

## 6. CLI Behavior & Exit Codes

### Exit Codes (from main.c, sysexits.h)
- 64: Usage error
- 65: Compile/input error
- 66: Missing input or module
- 69: Package or sync failure
- 70: Runtime error
- 74: IO error

### CLI Flags
- `flint [script.fl]` - run script
- `flint -e 'code'` - run inline code
- `flint --version` - show version
- `flint --help` - show help
- `flint --check` - syntax check only
- `flint --error-format=human|short|json` - diagnostic format
- `flint --color=auto|always|never` - color output
- `flint --dump-bytecode` - show compiled bytecode
- `flint --fix` - apply fixes (parser errors only)
- `flint --explain CODE` - explain error code
- `flint --verbose` - verbose mode (passes to program as `-v`)

## 7. Known Issues & Stale Documentation

### Resolved Issues (previously reported failures)
- `tests/language/errors/closure_throw.fl` - now passes
- `tests/language/errors/throw_gc_stress.fl` - now passes

### Documentation Discrepancies
- README.md says "nine .fl stdlib files" but there are 10 (as of 0.7)
- SPEC.md claims version 1.0.0 but is based on 0.7 behavior
- Some docs reference 0.5/0.6 constructs no longer present
- docs/audit-0.7.md references tests that no longer fail

### Unaddressed Issues
- REPL has no multi-line input (syntax error if block not closed on one line)
- No `flint fmt`, `flint lint`, `flint test`, `flint check`, `flint debug`, `flint bench`, `flint profile`, `flint doc`, `flint lsp` commands
- No native ABI or embedding API
- No package manager or resolver
- No websocket, UDP, TLS server, or concurrency primitives
- No crypto primitives (SHA256, HMAC, password hashing)
- No archive extraction (gzip, zip, tar)
- No SQLite support

## 8. Build System

### Configurations
- Release: `-O2 -DNDEBUG` → `./flint`
- Debug: `-O0 -g3 -DFL_DEBUG_PRINT_CODE -DFL_DEBUG_TRACE_EXECUTION` → `./flint-debug`
- Stress: `-O1 -g -fsanitize=address,undefined -DFL_GC_STRESS` → `./flint-stress`
- Computed-goto: transformed VM → `./flint-goto`

### Makefile Targets
- `make` or `make release` - release binary
- `make debug` - debug binary
- `make stress` - stress build with ASan/UBSan
- `make test` - run language suite
- `make unit` - run unit tests (value, chunk, verify)
- `make diagnostic-test` - run diagnostic tests
- `make bench` - benchmark release build
- `make bench-goto` - benchmark computed-goto build
- `make check` - clean + release + test + unit (gate)
- `make fmt` - format source
- `make fmt-check` - check formatting
- `make lint` - clang-tidy
- `make clean` - remove build artifacts
- `make install` - install to PREFIX/BINDIR and LIBDIR

## 9. Version Information

### Current Version
```
Flint v0.7.0-5-gb340d5f
```

### Version Numbering (proposed for 1.0)
- Language version: 1.0.0
- Runtime version: 1.0.0
- Package format version: 1
- Bytecode format version: 1
- Native ABI version: 1
- Lockfile version: 1

## 10. Architecture Summary

```
source → scanner → parser/compiler → bytecode → verifier → VM → execution
                         ↓
                   bytecode emitter
```

- **Scanner**: one-pass, tokens are windows into source
- **Parser**: Pratt parser with precedence climbing
- **Compiler**: single-pass, emits bytecode directly (no AST)
- **Verifier**: validates bytecode before execution
- **VM**: stack-based, NaN-boxed values, mark-sweep GC

## 11. Testing

### Test Suite
- 109 language tests under `tests/language/`
- 135 unit tests in `tests/unit/unit_value.c`
- 14 diagnostic tests in `tests/diagnostics.sh`
- 20 verifier checks in `tests/unit/test_verify.c`

### Build Verification
- Release build: green
- Debug build: green
- Stress build (ASan/UBSan + GC_STRESS): green
- Lint (clang-tidy): green
- Formatting (clang-format): green

## 12. Baseline Performance

Recorded in `bench/baseline/results.md`.

Key metrics (approximate):
- Startup: ~0.5ms
- Arithmetics: ~1.6s total
- String ops: ~100ms total
- Table ops: ~600ms total
- Closure calls: ~100ms per call
- GC stress: all tests pass with -DFL_GC_STRESS

## 13. Next Steps (Phase 1: Language Core)

Implement and test:
1. Bindings: `let`, `const`, scoping, shadowing, compound assignment
2. Functions: parameters, defaults, variadic, recursion, argument validation
3. Closures: escaping, shared mutation, recursion, GC safety
4. Errors: first-class `try`/`catch`/`throw`, categories, stack traces
5. Iteration: `for x in value`, one protocol, all iterables
6. Ranges: `a..b` half-open, stepped, negative steps, overflow handling
7. Pattern matching: literals, identifiers, wildcards, ranges, list/table patterns
8. Destructuring: table and list forms, nesting, defaults, aliases
9. Data model: all value types, equality, truthiness, conversion, hashing
10. Strings: UTF-8, len/index/slice, contains/starts_with/ends_with/find/replace/split/join/trim/lower/upper
11. Bytes: dedicated type, indexing, slicing, explicit conversion
12. Collections: list, table, set, tuple operations
13. Modules: exports, private names, aliases, namespaces, caching, cycles
14. Optional type annotations: decide on syntax, document as ignored at runtime
