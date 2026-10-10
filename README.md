# flint

a small, fast scripting language and bytecode vm written in portable c11.

flint is deliberately small.

no llvm. no cmake. no configure script. no dependency jungle.

just c11, libc, libm, a bytecode vm, and enough language to get things done.

```sh
make release
./flint -e 'print(1 + 2)'
```

## features

* portable c11
* stack-based bytecode vm
* 64-bit nan-boxed values
* single-pass pratt parser
* mark-and-sweep garbage collector
* closures with captured locals
* lists, tables, strings and modules
* bytecode verifier
* computed-goto interpreter
* standard library with os, process and http
* human and json diagnostics
* `flint sync` for updating the library
* no runtime dependencies beyond libc and libm

the implementation is small enough to read.

that's intentional. large codebases have enough fans already.

## installation

to build from source:

```sh
git clone https://github.com/programmersd21/flint
cd flint
make release
```

the binary is `./flint`.

to install flint to your path:

```sh
make install
```

by default, this installs the binary to `~/.local/bin` and the standard library to `~/.flint/stdlib`.

for packaging, override the installation paths:

```sh
make install PREFIX=/usr/local LIBDIR=/usr/local/share/flint/lib
```

to uninstall:

```sh
make uninstall
```

flint requires no runtime dependencies beyond libc and libm. the installation consists of one binary and fifteen `.fl` files.

### standard library lookup

flint searches for its standard library in this order:

1. `$FLINT_STDLIB`, an explicit override.
2. `<executable>/lib`, beside the binary.
3. `~/.flint/stdlib`, the default installation directory.

the third location is necessary because a binary on `PATH` does not necessarily have a `lib/` directory beside it.

if you move the binary somewhere without an adjacent `lib/` directory, keep the standard library in `~/.flint/stdlib` or set `FLINT_STDLIB`.

## example

```flint
fn counter() {
    let n = 0

    fn next() {
        n += 1
        return n
    }

    return next
}

let c = counter()

print(c())
print(c())
```

output:

```text
1
2
```

### modules

modules are files, each with its own namespace.

`shapes.fl`:

```flint
let tax = 0.2

export fn taxed(amount) {
    return amount * (1 + tax)
}
```

`main.fl`:

```flint
import "shapes.fl"

print(shapes.taxed(10))
print(shapes.tax)
```

output:

```text
12
nil
```

`tax` is private to `shapes.fl`. only exported names are accessible through the module namespace.

two modules can define the same private name without exposing or overwriting each other's values.

## standard library

| module        | purpose                                |
| :------------ | :------------------------------------- |
| `math`        | math functions and constants           |
| `random`      | random numbers and shuffling           |
| `time`        | clocks and time utilities              |
| `fs`          | files and directories                  |
| `path`        | path manipulation                      |
| `collections` | collection helpers                     |
| `json`        | parsing and serialization              |
| `strings`     | repeat, reverse, padding, counting     |
| `os`          | information about the host system      |
| `process`     | spawning programs and capturing output |
| `http`        | http client requests                   |
| `args`        | script arguments and flags             |
| `encoding`    | hex, base64 and url encoding           |
| `ansi`        | terminal escape codes                  |
| `pretty_print`| multi-line value rendering             |
| `csv`         | comma-separated values, rows and objects |
| `toml`        | configuration files, read              |
| `url`         | split URLs into parts and build them back |
| `datetime`    | UTC calendar dates as tables           |
| `glob`        | shell-style filename matching          |
| `terminal`    | sizes, tty guessing, progress widgets  |
| `env`         | environment variables, parsed          |
| `log`         | leveled logging to stdout              |
| `test`        | checks, cases, and a gating finish     |
| `hash`        | djb2 values and hex, non-cryptographic |
| `debug`       | call-stack frames and value dumps      |
| `regex`       | Thompson NFA matching over bytes       |
| `compress`    | LZSS for small byte strings            |
| `signal`      | signal numbers, raise, send            |

the library is written in flint wherever possible.

## updating the standard library

no package manager. no registry. no twelve-layer dependency tree to print a number.

`flint sync` downloads the standard library from the repository and installs it into `~/.flint/stdlib`.

each file is compiled before installation. a failed download, truncated response or 404 leaves the existing files untouched.

use `--dry-run` to preview changes or `--ref` to select a release tag instead of `main`:

```sh
flint sync --dry-run
flint sync
flint sync --ref=v0.7.0
```

## virtual machine

```text
source
  │
  ▼
pratt parser
  │
  ▼
bytecode
  │
  ▼
verifier
  │
  ▼
stack vm
```

the parser emits bytecode directly.

values fit in 64 bits. closures capture locals. memory is managed by a mark-and-sweep garbage collector.

there is no giant intermediate representation sitting around because apparently compilers enjoy paperwork.

## performance

the default interpreter uses a portable c11 dispatch loop.

on compilers that support computed goto, build the alternative interpreter:

```sh
make flint-goto
```

benchmark both implementations:

```sh
make bench
make bench-goto
```

results are documented in [`bench/RESULTS.md`](bench/RESULTS.md).

measurements are preferred over benchmark folklore. the cpu has already suffered enough.

## diagnostics

```sh
flint --check program.fl
flint --error-format=human program.fl
flint --error-format=json program.fl
flint --dump-bytecode program.fl
```

diagnostics include source locations and stable error codes.

the verifier checks generated bytecode before execution, including opcodes, operands, indices and jump targets.

executing invalid bytecode and hoping for the best is not a runtime strategy.

## development

run the main checks:

```sh
make check
make stress
```

* `make check` performs a clean release build and runs the test suites.
* `make stress` runs with aggressive garbage collection under asan and ubsan.

other useful targets:

```sh
make debug
make test
make unit
make diagnostic-test
make bench
make lint
make fmt
make fmt-check
```

if `make stress` finds something, congratulations. the garbage collector has opinions.

## documentation

* [`docs/language.md`](docs/language.md) - language reference
* [`docs/syntax.md`](docs/syntax.md) - syntax and expressions
* [`docs/functions.md`](docs/functions.md) - functions and closures
* [`docs/data.md`](docs/data.md) - strings, lists and tables
* [`docs/modules.md`](docs/modules.md) - modules
* [`docs/library.md`](docs/library.md) - standard library
* [`docs/cli.md`](docs/cli.md) - commands, flags, exit codes
* [`docs/diagnostics.md`](docs/diagnostics.md) - diagnostics
* [`docs/internals.md`](docs/internals.md) - vm internals
* [`ARCHITECTURE.md`](ARCHITECTURE.md) - architecture
* [`SPEC.md`](SPEC.md) - language specification
* [`examples/`](examples/) - examples

## contributing

keep changes focused.

if you find a bug, add a test.

before opening a pull request, run:

```sh
make check
make stress
```

see [`CONTRIBUTING.md`](CONTRIBUTING.md).

if a patch adds a dependency, explain why c could not do the job.

## banner

![flint banner](assets/banner.png)

## license

flint is licensed under the mit license. see [`LICENSE`](LICENSE).
