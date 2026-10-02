# releases

## v0.6.0

a module-system release. `import` binds one name to a module's exports,
`export` decides what those are, and the REPL takes a block.

### the bug this fixes

Two modules could not both have a private helper of the same name. Both wrote
into one shared global table, so:

```flint
# geometry.fl          # display.fl
let scale = 2          let scale = 10
export fn area(r) {    export fn show(v) {
  return 3*r*r*scale     return v * scale
}                     }
```

```flint
import "geometry.fl"
import "display.fl"
print(geometry.area(2))
```

printed **120**. `scale` resolved to 10, because `display.fl` loaded second.
No error and no warning -- a wrong number, which is the worst failure a
language can have. It is now 24, because each module has its own environment.

### what changed

**every module has its own globals.** `vm->globals` became a pointer into a
heap array of per-module tables, and the bytecode did not change at all --
`OP_DEFINE_GLOBAL` means "whatever table this module is running in". A closure
remembers the environment it was created in, so a function called long after
its module loaded still sees that module's private names.

**`export` means something.** Four opcodes flag a binding exported at compile
time. Previously `export` was a comment and the exports were found by diffing
the global table across the module's run, which cannot distinguish a helper
from a public function -- both are a name that appeared.

**failed imports are transactional.** A module that fails binds nothing
anywhere. Its partial globals used to survive, and a second import retried
against them. Its environment is deliberately not freed, though: it may have
handed out a closure the importer holds, and freeing it turns every later call
into a use-after-free.

**`import "x.fl" as name`.** The default binding is the last path component
without the extension, which is what existing scripts already spelled.

**the REPL takes a block.** Delimiter depth with strings and comments skipped,
because a brace inside a string literal is not an open block.

**a bug found and fixed.** `OP_LIST_LEN`, added in 0.5.0 to remove a native
call from every for-in iteration, only understood lists. `len` takes strings
too, so `for c in s` over a string stopped working. Nothing in the test suite
noticed, because every for-in loop in the tree iterates a list. It surfaced
when every example was run as part of this release. There is a regression test
now.

### breaking change

an import no longer dumps names into the importer.

```flint
# before                      # after
import "helper.fl"            import "helper.fl"
print(square(6))              print(helper.square(6))
print(LIMIT)                  print(helper.LIMIT)
```

`docs/modules.md` has the full model and a migration section.

### not in this release

**no native extension ABI.** Sections 15-23 of the 0.6.0 plan were left out.
A versioned public header, dlopen/dylib loading, and ownership rules are a
real piece of work, and a half-specified one is worse than none: an extension
with a subtly wrong ownership rule corrupts memory rather than failing. It
should be its own release with its own differential tests.

**no JIT**, per the plan.

**no table `has`/`delete`/`keys` or list `insert`/`remove`.** The collections
work and are tested; the additions were not reached. The spec says defer rather
than compromise, and these are additive rather than correctness fixes, so
deferring them costs nothing today.

## v0.5.0

a runtime release. the language does not change at all -- not one keyword, not
one builtin, not one diagnostic code. what changes is how fast things run, how
much of the runtime is measured rather than guessed at, and how many ways a
compiler bug can turn into a crash instead of a message.

### what got faster, and why

**strings stopped being interned at run time.** this is the big one, and it
came from a measurement rather than an idea: strings were the documented weak
point at 0.37x CPython.

identifiers and literals are still interned, so `==` on those is a pointer
compare. strings built while the program runs -- concatenation, a slice,
`str()`, a parsed json value -- are not. they are equal by content, which costs
a length compare and a `memcmp` and saves a hash, a probe, an insertion into a
weak table, and the collector's later walk of that table.

interning a string that is used once was paying a real cost for a payoff that
essentially never arrives: two slices of a log file being byte-identical is
rare, and `s = s + "x"` produces different bytes every time. the cost moved
from creation to comparison, and comparison happens far less often.

strings also got their own nan-box tag, so `IS_STRING` is a mask instead of a
pointer chase to read a type byte -- and, more usefully, so equality stops
assuming that equal means identical.

