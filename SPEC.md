# flint language spec

version 0.5. this is the grammar and the semantics. the implementation is not
always right; when they disagree, file a bug.

[docs/language.md](docs/language.md) is a prose version of this for people who
want to read it rather than implement it. [docs/diagnostics.md](docs/diagnostics.md)
documents the current diagnostic output and its limits.

## lexical

### keywords

```
and      as       break    const    continue else
export   false    fn       for      if       import
in       let      nil      not      or       print
return   true     while    try      catch    throw
```

### literals

- numbers: decimal and exponent notation. `123`, `3.14159`, `1e-5`
- strings: double-quoted bytes, escapes `\n \t \r \" \\ \0`. utf-8 passes
  through untouched
- booleans: `true`, `false`
- nil: `nil`

### operators

```
(   )   {   }   [   ]
+   -   *   /   %   !
=   ==  !=  <   <=  >   >=
+=  -=  *=  /=
..  :   .   ,   ;
```

`#` starts a comment that runs to the end of the line.

## grammar

### statements

statements end at a `;` or a newline.

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

A `try` block runs its body, and routes the first runtime error or `throw`ed
value to the matching `catch` body, which binds the error to the optional
identifier. When nothing catches the error, the script reports it and exits
with code 70. Nested `try`s match innermost first. `break`, `continue` and
`return` inside a `try` body retire its handler before leaving the block.

An error is a table with `type` (a string naming the category) and `message`
(a string) fields. The constructors `Error`, `TypeError`, `ValueError`,
`IOError`, `NetworkError`, `TimeoutError`, `ProcessError`, `ModuleError` and
`PackageError` build these tables. `throw` accepts any value, not just
error tables.

`for x in expr` iterates a list, or a range when `expr` contains `..`.
ranges are half-open: `1..5` is 1, 2, 3, 4.

### expression precedence

loosest to tightest:

1. assignment: `=`, `+=`, `-=`, `*=`, `/=`
2. nil-coalescing: `??` (right associative)
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

`<=` is not `!(>)`. `NaN` is unordered, so the two forms differ on NaN and the
compiler emits a separate opcode for each.

A function literal (`fnExpr` above) is a primary expression: `fn(x) { return x }`
evaluates to a closure, and the same rules for parameters, returns, closures and
naming apply as for a declared function. An unnamed function reports itself as
`<anonymous>` in a stack trace.

### type assertions

`expr as T` is an infix operator. `T` is one of the seven names `type()`
returns: `number`, `string`, `bool`, `nil`, `list`, `table`, `function`. any
other name is a compile error.

`as` asserts; it does not convert. the value is unchanged on success. on
failure the program stops with a runtime error naming both types.

`"9" as number` fails, correctly: a string is not a number. converting one is
`num("9")`, which is 9. see `num` in the standard library.

the assertion runs at run time, not at compile time, because the value may come
from a module that has not been read yet or from a function parameter.

### nil-coalescing

`a ?? b` evaluates `a`. when it is not nil that is the result and `b` never
runs. when it is nil, `b` runs and its value is the result.

only nil triggers the fallback. `false`, `0`, `""`, `[]` and `{}` all stay.
right-associative: `a ?? b ?? c` is `a ?? (b ?? c)`. binds looser than `or`
and tighter than assignment.

```flint
let port = config["port"] ?? 8080
```

### destructuring

`let {a, b} = t` binds each name from the table's fields, by the ordinary
binding rules. flat names only: no nesting, no defaults, no renaming.
`const` works the same way. a missing key reads nil. duplicates follow plain
`let`: an error inside a function, an overwrite at the top level.

```flint
let {host, port} = config
```

## semantics

### truthiness

only `nil` and `false` are falsy. `0`, `0.0`, `NaN`, `""`, `[]` and `{}` are
all truthy.

### equality

- `nil == nil` is true
- numbers compare by IEEE 754 value, so `NaN == NaN` is false
- strings compare by **contents**. two strings with the same bytes are equal
  whether or not they are the same object
