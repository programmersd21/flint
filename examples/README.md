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
| [fibonacci.fl](fibonacci.fl) | the same function as a loop, as recursion, and memoized |
| [data.fl](data.fl) | lists, tables, strings, and the functions the library omits |
| [collections.fl](collections.fl) | the collections module: map, filter, reduce, sort, zip, enumerate |
| [errors.fl](errors.fl) | try/catch with a type filter, throwing, rethrowing, `finally` |
| [files.fl](files.fl) | the fs and path modules: write a file, read it, clean up |
| [json.fl](json.fl) | the json module: a round trip, and a parse error caught |
| [url.fl](url.fl) | the url module: parse, edit, reassemble |
| [datetime.fl](datetime.fl) | the datetime module: UTC dates as tables |
| [toml.fl](toml.fl) | the toml module: a configuration file, read |
| [http.fl](http.fl) | the http module, behind an env var so it cannot hang ci |
| [terminal.fl](terminal.fl) | the terminal module: progress bar, spinner frames, a hyperlink |
| [envconfig.fl](envconfig.fl) | the env module: PORT/DEBUG/HOST with fallbacks |
| [argsdemo.fl](argsdemo.fl) | the args module: count, get, flags, and flag values |
| [cli.fl](cli.fl) | a `--name` flag with defaults, from the command line or the env |
| [chaining.fl](chaining.fl) | `?.` through missing config, lazy calls, and `??` fallbacks |
| [modules/](modules/) | a four-file program: import, export, shared globals, const |

## running them

from the repository root:

```sh
./flint examples/hello.fl
./flint examples/fizzbuzz.fl
./flint examples/as.fl
./flint examples/math.fl
./flint examples/closures.fl
./flint examples/fibonacci.fl
./flint examples/data.fl
./flint examples/collections.fl
./flint examples/errors.fl
./flint examples/files.fl
./flint examples/json.fl
./flint examples/url.fl
./flint examples/datetime.fl
./flint examples/toml.fl
./flint examples/terminal.fl
./flint examples/envconfig.fl
./flint examples/argsdemo.fl hello --out result.txt
./flint examples/cli.fl --name Ada
FLINT_HTTP_DEMO=1 ./flint examples/http.fl
./flint examples/chaining.fl
printf 'Ada\nsecond\n\nAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n21' | ./flint examples/input.fl
```

the module example has no such constraint: imports resolve against the
importing file, so it runs from anywhere.

```sh
./flint examples/modules/main.fl
```

[cli.fl](cli.fl) takes its name from the command line, the environment, or a
literal, in that order, and prints the defaults when it gets none of them:

```sh
./flint examples/cli.fl                    # hello, world!
NAME=Grace ./flint examples/cli.fl         # hello, Grace!
./flint examples/cli.fl --name Ada         # hello, Ada!
```

[http.fl](http.fl) is the only example that touches the network, so the request
sits behind an env var. without `FLINT_HTTP_DEMO` it makes no request at all:
it prints the url it would have fetched and exits, which is what keeps it safe
in ci. set `FLINT_HTTP_URL` as well to point it somewhere else.

see [../docs/modules.md](../docs/modules.md) for the path and cache rules.

## a note on the comments

they are not explaining the syntax. `print` is not a function call, `let` needs
no type, and none of that is worth a line. the comments are on the things that
are not guessable: that `0` is truthy, that ranges are half-open, that a name
is resolved when the line runs rather than when the file is read, and that
`str` and `print` are two different code paths.
