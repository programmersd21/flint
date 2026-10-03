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

a runtime error stops the script unless it is inside a `try` block, in which
case it is delivered to the nearest `catch`.

## structured formats

the default output is one line per error and is deliberately unchanged, so
anything already comparing stderr keeps working. the rest is opt in:

```sh
flint --error-format=human script.fl   # excerpt, caret, and a label
flint --error-format=short script.fl   # error[CODE]: message at file:line:col
flint --error-format=json script.fl    # one object per diagnostic
flint --color=never script.fl          # auto (default) | always | never
flint --explain E0102                  # what a code means
```

see [diagnostics.md](diagnostics.md) for the code groups, the span rules, and
`--fix`.

## diagnostic codes

These are all codes emitted by the current compiler and VM. The grouping is
provisional. `E0100` and `E0600` are catchalls while call sites are still being
migrated.

| code | used for |
|---|---|
| `E0001` | unexpected source character |
| `E0002` | unterminated scientific notation, such as `1e+` |
| `E0003` | unterminated string literal |
| `E0100` | parser or compile error without a more specific code |
| `E0102` | missing `)`, `]`, or `}` |
| `E0202` | undefined variable or global name |
| `E0301` | incompatible operator operands |
| `E0302` | failed `as` type assertion |
| `E0401` | call error, including wrong arity |
| `E0501` | module operation error, including an import cycle |
| `E0600` | runtime error without a more specific code |
| `E0601` | runtime index error |

`flint --explain CODE` prints a short explanation for each listed code.
Codes appear only with `--error-format=human`, `short`, or `json`; the default
legacy format keeps its existing output.

## the messages

runtime messages name the failure and usually end with a period. Some begin
with a capital because the legacy wording predates structured diagnostics.

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

## recoverable errors: try/catch/throw

A `try` block catches runtime errors and explicitly thrown values at the
nearest enclosing `catch`:

```flint
try {
    let x = [1, 2][10]
} catch err {
    print(err.message)   # "list index 10 out of bounds (len 2)."
}
```

Errors produced by the runtime and values thrown with `throw` both reach
`catch`. A caught error is a table with `type` and `message` fields:

```flint
throw Error("file not found")
throw TypeError("expected a number")
```

Available constructors: `Error`, `TypeError`, `ValueError`, `IOError`,
`NetworkError`, `TimeoutError`, `ProcessError`, `ModuleError`,
`PackageError`. Throwing a plain value (string, number, ...) also works;
`catch` binds it directly.

Rules:

- an error no `catch` handles prints the message and trace, and the exit
  code is 70
- `try` blocks nest; `break`/`continue`/`return` out of a `try` body drop
  their handlers
- a `try` body's locals do not leak into the `catch` body
- a `try` in an importing script catches errors thrown while importing
  another module

## what there is not

the default format shows no source excerpt or caret. Human and JSON formats
include source locations; the runtime currently maps an error to its executing
line. See [diagnostics.md](diagnostics.md) for the span limits and supported
fixes.

there is no `finally`, no typed catch filters, and no stack-trace field on the
error table. the runtime reports one script-level trace when an error finally
escapes.
