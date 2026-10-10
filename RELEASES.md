# releases

## v0.14.0

Package and dependency handling get a reliability pass.

### Package imports

- Resolve package-named single-file modules such as
  `flint_modules/levenshtein/levenshtein.fl`, alongside the existing
  `main.fl` package entry point and flat-module layout.
- Document the supported package layouts and import lookup behavior.

### Package installation

- Clone Git dependencies into a staging directory before publishing a
  mirror, so interrupted or incomplete clones do not become the active
  cache.
- Serialize operations on a shared Git mirror and avoid following
  symlinks during cache cleanup.
- Recover from damaged or incomplete mirrors on a subsequent install.
- Keep dependency arrays valid when expansion fails, clean up partial
  dependency state, reject overlong paths, and bound cyclic dependency
  resolution with a clear error.

### Release and compatibility notes

- Update the Rust `flintlang` and `flintlang-sys` crate metadata and
  examples to version `0.14.0`.
- Report language, runtime, package format, bytecode, and lockfile
  versions as `0.14.0`. The native ABI remains version `1`; it is
  versioned independently.
- Identify HTTP requests with the `flint/0.14.0` User-Agent by default.
- Keep the standalone C runtime and the Rust-embedded C source in sync.

Release gate: this changelog is a draft until PRs #10, #11, and #13 are
merged and the final release checks pass. Do not publish a `v0.14.0`
tag or artifacts before then.

## v0.13.2

native modules load through `import` -- no subcommand, no loader
ceremony:

```flint
import mymod
print(mymod.add(2, 3))
```

`import mymod` finds `mymod.so` (`.dylib`/`.dll` elsewhere) beside the
script, in `flint_modules/`, or in the standard library. source always
wins: a `.fl` shadows a stale shared object, in either directory.
explicit paths (`import "./x.so"`, absolute paths) load directly.
registered functions arrive as a table under the imported name, under
the same cache, cycle detection, and failure discipline as source
imports. the old `flint native` loader is gone -- running it explains
the removal instead of failing cryptically -- while `native-unload`
stays, since unload requests have no import equivalent. see
[docs/native-abi.md](docs/native-abi.md).