| case                  | change   |
|-----------------------|----------|
| str_concat            | -92%     |
| str_utf8              | -77%     |
| str_ascii             | -76%     |
| mem_retain            | -58%     |
| call_native           | -55%     |
| str_find              | -47%     |
| strings               | -12%     |

**`OP_LIST_LEN`.** the compiler's for-in-over-a-list loop was loading the `len`
global, pushing the list and making a real call into C on *every iteration* --
a hash lookup, an arity check and a native frame, to read an integer that was
already in the object's header. one opcode now reads it.

**a computed-goto interpreter**, as `make flint-goto`. 7-21% on loop-heavy
programs. it is a separate binary rather than an `#ifdef` because the default
build stays portable C11 and warning-clean, and because one copy of sixty-odd
handlers beats two that have to be kept in step. `scripts/to_computed_goto.py`
is the transformation.

### what got safer

**a bytecode verifier.** every chunk is checked before it runs: opcode validity,
operand width, constant and local indices, and jump targets landing on
instruction boundaries. it recurses into closures.

the boundary check is the one that matters. a jump into the middle of a two-byte
operand reads that operand byte as an opcode.

it found three compiler bugs while being written:

- `OP_EXPORT` was the upper bound of the valid opcode range, and it sits in the
  *middle* of the enum. every opcode above it read as invalid.
- the backward jump target was computed from the start of the instruction
  rather than the end, so every loop looked malformed.
- the local-slot high-water mark was assigned rather than raised, so a shallower
  later scope lowered it and every slot an earlier scope had used then looked
  out of range.

**a use-after-free at exit.** `vm_free()` freed the globals, strings and modules
tables before the objects. freeing an object can trigger a collection; that
collection reads `vm->globals`; if the table was already freed, the mark phase
walks freed memory. found by ASan on a new JSON benchmark, and it only showed up
above a certain object count, which is why it survived v0.4.

**the instruction-width table** now lives in `chunk_instruction_size()`, which
the compiler, the disassembler and the verifier all consult. a unit test pins it
against what the compiler actually emits -- that is how `OP_SET_FIELD_TOP`'s
width was found to be wrong.

### new tools

```sh
flint --profile program.fl        # counters: calls, allocs, GC, strings
flint --check program.fl          # compile and verify, do not run
flint --dump-bytecode program.fl  # human-readable disassembly
```

and `python3 bench/bench.py`, which reports medians and records the environment
with them. 49 cases across startup, arithmetic, calls, collections, strings, json
and memory.

### what did not happen

**there is no JIT.** this release was supposed to have a tier-1 baseline JIT and
does not. an earlier draft of the x86-64 encoder and template compiler was
written and then deleted rather than shipped half-finished, because a JIT whose
deoptimization path has never been tested is not a performance feature, it is a
way to corrupt a program silently.

the pieces that make a JIT tractable are now in place -- a verifier that makes
the bytecode trustworthy, type counters on every function, thresholds in
`config.h`, and a disassembler -- but the compiler itself is future work.

what is already measured and real: the strings work above, `OP_LIST_LEN`, the
computed-goto dispatch, and a collector that no longer reads freed memory.

## v0.4.0

a more useful flint. the language stays the same; the stdlib and runtime do not.

**new: standard library modules**

seven modules ship in `lib/` and import by bare name. no package manager, no
network, no install step beyond copying the directory next to the binary.

- `math` -- full libm wrapper: sin, cos, tan, asin, acos, atan, atan2, exp, log,
  log2, log10, sqrt, cbrt, pow, floor, ceil, round, trunc, abs, sign, clamp,
  hypot, and the constants PI, E, TAU, INF, NAN
- `random` -- xorshift64* seeded from clock and pid. `rand()`, `rand_int(a, b)`,
  `rand_float()`, `shuffle(list)`, `choice(list)`. not cryptographic; says so
  in the source
- `time` -- `now()` (unix epoch as a number), `clock_ms()`, `sleep(ms)`,
  `format(t)` (UTC string), `measure(fn)` (returns elapsed ms)
