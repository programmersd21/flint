# releases

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