also in this release: [`flintlang`](https://crates.io/crates/flintlang)
and [`flintlang-sys`](https://crates.io/crates/flintlang-sys) 0.13.2 on
crates.io (the `flint` names were taken); styled `-h`/`--help` for
`fmt`, `test`, and `native-unload`; and a fix where mid-execution
native registration read the running script's stack slots instead of
the values just pushed.

gates: 157 language tests, 33 native ABI checks, and the full C and
Rust suites green on Linux, macOS, and Windows. artifacts below hold
the binary, standard library, and docs per platform:
`linux-x86_64`, `macos-arm64`, `windows-x86_64`.

## v0.13.1

the release the CI probes wrote. 0.13.0 shipped Linux-green with two
non-gating portability probes, and the probes spent the week finding
real defects: a macOS stdlib lookup that never existed, Windows-only
compile failures, test scripts with GNU assumptions, and tests that
asserted one platform's child behavior as universal. every one below
was found by a probe job, fixed, and re-proven by the same job.

alongside the fixes, the seven deferred standard-library modules land:
log, test, hash, debug, regex, compress, and signal. 0.13.0 deferred
regex and compress as easy to write badly and hard to notice; they
shipped here because each carries the proof the worry demanded --
linear-time matching with no backtracking, and exact round-trips over
all 256 byte values.

### the probes and what they found

- macOS built clean and failed every import: without `/proc/self/exe`
  there was no executable-relative stdlib lookup. `_NSGetExecutablePath`
  fills the branch, and the suite runs 150/150 on Apple silicon.
- Windows mingw64 failed on loader-only symbols (`-Wunused` under
  `-Werror`): the vtable implementations, installer, trampoline,
  registry, and handle decoder are fenced behind `_WIN32` with the
  loader, and the public entry points degrade through their existing
  NULL-vtable fallbacks.
- `pkg_test.sh` used GNU `sed -i`, invalid on BSD sed: in-place edits
  go through a temp file plus `mv` now.
- `process.fl` asserted GNU `ls` exit codes and a non-symlinked `/tmp`:
  the failing child is `sh` exiting 3, and the cwd is `/`.
- macOS `ld` resolves every symbol at link time, so test modules link
  `-undefined dynamic_lookup` and resolve `fl_*` against the loading
  host, as on Linux.
- `--fix`'s atomic replace is POSIX-only, so Windows rewrites in place
  and documents the failure mode.

- `clock_ms()` was CPU time, which sits at zero on a fast machine and
  failed its own test on Windows: it is monotonic wall time now, as the
  docs already promised.
- `glob.fl` hardcoded `/tmp`, absent on Windows: temp dirs come from
  `os.tmpdir()` instead.
- `process.fl` and `signal.fl` assert POSIX-only behavior, so the
  runner prefers `.expected.windows` (and `.status.windows`) transcripts
  beside the shared ones -- one file per contract, not a fork.
- the rust cdylibs called `fl_*` host functions, which Linux leaves for
  the loader: on Darwin they link one `-C link-arg` per word with
  `-undefined dynamic_lookup`, and load as `.dylib`.
- `ratatui` lives on the registry, so the tui example builds online
  (version still pinned by its lockfile); zero-dependency crates stay
  `--offline` and hermetic.
- `-rdynamic` is ELF-only: mingw rejects it, and needs nothing in its
  place because the Windows loader refuses up front.

all three probes are green in CI -- 157 of 157 language tests on
Linux, macOS, and Windows alike -- so `docs/platforms.md` now marks
all three supported, and this release ships a tarball for each:
linux-x86_64, macos-arm64, windows-x86_64.

### the seven modules

`import log` levels messages to stdout with per-logger levels and
optional timestamps. `import test` runs checks and named cases, catches
throws into failures, and `finish()` exits nonzero -- a suite file as a
gate. `import hash` is djb2 plus hex, exact in doubles, and says
non-cryptographic on the tin. `import debug` exposes the live call
stack through one small read-only `__debug_frames` native, formats
traces like the runtime, and dumps values. `import regex` is a Thompson
NFA -- full match, leftmost-longest search, no backtracking, invalid
patterns return nil. `import compress` is LZSS with a length-prefixed
framing, exact round-trips, nil on corruption, documented for small
payloads. `import signal` maps seven portable signal numbers and offers
raise, kill-based send (POSIX-only), and pid -- with no handlers, ever,
because a collector and async C handlers do not mix.

### version and gates

version metadata moves to 0.13.1. the gates hold: 157 language tests,
the sanitizer and computed-goto suites, verifier, formatter, package,
runner, native ABI, and Rust workspace checks all green on Linux --
and the full language suite passes on macOS and Windows too, in CI,
on the same commit this tag points at.

## v0.13.0

the release that finishes what 0.12.0 deferred and makes embedding a
supported claim. `match` binds payloads and matches literals, the
lockfile carries content hashes, native modules have an explicit
lifecycle, and a host program -- C or Rust -- can create an engine, run
source, and free it, all tested.

### match binds payloads and matches literals

`Colour.Blue(n)` works now. 0.12.0 refused it at compile time because the
subject's slot was destroyed by the arm bookkeeping: the subject value
was stored and popped instead of being claimed where it sat, so every
read after the first saw the wrong slot. the fix claims the slot the
value already occupies -- the same discipline `let` uses -- pops each
arm's test boolean on both paths, and releases the subject at the exit,
so a match is stack-neutral.

```flint
match Colour.Blue(7) {
    Colour.Blue(n) { print(n) }   # 7
    _ { print("other") }
}

match count {
    1 { print("one") }
    -3 { print("negative three") }
    "hi" { print("greeting") }
    _ { print("other") }
}
```

literal patterns take numbers (including negatives), strings, `true`,
`false`, and `nil`, compared with the existing equality. a bare name
still binds the whole subject, and binding a payload of a variant
declared without one is a compile-time error. exhaustiveness still
applies only where it can be proved: a known-enum subject needs every
variant or a wildcard, anything else needs a wildcard.

### lockfile content hashes

a lockfile that pins a commit is reproducible against movement of the
branch, not against tampering with the object. every installed tree is
now hashed -- SHA-256 over sorted relative paths and file bytes, with
dotfiles, symlinks, and metadata excluded so a checkout hashes
identically everywhere -- and the digest is recorded beside the pin as
`content`. reinstalling an unchanged pin verifies the tree in place
without touching the network (`verified shout 0.2.0`); a fresh
materialization whose bytes differ from the recorded hash stops the
install, keeps the old lock, and removes the suspect tree. path
dependencies are live source, so their hash is recorded, not enforced.
locks written before content hashes still install; the next install
records the hashes. the SHA-256 implementation is dependency-free and
covered by FIPS vectors.

### native modules: an explicit lifecycle

`flint native-unload LIB.so MOD` asks a loaded module to shut down and
unload. the request quiesces the module -- running calls finish, new
calls are refused -- then checks every route to native code in the order
that matters: active calls, retained handles, registered functions. the
library closes only when nothing references it; otherwise the answer is
busy and names what holds it:

```
busy: module 'mymod' is busy: 1 retained handle(s) outstanding -- release them and ask again
```

busy is the structured answer, not a failure, so the exit status stays 0.
in practice the answer is always busy today -- registration puts the
module's functions into the VM's globals for as long as the VM lives --
and the documentation says so rather than letting you discover it.
nothing is ever force-closed. failed loads now close what they opened:
a library whose entry point is missing or whose init fails is `dlclose`d
before the error is reported.

### embedding: a C engine API and a safe Rust crate

`include/flint.h` grows three functions, and adding functions does not
change the ABI version:

```c
FlEngine *fl_engine_new(void);
void fl_engine_free(FlEngine *engine);
int fl_engine_run(FlEngine *engine, const char *source, const char *name);
```

run returns the CLI's exit codes -- 0, 64, 65, 70 -- so a host and a
script agree on what happened. diagnostics go to stderr in the CLI's
rendering. `make libflint.a` builds the static runtime hosts link
against. values are deliberately not exchanged across this API: run
source, read the code. a small honest API beats a wide speculative one.

`rust/flint` is the safe wrapper over it: an `Engine` you create and
drop, a `run` that takes `&str`, and an `Error` enum mirroring the exit
codes. ordinary use needs no `unsafe`. the crate is `!Send` and `!Sync`
by construction, and a process-wide lock serializes every engine
operation, because the compiler keeps process-global scanner state --
two threads must not touch flint at once on any engines. eleven
integration tests run against the real runtime, including parallel
engines proving the lock holds, plus a runnable example (`cargo run
--example embed`).

threading, in one place: flint is single-threaded. one thread for
flint, period; a multithreaded host puts one mutex around all engine
use. this is stated in `flint.h`, in the crate docs, and in the tests.

### csv, toml, url, datetime

`import csv` parses RFC-4180-tolerant text into rows, reads objects
against a header row, and stringifies back with minimal quoting.
everything is strings, because csv has no numbers. an unterminated
quote returns nil.

`import toml` reads the configuration files flint itself reads: the
grammar the package manager parses -- sections, key/value lines,
comments, inline tables -- plus numbers, booleans, and single-line
arrays. a duplicate key or section is malformed rather than last-wins.

`import url` splits a URL into scheme, user, host, port, path, query
and fragment, and builds one back. a missing piece reads as `""`, and
structural nonsense (a non-digit port) returns nil.

`import datetime` does calendar dates in UTC and nothing else: dates as
tables, epoch seconds both ways, formatting. local time is deliberately
absent -- a formatter that depends on the machine's timezone is right
on one machine and wrong on the next.

### portability probes

CI keeps its Linux gates and gains two non-gating probes both marked
`continue-on-error`: a macOS job (release, language, unit) and a
Windows msys2 job (release build). neither platform is claimed as
supported until its probe goes green and stays green; their job is to
show where the POSIX assumptions break, in CI, instead of in a user's
terminal.

### still deferred, and why

a package registry: hosting is not a language problem. a full version
solver: a resolver that picks the first conflict rather than the right
one makes a lockfile untrustworthy. value exchange and host callbacks
on the engine API: each needs a lifetime story the engine side does not
yet have, and an API that hands out dangling values would be worse than
none. Windows and macOS support: probed, not claimed. `regex` and
`compress` were deferred here as easy to write badly and hard to
notice; both shipped in 0.13.1 with the proofs attached -- linear-time
matching, exact round-trips.

### version and gates

version metadata moves to 0.13.0. `make quick` and `make validate`
remain the two gates; the counts move with the release: 150 language
tests, 31 native ABI checks, 40 package checks, 5 SHA-256 checks, 11
Rust integration tests plus a doctest. `cargo fmt --check` and `cargo
test` on the workspace join CI.

## v0.12.0

the release that adds the first user-defined kind of value. structs are
named shapes whose construction is checked, so a misspelled field is a
message where the value was built rather than a nil three functions later.
alongside it, a failed module import has a contract and a test, and the
roadmap says plainly what is not in this release and why.

### structs

`struct Point { x, y }` declares a shape and binds `Point` to a
constructor:

```flint
struct Point {
    x,
    y,
}

let p = Point({x: 1, y: 2})
print(p)          # Point{x: 1, y: 2}
p.x = 10
print(type(p))    # Point
```

construction checks the fields in both directions and names what is wrong:
`Point({x: 1})` says which field is missing, `Point({x: 1, y: 2, z: 3})`
says `z` is not a field. both errors land on the line that built the value,
which is the whole reason to declare a shape.

a struct value is a table with a name. field access, assignment, `in`,
iteration, `keys`, indexing and printing were already table code, so the
feature needed no new opcode for any of them -- which is why it fits in one
object type and one instruction, and why `type()` says `Point` rather than
`table`.

**the annotations are descriptive.** `x: number` documents and checks
nothing. flint has one numeric type, no inference and no generics; the only
question an annotation can honestly answer today is which one you meant,
and that is what it answers. making it enforce anything would be a type
system arriving one keyword at a time, and enforcing it at runtime would
break programs that work today.

there are no methods, no inheritance, no enums and no pattern matching. each
is named in `docs/roadmap-0.12.md` with the reason it was deferred rather
than shipped half-built.

### modules: a failed import commits nothing

a module that fails halfway through initialization discards its
environment and never builds its export table, so nothing it defined
reaches the importer. that was already true; `docs/internals.md` had
claimed a rollback, which is a different and stronger claim than what
happens -- nothing is undone, state is never committed. the behaviour now
has a test that imports a module which defines a const, mutates a global
and only then throws, then defines every one of those names itself. if any
of it survived, one of them would report a dead module's const as already
defined.

the boundary is stated rather than papered over: external side effects --
files written, processes started, requests sent -- are not rolled back.

### native modules, rust and ratatui

`include/flint.h` is a versioned C ABI: opaque handles, no `Value`, no
`Obj`, no VM. a module is refused before its entry point runs when the
ABI version does not match. handles are not pointers into the VM, so a
value that must outlive its call says so with `fl_retain`. errors go
through the existing model -- `fl_raise` produces an error flint code
catches with the same `catch e as` as anything else.

loading is `dlopen` behind a documented platform seam, and a loaded
library is never unloaded: its functions stay callable and a value it
produced can outlive the call that made it, so unloading would be a
use-after-free waiting for the collector.

`rust/flint-sys` is a safe wrapper over that same header -- a wrapper,
not a second interface -- and the examples show the two things a C
extension cannot: a contained panic (`explode()` panics on purpose and
flint catches an ordinary error) and a retained handle. the ratatui
example is the shape the whole thing was aiming at: a flint script asks
for a frame, ratatui draws one, and what comes back across the ABI is
text.

### transitive dependencies

a package could declare dependencies and have them silently ignored.
`mid` installed cleanly and then could not `import "base`: a green
install and a broken program, which is the worst shape a package manager
can fail in. a dependency's own `[dependencies]` are now followed,
recursively, with a nested path resolved relative to the package that
declares it.

three bugs, and the third was the real one. a nested path needed joining
and normalizing, or `../base` meant two different directories; the
expansion reallocs, so reading the old array was a use-after-free that
surfaced as `git: repository '<garbage>' does not exist`; and an
installed package carries its own `flint.toml` -- it was copied there --
which the resolver read as a project root, and then looked for
`flint_modules/mid/flint_modules/base`. that one is fixed in the
resolver: anything below a `flint_modules/` directory is inside a
project, not one.

two packages reaching the same one install it once. two *versions* of one
package is reported rather than resolved by picking, because a lockfile
that silently chose is not trustworthy.

### still deferred

enums and pattern matching, deliberately: a matcher over open-ended
dynamic values cannot prove exhaustiveness, and one that cannot prove it
should not claim to. integrity checksums and native-package platform
metadata for the package manager, and a registry.

### version and gates

version metadata moved to 0.12.0. `make quick` and `make validate` remain
the two gates, and `make native-test` is a third: it builds real shared
objects against the header and loads them.

## v0.11.0

the release that makes flint correct: a verifier that means what it says,
a test runner that checks what it claims, and one rule for spelling a
number. no new keywords, no new subsystems. the language grows by nothing
at all this time -- 0.11.0 is about the code that was already there being
right.

### the verifier checks what it claims

the upvalue bounds check read `count > 0 && operand >= (uint8_t)count`,
which is wrong at both ends. a function capturing nothing skipped the
check entirely, so any operand reached the VM with nothing behind it.
and 256 upvalues narrowed to a byte is zero, so a function at the maximum
rejected *every* access including all 256 valid ones. the local-slot check
had the same shape of bug. "no function" is now distinct from "a function
with nothing to capture": a bare chunk has no count to check against, a
real function with none has an answer, and the answer is that no slot is
readable.

regression coverage to match: zero, one, 255 and 256 upvalues and locals,
first and last valid slot, first invalid slot, GET and SET for each, every
cast tag above the last enum member, jumps landing inside operands, and a
truncation sweep over every multi-byte opcode. the new tests were confirmed
to fail against the old condition before they were confirmed to pass
against the new one.

### the runner checks the exit status

`run_tests.sh` compared stdout and discarded the status with `|| true`, so
a script that printed exactly the right lines and then exited non-zero
passed. 38 existing tests legitimately exit nonzero and had been relying
on that tolerance; each now declares its status in a `name.status` file
beside the test, absent meaning 0. `runner_test.sh` covers the runner's own
contract from the other direction: right output with the wrong status
fails, a declared nonzero status passes, a malformed status file is
reported rather than defaulted.

### one rule for spelling a double

`print(1/3)` gave 16 digits, `print([1/3])` gave 17, and `str(1/3)` gave 17
while losing digits on values needing 16. three copies of one rule, and
the top-level one was the only one that re-read its output to check the
digits round-tripped. `fl_double_to_text()` is the one rule now: the digit
loop for small integers, then 15/16/17 widening until the text reads back
identically. the measured fast path stays -- 19ms per 200k `str(i)` calls
against 45ms through a stdio stream -- as an optimization with the same
answer, not a second rule.

along the way `str()` learned containers and tables learned to print:
`str([1, 2])` is `[1, 2]` and `print({a: 1})` is `{a: 1}`, because the
printer takes a FILE* now instead of writing to stdout, and `str` is the
same code with a different stream.

### two codes for two common mistakes

redeclaring a name and leaving an import unread both fell into E0100, the
"no more specific code" bucket, so `--explain` had nothing useful to say
about either. E0201 (declared twice, including redefining a constant) and
E0203 (import never read) join E0202, which the runtime already used for
the same question from the other direction. matching on message text is
how every code here is derived, and these two are produced in the same
file as the code that assigns them.

### the gates have names for what they cost

`make quick` is the working loop -- build, all suites, fmt-check, lint --
in seconds. `make validate` is the release gate: everything, ordered so
the cheap failures come first, with the sanitizer build and computed-goto
where they belong. `make check` is an alias for validate rather than a
second, weaker gate. `make help` describes all of it truthfully now.

## v0.10.0

the release that makes flint comfortable: safer expressions around missing
data, a real package manager, and a plainer table api. the language grows
by three small things, and nothing else grows at all -- no new keywords,
no new subsystems, no jvm hiding behind the bytecode.

### ?. reads through missing data

`a?.b` is `a.b` when `a` is not nil, and nil when it is. the whole postfix
chain is conditioned, not just the first link, and a call's arguments are
only evaluated when the call happens:

```flint
let config = {server: {host: "example.com"}}

print(config?.server?.host)      # example.com
print(config?.missing?.deep)     # nil, no error
print(config?.server?.host ?? "localhost")
```

assignment through `?.` does not compile. a statement that sometimes does
nothing is a typo waiting for a production incident.

### catch filters

`catch e as TypeError` keeps only errors of that shape; anything else
propagates outward to whoever handles it. the `type` field is data, so a
package can filter on its own categories:

```flint
try {
    risky()
} catch e as ValueError {
    print("bad value: " + e.message)
} catch e as NetworkError {
    print("try again later")
}
```

one clause per try -- two `catch` clauses do not chain, they nest, because
a filter that silently falls through to a second clause is a control-flow
rule nobody remembers. a bare string thrown the old way never matches a
filter; it propagates, which is what "catch only this" means.

### list destructuring

`let [first, second] = values`, the twin of the table form that already
existed, with the same rules: flat names, every binding an ordinary
declaration, `const` threading through. a short source list fails with the
index error any index would hit, because a list knows its length and a
table does not.

### packages

`flint pkg` and `flint.toml`. dependencies are declared once and installed
into `flint_modules/`, where imports find them:

```flint
[package]
name = "myapp"
version = "0.1.0"

[dependencies]
utils = { path = "../utils", version = "^1.0.0" }
shout = { git = "https://github.com/someone/shout.git" }
```

`pkg add` records and installs, `pkg install` re-resolves, `pkg update`
moves the pins, `pkg list` shows what is there. a path dependency copies
live source; a git dependency clones once into `~/.flint/git` and installs
the commit `flint.lock` pins, so a second install needs no network and
produces identical bytes. `pkg update` is the only thing that moves a
pin. there is no registry, and `pkg add` says so rather than pretending:
what exists is local dependencies, done properly.

module imports also fixed along the way: a module's relative imports
resolve against its own directory, which is what the documentation had
always promised.

### the table api finished

`values(t)` and `items(t)` join `keys(t)`, all three insertion ordered and
all three returning fresh lists. `items()` gives `[key, value]` pairs that
destructuring reads directly:

```flint
for entry in items(config) {
    let [key, value] = entry
    print(key + "=" + str(value))
}
```

### tooling and robustness

`flint fmt` gained a contract and a test: `fmt(fmt(source)) == fmt(source)`,
checked over every source in the repository plus a set of awkward shapes,
so canonical style cannot drift. the verifier gained a fuzz sweep over
random and structured malformed bytecode, and the runtime grew two opcodes
with the same discipline as the rest: `OP_RETHROW` reports the fault's
location rather than the landing pad's, and `OP_CHECK_CATCH` is the filter
test.

the repl grew `:clear` and `:load path`, and the cli grew color where a
terminal will show it -- banner, help sections -- and stays plain when
piped, because a test comparing output must never see escape codes it did
not ask for.

## v0.9.0

the release that finishes what 0.8.0 left open and sharpens the tools
around the language. `finally` closes the error-handling story: a block
that runs however its `try` completes, with the original error -- and its
location -- surviving the trip. two operators answer questions scripts
kept asking by hand: `??=` for defaulting a variable only when it is nil,
and `in` for membership in strings, lists and tables. around them, a
formatter, program statistics, repl history, and three stdlib modules.

### finally

`try`/`catch`/`throw` gain the missing third clause, in both shapes:

```flint
try {
    let f = open(path)
    work(f)
} finally {
    close(f)
}

try {
    risky()
} catch e {
    log(e.message)
} finally {
    cleanup()
}
```

the finally body runs on success, on caught error, and on propagating
error alike. an error thrown by the `catch` body runs the finally before
propagating outward. a rethrow is not a new error: it reports the fault's
line, not the landing pad's, through a dedicated `OP_RETHROW` opcode that
carries the saved fault address. `return` inside a finally body replaces a
propagating error the way any block exit does; there is no special casing.

### ??= and in

`a ??= b` assigns `b` only when `a` is nil. `false`, `0` and `""` are all
values that stay, and the right side evaluates only when needed:

```flint
let port = nil
port ??= 8080      # 8080 now; a second ??= would leave it
```

`item in collection` asks membership: substring for strings, element for
lists (compared with `==`), key for tables. anything else is a runtime
error rather than false, because membership in a number is a mistake:

```flint
print("ell" in "hello")   # true
print(2 in [1, 2, 3])     # true
print("k" in {k: 1})      # true
```

### tooling

`flint fmt` rewrites sources in canonical layout -- four-space
indentation, no trailing whitespace, one trailing newline -- and never
touches bytes inside a multiline string. `fmt --check` lists what would
change and exits 1, rewriting nothing. `flint --stats` prints functions,
bytecode bytes and constants to stderr, then runs as normal: `--profile`
asks what the run did, `--stats` asks what the program is. the repl keeps
history in `~/.flint_history`, lists it with `:history`, and re-runs an
entry with `!N`.

### stdlib and builtins

three modules: `env` reads variables as text, numbers, flags and path
lists with fallbacks, so callers never branch on nil; `glob` matches `*`,
`?` and `[...]` with no dependencies; `terminal` brings width, hyperlinks,
a progress bar and a spinner. `collections` gains `sort`, `map`, `filter`,
`reduce`, `reversed`, `enumerate`, `zip` and `range_list`; `strings` and
`encoding` arrive alongside. builtins gain `assert`, variadic `min` and
`max`, `char_at` and `find`, `ord` and `chr`. `flint test` runs every
`*_test.fl` under `tests/` with `--filter`, in isolated VMs.

## v0.8.0

the release that stops treating errors as a stop sign. runtime failures are
values now, so a script can recover, and a module that throws can be caught
by the script that imported it. the language picks up three features the
stdlib and the tests kept needing: anonymous function literals, stepped
ranges, and trailing commas in multi-line literals.

### recoverable errors

`try`, `catch` and `throw` are in the language. a thrown value is bound by
the catch clause; a runtime error arrives as a table with `type` and
`message`, the same shape everywhere a script can fail:

```flint
try {
    let xs = [1, 2]
    print(xs[10])
} catch e {
    print(e.type)      # ValueError
    print(e.message)   # list index 10 out of bounds (len 2).
}
```

there is no `finally` and no typed catch. bubbling up unchanged is the
default path for code that does not care, and an uncaught error still
prints one message plus one line per frame and exits 70.

constructors name the categories: `Error`, `TypeError`, `ValueError`,
`IOError`, `NetworkError`, `TimeoutError`, `ProcessError`, `ModuleError`,
`PackageError`. they are ordinary functions with ordinary arity checks.

runtime errors are categorized too, so a script can branch on the failure
instead of matching on the message text:

| failure | type |
|---|---|
| index out of bounds, empty `pop()`, bad `num()` conversion | `ValueError` |
| wrong argument type, `as` assertion, calling a non-function | `TypeError` |
| file read/write/remove | `IOError` |
| import resolution, cycles, a module that failed to load | `ModuleError` |
| http transport, bad URL scheme | `NetworkError` |
| fork, exec, `process.run` | `ProcessError` |
| malformed json | `ParseError` |
| undefined variable, out of memory, stack overflow | `Error` |

the classification is one function over the message prefix
(`error_type_for` in `src/runtime/vm.c`) rather than a category argument on
every call site. one table to read, one place to extend. the trade is that
a native with a novel message stays `Error` until its prefix is added,
which is visible rather than silent: the type is data.

errors unwind frames, locals, open upvalues and the operand stack, so a
module whose top level throws rolls back cleanly: the importer's bindings
are untouched, the failed module is remembered as failed, and an import
inside a `try` is not also reported as an unused import.

### stepped ranges

`a..b` already walked a range by one. it now takes a step: `a..b..s`.

```flint
for n in 0..10..2 { print(n) }    # 0, 2, 4, 6, 8
for n in 10..0..-3 { print(n) }   # 10, 7, 4, 1
```

a step of zero raises before the loop runs, rather than hanging inside it.
an empty range is fine and runs zero iterations. the step can be a
negative literal, a variable, or any expression evaluating to a number,
and the comparison follows the step's sign each iteration.

### anonymous functions

`fn` is a declaration and an expression now, so a function can be a value
without a name:

```flint
let square = fn(x) { return x * x }
print(square(5))    # 25
```

the closure semantics are the same as for `fn name(...)`: capture by
reference, late binding. a bare name in a function position still does not
resolve, so `fn name() {...}` is how a function binds a name. an unnamed
function reports itself as `<anonymous>` in a stack trace.

### trailing commas

multi-line list and table literals accept a trailing comma now, so adding
or removing the last entry is a one-line diff instead of a two-line one.

```flint
let config = {
    name: "flint",
    port: 8080,
    debug: false,
}
```

single-line literals accept one too (`[1, 2,]`). the comma carries no
meaning; it is a prefix that did not get its entry.

### module globals, fixed

a module's own functions can now both read and write module-level globals.
the write path resolves the closure's defining environment the same way the
read path does, instead of resolving whatever module happened to be running
when the call came from somewhere else.

this was a quiet bug and it killed any program where a module function
updated module-level state and was called from another module. no old test
caught it because every test that wrote module state also read it in the
same function, and the read path was already right.

### stdlib and natives

two builtins: `ord(s)` is the byte value of a one-character string, `chr(n)`
is the inverse for 0-255. both reject anything that is not obviously a byte,
so a multibyte character fails rather than returning half of one.

four modules are new in the library, and it now ships fourteen `.fl` files:

* `encoding`: base64 and hex encode and decode, url percent-encode and
  decode. malformed input raises rather than passing through silently.
* `args`: the script's argv as data -- `all()`, `count()`, `get(i)`,
  `has(flag)`, `value(flag)`. it needs an alias (`import args as argv`),
  because a bare `import args` binds the name `args` and would shadow the
  builtin `args()` the module is written on top of.
* `ansi`: terminal escape sequences, so a cli can colour its output
  without hand-writing the escapes.
* `pretty_print`: multi-line rendering of nested lists and tables, which is
  what you want when the structure is the message.

`fs.remove` now removes an empty directory as well as a file.

`--version --verbose` now reports the language version, bytecode format
version, native ABI version, package format version, lockfile version and
runtime version, so a script can check what it was shipped against.

### deliberately not shipped

documents describing what the tree does not have -- a native ABI,
`flint.toml` manifests, TLS certificate validation -- were deleted rather
than edited down to "not yet". the facts they uniquely stated now live in
[docs/limits.md](docs/limits.md), which is the one page that says what
flint does not do.

**no native extension ABI.** There is no public header, no `native.load`,
and no ownership rules, so a shared library cannot be taught to flint at
all. a versioned public `flint.h` is real work, and a half-specified one is
worse than none: an extension with a subtly wrong ownership rule corrupts
memory rather than failing. it should be its own release.

**no package manager.** No `flint.toml`, no registry, no lockfile, and no
`add`/`install`/`build` subcommands. reproducible dependency selection is
the whole point of the design, and a resolver that does not resolve
reproducibly is just a script that downloads things.

**no threads, channels or locks.** The collector is single-threaded, and
a second thread without root registration and a safepoint model is how
collectors get corrupted rather than how programs get faster.

**no TLS of flint's own, and no crypto.** `http://` goes out over native
sockets; `https://` is handed to `curl(1)` as a subprocess, which brings
its own TLS and its own certificate verification -- flint passes no
`--insecure`, so a bad certificate fails, but the trust store, the TLS
version policy and the hostname check are curl's, not flint's. there are no
hashes, no hmac, no password hashing anywhere in the tree. `random` is
xorshift64* and says so in its own header. shipping a hand-rolled cipher
is worse than shipping none.

**no regex.** A regex engine is a parser with a backtracking matcher
attached, and it wants its own test corpus before it wants to be a module.

**no `finally`.** It needs cleanup actions to run on the way out, and
unwinding currently discards frames rather than visiting them. the error
model shipped whole; half of it would have been worse.

### gate

116 tests on release, under ASan + UBSan + GC-on-every-allocation, and on
the computed-goto build -- plus new tests for every feature above, and a
regression test for each bug fixed. unit tests, diagnostics,
clang-tidy (default and debug configurations), clang-format clean.
macos and windows were not executed; no fuzz, fault-injection or
thread-sanitizer target exists yet, so "no leaks" is as far as the claim
goes.

## v0.7.0

a daily-use release. the language learns to talk to the operating system,
tables learn to be iterated, two small syntax additions cover the config
patterns every script rewrites by hand, and the library stops needing a
rebuild: `flint sync` updates it in place, and `import http` means a script
can talk to the network.

### os and process

`import os` answers what the machine knows: platform and architecture,
working directory, environment variables with defaults, home and temporary
directories, and the process id.

```flint
import os
print(os.name() + "/" + os.arch())
print(os.getenv("HOME", ""))
```

`import process` runs programs and reads what they said. the command is a
list, never a string, and there is no shell anywhere in the path. both
streams are captured, so a child that writes a lot to stderr cannot deadlock
a parent that reads stdout.

```flint
import process
let r = process.run(["git", "status", "--short"])
print(r.code)
print(r.stdout)
```

options are a table with `cwd`, `stdin` and `timeout` keys, all optional.
unknown keys are an error rather than ignored. a timeout kills with SIGKILL
and reports `timed_out: true` alongside the wait status, so a timeout kill
is distinguishable from a signal death.

### tables grow up

`for k, v in t` walks entries in insertion order. `t[k]` reads and writes
through computed keys. `keys(t)`, `has(t, k)` and `delete(t, k)` ask and
remove. field access compares by content now, so a key built at run time
finds the entry a literal created -- pointer comparison would answer nil
there, which was a latent 0.5.0 inconsistency.

lists gain `insert` and `remove` with the same index rules as subscript.

### two small syntax additions

`a ?? b` is `b` when `a` is nil and `a` otherwise, with the right side
running only when needed. only nil triggers it: `false`, `0` and `""` stay.
right-associative, looser than `or`, tighter than `=`.

`let {host, port} = config` binds each name from the table's fields. flat
names only; missing keys read nil; `const` works the same way.

### `flint sync`

updating the standard library no longer requires a rebuild. `flint sync`
downloads the library from the project's github and installs it into
`~/.flint/stdlib`. every file is compiled in the binary before it is
renamed over the old one, so a truncated transfer or a 404 leaves the old
files alone; identical files are left alone. `--dry-run` shows the change
set without writing, and `--ref=v0.7.0` pins to a release tag. a module
whose imports fail verification is skipped with a suggestion to pin the
reference and bump flint itself.

### `http`

`import http` is a new client for requests. `http.get`, `http.post`,
`http.put`, `http.delete`, `http.request`, and `http.get_json` cover the
orthodox cases. `http://` is handled by a small built-in client -- the
one new piece of libc facing code this release adds -- and `https://`
delegates to `curl(1)`, because https is TLS and TLS is not something to
reimplement in a dependency-free language. results are tables: `ok`,
`status`, `headers`, `body`, `url`, `redirects`, `error`.

### module exports fix

a module that imported a sibling module could be exported under the
*sibling's* name set. `import_file` built a module's export table from
`globals_envs[globals_used - 1]`, but a nested import pushes the nested
module's env on top, shifting the index. `http.fl`, the first
shipped module to import a sibling (`json`), surfaced the bug at
imported-by-two-degrees depth. The module's env is now held by pointer.
covered by a regression test that checks the export keys of a module
with a sibling import.

### deliberately not shipped

`defer` was evaluated and deferred. its motivating example needs file
handles, which do not exist -- only whole-file `read`/`write` -- so there is
nothing to clean up yet. and the failure policy for a deferred action that
itself fails needs per-action error isolation the unwind model does not
have. the design is in ARCHITECTURE.md for when handles land. a cleanup
construct with nothing to clean up would be scope creep with a good name.

no table `sort`: heterogeneous values have no total order worth promising.
no native loader, no JIT, per the plan.

### gate

63 tests on release under gcc and clang, on the computed-goto build, and
under ASan + UBSan + GC-on-every-allocation -- plus new tests for every
feature above. diagnostics, unit tests, clang-tidy, clang-format,
trailing-newline check clean. every example runs. `flint sync` is
exercised against the live repository and a local mirror.

## v0.6.1

`num()`, the install story, and imports that enforce themselves.

### unused imports are an error

An import whose bound name is never read fails compilation:

```
[line 1] Error at 'math': imported 'math' but never used. remove the import,
or use it.
```

An import always runs its file, so an unused one is dead code with a side
effect. Either use the binding or delete the line. `import "x.fl" as _` opts
out explicitly, for the one legitimate case: importing a module for its
failure, where there is nothing to use. The REPL is exempt, since each
submission compiles separately.

### missing imports suggest themselves

Reading a name that was never defined, where a module file exists with the
matching shape, names the import instead of guessing at a typo:

```
= help: did you forget to `import math`?
```

This replaces the "did you mean" suggestion when it fires -- an exact hit on
a real file beats a fuzzy match. Covers the standard library and sibling
files beside the importing one.

### num

`input()` returns a string and there was no way to get a number out of one.
`as number` is a type assertion, not a conversion, so `"9" as number`
correctly fails -- and then the user has a string that looks like a number
and no function that agrees.

```flint
import math

const a = num(input("a: "))
print(math.sqrt(a))
```

`num("42")` is 42. `num("  7  ")` is 7, because what `input()` hands back
includes whatever whitespace the user typed. `num("12abc")` fails rather
than returning 12; returning a prefix would be guessing. Numbers pass
through, so it is safe to call on something that might already be one.

A failed conversion is an error naming the value, not nil. Nil would surface
three calls later as an operand error in code that had nothing to do with it.

### make install

`make install` puts the binary in `~/.local/bin` and the library in
`~/.flint/stdlib`, which is the third entry in the lookup order the runtime
already documents. `PREFIX`, `BINDIR`, `LIBDIR` and `DESTDIR` override all of
that for packaging. `README.md` installation instructions match what the
Makefile does, which they previously did not -- they described building in
place and stopped there.

## v0.6.0

a module-system release. `import` binds one name to a module's exports,
`export` decides what those are, and the REPL takes a block.

### the bug this fixes

Two modules could not both have a private helper of the same name. Both wrote
into one shared global table, so:

```flint
# geometry.fl          # display.fl
let scale = 2          let scale = 10
export fn area(r) {    export fn show(v) {
  return 3*r*r*scale     return v * scale
}                     }
```

```flint
import "geometry.fl"
import "display.fl"
print(geometry.area(2))
```

printed **120**. `scale` resolved to 10, because `display.fl` loaded second.
No error and no warning -- a wrong number, which is the worst failure a
language can have. It is now 24, because each module has its own environment.

### what changed

**every module has its own globals.** `vm->globals` became a pointer into a
heap array of per-module tables, and the bytecode did not change at all --
`OP_DEFINE_GLOBAL` means "whatever table this module is running in". A closure
remembers the environment it was created in, so a function called long after
its module loaded still sees that module's private names.

**`export` means something.** Four opcodes flag a binding exported at compile
time. Previously `export` was a comment and the exports were found by diffing
the global table across the module's run, which cannot distinguish a helper
from a public function -- both are a name that appeared.

**failed imports are transactional.** A module that fails binds nothing
anywhere. Its partial globals used to survive, and a second import retried
against them. Its environment is deliberately not freed, though: it may have
handed out a closure the importer holds, and freeing it turns every later call
into a use-after-free.

**`import "x.fl" as name`.** The default binding is the last path component
without the extension, which is what existing scripts already spelled.

**the REPL takes a block.** Delimiter depth with strings and comments skipped,
because a brace inside a string literal is not an open block.

**a bug found and fixed.** `OP_LIST_LEN`, added in 0.5.0 to remove a native
call from every for-in iteration, only understood lists. `len` takes strings
too, so `for c in s` over a string stopped working. Nothing in the test suite
noticed, because every for-in loop in the tree iterates a list. It surfaced
when every example was run as part of this release. There is a regression test
now.

### breaking change

an import no longer dumps names into the importer.

```flint
# before                      # after
import "helper.fl"            import "helper.fl"
print(square(6))              print(helper.square(6))
print(LIMIT)                  print(helper.LIMIT)
```

`docs/modules.md` has the full model and a migration section.

### not in this release

**no native extension ABI.** Sections 15-23 of the 0.6.0 plan were left out.
A versioned public header, dlopen/dylib loading, and ownership rules are a
real piece of work, and a half-specified one is worse than none: an extension
with a subtly wrong ownership rule corrupts memory rather than failing. It
should be its own release with its own differential tests.

**no JIT**, per the plan.

**no table `has`/`delete`/`keys` or list `insert`/`remove`.** The collections
work and are tested; the additions were not reached. The spec says defer rather
than compromise, and these are additive rather than correctness fixes, so
deferring them costs nothing today.

## v0.5.0

a runtime release. the language does not change at all -- not one keyword, not
one builtin, not one diagnostic code. what changes is how fast things run, how
much of the runtime is measured rather than guessed at, and how many ways a
compiler bug can turn into a crash instead of a message.

### what got faster, and why

**strings stopped being interned at run time.** this is the big one, and it
came from a measurement rather than an idea: strings were the documented weak
point at 0.37x CPython.

identifiers and literals are still interned, so `==` on those is a pointer
compare. strings built while the program runs -- concatenation, a slice,
`str()`, a parsed json value -- are not. they are equal by content, which costs
a length compare and a `memcmp` and saves a hash, a probe, an insertion into a
weak table, and the collector's later walk of that table.

interning a string that is used once was paying a real cost for a payoff that
essentially never arrives: two slices of a log file being byte-identical is
rare, and `s = s + "x"` produces different bytes every time. the cost moved
from creation to comparison, and comparison happens far less often.

strings also got their own nan-box tag, so `IS_STRING` is a mask instead of a
pointer chase to read a type byte -- and, more usefully, so equality stops
assuming that equal means identical.

| case                  | change   |
|-----------------------|----------|
| str_concat            | -92%     |
| str_utf8              | -77%     |
| str_ascii             | -76%     |
| mem_retain            | -58%     |
| call_native           | -55%     |
| str_find              | -47%     |
| strings               | -12%     |

**`OP_LIST_LEN`.** the compiler's for-in-over-a-list loop was loading the `len`
global, pushing the list and making a real call into C on *every iteration* --
a hash lookup, an arity check and a native frame, to read an integer that was
already in the object's header. one opcode now reads it.

**a computed-goto interpreter**, as `make flint-goto`. 7-21% on loop-heavy
programs. it is a separate binary rather than an `#ifdef` because the default
build stays portable C11 and warning-clean, and because one copy of sixty-odd
handlers beats two that have to be kept in step. `scripts/to_computed_goto.py`
is the transformation.

### what got safer

**a bytecode verifier.** every chunk is checked before it runs: opcode validity,
operand width, constant and local indices, and jump targets landing on
instruction boundaries. it recurses into closures.

the boundary check is the one that matters. a jump into the middle of a two-byte
operand reads that operand byte as an opcode.

it found three compiler bugs while being written:

- `OP_EXPORT` was the upper bound of the valid opcode range, and it sits in the
  *middle* of the enum. every opcode above it read as invalid.
- the backward jump target was computed from the start of the instruction
  rather than the end, so every loop looked malformed.
- the local-slot high-water mark was assigned rather than raised, so a shallower
  later scope lowered it and every slot an earlier scope had used then looked
  out of range.

**a use-after-free at exit.** `vm_free()` freed the globals, strings and modules
tables before the objects. freeing an object can trigger a collection; that
collection reads `vm->globals`; if the table was already freed, the mark phase
walks freed memory. found by ASan on a new JSON benchmark, and it only showed up
above a certain object count, which is why it survived v0.4.

**the instruction-width table** now lives in `chunk_instruction_size()`, which
the compiler, the disassembler and the verifier all consult. a unit test pins it
against what the compiler actually emits -- that is how `OP_SET_FIELD_TOP`'s
width was found to be wrong.

### new tools

```sh
flint --profile program.fl        # counters: calls, allocs, GC, strings
flint --check program.fl          # compile and verify, do not run
flint --dump-bytecode program.fl  # human-readable disassembly
```

and `python3 bench/bench.py`, which reports medians and records the environment
with them. 49 cases across startup, arithmetic, calls, collections, strings, json
and memory.

### what did not happen

**there is no JIT.** this release was supposed to have a tier-1 baseline JIT and
does not. an earlier draft of the x86-64 encoder and template compiler was
written and then deleted rather than shipped half-finished, because a JIT whose
deoptimization path has never been tested is not a performance feature, it is a
way to corrupt a program silently.

the pieces that make a JIT tractable are now in place -- a verifier that makes
the bytecode trustworthy, type counters on every function, thresholds in
`config.h`, and a disassembler -- but the compiler itself is future work.

what is already measured and real: the strings work above, `OP_LIST_LEN`, the
computed-goto dispatch, and a collector that no longer reads freed memory.

## v0.4.0

a more useful flint. the language stays the same; the stdlib and runtime do not.

**new: standard library modules**

seven modules ship in `lib/` and import by bare name. no package manager, no
network, no install step beyond copying the directory next to the binary.

- `math` -- full libm wrapper: sin, cos, tan, asin, acos, atan, atan2, exp, log,
  log2, log10, sqrt, cbrt, pow, floor, ceil, round, trunc, abs, sign, clamp,
  hypot, and the constants PI, E, TAU, INF, NAN
- `random` -- xorshift64* seeded from clock and pid. `rand()`, `rand_int(a, b)`,
  `rand_float()`, `shuffle(list)`, `choice(list)`. not cryptographic; says so
  in the source
- `time` -- `now()` (unix epoch as a number), `clock_ms()`, `sleep(ms)`,
  `format(t)` (UTC string), `measure(fn)` (returns elapsed ms)
- `fs` -- `exists`, `read`, `write`, `append`, `remove`, `mkdir`, `isdir`.
  everything a script needs to touch the filesystem without reaching for a
  shell
- `path` -- string arithmetic over paths: `join`, `dir`, `base`, `ext`, `abs`,
  `strip_ext`. never touches the filesystem
- `collections` -- `reverse`, `contains`, `min`, `max`, `sum`, `flatten`,
  `zip`, `uniq`. the list operations that come up in every second script
- `json` -- `parse(s)`, `stringify(v)`, `pretty(v)`. objects and arrays
  round-trip cleanly. numbers stay numbers

**new: module resolution**

bare import names resolve via `FLINT_STDLIB` env, then `<exe-dir>/lib`, then
`~/.flint/stdlib`. install `lib/` next to the binary and it just works.

**new: long opcode variants** *(contributed by Artem Tsitronov, [#9](https://github.com/programmersd21/flint/pull/9))*

`OP_GET_GLOBAL_LONG`, `OP_DEFINE_GLOBAL_LONG`, `OP_SET_GLOBAL_LONG`,
`OP_GET_FIELD_LONG`, `OP_SET_FIELD_LONG`, `OP_CLOSURE_LONG` lift the
256-global and 256-constant limits that blocked programs with large global
tables. each variable reference now emits a 1- or 3-byte index depending on
pool size. programs with fewer than 256 globals pay nothing.

**new: make check**

`make check` runs clean → build → test → unit from scratch. the gate for
anyone about to push.

**fixed**

- release workflow no longer auto-generates release notes from commit messages
  (which were not written for end users). releases now get a manual body.
- the `generate_release_notes` accident in the workflow is gone. a release that
  says "Merge pull request #3" instead of what changed is not a release; it is
  a git log with extra steps.

## v0.3.0


a language for small unix programs. one binary, no dependencies, and a script
that reads stdin, calls a program, and writes a line is the whole toolchain.

**new: the standard library a script actually needs**

- `args()`, `env()`, `exit()`, `read_file()`, `write_file()`, `exec()`
- `split` `join` `trim` `contains` `starts_with` `ends_with` `replace` `lower`
  `upper`
- `exec` calls `execvp` and never a shell. there is no path from the API to
  `/bin/sh`, so a filename with a semicolon in it is an argument, not an
  injection
- arguments pass through to the script: `flint x.fl -v` runs `x.fl` with `-v`
  as an argument

**new: diagnostics**

- rustc-style errors, opt in: `--error-format=human|short|json`
- stable codes by origin, `--explain E0102`, `--color=`
- `--fix` for machine-applicable closing-delimiter insertions, now offered for
  a missing delimiter anywhere and not only at end of file
- secondary spans: a redeclaration shows where the name was first defined
- more than one error per file, with a count, capped at 20
- every message lowercase, matching the rest of the repository
- `--quiet` and `--warnings=default|none|all`
- `did you mean` for a misspelled name, including keywords, which are not
  globals and were therefore never suggested before
- JSON includes source spans and structured delimiter replacements; runtime
  spans underline the failing expression rather than the whole line

**new: cli**

- `flint -` reads a script from stdin, next to `-e` and the repl
- `flint script.fl args...` passes everything after the script to the script

**internal math**

- ten `__`-prefixed natives for a math library that is not in this repository
  yet: `__floor` `__sqrt` `__fma` `__ldexp` `__logb` `__fabs` `__copysign`
  `__hi32` `__lo32` `__from_bits`
- the wrappers live in `src/util/fl_math.c`, separate from the language, so a
  future library can change them without touching the runtime
- contributed in [#1](https://github.com/programmersd21/flint/pull/1)

**the repl**

- opens with the version and what to type, instead of a bare cursor
- `:help` and `:quit`
- an expression prints its value and a statement does not, which is what makes
  a repl a repl and not a shell with an `eval` in it
- a session where a line failed exits 70, even though the session carried on

**modules**

- imports resolve against the importing file, not the working directory. a
  script runs from any directory now, which it did not
- a module runs once per VM; a repeat import is a no-op
- an import cycle reports the in-flight module path instead of overflowing

**fixed**

- `split` pushed every piece on the value stack. past 65536 separators that
  wrote off the end of the array, silently, with no bounds check to catch it
- the growing-read loops in `read_file` and the stdin reader left no byte for
  the terminator when a read landed exactly on a capacity boundary, which is
  every file whose size is a power of two
- those loops also called the read again after a zero-byte read, which is
  undefined behaviour on a stream in an error state

**measured**

against CPython 3.14.7 on an i5-1235U, best of 7, matched `.fl`/`.py` pairs
whose outputs are compared before the timing is reported. full numbers and
method in [bench/RESULTS.md](bench/RESULTS.md).

| case | flint | python | |
|---|---|---|---|
| startup | 0.47 ms | 11.1 ms | **23x** |
| hello | 0.54 ms | 8.9 ms | **17x** |
| arith | 1.36 s | 2.55 s | **1.88x** |
| lists | 107 ms | 198 ms | **1.85x** |
| fib | 15.0 ms | 26.4 ms | **1.75x** |
| calls | 168 ms | 265 ms | **1.58x** |
| closures | 114 ms | 173 ms | **1.52x** |
| strings | 148 ms | 64 ms | **0.43x** |

startup is the result that matters for this language. the others are honest
but modest, and `strings` is a loss that stays in the table.

## current development notes

`--error-format=human` and `short` render the diagnostics currently emitted by
the compiler and VM. They do not yet provide multiple source labels, multiline
underlines, or full parser recovery. `--fix` is limited to supported closing
delimiters. See [docs/diagnostics.md](docs/diagnostics.md) for the actual
coverage.

## v0.2.0

the same flint, substantially more correct. no new language, one new
builtin, and a pile of crashes turned into errors.

**runtime**

- `input([prompt])`: prompt with no newline, one line, nil at eof. empty
  line is `""`, not nil. CRLF tolerated. buffer grows, never truncates
- `x[1.5]` is an error, not a silent truncation. non-finite, out-of-range
  and non-numeric indices rejected with their own messages
- string concatenation refuses to overflow instead of invoking undefined
  behaviour. concatenation failing mid-expression no longer falls through
  to the wrong error
- a native that fails (len on a table, pop on empty) stops the script
  instead of the dispatch loop reading `frames[-1]`

**const**

- `const x = 1; const x = 2` refused. same value twice (module re-import)
  allowed, anything else is a contradiction
- `let` on an existing const refused. `let` on an ordinary name still
  overwrites. `let` promoted to `const` allowed
- the flag survives table rehash; copied on `table_add_all` so imported
  constants stay read-only

**compiler**

- parser state is one struct, saved and restored around `compile()`, so a
  nested compile starts clean and puts the outer state back

**tests**

- stdin support in the runner: a `.stdin` file next to the `.fl`, `/dev/null`
  otherwise so no test can hang on a terminal
- coverage for input, index validation, const rules, shared upvalues,
  gc-stress allocation, concat overflow

## v0.1.0

first release. everything below is new.

**runtime**

- nan-boxed 64-bit values: numbers, booleans, nil, heap pointers
- stack vm with 256 call frames, recursion, and a stack overflow check
- mark and sweep gc with an explicit gray stack and weak intern table
- closures with open upvalues, closed on block and frame exit
- lists with negative indexing, tables with dot access
- modules: `import "path.fl"`, `export` on `fn`, `let` and `const`
- `const` on a global, enforced at run time and across module boundaries

**compiler**

- single-pass pratt parser, no ast
- streaming scanner with a keyword trie
- `if`/`else`, `while`, `for` over ranges and lists, `break`, `continue`
- functions, recursion, arity checking
- `as` type assertions against the seven `type()` names
- string escapes, `#` comments, statements terminated by newline or `;`

**tooling**

- repl
- `-e` for inline code, `-h`, `-v`
- bytecode disassembler and execution tracer in the debug build
- language test suite and unit tests
- `make lint` and `make fmt`, policy in `.clang-tidy` and `.clang-format`
- ci on gcc and clang
