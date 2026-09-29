# the library

seven built-in functions. that is the entire standard library, and it is
deliberately that small. anything else is a flint function you write.

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

## what is missing

deliberately, and each would be a function you write:

- no `print`-to-a-string, so you cannot build a log line without `+` and `str()`
- no file io, no environment, no process control
- no echo control or history on `input`. it prompts and reads, and that is it.
- no random numbers. there is no seedable PRNG in the runtime, and adding one
  means picking a source of entropy that is not a portability problem
- no string methods, so `"a,b".split(",")` is a `while` loop
- no sorting, on either lists or tables
- no integer, so there is no integer division or overflow to think about, and
  no exact arithmetic above 2^53

each of these is a few lines of flint, and keeping them out keeps the runtime
small enough to read in an afternoon.
