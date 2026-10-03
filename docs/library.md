# the library

Everything a script can call. The core language and the system functions are
built in; everything else is a module in `lib/`, imported by name.

## input

read one line from stdin, and return it without the trailing newline.

```flint
let name = input("What is your name? ")
print("hello, " + name)
```

the argument is a prompt, and it is optional. `input()` with nothing reads a
line in silence. a prompt is written exactly as given, with no newline added,
so the line the user types starts where the prompt ends.

a prompt that is not a string is an error, and so is calling it with two
arguments. arity zero or one is not expressible in the fixed-arity check the
VM does for every other native, so `input()` checks inside instead.

the return value is a normal string: empty for an empty line, which is
deliberately a different value from what you get at end of file. pressing
enter on an otherwise empty line gives `""`, and reaching end of file gives
`nil`, because a script has to be able to tell "the user typed nothing" from
"there is nothing left to read".

```flint
print(input())      # with nothing piped in: nil
```

an input line can be any length. the buffer starts at 64 bytes and doubles,
which is the same growth shape as every other array in the runtime and is not
coincidence. a fixed-size buffer would silently truncate a long line, which is
the kind of failure a script cannot possibly notice, let alone handle.

windows line endings are tolerated: a `\r` is skipped, so a file written there
arrives as plain text. a script should not have to know which platform produced
its input.

there is no echo control, no history, no line editing and no signal handling.
it is a prompt and a read.

## string functions

`split(s, sep)` returns a list of byte strings. An empty separator splits into
one-byte strings. `join(xs, sep)` joins string elements with the separator.
`trim(s)` removes surrounding whitespace. `contains(s, part)`,
`starts_with(s, prefix)`, and `ends_with(s, suffix)` return booleans.
`replace(s, old, new)` replaces occurrences; `lower(s)` and `upper(s)` change
ASCII letters. These operate on bytes; UTF-8 characters are not decoded.

```flint
print(split("a,b", ","))
print(join(["a", "b"], ","))
print(trim("  flint  "))
print(contains("flint", "lin"))
```

## system functions

`args()` returns arguments after the script path. `env(name)` returns the
environment value or `nil` when the variable is unset. An empty environment
value is still a string.

`read_file(path)` reads a whole file into a string. `write_file(path, data)`
writes a string and returns `true` on success. These are not sandboxed. A path
is a path on the host, and a script can overwrite a file it can name.

`exec(program, arg...)` searches `PATH`, starts the program directly, waits,
and returns its exit status as a number. It does not invoke a shell and does
not capture output; the child inherits the process streams.

`exit()` terminates the process with status zero. `exit(code)` accepts an
integer status from 0 through 255. It does not return to the calling Flint
function.

## len

length of a string in bytes, or of a list in elements.

```flint
print(len("hello"))    # 5
print(len([1, 2, 3]))  # 3
print(len([]))         # 0
print(len(""))         # 0
```

anything else is a runtime error, including a table.

```flint
print(len({a: 1}))    # error: must be a string or list
```

a string's length is in bytes, not characters, so a utf-8 string is longer
than it looks. see [data.md](data.md).

## push

append to a list. returns the item, so it is usable as an expression.

```flint
let xs = []
push(xs, 1)
push(xs, 2)
print(xs)    # [1, 2]
print(push(xs, 3))    # 3
```

the first argument must be a list. the list is mutated in place; nothing is
copied.

## pop

remove and return the last element.

```flint
let xs = [1, 2, 3]
print(pop(xs))    # 3
print(xs)         # [1, 2]
```

popping an empty list is an error, not `nil`. there is no `tryPop`.

```flint
print(pop([]))    # error: cannot pop from an empty list
```

## insert

place a value at a position, shifting everything after it right. returns the
item, like `push`.

```flint
let xs = [1, 2, 3]
print(insert(xs, 1, 9))   # 9
print(xs)                 # [1, 9, 2, 3]
```

index rules match subscript exactly: negatives count from the end, so
`insert(xs, -1, v)` goes where `xs[-1]` reads. exactly `len(xs)` appends.
anything else out of range, fractional, or non-numeric fails the same way
subscript does.

## remove

take the value out at a position and return it. entries after it shift left.

