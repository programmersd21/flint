# language reference

Flint compiles source directly to bytecode. Statements end at a newline,
semicolon, closing brace, or end of file. Braces are required for control
flow bodies.

```flint
let count = 0
while count < 3 {
	print(count)
	count += 1
}
```

Output:

```text
0
1
2
```

## values and variables

Values are numbers, strings, booleans, `nil`, lists, tables, and functions.
Numbers are doubles. Strings are bytes. Lists hold any values; table keys are
strings.

```flint
let xs = [1, "two", nil]
const host = "localhost"
let cfg = {host: host, port: 8080}
print(cfg.host)
```

`let` without an initializer gives `nil`. `const` requires an initializer and
rejects assignment. A local duplicate in the same scope is a compile error;
globals may be redeclared, subject to the const rule.

Only `nil` and `false` are falsy. `0`, `""`, and empty containers are true.
This is where a C programmer's first `if (count)` port usually goes wrong.

## expressions

Operators, from loose to tight:

```text
= -= *= /= =
or
and
== !=
< <= > >=
..
+ -
* / %
as
! not -
call, index, field
```

`and` and `or` short-circuit and return an operand. `+` adds two numbers or
joins two strings. There is no implicit conversion. Comparisons do not chain:
`1 < 2 < 3` compares the boolean result of `1 < 2` with `3` and errors.

`as` checks a value's type and leaves the value alone:

```flint
print(4 as number)
print(type("x"))
```

Output:

```text
4
string
```

Valid names are `number`, `string`, `bool`, `nil`, `list`, `table`, and
`function`. A bad type name is a compile error; a value of the wrong type is
a runtime error.

## lists, strings, and tables

Lists and strings accept zero-based and negative indices. Negative indices
count back from the end. Indices must be finite whole numbers; assignment
does not grow a list.

```flint
let xs = [10, 20, 30]
print(xs[-1])
xs[0] = 5
```

Output:

```text
30
```

`len` accepts strings and lists. `push` appends and returns the item; `pop`
removes and returns the last item. An empty `pop` is an error. Table fields
use `t.name` or `t["name"]`; a missing field reads as `nil`.

## functions and loops

Functions are declared with `fn`. They return `nil` unless a `return` runs.
Closures capture locals by reference, so a write through a closure changes
the captured binding.

```flint
fn make_counter() {
	let n = 0
	fn next() { n += 1; return n }
	return next
}
let next = make_counter()
print(next())
```

Output:

```text
1
```

`while` repeats while its condition is truthy. `for x in list` visits values.
`for n in start..end` visits numbers from `start` up to, but not including,
`end`. `break` leaves the innermost loop; `continue` starts its next
iteration. Ranges exist only in this `for` form. They are not values.

## modules and input

`import "path.fl"` runs a file in the same VM. Relative paths use the
importing file's directory. Globals are shared; `export` does not make a name
private or enforce visibility. Successful imports run once per VM.

`input()` reads a line and returns a string, or `nil` at end of file.
`input(prompt)` writes the prompt without a newline first. Empty input is `""`;
end of file is `nil`.

For file, environment, string, and process functions, see
[library.md](library.md). For errors and exit codes, see [errors.md](errors.md).
