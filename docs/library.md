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

`str` renders containers the way `print` does. a list becomes `[1, 2, 3]`, a
table becomes `{a: 1, b: 2}` in insertion order, and both nest.

```flint
print(str([1, 2]))      # [1, 2]
print(str({a: 1}))      # {a: 1}
print(str([[1], [2]]))  # [[1], [2]]
```

this used to be `<object>` for anything structured, while `print` showed the
contents -- so there was no way to turn a list into text and every caller
built its own. the rendering is now the same code `print` uses, with a
different output stream, so the two cannot drift apart.

a function or a native has no useful text form and still gives `<object>`.

`print` quotes a string inside a container, so elements stay tellable apart:

```flint
print(["a", "b"])    # ["a", "b"]
print([1, "two"])    # [1, "two"]
print(str(["a", "b"]))    # ["a", "b"]
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

## ord

the byte value of a one-character string, as a number. the argument has to be
exactly one byte long: an empty string and a two-character string are both
runtime errors, because either one would be guessing at which byte was meant.

```flint
print(ord("A"))    # 65
print(ord("a"))    # 97
```

## chr

the inverse direction: a number from 0 to 255 becomes the one-character
string holding that byte. anything outside the range is a runtime error, as
is anything that is not a whole number.

```flint
print(chr(65))         # A
print(chr(ord("z")))   # z
```

`ord` and `chr` round-trip: `chr(ord(s)) == s` for every one-byte string `s`.
multibyte utf-8 is bytes here, not characters, so `ord` of a two-byte
character fails rather than returning half of it. see [values.md](values.md).

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

sorting is not a built-in; use a comparison loop or reach for `collections`,
which has `sort`. a function or native has no useful text form, so
`str(some_fn)` is `<object>` rather than a rendering of something.


## internal math

ten `__`-prefixed natives exist underneath `lib/math.fl`. they are
deliberately not part of the language surface: a leading underscore means
"not for you", and programs are expected to use the `math` module, which
wraps them. nothing in the documentation or the examples reaches for the
bare natives directly.

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
| `math.fma(a, b, c)` | `a*b+c` with one rounding, not two |
| `math.ldexp(x, n)` | `x` times 2 to the `n`, exactly |
| `math.hi32(x)` `math.lo32(x)` | the two halves of a double's bits |
| `math.isnan` `math.isinf` `math.isfinite` | |
| `math.math_pi` `math.math_tau` `math.math_e` | the explicit constants: same values, `math_` prefix |
| `math.math_pi_2` `math.math_pi_4` `math.math_1_pi` `math.math_2_pi` | |
| `math.math_ln2` `math.math_ln10` `math.math_log2e` `math.math_log10e` | |
| `math.math_sqrt2` `math.math_sqrt1_2` | |
| `math.math_deg2rad` `math.math_rad2deg` | |
| `math.math_epsilon` `math.math_max` `math.math_min` `math.math_tiny` | double limits |
| `math.math_inf` `math.math_nan` | |

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
print(c.avg(xs))               # 3.8333333333333335
print(c.reverse(xs))           # [9, 5, 1, 4, 1, 3]
print(c.unique(xs))            # [3, 1, 4, 5, 9]  (order preserved)
print(c.contains(xs, 4))       # true
print(c.index_of(xs, 4))       # 2
print(c.take(xs, 2))           # [3, 1]
print(c.drop(xs, 2))           # [4, 1, 5, 9]
```

`reverse` returns a new list; the original is unchanged. `unique` preserves
first occurrence. `min`, `max`, `sum` and `avg` require a non-empty list and
operate on numbers.

## keys

the key strings of a table, in insertion order, as a fresh list. mutating the
result never touches the table.

```flint
let t = {b: 1, a: 2}
print(keys(t))   # ["b", "a"]
```

## values

the values of a table, in insertion order, as a fresh list. the mirror of
`keys`: same order, same independence, so a write through the result leaves
the table alone.

