# values

flint has eight types. that is the whole list.

| type | what it is | how it is written |
|---|---|---|
| number | a double | `1`, `1.5`, `1e10` |
| bool | true or false | `true`, `false` |
| nil | the absence of a value | `nil` |
| string | bytes, not text | `"hello"` |
| list | an ordered sequence | `[1, 2, 3]` |
| table | a key/value map | `{a: 1}` |
| function | flint code | `fn f() {}` |
| native | c code | `len`, `print` and friends |

`type()` tells you which one you have, as a string. `as` checks it without
converting. see [syntax.md](syntax.md).

```flint
print(type(1))        # number
print(type("s"))      # string
print(type(true))     # bool
print(type(nil))      # nil
print(type([1]))      # list
print(type({a: 1}))   # table
print(type(len))      # function
```

## numbers are doubles

every number is a 64-bit ieee 754 double. there is no integer type.

that is a deliberate trade. one representation means no type promotion rules, no
integer division that surprises you, and no overflow to think about. what you
give up is precision on large integers:

```flint
print(2 + 2)            # 4, not 4.0. integral values print as integers
print(9007199254740993) # 9007199254740992. 2^53 + 1, rounded
```

if you need exact integers above 2^53 you need a different language. this is
the cost of the simplest value representation that still fits a number, a
string pointer, and a tag in eight bytes.

dividing by zero is not an error, because there is no integer division to
divide by:

```flint
print(1 / 0)    # inf
print(-1 / 0)   # -inf
print(0 / 0)    # nan
print(0 % 0)    # nan
```

`nan` is not equal to itself. every comparison involving it is false, which is
correct and is also why `<=` and `>` are not the same opcode:

```flint
print(0 / 0 == 0 / 0)   # false
print(0 / 0 < 1)        # false
print(0 / 0 <= 1)       # false
```

## truthiness

only two values are false: `nil` and `false`.

this trips up everyone coming from c, and almost everyone coming from
javascript:

```flint
print(0)        # 0. zero is true
print("")       # empty string, and still true
print([])       # empty list, and still true
print(nil)      # nil, which is false
if 0 { print("yes") }   # yes
```

use `if x == nil` or `if not x` to test for absence. `if not x` is the idiomatic
form and is what the rest of the documentation uses.

## equality

`==` and `!=` compare by type, and the rules are the ones you would guess:

- numbers compare by value. `1 == 1.0` is true
- `nan == nan` is false, because `nan` is not equal to anything
- strings compare by content. identifiers and literals are interned so that
  is a pointer compare; strings built while a program runs are not, so those
  compare by length and then by bytes. a script cannot tell the difference
- lists and tables compare by identity. two lists with the same contents are
  not equal
- `nil == nil` is true

```flint
print(1 == 1.0)       # true
print("a" == "a")     # true
print([1, 2] == [1, 2])   # false. different objects.
print(nil == nil)     # true
```

there is no `===`. identity is not something a script can ask about.

## copying

everything you have seen so far is a value. lists and tables are the exception:
they are heap objects, and assigning one copies the reference, not the contents.

```flint
let a = [1, 2]
let b = a
push(b, 3)
print(a)    # [1, 2, 3]. b and a are the same list.
```

there is no copy constructor and no way to deep copy. if you need a real copy,
build it:

```flint
fn copy_list(xs) {
    let out = []
    let i = 0
    while i < len(xs) {
        push(out, xs[i])
        i += 1
    }
    return out
}
```

the same applies to tables, and the same fix works.
