# flint

a small bytecode interpreter and scripting language, in portable c11. no
dependencies beyond libc and libm.

```sh
make release
```

that's the whole build. no llvm, no cmake, no configure step, no code
generator. the compiler runs once, most of it in parallel, and you get a
binary. if that takes more than a second, check your machine.

```sh
./flint -e 'print(1 + 2)'
```

## install

from source, which is two commands and works anywhere with a c11 compiler:

```sh
git clone https://github.com/programmersd21/flint
cd flint && make release
```

on arch, there is an aur package. it is community-maintained, not owned by
this repo, so it may trail the releases here by a version:

```sh
yay -S flint-bin
```

if `yay` reports a version older than the latest tag above, build from source.
a package manager that installs last month's interpreter is a package manager
doing its best, and the build takes less than a second anyway.

## what it is

- a stack vm over 64-bit nan-boxed values: a number, or a tagged pointer, in
  eight bytes
- a single-pass pratt parser. no ast. tokens go in one at a time, bytecode
  comes out the other end
- mark-and-sweep gc with an explicit gray stack, so a deeply nested list is
  heap work rather than a segfault in the collector's third frame
- interned strings, which is what makes `==` on two strings cost one pointer
  compare instead of a memcmp on every loop iteration
- closures that capture by reference, lists, tables, modules, and a scripting
  language small enough that the whole thing reads in an afternoon. if there
  is a comment above a function, it is there because the reason is not in the
  code.

## build targets

```sh
make release    # -O2, no asserts. the one you ship.
make debug      # -O0 -g3, dumps the bytecode and traces every instruction
make stress     # gc on every allocation, under asan and ubsan
make test       # builds release, runs the language suite
make diagnostic-test # checks human, short, JSON, and fix output
make unit       # the value and chunk unit tests
make lint       # clang-tidy, policy in .clang-tidy
make fmt        # clang-format, policy in .clang-format
make fmt-check  # same, but reports instead of rewriting. this is the ci one.
```

`make stress` is the one that finds things. it runs the whole suite with the
collector firing on every single allocation, so a missing root turns into a
use-after-free immediately rather than on a thursday.

## running

```sh
./flint                    # repl, one line at a time
./flint path/to/script.fl  # run a file
./flint -e 'print(1 + 2)'  # run one expression
./flint -                  # read a script from stdin
```

exit codes follow sysexits, so a shell can tell the failures apart: 64 for
usage, 65 for a file that did not compile, 70 for a program that blew up, 74
for a file that could not be read.

## the language

three lines to start:

```flint
let xs = [1, 2, 3]
for x in xs { print(x * 2) }
```

### variables

```flint
let a = 10
a += 5                    # 15
const b = "immutable"
b = "nope"                # error: cannot assign to constant
```

`const` is read-only wherever it lives. a local one is caught when the file
compiles; a global one is caught when the assignment runs, because it may have
come from a module that hadn't been read yet. same rule, two timings.

### control flow

```flint
if a > 10 {
    print("greater")
} else {
    print("not greater")
}

let i = 0
while i < 3 { i += 1 }

for n in 1..5 { print(n) }              # 1 2 3 4. the end is excluded.
for item in ["apple", "banana"] { print(item) }
```

### functions and closures

a closure grabs the enclosing locals by reference. when the frame exits, any
upvalue that outlives it moves to the heap, so the closure keeps working.

```flint
fn make_counter() {
    let n = 0
    fn bump() {
        n += 1
        return n
    }
    return bump
}

let c = make_counter()
print(c())    # 1
print(c())    # 2
```

this is the main substitute for classes. two counters from the same factory
don't interfere, because each call gets its own `n`. if you need a small
object with a few methods, return a table of closures.

### lists and tables

```flint
let list = [1, 2, 3]
push(list, 4)
print(list[0])     # 1
print(list[-1])    # 4. negative counts from the end.
print(pop(list))  # 4

let table = { host: "localhost", port: 8080 }
print(table.host)
table.port = 9000
print(table.missing)    # nil. a miss is not an error, which is a tradeoff.
```

### `as` -- checking a type

```flint
print(1 as number)      # 1
print(1 as string)      # error: expected type 'string' but got 'number'
```

an assertion, not a conversion. there is nothing to convert between: one
numeric type, everything else already distinct in the value. `str()` is the
conversion; `as` is the check.

what it buys is an error on the line you wrote, instead of a `nil` three
functions later. the seven type names are the seven `type()` returns: `number`,
`string`, `bool`, `nil`, `list`, `table`, `function`.

### modules

one file is a module. `import` runs it, and everything it defines becomes
global.

```flint
# math.fl
export fn square(x) {
    return x * x
}
```

```flint
# main.fl
import "math.fl"
print(square(6))
```

