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

**paths resolve against the working directory, not the importing file.** this
bites everyone once, and it bites in the direction you would not guess:

```sh
$ flint myproject/main.fl                 # works. cwd is already myproject's parent
$ cd myproject && flint main.fl           # cannot open module file 'math.fl'
```

the second one is the surprise. you are *in* the directory and it still cannot
find the file next to the script, because "math.fl" is looked up as
`./math.fl` and your cwd is now `myproject`, where the file does not live.

so a script only finds its imports if you run it from the directory the paths
were written for. there is no search path and no `private` -- all files share
one global table, so two modules defining the same name is a collision that
import order decides. see [docs/modules.md](docs/modules.md).

### the whole library

seven functions. that is deliberate, and the size is the point: anything else
you reach for before you write a loop is a runtime nobody can hold in their
head.

```flint
print(len("abc"))   # 3. string or list.
print(len({a: 1}))  # error. tables have no len.
print(str(42))      # "42"
print(type([1]))    # "list"
```

the one that talks back:

```flint
let name = input("What is your name? ")
print("hello, " + name)
```

`input()` writes its prompt with no newline and reads one line, so the typing
starts where the prompt ends. an empty line gives `""` and end of file gives
`nil`, which are different values on purpose: the script has to be able to
tell "the user typed nothing" from "there is nothing left to read". see
[docs/library.md](docs/library.md).

`clock()` returns process cpu time. `push` and `pop` work on lists. no
`map`, no `sort`, no file io, no random -- see
[docs/library.md](docs/library.md) for the list and why.

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
| [library](docs/library.md) | the seven built-in functions |
| [errors](docs/errors.md) | what goes wrong, and the exit codes |
| [limits](docs/limits.md) | what it doesn't do, and what that costs |

[examples/](examples) has runnable programs, commented line by line.
`hello.fl` first, then work up. the module example needs a `cd`, which is the
path thing above demonstrating itself.

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