```flint
let xs = [1, 2, 3]
print(remove(xs, 0))   # 1
print(xs)              # [2, 3]
print(remove(xs, -1))  # 3
print(xs)              # [2]
```

removing past either end, from an empty list, or through a fractional index
is an error rather than `nil`: silently returning nothing for a removal that
removed nothing would hide the off-by-one that caused it.

the slot is not cleared, so the popped value stays reachable until the list is
collected. that is a deliberate simplification, and it is why a large list that
you repeatedly pop does not shrink its memory.

## str

the string form of any value. a string returns itself, so the common case costs
nothing.

```flint
print(str(42))     # 42
print(str(1.5))    # 1.5
print(str(true))   # true
print(str(nil))    # nil
print(str("s"))    # s
```

`str` only converts scalars. a list, a table or a function gives `<object>`,
because `str` is a native and has no access to the printing the `print`
statement does.

```flint
print(str([1, 2]))     # <object>
print([1, 2])          # [1, 2]. the print statement is richer.
```

that difference is real and it will surprise you the first time. `print` and
`str` are two different code paths, and only `print` knows how to render a
container. there is no way to get a string form of a list, so if you need one,
build it yourself:

```flint
fn join(xs, sep) {
    let out = ""
    let i = 0
    while i < len(xs) {
        if i > 0 { out = out + sep }
        out = out + str(xs[i])
        i += 1
    }
    return out
}
print(join([1, 2, 3], ", "))    # 1, 2, 3
```

`print` does quote a string inside a list, so the two cases stay tellable apart:

```flint
print(["a", "b"])    # ["a", "b"]
print([1, "two"])    # [1, "two"]
```

numbers are formatted as flint formats them at the `print` statement: integral
values have no decimal point, and everything else uses the shortest form that
reads back as the same double. see [values.md](values.md).

## num

the number form of a string, and the inverse direction of `str()`. a number
passes through, so `num` is safe to call on something that might already be
one.

```flint
print(num("42"))       # 42
print(num("3.14"))     # 3.14
print(num("-0.5"))     # -0.5
print(num("  7  "))    # 7. leading and trailing whitespace is fine.
print(num(7))          # 7
```

the string has to parse whole. `num("12abc")` fails rather than returning 12,
because returning a prefix would be guessing at what was meant. an empty
string fails too, and so does anything that is neither a number nor a string.

```flint
print(num("abc"))      # error: cannot convert "abc" to a number.
print(num(""))         # error: cannot convert an empty string to a number.
print(num(nil))        # error: must be a number or a string, got a nil.
```

this is deliberately an error and not `nil`. a conversion that cannot be done
is a fact about this line, and reporting it here -- naming the value -- beats
returning nil and letting it surface three calls later as an operand error in
code that had nothing to do with it.

note that `as number` is not this. `as` is a type assertion: `"9" as number`
fails, correctly, because a string is not a number. `num("9")` is 9.

the common shape is reading from the user, since `input()` returns a string:

```flint
import math

const a = num(input("a: "))
print(math.sqrt(a))
```

## type

the name of a value's type, as a string.

```flint
print(type(1))          # number
print(type(1.5))        # number. there is no separate int type.
print(type("s"))        # string
print(type(true))       # bool
print(type(nil))        # nil
print(type([1]))        # list
print(type({a: 1}))     # table
print(type(len))        # function
```

a closure, a flint function and a native all report `function`. there is no
way to tell them apart, and no reason to.

these seven words are the only type names in the language, and they are what
`x as T` takes. a cast and `type()` cannot disagree about what a value is,
because both ask the same function. see [syntax.md](syntax.md).

to check rather than ask, use `as`:

```flint
print(1 as number)    # fine
print(1 as string)    # error: expected type 'string' but got 'number'
```

## clock

process cpu time in seconds, as a double.

```flint
let t = clock()
```

this is cpu time, not wall clock time, so it does not advance while the process
is waiting. that makes it useless for timing anything that blocks, and exactly
right for measuring how much work a benchmark did.

for wall clock timing, measure outside the interpreter, with `time`.

## import_file

not part of the language. the compiler emits a call to it for every `import`
statement, and you should not call it yourself. see [modules.md](modules.md).

