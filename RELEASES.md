# releases

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