```flint
let t = {b: 1, a: 2}
print(values(t))   # [1, 2]

let vs = values(t)
vs[0] = 99
print(values(t))   # [1, 2]. vs is a different list.
print(vs)          # [99, 2]
```

the values carry no keys, so two keys holding equal values produce two equal
entries and there is no way to tell which was which. a value that is itself a
container is the table's own object rather than a copy.

## items

the entries as two-element lists, `[key, value]`, in insertion order. the
outer list is fresh and each pair is a fresh list, so a write through either
leaves the table alone.

```flint
let t = {b: 1, a: 2}
print(items(t))   # [["b", 1], ["a", 2]]

let its = items(t)
its[0][1] = 99
print(values(t))  # [1, 2]
print(its)        # [["b", 99], ["a", 2]]
```

an entry is an ordinary list, which means it destructures:

```flint
for entry in items(t) {
    let [key, value] = entry
    print(key + "=" + str(value))
}
```

all three builtins return `[]` for an empty table, never `nil`, and each call
builds a new list, so `values(t) == values(t)` is false. anything other than a
table is a `TypeError` naming the builtin.

```flint
print(values({}))   # []
print(items("ab"))  # error: argument to items() must be a table.
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

## http

`import http` makes outbound request. `http.get`, `http.post`, `http.put`,
`http.delete`, `http.request` and `http.get_json` are the entry points. they
accept options tables: `url`, `method`, `body`, `headers` (a table),
`timeout` in milliseconds (default 10000), and `follow` for redirects
(default true, up to five).

```flint
import http

let headers = {}
headers["User-Agent"] = "flint"
let res = http.get("https://api.example.com/data", {headers: headers})
if res.ok {
    print(res.body)
} else {
    print(res.error)
}
```

table literals only take identifier keys, so a header name with a dash is
set through a computed key first, as above. the options table itself takes
identifier keys only.

the result is a table with:

| key           | meaning                                   |
| ------------- | ----------------------------------------- |
| `ok`          | true only for a 2xx response             |
| `status`      | the http status number                    |
| `status_text` | the reason phrase                         |
| `headers`     | response headers, a table                 |
| `body`        | the response body as a string             |
| `url`         | the final url after redirects             |
| `redirects`   | how many redirects were followed          |
| `error`       | an error message, or an empty string      |

`http://` and `https://` are both supported. redirects 301/302/303 collapse
to a GET without a body; 307/308 keep method and body. response bodies are
returned as-is: no automatic gzip decoding.

`http.get_json(url, opts)` returns `json.parse(res.body)` when the request
succeeds, and `nil` on failure or redirect chains the server chose that no
longer carry the body you expect.

max redirects is five. a transport failure -- dns, refused, timeout -- is
reported with `ok: false`, `status: 0`, and the error message, not a runtime
error. an unparseable url like `ftp://...` is a runtime error.


## ansi

```flint
import ansi
print(ansi.red("nope") + ansi.ANSI_RESET)
```

terminal escape codes, as plain strings. every constant is the bytes for one
code, so they compose with `+` like anything else. nothing here touches the
terminal: whether the other end interprets the bytes is up to it, and piping
the output to a file writes the codes into the file.

the names follow the code they wrap. `ANSI_RED` is the `31m` foreground,
`ANSI_BG_BLUE` is the `44m` background, `ANSI_CURSOR_UP` moves one line, and
the builders take numbers: `ANSI_fg256(9)`, `ANSI_RGB(255, 0, 128)`,
`ANSI_up(5)`, `ANSI_goto(3, 1)`. the named wrappers put the code around a
string and reset after it: `red`, `green`, `yellow`, `blue`, `magenta`,
`cyan`, `white`, `gray`, `bold`, `dim`, `italic`, `underline`, `strike`.

## pretty_print

```flint
import pretty_print as pretty
pretty.pretty_print([1, [2, 3]])
```

