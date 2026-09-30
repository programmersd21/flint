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

There is no random number generator, sorting, or conversion of a printed
container into a string. `str([1, 2])` is still `<object>`. The `print`
statement knows how to render containers; `str()` does not.

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
