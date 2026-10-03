# data

three containers, and a string type that is not quite a container. the
differences between them are the thing to internalise: they do not
interoperate, and there is no generic container.

## strings

a string is bytes with a length. it is not a list of characters, but you can
index it and iterate it, both of which give you one-character strings.

```flint
let s = "hello"
print(s[0])       # h
print(s[-1])      # o. negative indices count from the end.
print(len(s))     # 5
for c in s { print(c) }
```

indexing out of range is an error, in either direction:

```flint
print(s[99])     # error: string index 99 out of bounds
```

`+` concatenates two strings. there is no interpolation, no format string and
no `%`:

```flint
print("n=" + str(1))    # n=1
```

a concatenation that would overflow the length is an error rather than
undefined behaviour. doubling a string each iteration reaches the limit in
about a dozen passes, and without the check the two lengths add in int,
overflow, and hand a negative number to the allocator as an enormous size.
scripts grow; lengths do not wrap.

`str()` is how you turn anything into a string, and it is the only conversion
you need. see [library.md](library.md).

### escapes

`\n`, `\t`, `\r`, `\\`, `\"` and `\0` are recognized. an unknown escape keeps
the character after the backslash, so `"\q"` is a `q`.

```flint
print("a\tb")
print("line\nbreak")
print("say \"hi\"")
print("back\\slash")
print("\q")    # q
```

utf-8 is not interpreted. a multibyte character is a run of bytes, and
`s[0]` gives you the first byte of it, not the character. this is a byte
language, not a text language.

## lists

an ordered, growable sequence. heterogeneous: elements can be any type, and
nothing stops a list holding a mix.

```flint
let xs = [1, "two", true, nil, [5]]
print(xs[0])
print(len(xs))
```

### building and reading

`push()` appends and returns the item. `pop()` removes the last and returns it.
indexing with a negative number counts from the end.

```flint
let xs = [1, 2, 3]
push(xs, 4)
print(xs[3])     # 4
print(xs[-1])    # 4
print(pop(xs))  # 4
print(len(xs))  # 3
```

`insert()` and `remove()` work by position, with the same index rules as
subscript: negatives count from the end, fractions and out-of-range fail the
same way. `insert` at exactly `len(xs)` appends; `remove` returns what it took.

```flint
let xs = [1, 2, 3]
insert(xs, 1, 9)
print(xs)          # [1, 9, 2, 3]
print(remove(xs, 1))  # 9
print(xs)          # [1, 2, 3]
```

there is no `slice`, no `sort`, no `map`, no `filter`, no `join`. every one
of those is a function you write:

```flint
fn map(xs, f) {
    let out = []
    let i = 0
    while i < len(xs) {
        push(out, f(xs[i]))
        i += 1
    }
    return out
}

fn filter(xs, pred) {
    let out = []
    let i = 0
    while i < len(xs) {
        if pred(xs[i]) {
            push(out, xs[i])
        }
        i += 1
    }
    return out
}
```

the reason for the absence is that generics do not exist and a built-in `map`
would have to be a native, and a native that takes a function is a callback the
runtime has no story for. writing them in flint costs one function each and
they are readable.

`for` iterates a list directly, which is how you get an index-free loop:

```flint
for x in [10, 20, 30] {
    print(x)
}
```

### out of range

both directions, and a negative index past the start, are errors.

```flint
let xs = [1, 2, 3]
print(xs[3])     # error: out of bounds (len 3)
print(xs[-4])    # error: out of bounds
```

an index that is not a whole number is an error, not a truncation. `xs[1.5]`
does not read element 1; it reports, because reading the wrong element
silently is worse than failing loudly. the same goes for anything beyond the
range of an int, for infinity and NaN, and for values that are not numbers at
all. negative indices still count from the end, and valid ones are untouched:
this changed what fails, not what succeeds.

### assignment

index assignment overwrites in place. it does not grow the list, and there is
no append-through-assignment.

```flint
let xs = [1, 2, 3]
xs[0] = 99
print(xs[0])    # 99
xs[3] = 4       # error: out of bounds
```

## tables

a key/value map. keys are always strings; there is no integer key, no tuple
key, and no nested key.

```flint
let t = {name: "flint", year: 2026}
print(t.name)
t.year = 2027
print(t.year)
```

a key that was never set reads as `nil`, which is not an error. that is a
deliberate choice: it makes partially-filled records cheap, and the cost is
that a typo is silent.

```flint
let t = {a: 1}
print(t.missing)    # nil, no error
```

because of that, a miss and a stored `nil` are indistinguishable. if you need
to tell them apart you need a sentinel of your own.

### computed keys

a literal name after the dot is one way to reach a field. a value in brackets
is the other: `t[k]` reads the entry whose key equals `k`, and `t[k] = v`
writes it, creating the entry when it is missing.

```flint
let t = {ab: 1}
let k = "a" + "b"
print(t[k])     # 1. the key was built at run time.
t[k] = 99
print(t.ab)     # 99
print(t["zz"])  # nil, like any missing key
```

both directions compare by content, so a key built at run time finds the
entry a literal created. the key must be a string; anything else is an error
rather than a miss, because `t[42]` is a bug in the key expression.

### iteration

`for k, v in t` walks every entry in insertion order:

```flint
let t = {a: 1, b: 2}
for k, v in t {
    print(k)
    print(v)
}
```

insertion order is the contract, because the table is insertion ordered to
begin with. the count is re-read every iteration, so entries appended in the
body are visited; entries removed shift everything after them down by
position. a loop that mutates its own table is the author's responsibility,
and the behaviour is positional rather than surprising.

nested loops keep separate positions, and `break` and `continue` work,
because the desugar is the same loop shape as list iteration with different
loads.

### asking and removing

```flint
let t = {a: 1}
print(keys(t))       # ["a"]. a fresh list, in insertion order.
print(has(t, "a"))   # true
print(has(t, "zz"))  # false
print(has(t, 42))    # false. only strings can be keys.
print(delete(t, "a"))  # true. the entry is gone.
print(has(t, "a"))     # false
print(delete(t, "a"))  # false. already gone is not an error.
```

`keys()` returns a new list every call, so mutating the result never touches
the table. `has()` with a non-string key is false rather than an error:
asking about something that cannot be a key is a no. `delete()` on a missing
key is false for the same reason "make sure this is gone" should not fail
when it already is.

### growth

adding a key grows the table by doubling, and a write to an existing key
overwrites. deleted entries shift the survivors down, preserving insertion
order for everything that remains.

## which one to use

| you have | use |
|---|---|
| an ordered sequence | list |
| named fields | table |
| text | string |

they do not convert between each other, and there is no `dict` view of a table
or list view of a table. a list of alternating key and value is a table in
spirit, and works fine, if you find yourself writing `xs[0]` and `xs[1]`
together often enough that you should stop.