- lists and tables compare by identity

the string rule is worth its own note, because it used to be the opposite.

identifiers and string literals are interned, so comparing those is a pointer
compare and stays one. strings produced while a program runs -- concatenation,
a slice, `str()`, a parsed json value -- are **not** interned. interning them
cost a hash, a probe and an insertion into a table the collector then had to
walk, in exchange for making a second identical string share an object, and for
almost any real program that second string never arrives.

so those strings compare by length and then by bytes. the cost moved from
creating a string to comparing one, and comparing happens far less often than
creating. `bench/RESULTS.md` has the measurements.

a program cannot observe the difference except through timing: two strings with
equal bytes have always been equal and still are.

### variables

- top-level names go in the global table
- block-scoped names are stack slots
- `const` rejects assignment. a local is caught when the function is
  compiled; a global is caught when the assignment runs, because the const
  may have come from an imported module that had not been read yet when this
  file was compiled
- `let` on a name that is already const is an error. an ordinary global can
  be redeclared, so this has to be checked separately, and silently
  overwriting would leave the name read-only anyway
- redeclaring a const with a different value is an error. re-running the
  same `const` statement with the same value is not, because importing a
  module executes its top level, and failing on the second import of any
  module that exports a const would be absurd
- promoting `let` to `const` is allowed; narrowing is not a contradiction
- a closure captures enclosing locals by reference. a write inside the closure
  is visible outside it
- a local cannot be read in its own initializer: `let a = a` is an error
- a name cannot be declared twice in the same scope

### strings

`+` concatenates when both operands are strings, adds when both are numbers,
and is an error otherwise. there is no implicit conversion.

strings are immutable. a flint string is a byte sequence, not text: `len` counts
bytes, indexing yields a one-byte string, and the case-mapping primitives
(`upper`, `lower`) are ASCII-only and leave every other byte alone. utf-8
therefore survives a round trip, and `len("☃")` is 3.

### indexing

`list[i]` accepts negative indices, counting from the end. out of range is a
runtime error. `str[i]` yields a one-character string.

the index must be a finite whole number. a fractional index is not truncated:
`xs[1.5]` is an error rather than a silent read of element 1. a value beyond
the range of int, an infinity, a NaN, or anything that is not a number is
rejected with its own message before any conversion happens, because a
floating-to-integer conversion of any of those is undefined.

### string concatenation

`a + b` on strings whose combined length would overflow is a runtime error,
not undefined behaviour. doubling a string each iteration reaches the limit
in a dozen passes, so the check is not theoretical.

### tables

`{ key: value }` is a table literal. `t.key` reads it, `t.key = v` writes it.
reading a key that was never set gives `nil`. there is no delete syntax.

a table literal works in any expression position. it keeps itself on the
stack between pairs rather than in a hidden local, which is what lets
`type({a: 1})` work: a hidden local collides with the callee already on the
stack in a call argument.

### input

`input()` reads one line from stdin and returns it without the newline.
`input("prompt")` writes the prompt first with no newline added. the prompt
must be a string; more than one argument is an error.

an empty line returns `""`. end of file returns `nil`, so a script tells
"the user pressed enter" from "there is nothing left to read". a final line
with no trailing newline is still a line, not nothing.

CRLF is tolerated: a `\r` is skipped, so input from a windows terminal
arrives as plain text. a line can be any length; the buffer grows.

a non-string prompt and a non-zero-or-one argument count are runtime errors,
reported the ordinary way.

## the standard library, v0.3

seven functions before v0.3, and sixteen more since. all of them are ordinary
values: there is no namespace, a builtin is a global holding a native, and a
script can pass one to another.

### core

