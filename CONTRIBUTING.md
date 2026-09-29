# contributing

## rules

1. portable c11. no compiler extensions, no intrinsics, no platform syscalls
   outside a clearly marked block. it must build with gcc and clang.
2. no dependencies. `libc` and `libm` are the whole list. a logging library or
   an argument parser is not an option.
3. zero warnings under `-Wall -Wextra -Wpedantic -Werror`. one warning is a
   rejected patch.
4. clean under asan and ubsan. run `make stress`. fix leaks and undefined
   behaviour before asking anyone to look at your code.
5. tabs for indent, 80 columns, `/* */` comments. do not reformat lines you
   did not otherwise touch.

## patches

- one change per patch. three bug fixes and a gc refactor in one commit is
  three patches.
- the commit body says what broke and why the change fixes it. "fix bug" and
  "refactor" say nothing.
- language behavior gets a `.fl` file and `.expected` file under
  `tests/language/`; `make test` runs those pairs. CLI diagnostic changes go
  in `tests/diagnostics.sh` and run with `make diagnostic-test`.

## bug reports

send the smallest script that reproduces it, the exact command line, and
`uname -a`. if it crashes under a debugger, send the backtrace. do not send
screenshots of terminal text.

## known limitations

things that are wrong on purpose, or wrong and not yet fixed. each one says
where the fix belongs, so nobody rediscovers the analysis.

See [docs/limits.md](docs/limits.md) for language limits and
[docs/diagnostics.md](docs/diagnostics.md) for diagnostics coverage.

**Ranges do not nest.** `for n in 1..3` works; `for n in (1..3)` is a parse
error, because `..` is not an operator and the parser has nothing to attach it
to inside a grouping. A clean error, not a crash.

**There is no way to delete a table key.** Setting a field writes; there is no
`delete` syntax. `OP_SET_FIELD` in `src/runtime/vm.c` is where one would go.

**The repl is line at a time.** Each line is compiled and run as a fresh
script, so state carries over only through globals, and an unclosed block on
one line is a syntax error. No multi-line input.

The parser state is file-scope static, but `compile_named()` saves and restores
it around each compile. Keep nested compilation covered if import behavior or
compiler reentrancy changes.

## when the interpreter is the suspect

if a script misbehaves and the fault is in the vm rather than the script,
`make debug` builds a binary that prints the stack and the next instruction
before every one. The bytecode dump is the fastest way to see what actually
happened. Note that the disassembler must agree with the compiler about
operand sizes; it did not, once, for `OP_BUILD_TABLE`, and a desynchronized
dump sends you looking at the wrong bytecode entirely.
