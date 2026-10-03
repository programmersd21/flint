# syntax

## statements

a statement ends at a `;` or at a newline. both work, and you can mix them.

```flint
let a = 1;
let b = 2
let c = 3;
```

a statement also ends at `}` or at the end of the file, which is what lets a
one-line body work:

```flint
if a > 0 { print("positive") }
for i in 1..3 { print(i) }
```

`;` is the safer habit in long files, because a newline inside a construct that
you did not mean to split is a silent bug in most languages. in flint the
parser is strict: a missing terminator is an error, not a guess.

## variables

`let` declares. the type is inferred, never written.

```flint
let x = 1          # a double, like every number
let s = "text"
let xs = [1, 2, 3]
let t = {key: "value"}
```

a declaration with no initializer is nil, which is occasionally what you want
and more often a mistake:

```flint
let x          # nil
let y = x      # also nil
```

`const` is `let` that refuses assignment. it works in every scope, and a global
const is protected across module boundaries too.

```flint
const LIMIT = 100
LIMIT = 200      # error: cannot assign to constant
```

a local const is caught when the file is compiled, so it costs nothing at run
time. a global const is caught when the assignment runs, because the binding
may have been made const by an imported module, and the import happens after
this file was compiled. see [values.md](values.md) and
[modules.md](modules.md).

you can redeclare an ordinary global. `let` on an existing name overwrites it:

```flint
let a = 1
let a = 2
print(a)    # 2
```

but not on a const, which would leave the name read-only while the source reads
as an ordinary redeclaration.

## scope

a `{}` block is a scope. locals declared in it disappear at the closing brace,
and you can reuse the name afterwards.

```flint
let v = 1
{
    let v = 2
    print(v)    # 2
}
print(v)        # 1
```

`if` bodies, `while` bodies, `for` bodies and bare blocks are all scopes. a
function body is a scope too, and starts fresh: it cannot see the locals of
whatever called it.

there is one function-level scoping rule worth knowing, because it is the only
place flint lets a name mean two things at once. a function can see globals
from the file it was written in, and globals are shared across every file, so
a global and a local can collide:

```flint
let total = 10

fn show() {
    let total = 20    # this is a local, it shadows the global
    return total
}

print(show())    # 20
print(total)     # 10
```

## expressions and operators

tightest binding first, which is the order you read them in:

| level | operators | notes |
|---|---|---|
| 1 | `=`, `+=`, `-=`, `*=`, `/=` | assignment. right associative. |
| 2 | `??` | nil-coalescing. right associative. |
| 3 | `or` | |
| 4 | `and` | |
| 5 | `==`, `!=` | |
| 6 | `<`, `<=`, `>`, `>=` | not chainable, see below |
| 7 | `..` | ranges only, inside `for` |
| 8 | `+`, `-` | |
| 9 | `*`, `/`, `%` | |
| 10 | `as` | type assertion |
| 11 | `!`, `not`, `-` | prefix, unary |
| 12 | `f()`, `a[i]`, `a.b` | postfix |
| 13 | literals, names, `(...)`, `[...]`, `{...}` | |

`and`, `or`, `not` and `else` are keywords, not operators. `&&`, `||` and `!`
are not syntax; `!` is a prefix operator and means the same as `not`.

```flint
let a = true
let b = false
print(a and b)     # false
print(a or b)      # true
print(not a)       # false
print(!a)          # false. same thing.
```

`and` and `or` short-circuit, and they return a value rather than a bool. `or`
returns the first truthy operand, which is how you get a default value:

```flint
fn pick(a, b) {
    return a or b
}
print(pick(nil, 7))    # 7
print(pick(1, 7))      # 1
```

`and` returns the first falsy operand:

```flint
print(nil and 7)    # nil
print(3 and 7)      # 7
```

### nil-coalescing

`a ?? b` is `b` when `a` is nil, and `a` otherwise. only nil triggers the
fallback: `false`, `0` and `""` are all values that stay, which is the entire
difference from `or` and the reason both exist.

```flint
let config = {port: nil}
print(config.port ?? 8080)   # 8080
print(0 ?? 8080)              # 0
print(false ?? true)          # false
```

the right side runs only when the left is nil. `5 ?? boom()` never calls
`boom`. right-associative, so `a ?? b ?? c` is `a ?? (b ?? c)`: the middle
is not evaluated before the left is known to need it.