## missing pieces

`str([1, 2])` is still `<object>`. `print` knows how to render containers;
`str()` does not. sorting is not a built-in; use a comparison loop or reach
for `collections` which has `min` and `max`.


## internal math

ten `__`-prefixed natives exist for a math library that is not in this
repository yet. they are deliberately not part of the language: a leading
underscore means "not for you", and nothing in the documentation or the
examples uses them.

| native | |
|---|---|
| `__floor(x)` | largest integer not above x |
| `__sqrt(x)` | square root |
| `__fma(a,b,c)` | `a*b+c` with one rounding |
| `__ldexp(x,n)` | x times 2 to the n |
| `__logb(x)` | exponent as a number |
| `__fabs(x)` | absolute value |
| `__copysign(x,y)` | x with y's sign |
| `__hi32(x)` / `__lo32(x)` | half of a double's bits |
| `__from_bits(hi,lo)` | a double rebuilt from two halves |

they take and return numbers, and a non-number argument is a runtime error
like any other. the wrappers live in `src/util/fl_math.c` so the language and
the maths stay separate.

contributed in [#1](https://github.com/programmersd21/flint/pull/1).

## math

```flint
import math
print(math.sqrt(2))
```

the first library anyone imports, and the one the rest of the standard
library leans on. every function takes and returns numbers, because that is
the only numeric type flint has.

a bare name is a library, not a file. `import "foo.fl"` looks next to the
importing file, `import math` looks in the standard library, and the module
loader tells them apart. there is one module system, not two.

| | |
|---|---|
| `math.pi` `math.e` `math.tau` | the usual constants |
| `math.sqrt2` `math.ln2` `math.ln10` | the two-letter ones |
| `math.abs(x)` | |
| `math.sqrt(x)` `math.cbrt(x)` `math.exp(x)` `math.exp2(x)` | |
| `math.log(x)` `math.log2(x)` `math.log10(x)` | |
| `math.pow(x, y)` | there is no `^`. it is xor elsewhere, and flint does not pretend otherwise |
| `math.sin` `cos` `tan` `asin` `acos` `atan` | radians. always. |
| `math.atan2(y, x)` | |
| `math.sinh` `cosh` `tanh` `asinh` `acosh` `atanh` | |
| `math.floor` `ceil` `trunc` | |
| `math.round(x)` | **half away from zero** |
| `math.fmod` `math.remainder` `math.copysign` | |
| `math.isnan` `math.isinf` `math.isfinite` | |

### the two that surprise people

`round` is half away from zero, so `round(0.5)` is 1 and `round(-0.5)` is -1.
the c library's `nearby()` rounds half to even and would give 0 and 0. flint
does not use it, because "what everyone means by round" is worth more than
consistency with a function whose name does not mean round.

`cbrt(-27)` is -3. `pow(-27, 1/3)` is `nan`, and that is correct for a
function that has to be right about negative zero and infinities. it is still
the wrong tool for a cube root, which is why both exist.

### what is not promised

libm is not bit-identical across platforms, and the last digit of a
transcendental function may differ. what flint promises is the behaviour at
the edges: `sqrt(0)` is 0, `cbrt` works on negatives, division by zero gives
infinity rather than an error, and `0/0` is `nan`, which is not equal to
itself.

## path

```flint
import path
print(path.join("a", "b", "c"))
```

path manipulation, and nothing else. every function here is string
arithmetic: this module never touches the disk. that is `fs`, and mixing the
two is how a `join` ends up doing io when somebody expected a string.

| | |
|---|---|
| `path.join(a, b)` | one separator, never two. empty parts are skipped |
| `path.basename(p)` | after the last separator |
| `path.dirname(p)` | before the last separator, or `"."` |
| `path.ext(p)` | with the dot, or `""`. a leading dot is not an extension |
| `path.stem(p)` | the name without the extension |
| `path.isabs(p)` | starts at the root |
| `path.has_ext(p, list)` | case-insensitive, list holds bare extensions |
| `path.sep` | `"/"` |

`has_ext("a.tar.gz", ["gz"])` matches on the *last* extension, which is what
`ext` returns. asking for `tar.gz` is a different question and is not the one
this answers.

the separator is always `/`, including on windows, so a path that arrived in a
config file behaves the same everywhere. a path written as `a\b` is treated as
one component. that is a known limitation, not an oversight: a module that
guesses at the host separator is a module that is wrong in one direction and
surprising in the other.

## random

```flint
import random
print(random.rand())              # a float in [0, 1)
print(random.rand_int(1, 6))     # an integer in [1, 6]
random.shuffle(my_list)          # shuffles in place, returns nil
print(random.choice(my_list))    # picks one element
```

xorshift64* seeded from the clock and process id at first use. fast and
adequate for scripts; not cryptographic. the source says so explicitly.

`seed(n)` resets the state to a known value, which makes a run reproducible.
useful in tests; not an invitation to assume global state across modules.

## time

```flint
import time
let t = time.now()           # unix epoch, fractional seconds
print(time.format(t))        # "2006-01-02T15:04:05Z" (always UTC)
time.sleep(500)              # milliseconds. blocks.
print(time.clock_ms())       # monotonic wall clock, milliseconds

let ms = time.measure(fn() {
    # something you want to time
})
print("took " + str(ms) + "ms")
```

`now()` and `format()` use wall clock time. `clock_ms()` is monotonic and
suitable for benchmarking. `sleep()` takes milliseconds and calls `nanosleep`
internally; a sleep of zero is a yield.

## fs

```flint
import fs

if fs.exists("config.txt") {
    let content = fs.read("config.txt")
    print(content)
}

fs.write("out.txt", "hello\n")
fs.append("log.txt", "one more line\n")
fs.mkdir("new_dir")
print(fs.isdir("new_dir"))   # true
fs.remove("tmp.txt")
```

`read` returns the entire file as a string. `write` and `append` return `nil`.
`exists`, `isdir` return booleans. `mkdir` creates one directory level (not
recursive). `remove` deletes a file; removing a directory that is not empty is
an error.

these are thin wrappers over `fopen`/`fread`/`fwrite`/`stat`. no buffering,
no magic. what posix gives you is what you get.

`read` and `write` stop the script with a clear error when they cannot do
their job -- a missing source, an unwritable destination. a file that is not
there is not an empty file, and returning nil for one would make every reader
check for a case that is really a failure. check `exists()` first when a
missing file is an expected outcome rather than an error.

## listdir

the names inside a directory, or `nil` when it cannot be read.

```flint
import fs

let names = fs.listdir(".")
if names == nil {
    print("cannot read directory")
    exit(1)
}
for name in names {
    print(name)
}
```

names, not paths: join them with `path.join`. `"."` and `".."` are included,
exactly as the filesystem reports them. order is whatever the filesystem
returns, which is to say unspecified.

`nil` covers missing directories, permission failures, and the window
between `exists()` and `listdir()` where the directory disappears. check
`exists()` first when the reason matters; accept nil when it does not.

## os

platform answers, the working directory, and the environment. filesystem
operations are `fs`, path strings are `path`, running programs is `exec()`
or `process`: this module answers questions about the machine rather than
changing it, with `chdir` and `setenv` as the two deliberate exceptions.

```flint
import os

print(os.name() + "/" + os.arch())   # linux/x86_64, say
print(os.getcwd())                   # where the process is standing
print(os.getenv("HOME", ""))         # the value, or "" when unset
print(os.pid())                      # this process's id
```

`name()` is one of `"linux"`, `"darwin"`, `"windows"`, `"freebsd"`,
`"unknown"`. `arch()` is one of `"x86_64"`, `"aarch64"`, `"x86"`, `"arm"`,
`"unknown"`. both answer from preprocessor macros, so they cannot be wrong
about the binary they are compiled into -- but "unknown" is a real answer on
a platform nobody taught them about, and a script that branches on it should
have a fallback.

`getenv(name, fallback)` takes the default explicitly, unlike `env(name)`
which gives nil for a missing name. the result is always a string and the
caller never branches on nil, which is what a config reader wants.

`setenv` and `unsetenv` return booleans. unsetting a name that was never set
succeeds. environment changes affect child processes, which is what makes
them useful before `exec()` or `process.run()`.

`homedir()` and `tmpdir()` give nil when the platform will not answer
(`$HOME` unset, no `TEMP` on windows). a nil there is information -- there
is no home to report -- not a failure.

## process

run a program and read what it said. the command is a list, never a string:
a single string would have to be split somewhere, and splitting on spaces
breaks on filenames that contain them.

```flint
import process

let r = process.run(["git", "status", "--short"])
print(r.code)      # 0
print(r.stdout)    # the output, as a string
```

the result always has the same four fields: `stdout`, `stderr`, `code`, and
`timed_out`. `code` follows the shell convention `exec()` already uses: the
exit status, 128 plus the signal number for a signal death, 127 for "not
found", 126 when exec could not run at all.

```flint
let r = process.run_opts(["ls", "/nonexistent"], {})
print(r.code)        # 2
print(r.timed_out)   # false
```

options are a table, and every key is optional. `cwd` runs there instead of
here. `stdin` is piped to the child's standard input. `timeout` is
milliseconds before the child is killed with SIGKILL:

```flint
let r = process.run_opts(["cat"], {stdin: "hello"})
print(r.stdout)   # hello

let slow = process.run_opts(["sleep", "5"], {timeout: 200})
print(slow.timed_out)   # true
print(slow.code)        # 137, which is 128 + 9
```

both streams are drained while the child runs, so a child that writes a lot
to stderr while the parent reads stdout cannot deadlock -- 64K of unread
stderr with nobody reading it is all a naive implementation takes. an
unknown option is an error rather than ignored: an option the runtime does
not understand is almost certainly a misspelled option it does.

no shell, ever. `execvp` searches PATH and interprets nothing, so arguments
keep their boundaries whatever they contain. a script that genuinely wants a
shell says so explicitly with `["sh", "-c", ...]` and owns the quoting.

## collections

```flint
import collections as c

let xs = [3, 1, 4, 1, 5, 9]
print(c.min(xs))               # 1
print(c.max(xs))               # 9
print(c.sum(xs))               # 23
print(c.reverse(xs))           # [9, 5, 1, 4, 1, 3]
print(c.uniq(xs))              # [3, 1, 4, 5, 9]  (order preserved)
print(c.contains(xs, 4))       # true
print(c.flatten([[1,2],[3]]))  # [1, 2, 3]
print(c.zip([1,2], ["a","b"])) # [[1, "a"], [2, "b"]]
```

`reverse` returns a new list; the original is unchanged. `uniq` preserves
first occurrence. `zip` stops at the shorter list. `min`, `max`, `sum` require
a non-empty list and operate on numbers.

## keys

the key strings of a table, in insertion order, as a fresh list. mutating the
result never touches the table.

```flint
let t = {b: 1, a: 2}
print(keys(t))   # ["b", "a"]
```

## has

whether the table holds the key. compares by content, so a key built at run
time finds the entry a literal created.

```flint
let t = {ab: 1}
print(has(t, "ab"))        # true
print(has(t, "a" + "b"))   # true
print(has(t, "zz"))        # false
print(has(t, 42))          # false. only strings can be keys.
```

## delete

remove an entry, reporting whether anything was removed. deleting a missing
key is false rather than an error.

```flint
let t = {a: 1, b: 2}
print(delete(t, "a"))   # true
print(has(t, "a"))      # false
print(delete(t, "a"))   # false
```

entries after the removed one shift down, preserving insertion order for
everything that remains.

## json

```flint
import json

let obj = json.parse("{\"x\": 1, \"ys\": [2, 3]}")
print(obj.x)           # 1
print(obj.ys[0])       # 2

let s = json.stringify(obj)      # {"x":1,"ys":[2,3]}
let p = json.pretty(obj)         # indented, 2 spaces
```

`parse` returns a table for objects, a list for arrays, a number for numbers,
a string for strings, a bool for booleans, and `nil` for null. a JSON error
is a runtime error naming the offset.

`stringify` and `pretty` accept numbers, strings, booleans, nil, lists, and
tables with string keys. a function in the value tree is a runtime error,
because a function is not JSON and pretending it is makes round-trips wrong.

circular references are not detected. the vm will overflow the call stack
first, which is a fine outcome: a circular structure is a bug, not an edge
case to handle gracefully.
