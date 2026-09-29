# flint documentation

the language, in the order you need it. every example here is a complete
program you can run: `./flint -e 'print(1)'`.

| file | what is in it |
|---|---|
| [language.md](language.md) | the whole language in prose, start to finish |
| [values.md](values.md) | the types, and what makes them equal |
| [syntax.md](syntax.md) | expressions, operators, statements, `as` |
| [control.md](control.md) | `if`, `while`, `for`, `break`, `continue` |
| [functions.md](functions.md) | functions, recursion, closures |
| [data.md](data.md) | strings, lists, tables |
| [modules.md](modules.md) | `import` and `export` |
| [library.md](library.md) | the built-in functions |
| [errors.md](errors.md) | what goes wrong, and what flint does |
| [diagnostics.md](diagnostics.md) | formats, current coverage, and `--fix` |
| [limits.md](limits.md) | what the language does not do |
| [internals.md](internals.md) | how the compiler and the vm work |

the formal grammar and semantics are in [../SPEC.md](../SPEC.md), and the
measured performance is in [../bench/RESULTS.md](../bench/RESULTS.md).

runnable programs are in [../examples](../examples). they are longer than the
snippets here and are commented line by line.

if something in here disagrees with the source, the source is right and this
is a bug in the documentation.
