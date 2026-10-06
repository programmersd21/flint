# limits

things flint does not do. these are language limits, not hidden switches.
each one was checked against the source; if a limit below stops being true,
the test suite should have a test proving it and this file should say so.

## numbers and types

every number is an IEEE 754 double. integers above 2^53 lose precision. there
are no integer arithmetic rules waiting to be discovered later.

there are no static types, generics, classes, or user-defined types. `as`
checks one value at run time; it does not convert it or constrain later calls.

## control and data

ranges are syntax for `for`, not values. they cannot be stored, passed, or
nested. `a..b` is half-open, and `a..b..s` steps it; a zero step is an error.

table keys are strings. a missing field reads as `nil`, so a typo looks like
an unset field. tables iterate in insertion order.

strings are byte sequences. indexing, iteration, length, and the string
helpers count bytes. utf-8 is left alone, which means a byte index can land
inside a multibyte character.

## errors

`try`/`catch`/`throw` recover from runtime errors, and an uncaught error
prints a trace and exits with status 70. errors are `{type, message}` tables.
there is no `finally`, no catch filters, and the error carries no stack-trace
field of its own.

## modules

every module has its own environment. `export` decides which of a module's
names an importer can reach; unexported names are private. a module runs
once per process and is cached by resolved path. an import that fails binds
nothing, and the failure is remembered so a later import reports it instead
of retrying.

## runtime edges

the repl compiles one line at a time. it cannot keep an unfinished block open
for the next line. it also has no line editing or history; the prompt is not
secretly an editor.

string growth copies bytes. repeated concatenation in a loop can therefore
copy the growing prefix on every pass. build a list of pieces and `join` it
when the string helpers fit the job.

the call stack has 256 frames. tail calls do not reuse a frame. the value
stack is also fixed-size; deeply nested expressions and large local stacks
can exhaust it.

the vm uses nan-boxed values and requires a 64-bit host with pointers that
fit in 48 bits. the build is c11, but the value representation is not portable
to every c11 target.

`process.run` starts a program from an argument vector and never invokes a
shell. file and environment functions expose the process's ordinary
permissions; flint adds no sandbox.

## toolchain

the `flint` executable runs scripts, `-e` code, stdin and the repl, plus
`flint sync`, `flint test`, `flint fmt` and the diagnostic flags. there are
no `check`, `lint`, `build`, `debug`, `profile`, `doc`, `package`,
`install` or `lsp` subcommands.

there is no package manager, no registry and no lockfile. `flint.toml` is
not a source format. the only ways to get code into a script are
`import "some/file.fl"`, an `import` of a standard-library module by name,
and `flint sync` to refresh the installed library.

there are no threads, channels or locks. the `http` module is a blocking
client: `http://` uses native sockets, and `https://` shells out to
`curl(1)`, so tls, its trust store and its certificate verification are
curl's rather than flint's. flint passes no `--insecure`. there is no
cryptography beyond the non-cryptographic `random` module, which says so
itself.

there is no c extension point: no public header, no `native.load`, no
ownership contract. `vm.h` and friends are internals and change between
commits.

## untrusted input

flint has no permission system and no sandbox. a script that can run can
read and write every file its user can. do not run flint programs you have
not read.

there is no bytecode file format and no loader, so there is no untrusted
bytecode boundary; all bytecode is produced in-process by the compiler and
checked by the in-process verifier before it runs. anything that arrives as
a `.fl` file is source.
