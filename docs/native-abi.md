# the native C ABI

`include/flint.h` is the whole contract between flint and a compiled
extension. This document is the prose around it: what the pieces mean,
what the rules are, and what is deliberately not here.

## what it is for

a native module is a shared object that registers functions flint code can
call. it exists for the work that should not be flint code -- an
unavailable system library, a hot loop, a terminal UI, a Rust library.

it is not an optimization channel. a native module does not make ordinary
flint code faster, and a boundary crossing is not free: the trampoline
converts every argument into a handle and back. a workload is a candidate
only when the work itself is native.

## versioning

`FL_ABI_VERSION` versions *this header's layout and semantics*, separately
from the language release. a module compiled against version N is refused
by a host implementing a different version, before its entry point runs. a
wrong struct layout is undefined behaviour, so the only safe answer is to
not enter it.

the check is exact, not a range. an ABI that can change shape within a
minor version cannot be versioned by major.minor alone, and pretending
otherwise is how a module built for the future loads into the present.

what changes the version: a struct layout, a function signature, what a
handle means. what does not: adding a function. a module built against an
older version keeps working, because it only calls what existed then.

## handles and lifetime

a `FlValue` is an opaque handle -- one pointer -- to a flint value. it is
valid for the duration of the native call that received it. it is *not* a
pointer into the VM's stack, and holding one past the call is the mistake
the whole design is arranged to prevent.

a value that must outlive its call -- stored in a table, returned to a
callback later -- is retained and released explicitly:

```c
FlValue kept = fl_retain(module, value);
/* ... later, once nothing can reach it: */
fl_release(module, kept);
```

a retained value is rooted against the collector, so it cannot be freed
underneath the module. every retain needs a release: there is no
automatic release on return, because the module usually stored the handle
somewhere the host cannot see. leaking one is a bug in the module, and
`fl_handle_count` makes it at least countable.

handles are per-VM. a handle from one VM means nothing in another, and
using one across threads is undefined -- the VM, its collector and its
handle table are not synchronised.

## values

eight kinds, matching `type()`: nil, bool, number, string, list, table,
function. reading is check-then-read, and the check cannot fail in a way
that surprises you:

```c
double x;
if (fl_to_number(v, &x)) { /* ... */ }
```

strings are byte sequences, as everywhere in flint. UTF-8 is not decoded
here, because an ABI that decoded it would be the only part of flint that
disagreed with the rest. a string may contain NUL, so `fl_to_string`
returns a length and the length is the authority.

a function returning a handle is returning a value, not an address. no
pointer into resizable VM storage is ever exposed.

## errors

`fl_raise` raises an ordinary flint runtime error. flint code catches it
the way it catches anything else:

```flint
try {
    boom()
} catch e as Error {
    print(e.message)
}
```

there is no separate error channel. one error model is the point: a native
failure and a script failure reaching the same handler is the property that
makes native code ordinary code.

a native function reports failure by calling `fl_raise` and returning
`fl_nil()`. returning a value after raising is allowed but discarded.

## registering a module

```c
int flint_module_init(FlModule *module, uint32_t abi_version)
{
    if (abi_version != FL_ABI_VERSION)
        return FL_INIT_ERROR;
    fl_module_name(module, "mymod");
    fl_module_func(module, "work", my_work, 2);
    return FL_INIT_OK;
}
```

the host checks the version before calling, so the check inside is belt
and braces -- a module that trusts the host has still told you which
version it was built against when it goes wrong.

registration after init returns is refused, not queued: a function
registered late could be called before it exists.

## loading

```flint
import mymod
```

no subcommand: a bare library name resolves to `mymod.so` (or `.dylib`,
or `.dll`) beside the importing script, in `flint_modules/`, or in the
standard library -- source first, so a `.fl` always shadows a stale
shared object. an explicit path loads directly: `import "./mymod.so"`
and absolute paths skip the search. the module name handed to
`flint_module_init` is the basename minus its suffix. like Python's
extension modules on `sys.path`, the file must be named after the
module, and the single fixed entry point needs no per-name mangling.

Linux and the other POSIX systems, via `dlopen(RTLD_NOW | RTLD_LOCAL)`.
a windows build refuses with a message rather than pretending to support
it; the loader is behind a platform seam so a second implementation is an
addition, not a rewrite. the old `flint native` explicit loader was
removed in 0.13.2; only `native-unload` keeps a CLI surface.

## unloading

```
flint native-unload libmymod.so mymod
```

an unload request moves the module to quiescing -- calls already inside
finish, new calls are refused with a runtime error -- and then checks
every route to native code, in the order that matters: active calls
first, retained handles next, registered functions last. the library is
closed only when nothing references it; otherwise it stays loaded and
the answer names what holds it:

```
busy: module 'mymod' is busy: 1 retained handle(s) outstanding -- release them and ask again
```

busy is the structured answer, not a failure, so the exit status stays 0.
release the references and ask again.

in practice the answer is always busy today, and the document says so
rather than letting you discover it: registration puts the module's
functions into the VM's globals, reachable for as long as the VM lives,
so there is always a route to the library's code. physical unloading
waits on deregistration, which does not exist yet; until then the
lifecycle -- loaded, quiescing, busy -- is explicit instead of implicit,
and nothing is ever force-closed. a failed load, by contrast, closes
what it opened: a library whose entry point is missing or whose init
fails is `dlclose`d before the error is reported, because nothing
registered can reach into it.

for untrusted or non-cooperating extensions, run them in a separate
process rather than loading them in-process. this ABI is not a sandbox:
a module runs with the host process's privileges.

## what this is not

a native module runs with the host process's privileges. loading an
untrusted module executes arbitrary code. this ABI is not a sandbox, does
not pretend to be one, and a checksum would establish integrity rather
than safety -- a well-checksummed module can still do anything.

## testing

`tests/native_test.sh` builds real shared objects against the header and
loads them: value conversion in both directions, a native list and table,
an error caught by flint, a retained handle surviving its own call, a
shared object with no entry point, a module built against a version the
host does not implement, and the header compiling on its own with no flint
symbols in sight. `make native-test` runs it.

an extension that only prints a message is not enough to prove an ABI, and
the tests do not settle for one.
