# examples

runnable flint programs. they are longer than the snippets in
[../docs](../docs) and are commented line by line, so read a file top to
bottom rather than dipping in.

| file | shows |
|---|---|
| [hello.fl](hello.fl) | the smallest complete program, and `type` |
| [fizzbuzz.fl](fizzbuzz.fl) | ranges, `if`/`else if`, `%`, truthiness |
| [as.fl](as.fl) | type assertions, and why they are not conversions |
| [math.fl](math.fl) | the maths library, and the two rounding surprises |
| [input.fl](input.fl) | prompts, lines, empty input, EOF, and a long line |
| [closures.fl](closures.fl) | capture by reference, the factory pattern, `map`/`filter` |
| [data.fl](data.fl) | lists, tables, strings, and the functions the library omits |
| [modules/](modules/) | a four-file program: import, export, shared globals, const |

## running them

from the repository root:

```sh
./flint examples/hello.fl
./flint examples/fizzbuzz.fl
./flint examples/as.fl
./flint examples/math.fl
./flint examples/closures.fl
./flint examples/data.fl
printf 'Ada\nsecond\n\nAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\nafter\nlast' | ./flint examples/input.fl
```

the module example is the exception because its imports are relative to the
importing file. Run it from its directory so the example paths stay short.

```sh
cd examples/modules && ../../flint main.fl
```

see [../docs/modules.md](../docs/modules.md) for the path and cache rules.

## a note on the comments

they are not explaining the syntax. `print` is not a function call, `let` needs
no type, and none of that is worth a line. the comments are on the things that
are not guessable: that `0` is truthy, that ranges are half-open, that a name
is resolved when the line runs rather than when the file is read, and that
`str` and `print` are two different code paths.
