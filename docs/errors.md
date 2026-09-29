# errors

two kinds, and they are not the same kind of thing.

## compile errors

a parse error, or a type of static check. the file does not run at all, nothing
is executed, and the exit code is 65.

```flint
let x =
```

```sh
$ flint bad.fl
[line 1] Error at end: Expect expression.
$ echo $?
65
```

you get one error, then the parser skips to the next statement it can
recognize. a single typo does not turn into fifty lines of noise, which is the
main thing a `synchronize()` buys you.

compile errors also cover the checks that are not parsing but are still decided
before anything runs. note that these apply to locals, where the compiler
knows the whole scope; a global can be written from a file that has not been
read yet, so the same mistakes are caught at run time instead.

| mistake | local | global |
|---|---|---|
| writing to a `const` | compile error | run time |
| `let` on a name already in scope | compile error | allowed, it overwrites |
| `let a = a` | compile error | run time: undefined variable |
| assigning to a call result | compile error | compile error |
| wrong argument count | run time | run time |

the type name in an `as` is checked at compile time, because the name is part
of the source and nothing outside the file can change it. the *value* is
checked at run time, because it can come from anywhere. so `1 as frobnicate` is
a compile error and `1 as string` is a run time one. see [syntax.md](syntax.md).

a global `const` is a runtime check for a reason: the binding may have been
made const by an imported module, and the import has not happened yet. see
[values.md](values.md) and [modules.md](modules.md).

## runtime errors

something went wrong while running. the message goes to stderr, a stack trace
follows, the stack is unwound, and the exit code is 70.

```flint
fn boom() {
    return [1][99]
}
boom()
```

```sh
$ flint boom.fl
List index 99 out of bounds (len 1).
[line 2] in boom()
[line 3] in script
$ echo $?
70
```

the trace is printed innermost first, one line per frame, with the source line
and the function name. a frame with no name is the top-level script.

a runtime error stops the script. there is no `try`, no `catch`, and no error
value. the one exception is a module that fails to import, which reports and
lets the importer continue. see [modules.md](modules.md).

## the messages

they are lowercase, they end with a period, and they name the thing that
failed. the list is short and worth knowing by heart:

| message | cause |
|---|---|
| `Undefined variable 'x'.` | read or write of a name that was never declared |
| `Cannot assign to constant 'x'.` | write to a `const` |
| `Cannot redefine constant 'x'.` | `let` on a name already bound with `const` |
| `Operands must be numbers.` | arithmetic on something that is not a number |
| `Operands must be two numbers or two strings.` | `+` on a mismatched pair |
| `Operand must be a number.` | unary `-` on a non-number |
| `Expected 0 or 1 arguments but got M.` | `input()` with two or more arguments |
| `Argument to input() must be a string.` | a non-string `input()` prompt |
| `List index N out of bounds (len M).` | list index, either direction |
| `List index must be a whole number.` | fractional index, e.g. `xs[1.5]` |
| `List index must be a finite number.` | `inf` or `NaN` as an index |
| `String index N out of bounds.` | string index |
| `String index must be a whole number.` | fractional string index |
| `String is too long to concatenate.` | the two lengths would overflow int |
| `Only tables have fields.` | `.name` on something that is not a table |
| `Expected type 'T' but got 'U'.` | an `as` assertion that did not hold |
| `Expected N arguments but got M.` | call arity |
| `Stack overflow.` | more than 256 frames, including runaway recursion |
| `Out of memory.` | the allocator failed, and the process exits |

## exit codes

they follow sysexits, so a script can be checked by a shell without parsing
its output.

| code | meaning |
|---|---|
| 0 | success |
| 64 | usage error. wrong flags, or more than one path |
| 65 | the file did not compile |
| 70 | the program failed at run time |
| 74 | the file could not be read |

## what there is not

no line numbers in the runtime trace, only in the compile error. no column. no
source line quoted, only the line number. no error object, so you cannot
inspect a failure in flint code.

that is a deliberate floor. an error type would mean an error class, a
`try`/`catch`, and a guarantee about unwinding that the runtime does not
currently make. adding them is a real design task, and until then the
interpreter exits rather than returning a failure to handle.