a value rendered across lines, for debugging. `pretty_print(v)` prints,
`pretty_output(v)` returns the string. nested lists and tables indent by two
spaces per level.

this is a debugging aid, not a serializer: strings print bare, and nothing
parses the output back.

## args

```flint
import args as argv

print(argv.all())       # the whole argv list
print(argv.count())     # how many arguments the script was given
print(argv.get(0))      # the first one, or nil when there is none
print(argv.has("--x"))  # true when the flag is present
print(argv.value("--out"))  # the argument after the flag, or nil
```

the alias is required, not style: a bare `import args` binds `args` in your
globals and shadows the builtin `args()` the module itself calls. aliased,
both names work.

the builtin `args()` returns the script's argument vector without the
interpreter's own flags. this module wraps the questions a script actually
asks so nobody rewrites the loop: how many, which one, is this flag there,
what follows it. `get` answers `nil` past the end rather than failing,
because a missing argument is a normal thing for a script to check for.

## encoding

```flint
import encoding as e

print(e.hex_encode("hi"))       # 6869
print(e.hex_decode("6869"))     # hi
print(e.base64_encode("hi"))    # aGk=
print(e.base64_decode("aGk=")) # hi
print(e.url_encode("a b&c"))    # a+b%26c
print(e.url_decode("a+b%26c"))  # a b&c
```

the encodings a program meets talking to something that is not flint: a
header, a checksum, a query string. all of them take and return strings,
because that is what the wire carries. url encoding is the query-string
form, where a space is `+`.

none of these is a cipher. base64 is not encryption, and neither is hex.
a malformed input is a runtime error naming the position, not a best
effort: decoding `"zz"` as hex fails rather than returning half a byte.

## csv

```flint
import csv

let rows = csv.parse("name,age\namy,30\nbo,\n")
print(rows)                    # [["name", "age"], ["amy", "30"], ["bo", ""]]
print(csv.parse_objects("name,age\namy,30\n"))
                               # [{name: amy, age: 30}]
print(csv.stringify([["a", "b,c"]]))  # a,"b,c"
```

everything is strings: csv has no numbers, and guessing which fields are
numeric is how a leading zero becomes a different value. lines end with
`\n` or `\r\n`; a field wrapped in double quotes may hold commas,
newlines, and doubled quotes (`"say ""hi"""` reads as `say "hi"`). a quote
anywhere else is literal, because rejecting a file over a stray quote helps
nobody.

`parse` returns `[]` for empty text, and a trailing newline ends the last
row rather than starting an empty one. an unterminated quoted field --
the file ended mid-field -- returns nil, since there is no row to return.
`parse_objects` reads the first row as a header and returns a list of
tables; a short row reads nil for the missing columns and an extra column
is ignored, because ragged csv is the common case. `stringify` quotes only
the fields that need it and ends every row with `\n`, so
`stringify(parse(text))` is text again whenever each line ends with `\n`.

## env

```flint
import env

print(env.string("HOST", "localhost"))  # text, or the fallback when unset
print(env.int("PORT", 8080))            # a number, or the fallback
print(env.bool("DEBUG", false))         # 1/true/yes/on/y mean true
print(env.has("HOME"))                  # true when the variable is set at all
print(env.paths("PATH"))                # split on the platform separator
print(env.expand("~/notes"))            # leading ~ becomes the home directory
env.set("FLINT_DEMO", "1")              # set it for this process and children
env.unset("FLINT_DEMO")                 # remove it again
```

the runtime already has `env(name)`, `os.getenv`, `os.setenv` and
`os.unsetenv`. this module is the parsing layer on top of them: a variable
arrives as text and the script wants a number, a yes-or-no, or a list of
paths. every getter takes the value to use when the variable is not set, so
a caller never branches on nil.

