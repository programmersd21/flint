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
return   true     while
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
importDecl     = "import" STRING terminator ;
exportDecl     = "export" ( fnDecl | letDecl | constDecl ) ;

statement      = exprStmt | printStmt | ifStmt | whileStmt | forStmt
               | breakStmt | continueStmt | returnStmt | "{" block "}" ;

printStmt      = "print" "(" arguments ")" terminator ;
ifStmt         = "if" expression block ( "else" ( ifStmt | block ) )? ;
whileStmt      = "while" expression block ;
forStmt        = "for" IDENTIFIER "in" expression block ;
returnStmt     = "return" expression? terminator ;
breakStmt      = "break" terminator ;
continueStmt   = "continue" terminator ;

terminator     = ";" | newline | "}" | EOF ;
```

`for x in expr` iterates a list, or a range when `expr` contains `..`.
ranges are half-open: `1..5` is 1, 2, 3, 4.

### expression precedence

loosest to tightest:

1. assignment: `=`, `+=`, `-=`, `*=`, `/=`
2. `or`
3. `and`
4. equality: `==`, `!=`
5. comparison: `<`, `<=`, `>`, `>=`
6. range: `..`
7. term: `+`, `-`
8. factor: `*`, `/`, `%`
9. type assertion: `as`
10. unary: `!`, `not`, `-`
11. call, subscript, field: `()`, `[]`, `.`
12. primary: literals, identifiers, grouping, list, table

`<=` is not `!(>)`. `NaN` is unordered, so the two forms differ on NaN and the
compiler emits a separate opcode for each.

### type assertions

`expr as T` is an infix operator. `T` is one of the seven names `type()`
returns: `number`, `string`, `bool`, `nil`, `list`, `table`, `function`. any
other name is a compile error.

`as` asserts; it does not convert. the value is unchanged on success. on
failure the program stops with a runtime error naming both types.

the assertion runs at run time, not at compile time, because the value may come
from a module that has not been read yet or from a function parameter.

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

## modules, v0.3

`import` resolves a relative path against the directory of the *importing
file*, not the process working directory. a module path is part of the
source, so it means the same thing no matter where the user is standing. an
absolute path is used as given.

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

a module runs once per VM. importing it again is a no-op, so a library's
top-level side effects happen once however many files pull it in, and a
repeated import of a module that exports a `const` is not a redeclaration of
it. the cache is keyed by resolved path, interned.

a file that imports itself, or two files that import each other, is an
error naming the file already in flight:

```
error[E0501]: Import cycle: 'cyc.fl' is already being loaded.
```

a failed import is not cached. a missing file or a module that throws can be
retried, and the retry is a real attempt rather than a cycle report.

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