**imports resolve against the importing file**, so a script runs from any
directory:

```sh
$ flint myproject/main.fl     # works
$ cd myproject && flint main.fl   # also works
```

a relative path is joined to the directory of the file doing the import, not
to wherever you happen to be standing. an absolute path is used as given. that
is the whole change, and it is the difference between a script that works and
a script that works only from one place.

modules run once per VM. importing one again is a no-op, so a library's
top-level code happens once however many files pull it in, and a repeated
import is not a redeclaration of its constants. a file that imports itself is
an error naming the file, not a stack overflow.

there is still no `private` and no namespace: all files share one global
table, so two modules defining the same name is a collision that import order
decides. see [docs/modules.md](docs/modules.md).

### the whole library

The built-ins cover core values, byte-string operations, and basic system
access. There is no package manager and no external runtime dependency.

```flint
let name = input("What is your name? ")   # a prompt, a line, no trailing \n
let path = args()                        # arguments after the script name
let home = env("HOME")                   # nil if it is not set
let text = read_file("in.txt")           # the whole file, as a string
print(write_file("out.txt", text))
exit(exec("grep", "-c", "error", "app.log"))   # no shell, ever
```

`input()` writes its prompt with no newline and reads one line, so the typing
starts where the prompt ends. an empty line gives `""` and end of file gives
`nil`, which are different values on purpose: the script has to be able to
tell "the user typed nothing" from "there is nothing left to read".

`exec()` calls `execvp` and never a shell. there is no path from this API to
`/bin/sh`, so a filename with a space or a semicolon in it is an argument
rather than an injection.

the string primitives a text script spends its time in:

```flint
print(split("a,b,c", ","))        # ["a", "b", "c"]
print(join(["a", "b"], "-"))      # "a-b"
print(trim("  hi  "))             # "hi"
print(contains("hello", "ell"))   # true
print(replace("a-b", "-", "+"))   # "a+b"
```

still absent on purpose: no `sort`, no random, no environment listing, no
process spawning beyond `exec`, and no regex. each is a portability question
or a design argument, and the answer so far has been no. see
[docs/library.md](docs/library.md) for the full list and the reasoning.

### errors

the default output is one line per error, and it is what scripts that compare
stderr already expect. when you want more:

```sh
$ flint --error-format=human bad.fl
error[E0100]: Expect expression.
 --> <command line>:1:8
  |
1 | let x =
  |        ^ expected here
  |
```

`short` prints one location line; `json` emits one object per diagnostic.
`--fix` applies the machine-applicable closing-delimiter edits currently
supported. `--explain E0102` prints a short explanation. Runtime spans still
underline the executing source line, and the compiler does not yet collect
multiple independent errors. See [docs/diagnostics.md](docs/diagnostics.md).

## two things that catch everyone

both of these have their own section in the docs, and both have cost this
project a bug:

- **0 is truthy.** only `nil` and `false` are falsy, so `if n` does not mean
  "if n is non-zero"
- **ranges are half-open.** `1..5` is four values: `1 2 3 4`

## documentation

[docs/](docs), in the order you need it:

| | |
|---|---|
| [values](docs/values.md) | the types, and what makes them equal |
| [syntax](docs/syntax.md) | expressions, operators, statements, `as` |
| [control](docs/control.md) | `if`, `while`, `for`, `break`, `continue` |
| [functions](docs/functions.md) | functions, recursion, closures |
| [data](docs/data.md) | strings, lists, tables |
| [modules](docs/modules.md) | `import` and `export` |
| [library](docs/library.md) | core, string, and system functions |
| [diagnostics](docs/diagnostics.md) | error formats, current coverage, and `--fix` |
| [language reference](docs/language.md) | the language in one document |
| [VM internals](docs/internals.md) | value representation, GC, closures, modules |
| [errors](docs/errors.md) | what goes wrong, and the exit codes |
| [limits](docs/limits.md) | what it doesn't do, and what that costs |

[examples/](examples) has runnable programs. `hello.fl` first, then work up.
The module example uses paths relative to its own file.

## hacking

- zero warnings under `-Wall -Wextra -Wpedantic -Werror`. one warning is a
  rejected patch.
- run `make stress`. if asan reports a leak, a use-after-free or a misaligned
  access, fix it before asking for a review.
- a bug gets a test: a `.fl` file under `tests/language/` and a `.expected`
  file next to it. `make test` runs every pair it finds.
- don't mix reformatting into a functional patch. `make fmt` is not an excuse.
- no emoji in commit messages.

[CONTRIBUTING.md](CONTRIBUTING.md) has the rest, including the list of things
that are known to be wrong and where each fix belongs.

## license

mit. see [LICENSE](LICENSE).