binds looser than `or` and tighter than `=`, which is the order C# uses and
reads the way you expect: `a or b ?? c` is `a or (b ?? c)`.

### destructuring

`let {host, port} = config` binds each name from the table's fields. flat
names only: no nesting, no defaults, no renaming.

```flint
let config = {host: "example.com", port: 8080}
let {host, port} = config
print(host)   # example.com
print(port)   # 8080
```

each name becomes an ordinary binding by the ordinary rules. a missing key
reads nil, as with any field access. a duplicate is a redeclaration error
inside a function and an overwrite at the top level, exactly as with plain
`let`. `const` works the same way.

```flint
let {missing} = config
print(missing)   # nil
```

### comparisons do not chain

`1 < 2 < 3` is not `1 < 2 and 2 < 3`. it parses as `(1 < 2) < 3`, the left side
is a bool, and comparing a bool with a number is an error.

```flint
print(1 < 2 < 3)    # error: operands must be numbers
```

write two tests. this is the one place the expression grammar will not do what
your fingers type, and it is not a bug in the implementation: chained
comparison has no meaning here, because `<=` is a real opcode and not sugar
for `not >`, since `nan` is unordered.

### arithmetic has exactly one overload

`+` adds two numbers or concatenates two strings. nothing else works, and
there is no implicit conversion, because there are no other types to convert
between.

```flint
print(1 + 2)         # 3
print("a" + "b")     # ab
print("n=" + 1)      # error: two numbers or two strings
print("n=" + str(1)) # n=1
```

`%` is modulo for numbers, and there is no string formatting. build strings
with `+` and `str()`.

## assignment

assignment is an expression, so it has a value and can be chained. it returns
what was assigned, not the old value.

```flint
let a = 1
a = 2
a += 3        # 5
a *= 2        # 10
print(a = 7)  # 7
```

the compound forms are not opcodes. the compiler expands `a += b` into a read,
an add and a store, so there is no way for them to behave differently from
writing it out.

assignment is right associative, so `a = b = 1` means `a = (b = 1)` and both
end up 1.

```flint
let a = 0
let b = 0
a = b = 1
print(a)    # 1
print(b)    # 1
```

what you cannot assign to is anything that is not a name. a call result, a list
element, a table field or a parenthesised expression are all rejected when the
file is compiled.

```flint
fn f() { return 1 }
f() = 2      # error: invalid assignment target
```

## `as` -- a type assertion

`expr as T` checks that `expr` is a `T` and passes the value through
untouched. a mismatch is an error.

```flint
print(1 as number)      # 1
print("s" as string)    # s
print(1 as string)      # error: expected type 'string' but got 'number'
```

the seven type names are exactly the seven `type()` returns:

`number`, `string`, `bool`, `nil`, `list`, `table`, `function`

a name that is not one of those is a compile error, and it is case
sensitive:

```flint
print(1 as Number)      # error. no type system to be consistent with.
```

**it is a check, not a conversion.** there is nothing to convert between:
flint has one numeric type, and every other type is already distinct in the
value itself. a conversion operator would have to invent a policy for all 49
ordered pairs and then document the ones it got wrong. `str()` is the
conversion. `as` is the assertion.

what it buys is a named error on the line you wrote. without it, a table where
a string was expected shows up three functions later as a `nil`, or worse, as
a number that happens to be zero.

```flint
fn needs_string(s) {
    return s as string
}

needs_string({a: 1})
# error: expected type 'string' but got 'table'
#        [line 1] in needs_string
```

the check happens when the line runs, not when the file is compiled, because
the value can come from anywhere -- a module that was not read yet, a function
parameter, a list element.

**precedence.** `as` binds tighter than the arithmetic operators and looser
than unary minus, so:

```flint
print(1 + 2 as number)     # 3.  as binds to the 2.
print((1 + 2) as number)   # 3.  same result, different parse.
print(-1 as number)        # -1. as binds to the 1, not to the -1.
```

it composes with the rest of the grammar anywhere an expression goes: a call
result, a field, a list element, a parenthesised expression.

```flint
fn greet() { return "hi" }
print(greet() as string)

let t = {n: 5}
print(t.n as number)

let xs = [1, "two"]
print(xs[1] as string)
```

a cast of something already the right type is legal and does nothing, so it is
safe to add defensively:

```flint
print(1 as number as number)    # 1
```
