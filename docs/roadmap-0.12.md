# roadmap: 0.12.0

what 0.12.0 sets out to do, in the order the work was done, what shipped,
and what did not. three of the four sections below are marked *delivered*;
the fourth says why it was not, in terms that would still be true a
release later.

## in this release

### enums and match

a declared set of variants, some carrying a value, and a `match` that
proves exhaustiveness where it can. see the section at the bottom for
what is deliberately absent.

### structs

named record types, declared and constructed the way a table literal is,
with fields read through the same `.` and `[...]` syntax a table already
supports. this is the first user-defined *kind of value* in flint: before
this, every value was one of eight built-ins. a struct is a value whose
shape is declared in one place and checked at construction, so a typo in a
field name is an error at the line where the struct was built rather than a
nil three functions later.

what is specified and tested: declaration, construction, field read and
write, missing and unknown field names, wrong-type fields, mutability,
equality (by value, like tables are by value today), string rendering,
`type()` naming, interaction with lists, destructuring, closures holding a
struct, GC under stress, module export, and reassignment of a module-level
struct binding.

field annotations are **descriptive, not enforced**. `struct P { port: number }`
documents the intent and is checked by nothing. flint has one numeric type,
no generics and no inference loop, and adding a checker to make one
annotation kind honest would be a type system arriving one keyword at a
time. the alternative -- annotations that are runtime-checked -- was
rejected because it would make `P { port = 8080 }` fail on values that
would have worked, and the annotation is documentation either way.

### transactional module initialization

a module that fails after it has already created globals currently leaves
them behind. `docs/internals.md` has named this as a defect since before
0.10; 0.12.0 fixes it. the fix is a snapshot of the importer's global table
taken at the start of the nested run and restored on failure, so a module
that dies halfway through initialization leaves no partially committed
state for the next import to find.

the boundary is documented and not papered over: external side effects --
files already written, processes already started, network requests already
sent -- are not rolled back. that would require every module to be a
transaction, which it is not.

### docs and version

`docs/roadmap-0.12.md` (this file), `docs/native-abi.md` for the C ABI,
`SPEC.md` for structs, `docs/data.md` beside the tables structs extend,
`docs/packages.md` for transitive resolution, `RELEASES.md` for the
release, and version metadata to 0.12.0.

## not in this release, and why

### the native C ABI, Rust wrapper and Ratatui

**delivered.** `include/flint.h` is a versioned C ABI with opaque
handles and no exposure of any runtime structure; `src/ext.c` is the host
side and `docs/native-abi.md` is the contract. handles are values rather
than pointers into the VM, so holding one past its call is the mistake the
API is arranged to make you have to be explicit about -- `fl_retain` and
`fl_release` for that, rooted against the collector.

loading is `dlopen` behind a platform seam, refusing an unsupported ABI
version before the entry point runs. `flint native LIB.so MOD script.fl`
loads one. a loaded library is never unloaded, because its functions stay
callable and a value it produced can outlive the call that made it --
unloading would be a use-after-free waiting for the collector.

`rust/flint-sys` wraps the same header, and the two examples show what a
C extension cannot: a contained panic and a retained handle. the ratatui
example draws a real frame from a flint script. `make native-test` builds
real shared objects and loads them.

what is still open: unloading, non-POSIX loaders, and passing
*pointers* into flint storage for zero-copy work -- which the handle
design deliberately refuses to offer until the lifetime story is stronger.

### package manager

**transitive resolution delivered.** a dependency's own dependencies are
followed recursively, with a nested path relative to the package that
declares it. two packages reaching the same one install it once; two
*versions* of one package is reported rather than resolved by picking.

what is still open: version solving across packages (a requirement is
checked against the single version being installed), content integrity
checksums, and native-package platform/ABI metadata. those are real gaps
and are recorded as gaps rather than approximated.

### enums and pattern matching

**partly delivered.** an enum's variants and a `match` over them are in:
`match value { arm ... }` takes variant arms, a wildcard and a bare
binding, and exhaustiveness is checked at compile time *where it can be
proved* -- over a declared enum, where the possible values are a closed
set. that is the buildable form the earlier version of this file argued
for: a matcher over open-ended dynamic values cannot promise
exhaustiveness, and one that cannot prove it must not claim to. anywhere
else the compiler knows nothing, so a wildcard is required and a value
matching no arm is a runtime error rather than a promise it cannot keep.

what is deliberately absent: binding a payload *by* an arm
(`Colour.Blue(n)`), which is an error saying so rather than reading a slot
the arm scope does not own; literal patterns (`1 { ... }`); nested
patterns; and guards. each is a small step on the same machinery, and each
is one this release did not need.

## the bar for adding anything else

every candidate has to clear the same bar it would if this release did not
already have a headline: does it make common programs shorter, is the
syntax obvious, does it compose, does it cost special cases in the
compiler, and could a library function do the job instead. `struct`
cleared it. nothing else did, and the release is better for the things it
declined.