an empty value is a value: set-but-empty returns `""`, not the fallback,
which is why `has` exists. `int` returns the fallback when the variable is
unset or does not name a number, and `bool` treats anything set-but-not-true
as false, because a misspelled config value should be a default, not a
crash. `paths` splits on the platform separator and drops empty entries, so
an unset variable is `[]` and safe to loop over. `expand` only touches a
leading `~`; anything else, or a missing home directory, comes back
unchanged. `set` returns false when the name is invalid -- empty or
containing `=` -- and `unset` succeeds even when nothing was set.

## strings

```flint
import strings

print(strings.repeat("ab", 3))       # ababab
print(strings.reverse("stressed"))   # desserts
print(strings.is_empty(""))          # true
print(strings.slice("hello", 1, 4))  # ell
print(strings.pad_start("7", 3, "0"))  # 007
print(strings.pad_end("7", 3, "0"))    # 700
print(strings.count("banana", "an"))   # 2
```

the runtime's own string primitives -- `split`, `join`, `trim`, `lower`,
`upper`, `replace`, `find`, `char_at` -- are builtins and need no import.
what is here is what a script writes often enough to deserve a name. a
negative `repeat` count gives `""`, and `pad_start`/`pad_end` return the
string unchanged when it is already at width or the fill is empty. `count`
tallies non-overlapping occurrences, and an empty needle counts zero.

everything here counts bytes, not characters. flint strings are byte
sequences, so `len`, subscripting, and these helpers all see a multibyte
utf-8 character as a run of bytes. that is consistent rather than
unicode-aware, and `reverse` on such a string breaks the character apart
rather than working around it.

## glob

```flint
import glob

print(glob.match("*.fl", "args.fl"))            # true
print(glob.filter("*.fl", ["a.fl", "b.txt"]))  # ["a.fl"]
print(glob.list("lib", "*.fl"))                # matching entries, sorted
```

shell-style filename matching with no dependencies: `*` matches any run
including empty, `?` matches exactly one character, and `[...]` classes
take ranges (`[a-z]`) and negation (`[!abc]`). anything else matches
literally, and an unclosed `[` is a literal bracket rather than an error.

`match` says whether the whole name fits the pattern. `filter` keeps the
matching names and keeps their order: filtering selects, it does not sort.
`list` matches against each entry of a directory, skipping `.` and `..`,
and returns the hits sorted, because the filesystem promises no order and
a test depending on readdir order fails on someone else's machine. it
returns nil when the directory cannot be read and `[]` when nothing
matches, so those two answers stay distinguishable. the underscore names
(`_match`, `_class_end`, `_class_hit`, `_sort`) are the module's own
helpers, not part of what a script imports it for.

## terminal

```flint
import terminal

print(terminal.width())              # $COLUMNS, or 80
print(terminal.height())             # $LINES, or 24
print(terminal.is_tty())             # false when TERM is unset or "dumb"
print(terminal.hyperlink("docs", "https://example.com"))
print(terminal.progress_bar(0.5, 10))
print(terminal.spinner_frame(0))
```

small terminal helpers, all pure flint. sizes come from the `COLUMNS` and
`LINES` environment variables, falling back to 80 and 24 when they are
unset or do not name a positive number. colors and cursor movement live in
`ansi.fl`; this module is the rest: sizes, tty guessing, hyperlinks, and
progress widgets.

`is_tty` is honestly a guess: flint has no isatty binding, so it is
env-based heuristics only -- false when `TERM` is unset, empty, or
`"dumb"`, true otherwise. a pipe with `TERM` set still reads true, which
is wrong, and unavoidable until the runtime exposes isatty. `hyperlink`
wraps text in an OSC-8 link that terminals without support render as plain
text. `progress_bar` clamps its fraction to 0..1 and fills with blocks
against light shade inside brackets; `spinner_frame` cycles `"|/-\\"`
for any integer, including negatives.

## log

```flint
import log

let logger = log.new("server")
logger.timestamps = false   # the clock moves; tests switch it off
log.info(logger, "listening")
logger.level = log.WARN
log.info(logger, "dropped")
log.error(logger, "refused")
```

