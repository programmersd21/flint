# modules

A module is a file with its own namespace. Importing one binds **one name**
to its exports, and nothing else is visible.

```flint
# geometry.fl
export fn area(w, h) { return w * h }
```

```flint
# main.fl
import "geometry.fl"
print(geometry.area(3, 4))     # 12
```

This is the change from v0.5.0, and it is a breaking one. Before, every file
shared one global table and an import dumped the module's names into the
importer. `import math` meant `math.floor` by accident of the loader poking a
name in, and `import "geometry.fl"` meant a bare `area` as well.

## why it changed

Two modules could not both have a private helper of the same name. This
worked:

```flint
# geometry.fl
let scale = 2
export fn area(r) { return 3 * r * r * scale }

# display.fl
let scale = 10
export fn show(v) { return v * scale }
```

```flint
import "geometry.fl"
import "display.fl"
print(geometry.area(2))
```

It printed **120**, not 24. Both modules wrote `scale` into one table,
`display.fl` loaded second, and `geometry.area` silently used 10. No error, no
warning -- just a wrong number, which is the worst kind of bug a language can
have.

Now `scale` is private to each module and the answer is 24.

## exports

A top-level `let`, `const` or `fn` is **private** to its module unless it is
marked `export`.

```flint
# shapes.fl
let tax = 0.2                 # private
export fn taxed(amount) {     # public
    return amount * (1 + tax)
}
```

```flint
import "shapes"
print(shapes.taxed(10))      # 12
print(shapes.tax)            # nil -- the private name is not in the table
```

`export` is applied at compile time now. In v0.5.0 it was a comment, and the
module's "exports" were discovered by diffing the global table before and
after it ran -- which cannot tell a helper from a public function, because
both are just a name that appeared.

A module cannot see its importer's names either, and an importer cannot see
its modules' private names. That is what makes "private" mean private.

## naming

The name an import binds:

| written | binds | you write |
|---|---|---|
| `import math` | `math` | `math.floor(2)` |
| `import json` | `json` | `json.parse(text)` |
| `import "lib/geometry.fl"` | `geometry` | `geometry.area(2)` |
| `import "util.fl" as u` | `u` | `u.helper()` |
| `import "a/b/c.fl"` | `c` | `c.name` |

The default is the last path component with the extension removed, which is
what a quoted import already spelled in practice. `as` is for when that is
wrong:

```flint
import "lib/geometry/circle.fl" as geometry
import "../shared/util.fl" as util
```

## resolution

A bare name with no quotes and no `/` is a standard library module. The
search order is:

1. `$FLINT_STDLIB` -- if set, look for `<name>.fl` there
2. `<exe-dir>/lib` -- the `lib/` directory next to the flint binary
3. `~/.flint/stdlib` -- a per-user fallback

A quoted path is a file. Relative paths resolve against the **importing
file's** directory, not the process working directory, so a script run from
anywhere works. With `-e` or stdin there is no source file, so a relative
path uses the working directory.

There is no search path for quoted imports and no symlink canonicalisation.
Two spellings of one file load it twice, which is also why a module that has
already loaded is cheap to import again: the cache is keyed by resolved path.

Keeping the standard library current is `flint sync`: it downloads the
modules from the project's github and installs them into `~/.flint/stdlib`,
compiling each one before it is allowed to replace the old file. `--ref`
pins to a release tag and `--url` points at a mirror. A library needing a
builtin this binary does not have fails to compile, and sync reports that
as "this stdlib is newer than your flint" rather than installing half a
module.

## repeated imports

A module runs once. Importing it again is a lookup, not a second run, and
both callers get the same table.

```flint
import "shapes"
import "shapes"
print(shapes.taxed(10))   # 12, and the module ran once
```

That also means module-level state is per-run, not per-import. A module that
caches something in a private global keeps it.

## unused imports

An import whose name is never read is a compile error.

```flint
import math
print("hello")
```

```
[line 1] Error at 'math': imported 'math' but never used. remove the import,
or use it.
```

An import always runs its file, so an unused one is dead code with a side
effect -- the kind that surprises people when the module prints, defines, or
fails. Either use the binding or delete the line.

The one exception is importing a module for its failure: a test that imports
a file it knows will throw, to check that the importer survives, cannot use
the binding because there is nothing to use. Say so explicitly:

```flint
import "../modules/mod_closing.fl" as _
```

`_` opts out of the check. Anything else that is never read is an error, and
the REPL is exempt -- each submission compiles separately, so `import math`
on one line and `math.floor(2)` on the next is the normal way to work
interactively.

## using a module without importing it

Reading a name that was never defined, where a module file with the matching
shape exists, suggests the import rather than guessing at a typo:

```flint
print(math.floor(2))
```

```
error[E0202]: undefined variable 'math'.
 --> <command line>:1:7
  |
1 | print(math.floor(2))
  |       ^~~~ undefined name
  |
  = help: did you forget to `import math`?
```

An exact hit on a real file beats a fuzzy match, so this replaces the "did
you mean" suggestion rather than adding a second one. It covers the standard
library and sibling files beside the importing one.

## circular imports

Detected while the module is in flight, and reported with the path:

```
import cycle: 'a.fl' is already being loaded.
```

Nothing partial runs. `a.fl` importing `b.fl` importing `a.fl` fails at the
second `a.fl`, and `b.fl` never finishes.

## failed imports

Transactional. A module that fails for any reason -- missing file, read
error, compile error, runtime error at its top level -- binds **nothing**:

```flint
# broken.fl
let good = 1
let bad = 1 / 0        # fails here
```

```flint
import "broken.fl"
print(good)            # runtime error: undefined variable 'good'
```

In v0.5.0 `good` survived, and a second import would retry against those
partial bindings.

The module's own environment is kept rather than freed, though it is
unreachable. It may have handed out a closure that the importer still holds,
and freeing it would turn every later call into a use-after-free. The
collector decides when it actually goes.

A failed module is remembered as failed. A later import of the same path
reports that rather than retrying a failure nothing has changed.

## module state and the collector

A module's environment is kept alive as long as anything can reach it, which
is as long as the VM lives. Two modules is two tables, and a program importing
N modules pays for N tables. That is the honest cost of N modules existing.

## migration from v0.5.0

The whole change is: qualify module names.

```flint
# before                          # after
import "helper.fl"                import "helper.fl"
print(square(6))                  print(helper.square(6))
print(LIMIT)                      print(helper.LIMIT)

import math                       import math
print(floor(1.7))                 print(math.floor(1.7))
print(PI)                         print(math.PI)
```

A bare name that used to come from a module is now either private to that
module -- in which case it is not reachable and the program is wrong -- or it
needs qualifying.

`import math` and the standard library are unaffected in shape: they already
meant `math.floor`, so only the ones that were relying on the old flat
behaviour change.
