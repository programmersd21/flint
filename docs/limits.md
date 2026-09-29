# limits

Things Flint does not do. These are language limits, not hidden switches.

## numbers and types

Every number is an IEEE 754 double. Integers above 2^53 lose precision. There
are no integer arithmetic rules waiting to be discovered later.

There are no static types, generics, classes, or user-defined types. `as`
checks one value at run time; it does not convert it or constrain later calls.

## control and data

Ranges are syntax for `for`, not values. They cannot be stored, passed, or
nested. Table keys are strings; tables cannot be iterated and have no delete
operation. A missing field reads as `nil`, so a typo looks like an unset field.

Strings are byte sequences. Indexing, iteration, length, and the string
helpers count bytes. UTF-8 is left alone, which means a byte index can land
inside a multibyte character.

There is no exception handling. A runtime error prints a trace and exits with
status 70. There is no error value for Flint code to inspect.

## runtime edges

The REPL compiles one line at a time. It cannot keep an unfinished block open
for the next line. It also has no line editing or history; the prompt is not
secretly an editor.

Modules share globals and have no namespaces. `export` is a marker, not a
visibility rule. A failed module can leave globals behind even though the
module itself is removed from the cache.

String growth copies bytes. Repeated concatenation in a loop can therefore
copy the growing prefix on every pass. Build a list of pieces and `join` it
when the string helpers fit the job.

The call stack has 256 frames. Tail calls do not reuse a frame. The value
stack is also fixed-size; deeply nested expressions and large local stacks
can exhaust it.

The VM uses NaN-boxed values and requires a 64-bit host with pointers that fit
in 48 bits. The build is C11, but the value representation is not portable to
every C11 target.

`exec()` starts a program directly and passes arguments separately. Flint
does not invoke a shell for it. File and environment functions expose the
process's ordinary permissions; Flint adds no sandbox.
