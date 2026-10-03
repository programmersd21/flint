# functions

```flint
fn add(a, b) {
    return a + b
}

print(add(2, 3))
```

`fn` is a keyword, and there is no `def`, no `function`, no `->` and no type
annotations. a parameter list needs parens even when it is empty.

there is no way to declare what a parameter should be. use `as` at the top of
the body to check it at run time, which is a weaker promise but a real one. see
[syntax.md](syntax.md).

functions are values. they can be passed around, stored in a list, and returned.

```flint
fn twice(f, x) {
    return f(f(x))
}
fn double(n) { return n * 2 }
print(twice(double, 5))    # 20
```

a function literal is also an expression, so a function can exist without ever
being named:

```flint
let square = fn(x) { return x * x }
print(square(5))           # 25

fn apply(f, v) { return f(v) }
print(apply(fn(x) { return x + 1 }, 41))    # 42
```

the anonymous form captures whatever the named one does, so a closure returned
from a function works the same either way:

```flint
fn adder(base) {
    return fn(x) { return base + x }
}
print(adder(10)(5))    # 15
```

use the named form when the function is called from more than one place, since
a name is what a stack trace shows. the anonymous form names itself
`<anonymous>` in traces.

## arity

the number of parameters is checked when the call happens, not when the file is
compiled, and a mismatch is a runtime error with the function name in the
message.

```flint
fn one(a) { return a }
print(one(1, 2))    # error: expected 1 arguments but got 2
```

there is no default argument, no variadic form, and no optional argument. a
function that wants optional behaviour takes the flag explicitly and branches.

## returning

`return` sends a value back. a function with no `return` returns `nil`, which
makes it a procedure.

```flint
fn nothing() {}
print(nothing())    # nil
```

`return` on its own is the same as returning nil, and is worth writing anyway
when a function returns early in some branches.

execution stops at the first `return`. code after it in the same block is dead,
and the compiler does not warn about it.

there is one return value. multiple returns mean a tuple, which flint does not
have, so return a list or a table and unpack it at the call site.

## recursion

functions may call themselves, up to 256 frames deep. past that you get
"Stack overflow" rather than a crash.

```flint
fn fact(n) {
    if n <= 1 { return 1 }
    return n * fact(n - 1)
}
print(fact(10))
```

tail calls are not optimized. a helper that recurses a million deep will hit
the limit, even if every call is in tail position. write it as a loop if the
depth matters.

## closures

a function defined inside another function can read and write the enclosing
function's locals. it captures them by reference, not by value, so two closures
sharing a variable see each other's writes.

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
print(c())    # 3
```

each call to `make_counter` builds a fresh `n`, so two counters are
independent:

```flint
let a = make_counter()
let b = make_counter()
print(a())    # 1. b is untouched.
print(b())    # 1
print(a())    # 2
```

this is the closure factory pattern, and it is the main use for nested
functions in flint. a language without classes gets encapsulation from this.

capturing a variable declared inside a loop body gives a fresh binding per
iteration, which is what you want:

```flint
fn make_adders() {
    let out = []
    let i = 0
    while i < 3 {
        let base = i * 10
        fn adder(x) { return base + x }
        push(out, adder)
        i += 1
    }
    return out
}

let adders = make_adders()
print(adders[0](1))    # 1
print(adders[1](1))    # 11
print(adders[2](1))    # 21
```

## naming

the value of a `fn` when printed is `<fn name>`, or `<script>` for the top-level
code. an anonymous function has no name in the source, so a stack trace shows
`<anonymous>`, which tells you the function is a literal without pretending to
know which one it was.

## what is not a function

`print` is a keyword, not a value. it has its own statement form, so it cannot
appear where an expression is expected, and there is no way to get a reference
to it.

```flint
let p = print    # error: expect expression
```

`len`, `push`, `pop`, `str`, `type` and `clock` are ordinary globals holding
native functions, and those you can pass around.

```flint
fn apply(f, x) { return f(x) }
print(apply(len, "abc"))    # 3
```