| function | behaviour |
|---|---|
| `len(x)` | bytes in a string, elements in a list. an error for anything else |
| `push(list, item)` | append in place, returns the item |
| `pop(list)` | remove and return the last element. an error when empty |
| `insert(list, i, v)` | place `v` at `i`, shifting right. returns `v`. `-1` is where `xs[-1]` reads |
| `remove(list, i)` | take out and return the element at `i`. negatives count from the end |
| `keys(t)` | key strings in insertion order, as a fresh list |
| `has(t, k)` | whether the table holds the key, by content |
| `delete(t, k)` | remove the key, true when something was removed |
| `str(val)` | string form of a scalar. containers give `<object>` |
| `type(val)` | one of the seven type names |
| `input([prompt])` | one line from stdin, `nil` at end of file |
| `clock()` | process cpu time in seconds |

### strings

| function | behaviour |
|---|---|
| `split(s, sep)` | list of pieces. an empty separator gives single characters |
| `join(list, sep)` | inverse of split. every element must be a string |
| `trim(s)` | whitespace off both ends |
| `contains(s, sub)` | true if `sub` occurs anywhere |
| `starts_with(s, p)` / `ends_with(s, p)` | prefix and suffix |
| `replace(s, from, to)` | every occurrence, not just the first |
| `lower(s)` / `upper(s)` | ASCII only, and deliberately so |

strings are bytes. `lower` and `upper` map ASCII and leave everything else
alone, because a locale-aware mapping would corrupt utf-8 rather than
case-fold it, which is the worse failure. a script that needs real unicode
case handling needs a different language.

`split` and `replace` use `memchr` to find candidate positions and `memcmp`
only to confirm, and `replace` copies the runs between matches wholesale.
this is an implementation note, not a semantic one, but it is why they are
competitive rather than three times slower.

### process and environment

| function | behaviour |
|---|---|
| `args()` | arguments after the script path, as a list of strings |
| `env(name)` | the variable's value, or `nil` if unset |
| `exit([status])` | leave the process, flushing both streams first |
| `read_file(path)` | the whole file as a string, binary |
| `write_file(path, text)` | truncate and write. returns true |
| `exec(cmd, arg...)` | run a program, return its exit status |

### internal maths

ten names beginning `__` exist for a math library that is not in this
repository. `__floor` `__sqrt` `__fma` `__ldexp` `__logb` `__fabs`
`__copysign` `__hi32` `__lo32` `__from_bits`. the leading underscore marks
them as not part of the language, and a script has no business calling
them.

`exec` calls `execvp` and never a shell. there is no path from this API to
`/bin/sh`, so a filename containing a space, a semicolon or a `$(...)` is an
ordinary argument rather than an injection. a script that genuinely wants a
shell has to ask for one and own the quoting.

`args()` excludes the interpreter and the script path, so a script sees only
what came after its own name. arguments are passed through untouched:
`flint x.fl -v` runs `x.fl` with `-v` as an argument rather than printing the
version, because an argument that looks like a flag is the script's business.

`exit()` clamps to 0..255, which is what a shell can represent, and rejects a
fractional or out-of-range status rather than exiting with something
arbitrary. a process killed by a signal reports `128 + signal`, again the
shell's convention.

`read_file` handles a non-seekable stream, so `/dev/stdin` and a pipe work,
and a file whose size is an exact power of two is read without a one-byte
overflow. `write_file` treats a failed `fclose` as a failure, because a close
that fails after a successful write means the data may not have landed.

## modules

every module has its own namespace. `import` binds one name to the module's
exports, and nothing else about the module is reachable.

### binding

| written | binds | accessed as |
|---|---|---|
| `import math` | `math` | `math.floor(1.7)` |
| `import "lib/geometry.fl"` | `geometry` | `geometry.area(2)` |
| `import "a/b/c.fl"` | `c` | `c.name` |
| `import "util.fl" as u` | `u` | `u.helper()` |

the default binding is the last path component with the extension removed.
`as` overrides it.

a bare name with no quotes and no `/` is a standard library module. the search
order is `$FLINT_STDLIB`, then `<exe-dir>/lib`, then `~/.flint/stdlib`.

