# roadmap: 0.12.0

what 0.12.0 sets out to do, in the order the work is being done, and what
it is not doing.

## in this release

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

`docs/roadmap-0.12.md` (this file), `SPEC.md` and `docs/language.md` for
structs, `docs/internals.md` for the module transaction, `RELEASES.md` for
the release, and version metadata to 0.12.0.

## not in this release

### the native C ABI, Rust wrapper and Ratatui

these are the stated goals of 0.12.0 and they are deferred. the reason is
not effort: an ABI that is missing GC-safe handles, defensive argument
validation, or a documented lifetime model is worse than no ABI, because a
native package written against it would break at the first GC stress run --
and a native package that breaks *silently* is indistinguishable from
corruption. the work required is a header with version negotiation, a
handle allocator whose lifetime survives allocation, registration and
rollback for partial registration, a loader behind a platform abstraction
with tested missing-symbol and bad-version paths, and tests that exercise
each of those. that is a release of its own and it will not be finished by
being rushed into the end of this one.

what is deferred exactly: `include/flint.h` (or `include/flint_extension.h`),
`docs/native-abi.md`, `dlopen`-based loading with version checks, the
`flint run main.fl` name for it, a Rust wrapper crate, and a Ratatui
proof of concept. none of them are stubbed; there is no half-ABI in the
tree for a package to accidentally build against.

### package manager hardening

`flint pkg` handles path and git dependencies, pins commits in the
lockfile, and is tested end to end. what it does not do: transitive
resolution, version constraints across packages, content integrity
checksums, and native-package platform/ABI metadata. transitive resolution
in particular is a real gap, but it is a graph-search feature and doing it
half-right -- resolving the first conflict it finds rather than the right
one -- is the kind of thing that makes a lockfile untrustworthy. deferred
with tests asserting the current, documented behavior.

### enums and pattern matching

specified but not implemented. the reason they are not shipped alongside
structs rather than after: a pattern matcher compiled over *open-ended
dynamic values* cannot prove exhaustiveness, and a matcher that cannot
prove exhaustiveness should not claim to. what is buildable is
compile-time exhaustiveness over enum variants only, which is worth doing
in the same shape structs use -- not as a separate feature riding on a
separate compiler pass.

## the bar for adding anything else

every candidate has to clear the same bar it would if this release did not
already have a headline: does it make common programs shorter, is the
syntax obvious, does it compose, does it cost special cases in the
compiler, and could a library function do the job instead. `struct`
cleared it. nothing else did, and the release is better for the things it
declined.
