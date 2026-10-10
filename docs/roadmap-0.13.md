# roadmap: 0.13.0

an audit of the repository as it stands after 0.12.0, what is actually
verified, and what is not. written from the source rather than from earlier
plans, because the plans were wrong more than once and the code is the only
thing that can be trusted here.

status: items 1-4 below landed in 0.13.0 (payload binding plus literal
patterns in `src/frontend/compiler.c` with `tests/language/data/
match_payload.fl`; SHA-256 content hashes in `src/util/sha256.c` and
`src/pkg.c` with `tests/unit/test_sha256.c`; the `flint` crate over a new
`fl_engine_*` API in `src/engine.c`; the `native-unload` request lifecycle
in `src/ext.c`). items 5-7 remain future work, as `RELEASES.md` states.
the audit stays as written; `RELEASES.md` is the record of what shipped.

0.13.1, on top: the portability probes and everything they found
(`docs/platforms.md`), plus the standard-library modules the audit
listed under item 7 -- csv, toml, url, datetime, then log, test, hash,
debug, regex, compress, signal, each with contract, tests, and docs.
regex and compress ship with the proofs the audit asked for.

every finding is labelled by severity and by how sure I am. a defect I could
reproduce is one; a suspicion from reading is another, and the difference
matters more than the label.

## what exists

a C11 bytecode VM: scanner, pratt parser, single-pass compiler, a verifier,
a mark-and-sweep collector, closures, modules, a standard library, a package
manager, a formatter, a CLI and a test suite. 145 language tests, a verifier
unit suite, a package-manager integration suite, a native-ABI suite that
builds and loads real shared objects, and a rust wrapper over the public
header. `make quick` and `make validate` are the two gates.

baseline at 0.12.0, all run:

| gate | result |
|---|---|
| language tests | 145 / 145 |
| verifier unit | 4386 checks |
| fmt idempotency | 243 checks |
| package manager | 30 checks |
| test runner self-test | 8 checks |
| native ABI | 21 checks |
| ASan + UBSan + GC stress | 145 / 145 |
| computed-goto interpreter | 145 / 145 |
| clang-tidy | 0 |
| clang-format | 47 files |

## verified defects

### the match subject's frame slot (high, certain, unresolved)

`match` reads its subject out of a local. the bytecode is right -- verified
by disassembly: the subject is stored with `OP_SET_LOCAL n` and read back
with `OP_GET_LOCAL n`, the same `n`, in both the tag test and the payload
read. the runtime disagrees: `OP_MATCH_PAYLOAD` sees a value that is not an
enum, so the arm's payload binding reads the wrong slot.

reproduced from:

```flint
enum C { B(number) }
fn f() {
    match C.B(1) {
        C.B(x) { print(x) }
        _ { print("no") }
    }
}
f()
```

the compiler's local index and the bytecode slot are offset by one, because
slot 0 is the callee. `for` gets this right -- its hidden locals are the
same shape and work -- so the offset exists somewhere in the `match`
compiler path and not in `add_local` itself. i did not find it.

**this is why `R.Ok(x)` is a compile error in 0.12.0 rather than a runtime
error.** a binding that reads the wrong slot is the kind of bug that passes
a test suite and corrupts a value later, and saying so is worth more than a
binding that appears to work. the arms that work -- variants, wildcards,
bare names -- all pass.

the fix is small and known in shape: follow `for_statement`'s arithmetic
exactly rather than reasoning about it. that is the first task below.

### the scanner's keyword logic was boolean where it needed equality (fixed)

`check_keyword` answers `TOKEN_IDENTIFIER` when the letters do not match, so
`check_keyword(...) && peek() != '('` is *true* for every `m`-word, and
`{memory: 1}` stopped being a table. comparing the result fixed it. noted
because the class of bug -- using a token-type enum as a boolean -- is easy to
repeat, and `check_keyword` is called from thirty places.

### missing trailing newlines (medium, fixed)

`include/flint.h` and eight other files ended without one. clang rejects
that under `-Werror`, so the clang build was red while gcc was green. my
audit had only checked tracked files, so everything new in 0.12.0 escaped
it. the CI step now performs the audit that catches it.

## suspicions, not yet demonstrated

each of these is a reason to look, not a defect I have reproduced.

