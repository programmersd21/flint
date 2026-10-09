# the flint command line

one binary. it runs a script, reads one from stdin, starts a repl, runs the
test suite, and updates the installed library.

```sh
flint                       # repl
flint program.fl            # run a file
flint program.fl -v --flag  # everything after the path belongs to the script
flint -                     # read the script from stdin
flint -e 'print(1 + 2)'     # run code and exit
```

anything after the script path is passed to the script and read back with
`args()`. that rule is why `flint program.fl --help` prints the script's
help, not flint's. use `--` to end flint's own options explicitly:
`flint t.fl -- --error-format is mine`.

## exit codes

stable across commands:

| code | meaning |
|---|---|
| 0 | success |
| 64 | usage error: a bad flag or a missing argument |
| 65 | compile or input error, including a missing test suite |
| 66 | missing input file or module |
| 69 | package or sync failure |
| 70 | runtime error |
| 74 | io error |

## test

```sh
flint test
flint test --filter collections
```

runs every `*_test.fl` under `tests/`, sorted, each in its own VM so one
test cannot leak state into the next. a failing test does not stop the
others; the summary reports how many ran and how many failed, and the exit
code is 70 when any failed.

there is no assertion framework. a test uses `assert()`, which raises an
ordinary runtime error, so a test file is an ordinary script:

```flint
assert(1 + 1 == 2)
assert(parse("a b") == ["a", "b"], "split on a single space")
```

the name of a test file is its description, so it should read as one:
`collections_sort_test.fl` rather than `test3.fl`.

## fmt

```sh
flint fmt program.fl
flint fmt [--check] a.fl b.fl
```

rewrites each file in canonical layout: 4-space indentation, no trailing
whitespace, a single trailing newline. the contents of multiline strings
are left alone. `--check` lists the files that would change and exits 1
when any would, rewriting nothing: the shape CI wants.

## native

```sh
flint native <library.so> <module> [script.fl]
```

loads a compiled module through the public C ABI and makes its functions
callable for the run. posix only; a windows build refuses with a message.
the loaded library is never unloaded -- see
[native-abi.md](native-abi.md).

| exit | meaning |
|---|---|
| 0 | the module loaded and the script succeeded |
| 64 | wrong arguments |
| 65 | the library could not be loaded: no such file, no entry point, an unsupported ABI version, or initialisation failure |

## sync

```sh
flint sync
```

updates the installed standard library in `~/.flint/stdlib` from the
repository. `sync` only means the subcommand when no file of that name
exists, so a script called `sync` still runs.

## pkg

```sh
flint pkg install
flint pkg add <path|url>
flint pkg update [name]
flint pkg list
```

resolves `flint.toml` into `flint.lock` and installs each dependency
under `flint_modules/`. path dependencies copy live source; git
dependencies clone once into `~/.flint/git` and pin the installed
commit. there is no registry. see [packages.md](packages.md).

## diagnostics

```sh
flint --error-format=human program.fl   # excerpt, caret and label
flint --error-format=short program.fl   # one location line
flint --error-format=json program.fl    # one object per diagnostic
flint --color=always program.fl         # color even when piped
flint --explain E0600                    # what a diagnostic code means
flint --fix program.fl                   # apply machine-applicable fixes
flint --warnings=none program.fl         # errors only
flint --quiet                            # no repl prompt
flint --stats -e 'print(1)'              # size summary on stderr, then runs
```

`--stats` prints the function count, the bytecode bytes and the constant
count to stderr, then runs the program as normal.

## repl history

the repl remembers lines across sessions. `:history` lists the numbered
entries, and `!N` re-runs entry `N`:

```sh
$ printf '1+2\n:history\n' | flint --quiet
3
  1  1+2
```

## repl `:clear` and `:load`

`:clear` wipes the screen. it writes nothing when the repl's output is not a
terminal, so a piped session is not polluted by escape codes.

`:load path` runs a file in the session you are already in, rather than in a
fresh one. it is not a script run: it is the current repl state before and
after, so what the file declares stays declared and what it defines is
available to the next line.

```sh
$ cat /tmp/t.fl
fn triple(n) { return n * 3 }
let greeting = "from file"

$ printf 'let x=5\n:load /tmp/t.fl\nx+1\ntriple(x)\n' | flint --quiet
6
15
```

`triple` and `greeting` came from the file, and `x` was already bound before
the file ran, which is the whole difference between this and
`flint /tmp/t.fl`. a file that does not parse is reported like any script and
leaves the session alone, so the lines after it still run. a path that cannot
be read reports `cannot read '<path>'` and the session continues; the repl
does not exit.

`:load` takes the rest of the line as the path. bare `:load` is not a command
and reports `unknown command. try :help`.

## version

```sh
flint --version
flint --version --verbose
```

`--version` reports the release. `--verbose` adds the version numbers that
are otherwise independent: language, runtime, bytecode format, native ABI,
package format and lockfile. a script that has to work across a toolchain
change reads them from there rather than parsing the release string.