leveled logging to stdout, one line per message: `[INFO] server:
listening`. a logger is a table `{name, level, timestamps}`, and levels
are numbers -- `DEBUG` 0, `INFO` 1, `WARN` 2, `ERROR` 3 -- so assignment
and comparison need no lookup. messages below the level return before
touching the clock. timestamps print the epoch time first and can be
switched off per logger for stable test output. stdout, not stderr:
flint has no stderr handle, and the document says so instead of
pretending.

## test

```flint
import test

test.eq(1 + 1, 2, "addition")
test.run("division", fn() {
    test.check(4 / 2 == 2, "halving")
})
test.finish()
```

a test runner in one import. checks record failures and continue, so one
run reports everything broken rather than stopping at the first; `run`
catches a throw and records it as the case's failure instead of aborting
the suite. `finish` prints the summary and exits nonzero when anything
failed, which is what makes a suite file a gate. `eq` names both sides
on mismatch, because "expected 3, got 4" locates the bug.

## hash

```flint
import hash

print(hash.djb2("hello"))   # 261238937
print(hash.hex("hello"))    # 0f923099
```

djb2 over the string's bytes, exact in doubles and reduced mod 2^32, for
hash tables, convenience checksums, and sharding. non-cryptographic, and
documented as such: the package manager's SHA-256 is the real hash with
vectors, and nothing here should authenticate anything. bytes, like the
rest of flint.

## debug

```flint
import debug

fn serve() {
    return debug.frames()
}
print(debug.format(serve()))
```

the live call stack, innermost first, as `{function, line}` tables --
`frames()[0]` is the caller, and the script level names itself
`<script>`, as in traces. `format` renders the CLI's one-line-per-frame
shape, and `err_str` condenses a caught error to `type: message`.
`dump` prints a value across lines for inspection. read-only throughout:
no breakpoints, no stepping, no locals -- frames carry names and lines
because locals live in slots the collector may reuse.

## regex

```flint
import regex

print(regex.match("[a-z]+@[a-z]+", "amy@x"))  # true
print(regex.find("a+", "xxaay"))              # [2, 4]
```

Thompson NFA matching over bytes: `match` is a full match, `find`
returns `[start, end]` of the leftmost-longest match or nil. literals,
`.`, `* + ?`, `|` alternation, `(...)` groups, `[...]` classes with
ranges and negation, `^ $` anchors, and `\` escapes plus `\d \w \s`.
linear in text times pattern states -- no backtracking, so no input goes
catastrophic. an invalid pattern returns nil rather than false, the way
a typo is distinct from a non-match. no captures, no backreferences, no
`{m,n}` counts: write `xx*`. bytes throughout, so `.` matches `\n`.

## compress

```flint
import compress

let packed = compress.compress("aaaabaaaab")
print(compress.decompress(packed) == "aaaabaaaab")  # true
```

LZSS for small byte strings: a four-byte big-endian length, then tokens
behind one control byte per eight -- literals verbatim, matches as 12
bits of distance plus 4 bits of length. decompress returns nil on
truncated or overreaching input. exact round-trips including NUL bytes;
repetition shrinks, random data roughly breaks even. quadratic window
search through interpreter loops, so kilobytes, not disk images -- a hot
path wants a C dependency, not a faster loop here.

## signal

```flint
import signal

print(signal.send(signal.pid(), 0))   # true: we exist
```

process signals by number: `HUP` 1, `INT` 2, `QUIT` 3, `ABRT` 6, `KILL`
9, `PIPE` 13, `TERM` 15 -- the seven identical on Linux, macOS, and the
BSDs, which is the whole list for exactly that reason. `raise` delivers
to this process synchronously (INT and TERM end it under the default
disposition); `send` is `kill`, and signal 0 checks existence without
delivering. `send` is POSIX-only and errors on Windows, which has no
kill. unknown names throw rather than guessing. deliberately no
handlers: a C signal handler may only touch async-signal-safe state,
and a VM with a collector is the opposite of that.
