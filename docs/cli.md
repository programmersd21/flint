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

## sync

```sh
flint sync
```

updates the installed standard library in `~/.flint/stdlib` from the
repository. `sync` only means the subcommand when no file of that name
exists, so a script called `sync` still runs.

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

## version

```sh
flint --version
flint --version --verbose
```

`--version` reports the release. `--verbose` adds the version numbers that
are otherwise independent: language, runtime, bytecode format, native ABI,
package format and lockfile. a script that has to work across a toolchain
change reads them from there rather than parsing the release string.