### exports

a top-level `let`, `const` or `fn` is private to its module unless marked
`export`. a module cannot read its importer's names, and an importer cannot
read a module's private names.

```flint
# shapes.fl
let tax = 0.2
export fn taxed(amount) { return amount * (1 + tax) }
```

```flint
import "shapes"
print(shapes.taxed(10))   # 12
print(shapes.tax)         # nil
```

reading a name a table does not have gives `nil`, as it does for any table.
that is how a private name is indistinguishable from a typo, which is the
intended behaviour: it is not there.

`export` applies to `fn`, `let` and `const`. anything else after it is a
syntax error.

### resolution

`import` resolves a relative path against the directory of the *importing
file*, not the process working directory. a module path is part of the source,
so it means the same thing no matter where the user is standing. an absolute
path is used as given.

```
project/
  main.fl
  lib/
    math.fl
```

`main.fl` containing `import "lib/math.fl"` finds `project/lib/math.fl` from
any directory. a module in `lib/` importing `"helpers.fl"` finds
`project/lib/helpers.fl`. this is the difference between a script that works
and a script that works only from one place.

with `-e` or stdin there is no source file, so a relative path resolves
against the working directory.

there is no search path for quoted imports and no symlink canonicalisation.
two spellings of one file load it twice.

### repeated imports

a module runs once per VM. importing it again is a lookup, not a second run,
and both callers receive the same table. a library's top-level side effects
happen once however many files pull it in, and a repeated import of a module
that exports a `const` is not a redeclaration of it. the cache is keyed by
resolved path, interned.

### cycles

a file that imports itself, or two files that import each other, is an error
naming the file already in flight:

```
error[E0501]: Import cycle: 'cyc.fl' is already being loaded.
```

nothing partial runs. the module in flight never finishes.

### failure

a failed import binds nothing anywhere. a module that throws at its top level
has not defined anything the importer can see, so a later `print(good)` is an
undefined variable rather than a partially-initialised module.

a failed module is remembered as failed. a later import of the same path
reports that rather than retrying a failure nothing has changed:

```
error[E0501]: Module 'broken.fl' failed to load earlier in this run.
```

a failure inside a module is delivered to the importing script's handlers:
`try { import "broken.fl" } catch err { ... }` catches the module's error. an
error that reaches the top level is reported against the file that raised it,
not the file that imported it.

## diagnostics, v0.3

errors carry a stable code, a source span, and a severity. the default
output is unchanged, so scripts that compare stderr keep working.

```sh
flint --error-format=human script.fl   # excerpt and caret
flint --error-format=short script.fl   # one location line
flint --error-format=json script.fl    # one JSON object per diagnostic
flint --color=never script.fl          # auto | always | never
flint --fix script.fl                  # apply machine-applicable fixes
flint --explain E0102                  # what a code means
```

human format:

```
error[E0100]: Expect expression.
 --> main.fl:2:1
  |
2 |
  | ^ expected here
  |
```

codes are grouped by origin: `E00xx` scanner, `E01xx` parser and compiler,
`E02xx` names and scope, `E03xx` operators and values, `E04xx` calls, `E05xx`
modules, `E06xx` runtime. the grouping is not a compatibility promise yet.

spans are zero-based half-open utf-8 byte offsets. a reported column counts
utf-8 leading bytes as characters and expands a tab to four columns; it does
not compute terminal display width, so a wide or combining character may
appear shifted.

`--fix` applies only edits marked machine-applicable, checks for overlapping
ranges, validates the result compiles, and replaces a regular file through a
temporary in the same directory. it does not follow a symlink. it is not
complete: it currently handles missing closing delimiters reported at end of
file. Other parser errors do not have automatic edits. Runtime spans identify
the executing line, not the specific bytecode operation. see
[docs/diagnostics.md](docs/diagnostics.md).