- `fs` -- `exists`, `read`, `write`, `append`, `remove`, `mkdir`, `isdir`.
  everything a script needs to touch the filesystem without reaching for a
  shell
- `path` -- string arithmetic over paths: `join`, `dir`, `base`, `ext`, `abs`,
  `strip_ext`. never touches the filesystem
- `collections` -- `reverse`, `contains`, `min`, `max`, `sum`, `flatten`,
  `zip`, `uniq`. the list operations that come up in every second script
- `json` -- `parse(s)`, `stringify(v)`, `pretty(v)`. objects and arrays
  round-trip cleanly. numbers stay numbers

**new: module resolution**

bare import names resolve via `FLINT_STDLIB` env, then `<exe-dir>/lib`, then
`~/.flint/stdlib`. install `lib/` next to the binary and it just works.

**new: long opcode variants** *(contributed by Artem Tsitronov, [#9](https://github.com/programmersd21/flint/pull/9))*

`OP_GET_GLOBAL_LONG`, `OP_DEFINE_GLOBAL_LONG`, `OP_SET_GLOBAL_LONG`,
`OP_GET_FIELD_LONG`, `OP_SET_FIELD_LONG`, `OP_CLOSURE_LONG` lift the
256-global and 256-constant limits that blocked programs with large global
tables. each variable reference now emits a 1- or 3-byte index depending on
pool size. programs with fewer than 256 globals pay nothing.

**new: make check**

`make check` runs clean → build → test → unit from scratch. the gate for
anyone about to push.

**fixed**

- release workflow no longer auto-generates release notes from commit messages
  (which were not written for end users). releases now get a manual body.
- the `generate_release_notes` accident in the workflow is gone. a release that
  says "Merge pull request #3" instead of what changed is not a release; it is
  a git log with extra steps.

## v0.3.0


a language for small unix programs. one binary, no dependencies, and a script
that reads stdin, calls a program, and writes a line is the whole toolchain.

**new: the standard library a script actually needs**

- `args()`, `env()`, `exit()`, `read_file()`, `write_file()`, `exec()`
- `split` `join` `trim` `contains` `starts_with` `ends_with` `replace` `lower`
  `upper`
- `exec` calls `execvp` and never a shell. there is no path from the API to
  `/bin/sh`, so a filename with a semicolon in it is an argument, not an
  injection
- arguments pass through to the script: `flint x.fl -v` runs `x.fl` with `-v`
  as an argument

**new: diagnostics**

- rustc-style errors, opt in: `--error-format=human|short|json`
- stable codes by origin, `--explain E0102`, `--color=`
- `--fix` for machine-applicable closing-delimiter insertions, now offered for
  a missing delimiter anywhere and not only at end of file
- secondary spans: a redeclaration shows where the name was first defined
- more than one error per file, with a count, capped at 20
- every message lowercase, matching the rest of the repository
- `--quiet` and `--warnings=default|none|all`
- `did you mean` for a misspelled name, including keywords, which are not
  globals and were therefore never suggested before
- JSON includes source spans and structured delimiter replacements; runtime
  spans underline the failing expression rather than the whole line

**new: cli**

- `flint -` reads a script from stdin, next to `-e` and the repl
- `flint script.fl args...` passes everything after the script to the script

**internal math**

- ten `__`-prefixed natives for a math library that is not in this repository
  yet: `__floor` `__sqrt` `__fma` `__ldexp` `__logb` `__fabs` `__copysign`
  `__hi32` `__lo32` `__from_bits`
- the wrappers live in `src/util/fl_math.c`, separate from the language, so a
  future library can change them without touching the runtime
- contributed in [#1](https://github.com/programmersd21/flint/pull/1)

**the repl**

- opens with the version and what to type, instead of a bare cursor
- `:help` and `:quit`
- an expression prints its value and a statement does not, which is what makes
  a repl a repl and not a shell with an `eval` in it
- a session where a line failed exits 70, even though the session carried on

**modules**

- imports resolve against the importing file, not the working directory. a
  script runs from any directory now, which it did not
- a module runs once per VM; a repeat import is a no-op
- an import cycle reports the in-flight module path instead of overflowing

**fixed**

- `split` pushed every piece on the value stack. past 65536 separators that
  wrote off the end of the array, silently, with no bounds check to catch it
- the growing-read loops in `read_file` and the stdin reader left no byte for
  the terminator when a read landed exactly on a capacity boundary, which is
  every file whose size is a power of two
- those loops also called the read again after a zero-byte read, which is
  undefined behaviour on a stream in an error state

**measured**

against CPython 3.14.7 on an i5-1235U, best of 7, matched `.fl`/`.py` pairs
whose outputs are compared before the timing is reported. full numbers and
method in [bench/RESULTS.md](bench/RESULTS.md).

| case | flint | python | |
|---|---|---|---|
| startup | 0.47 ms | 11.1 ms | **23x** |
| hello | 0.54 ms | 8.9 ms | **17x** |
| arith | 1.36 s | 2.55 s | **1.88x** |
| lists | 107 ms | 198 ms | **1.85x** |
| fib | 15.0 ms | 26.4 ms | **1.75x** |
| calls | 168 ms | 265 ms | **1.58x** |
| closures | 114 ms | 173 ms | **1.52x** |
| strings | 148 ms | 64 ms | **0.43x** |

startup is the result that matters for this language. the others are honest
but modest, and `strings` is a loss that stays in the table.

## current development notes

`--error-format=human` and `short` render the diagnostics currently emitted by
the compiler and VM. They do not yet provide multiple source labels, multiline
underlines, or full parser recovery. `--fix` is limited to supported closing
delimiters. See [docs/diagnostics.md](docs/diagnostics.md) for the actual
coverage.

## v0.2.0

the same flint, substantially more correct. no new language, one new
builtin, and a pile of crashes turned into errors.

**runtime**

- `input([prompt])`: prompt with no newline, one line, nil at eof. empty
  line is `""`, not nil. CRLF tolerated. buffer grows, never truncates
- `x[1.5]` is an error, not a silent truncation. non-finite, out-of-range
  and non-numeric indices rejected with their own messages
- string concatenation refuses to overflow instead of invoking undefined
  behaviour. concatenation failing mid-expression no longer falls through
  to the wrong error
- a native that fails (len on a table, pop on empty) stops the script
  instead of the dispatch loop reading `frames[-1]`

**const**

- `const x = 1; const x = 2` refused. same value twice (module re-import)
  allowed, anything else is a contradiction
- `let` on an existing const refused. `let` on an ordinary name still
  overwrites. `let` promoted to `const` allowed
- the flag survives table rehash; copied on `table_add_all` so imported
  constants stay read-only

**compiler**

- parser state is one struct, saved and restored around `compile()`, so a
  nested compile starts clean and puts the outer state back

**tests**

- stdin support in the runner: a `.stdin` file next to the `.fl`, `/dev/null`
  otherwise so no test can hang on a terminal
- coverage for input, index validation, const rules, shared upvalues,
  gc-stress allocation, concat overflow

## v0.1.0

first release. everything below is new.

**runtime**

- nan-boxed 64-bit values: numbers, booleans, nil, heap pointers
- stack vm with 256 call frames, recursion, and a stack overflow check
- mark and sweep gc with an explicit gray stack and weak intern table
- closures with open upvalues, closed on block and frame exit
- lists with negative indexing, tables with dot access
- modules: `import "path.fl"`, `export` on `fn`, `let` and `const`
- `const` on a global, enforced at run time and across module boundaries

**compiler**

- single-pass pratt parser, no ast
- streaming scanner with a keyword trie
- `if`/`else`, `while`, `for` over ranges and lists, `break`, `continue`
- functions, recursion, arity checking
- `as` type assertions against the seven `type()` names
- string escapes, `#` comments, statements terminated by newline or `;`

**tooling**

- repl
- `-e` for inline code, `-h`, `-v`
- bytecode disassembler and execution tracer in the debug build
- language test suite and unit tests
- `make lint` and `make fmt`, policy in `.clang-tidy` and `.clang-format`
- ci on gcc and clang