- **the collector and temporary values.** every native allocation path was
  audited while writing the ABI, and the ones found were rooted. that is not
  the same as having audited all of them. the GC stress suite runs every
  language test, which is broad but not adversarial.
- **a module that fails after partial initialization.** the environment is
  discarded rather than undone, which is sound for globals and unsound for
  anything a module cached outside the VM. nothing in the tree does that
  today; a native module could.
- **the compiler's file-scope state.** `CompilerState` is saved and restored
  per compile, and the compiler state is bundled, but the scanner is still a
  global. two compilers cannot run at once. this is documented and is not a
  defect unless something needs it.
- **module identity.** two paths to one file load it twice. documented, not
  fixed; a symlinked package directory is the realistic way to hit it.

## what 0.13.0 should be

ordered by dependency, not by how interesting each is. a later item that
assumes an earlier one is not a later item, it is a blocked one.

### 1. the match payload binding (high, small)

find the offset, fix it, and re-enable `R.Ok(x)`. blocked on one afternoon of
frame arithmetic. everything after it is safer once a match arm can bind,
because the workaround -- matching and reading in the body -- is the shape
most readers will otherwise copy.

### 2. literal and nested patterns (medium, medium)

`match n { 1 { } 2 { } }` and `match c { Colour.Blue(x) { } }`. the first
needs a comparison opcode that already exists; the second is the binding from
item 1. together they are what makes `match` worth having rather than a
switch with extra punctuation.

### 3. checksums in the lockfile (medium, medium)

a lockfile that pins a commit is reproducible against *movement of the
branch*, not against *tampering with the object*. a content hash per pinned
package, verified on install, is the difference between "the same as last
time" and "the same as recorded". the manifest already has a hash to copy:
`fnv-1a` over the git URL, used for the mirror directory name. it is not a
checksum and must not become one -- a hash of a path proves nothing.

### 4. a safe `flint` crate above `flint-sys` (medium, large)

`flint-sys` exists and works. what does not exist is a Rust API someone
would write a program with: an engine you can create and drop, `Value` you
can inspect without `unsafe`, and an error type. this is the piece that makes
"embed flint" a supported claim rather than a demonstration. the spec's
demand -- that ordinary use needs no `unsafe` -- is the bar, and it is
reachable, but it is not a weekend.

### 5. native module unloading (high if done wrong, large)

the honest position is in `docs/native-abi.md`: a loaded library is never
unloaded, because its functions stay callable and a value it produced can
outlive the call that made it. doing better needs reference tracking over
every route to native code -- function objects, closures, retained handles,
module globals, values reachable through collections -- and `Busy` when the
answer is not provable. the spec is right that this must not be faked, and
right that the safe lifecycle API is the shippable part. the lifecycle
machine (loaded → quiescing → busy or unloaded) is worth doing even while
the answer is always "busy until exit", because it makes the limit
expressible instead of implicit.

### 6. linux, macos, windows (medium, medium)

the code has windows paths (`_WIN32` in the loader, `os.pathsep`, the env
reader) and no windows CI. the honest claim today is linux. adding a
windows job to the existing matrix is the cheapest way to find out what is
actually broken, and nothing here should be documented as supported until a
job says so.

### 7. more standard library (low, ongoing)

the modules with the best effort-to-use ratio are `csv`, `toml` (as a
reader over the subset the package manager already parses), `url`, and
`datetime`. each is a module with a contract, tests for its failure paths,
and one example -- not a wrapper list. `regex` and `compress` are the ones
to be careful about: both are easy to write badly and hard to notice.

## explicitly not in 0.13.0

- a JIT, a second type system, generics, traits, macros, async. the runtime
  is a C11 stack VM and stays one.
- a package registry. the honest scope is identity, versions, integrity and
  reproducibility for what exists locally; hosting is not a language
  problem.
- a full solver. a resolver that takes the first conflict rather than the
  right one makes a lockfile untrustworthy, which is worse than reporting
  the conflict.

## how to tell this file has gone stale

every claim above names a command or a file. if one of them stops being
true, the file is wrong and the command is the evidence. the roadmap for
0.12.0 claimed enums and match were deferred for three releases while they
were half-built in the tree; this one is meant to be checked against
`make validate` rather than believed.
