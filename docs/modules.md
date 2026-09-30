# modules

A module is a file run in the current VM. Its globals go in the same table as
the importer's. `export` marks intent; it does not hide names.

```flint
# math.fl
export fn square(x) { return x * x }
```

```flint
# main.fl
import "math.fl"
print(square(6))
```

## imports

An import runs when execution reaches it. The resolved path is cached, so a
successful import runs once per VM. Importing the file again does nothing.
Imports inside a function wait until that function runs.

Relative paths are resolved against the importing file's directory. A script
run as `project/main.fl` can import `lib/math.fl` from any working directory.
With `-e` or stdin there is no source file, so relative imports use the current
working directory. Absolute paths are accepted as written. There is no search
path and no canonicalization of symlinks; two spellings of one file can load
it twice.

An import cycle is detected by the resolved path while the module is loading.
The VM reports the cycle instead of spending 256 frames proving it exists.

## names and failures

Every file shares one global table. A module can read and overwrite names from
any other file. `export` is not access control, and duplicate global names are
resolved by execution order.

If a module fails, its error is reported and the importing script continues.
Globals written before the failure remain. The failed path is removed from the
module cache, so a later import can try again. That retry starts with the
partial globals still in place.

`const` belongs to the global binding. Once a module defines a constant, every
file sees the same read-only name.

## bare names and the standard library

a bare name (no quotes, no path separator) imports a standard library module:

```flint
import math
import json
import fs
```

the interpreter searches in this order:

1. `FLINT_STDLIB` environment variable — if set, look for `<name>.fl` there
2. `<exe-dir>/lib` — the `lib/` directory next to the flint binary
3. `~/.flint/stdlib` — a per-user fallback

the first match wins. if none match, the import fails with a message naming
the paths tried.

shipping the standard library is as simple as copying `lib/` next to the
binary. the interpreter finds it without configuration.

```sh
cp -r lib/ /usr/local/lib/flint
cp flint /usr/local/bin/
```

or point the environment variable:

```sh
export FLINT_STDLIB=/opt/flint/lib
```

a bare import does not inhibit relative imports. both forms work in the same
file.

## the standard library modules

| module | provides |
|---|---|
| `math` | sin, cos, tan, exp, log, sqrt, floor, ceil, ... and PI, E, TAU |
| `random` | rand, rand_int, rand_float, shuffle, choice, seed |
| `time` | now, clock_ms, sleep, format, measure |
| `fs` | read, write, append, exists, remove, mkdir, isdir |
| `path` | join, dir, base, ext, abs, strip_ext, sep |
| `collections` | reverse, contains, min, max, sum, flatten, zip, uniq |
| `json` | parse, stringify, pretty |

see [library.md](library.md) for the full reference.
