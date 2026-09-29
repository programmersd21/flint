# limits

what the language does not do. each of these is a real absence, not a missing
documentation entry, and each says what it would cost to add.

## no integers

every number is a double. there is no integer type, no promotion rule, no
integer division, and no overflow to think about.

the cost is precision above 2^53, and it is permanent: `2^53 + 1` is `2^53`.
adding an int type means a tag for it, a rule for when a double becomes an
int, arithmetic that has to check, and a printer that has to know which to use.
see [values.md](values.md).

## no static types

there are no type annotations, no inference, no generics, and no user-defined
types. `x as T` checks a type at run time; it does not tell the compiler
anything, and it converts nothing. see [syntax.md](syntax.md).

what you get instead is one runtime assertion and a `type()` function. be
honest about how thin that is: the check is where you put it and nowhere else,
and the compiler will not complain about the call three lines down that wants
a number.

## no classes, objects, or user types

there are functions, closures, lists and tables. there is nothing else. no
struct, no record, no `new`, no inheritance, no interfaces.

the language is small because of this. the shape of a program is its function
signatures and its tables. if you need more, the closure factory pattern
covers encapsulation, and a table covers data. see [functions.md](functions.md).

## no error handling

no `try`, no `catch`, no error values, no `finally`. a runtime error prints and
exits with 70.

the interpreter unwinds the stack on a failure and calls `reset_stack`, which
is a strong enough operation that recovering from it inside the language would
need the runtime to make promises about state it does not currently make. see
[errors.md](errors.md).

## no module cycle detection

a file that imports itself recurses to the frame limit and reports "Stack
overflow". there is also no module cache, so importing a file twice runs it
twice. see [modules.md](modules.md).

## import paths are working-directory relative

a path is resolved against the process working directory, not the importing
file. the same script run from two directories gives two different results.

the fix is a directory per call frame, so a nested import resolves against the
importer rather than the process. it is a real change to `CallFrame` and to
`import_file_native`.

## no table iteration

`for` does not work on a table. tables have insertion order and no defined
iteration order beyond that, and there is no `keys()`, so you cannot get one
without the keys you already know.

## no range type

`a..b` works inside a `for` and nowhere else. it is not an expression, cannot
be stored in a variable, and does not nest.

```flint
let r = 1..5      # error
for n in (1..5) {} # error
```

`..` has no infix rule, so inside a grouping there is nothing to attach it to.

## no string library

no methods, no `split`, no `join`, no `replace`, no `find`, no
`upper`/`lower`, no trimming. a string is bytes with a length and two
operators.

this is deliberate and each function is a few lines. note that there is no
slice syntax either, so building a substring means copying character by
character:

```flint
fn substr(s, from, to) {
    let out = ""
    let i = from
    while i < to {
        out = out + s[i]
        i += 1
    }
    return out
}

fn split(s, sep) {
    let out = []
    let start = 0
    let i = 0
    while i + len(sep) <= len(s) {
        if substr(s, i, i + len(sep)) == sep {
            push(out, substr(s, start, i))
            start = i + len(sep)
        }
        i += 1
    }
    push(out, substr(s, start, len(s)))
    return out
}

print(split("a,b,c", ","))    # ["a", "b", "c"]
```

strings do not compare with `<` and `>`. there is no ordering on bytes, so
`"a" < "b"` is an error rather than an answer.

## no io, no env, no processes

`print`, `input` and the `import_file` the compiler generates are the ways
anything enters or leaves the interpreter. there is no file reading, no echo
control, no line editing, no history and no signal handling on input, no
environment variables, no environment access, and no time other than
`clock()`, which is cpu time.

## no random

there is no PRNG. adding one means picking a source of entropy that is a
portability problem, which is the kind of thing this language does not have
yet.

## 64-bit only

NaN-boxing needs a pointer in 48 bits, so `sizeof(void *) == 8` is a compile
time assertion. a 32-bit host would need a second value representation.
