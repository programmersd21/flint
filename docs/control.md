# control flow

## if

`if` and `else` are keywords, not operators. braces are required.

```flint
if a > 10 {
    print("big")
} else {
    print("small")
}
```

`else if` chains, and there is no `elif`.

```flint
fn grade(n) {
    if n >= 90 { return "A" }
    if n >= 80 { return "B" }
    if n >= 70 { return "C" }
    return "F"
}
```

that form, with early returns and no `else`, is the idiomatic one. a chain of
`else if` on a single function does the same thing and reads worse once there
are more than three cases.

an `if` with no `else` is fine, and a body of one statement on the same line
works:

```flint
if a > 0 { print("positive") }
```

the condition is an expression, and only `nil` and `false` are falsy. see
[values.md](values.md), because `if 0` is true and always surprises someone.

## while

```flint
let i = 0
while i < 5 {
    print(i)
    i += 1
}
```

nothing stops a runaway loop. there is no iteration limit, so a loop whose
condition never becomes false runs until the operating system kills the
process. keep a counter you can see.

## for

`for` has two forms, and which one you get is decided by the iterable.

### ranges

`a..b` is a half-open range: the start is included, the end is not.

```flint
for n in 1..5 { print(n) }    # 1 2 3 4
for n in 0..0 { print(n) }    # nothing
```

a range is a compile-time construct, not a value. there is no range type, you
cannot store `1..5` in a variable, and `..` is not an operator, which is why
`for n in (1..5)` is a syntax error. the parentheses have nothing for it to
bind to. see [limits.md](limits.md).

the loop variable is scoped to the loop, and does not survive it. an outer `i`
is untouched, which is the behaviour you want and saves a name.

```flint
let i = 100
for i in 1..3 { print(i) }    # 1 2
print(i)                      # 100
```

### lists

a list is iterable. the loop runs over its elements, not its indices.

```flint
for x in [10, 20, 30] { print(x) }
for x in [] { print("never") }
```

`for` over a string also works, because strings are sequences of bytes:

```flint
for c in "abc" { print(c) }
```

`for k, v in t` walks every entry of a table in insertion order:

```flint
let t = {a: 1, b: 2}
for k, v in t {
    print(k)
    print(v)
}
```

insertion order is the contract, because tables are insertion ordered to
begin with. the count is re-read every iteration, so entries appended in the
body are visited; entries removed shift everything after them down. a loop
that mutates its own table is the author's responsibility. nested loops keep
separate positions, and `break` and `continue` work.

## break and continue

both work in any loop, and neither works outside one.

```flint
let i = 0
while i < 10 {
    i += 1
    if i == 3 { continue }    # skip this pass
    if i == 6 { break }       # leave the loop
    print(i)                  # 1 2 4 5
}
```

`continue` in a `for` loop jumps to the increment, not to the condition, which
is the behaviour you want and the one that is easy to get wrong by hand.

in nested loops, `break` only leaves the inner one. there is no labelled break,
so the idiom is a flag checked by the outer loop.

```flint
let done = false
let found = 0
for a in 1..4 {
    for b in 1..4 {
        if a * b == 6 {
            found = a * 10 + b
            done = true
            break          # leaves the b loop only
        }
    }
    if done { break }       # now leave the a loop too
}
print(found)    # 32. 3*2, the first row that can produce it.
```

the flag has to be a bool. `if found` does not work, and this is the single
most common flint bug, because `0` is truthy. a numeric result of 0 would take
the branch and end the search on the first iteration. use `done = true` and
`if done`, or test with `if found != 0`.

remember the ranges are half-open, so `1..4` is 1, 2 and 3. when you find
yourself repeating this flag, move the search into a function and `return`
instead, which is clearer than either.

## blocks

a bare block is a scope, which is occasionally useful for narrowing a name
without writing a function.

```flint
let v = 1
{
    let v = 2
    print(v)    # 2
}
print(v)        # 1
```
