# packages

A project declares dependencies in `flint.toml`. `pkg install` puts them
into `flint_modules/`, and imports resolve from there. Paths copy live
source; git URLs clone once into `~/.flint/git` and install pinned
commits, recorded in `flint.lock`. There is no registry.

## flint.toml

```toml
[package]
name = "demo"
version = "0.1.0"

[dependencies]
libfoo = "../libfoo"
libbar = { path = "../libbar", version = "^1.0.0" }
gitlib = { git = "https://example.com/gitlib.git", rev = "v1.0.0" }
```

- `[package]` carries `name` and `version`.
- `[dependencies]` maps a name to a path string, a `{ path, version }`
  table, or a `{ git, rev, version }` table. `rev` may be empty, which
  tracks the default branch; the lock always pins the commit installed.
- Names are the import keys: lowercase letters, digits, `_` and `-`,
  starting with a letter.

## commands

```sh
flint pkg install     # resolve the manifest, write flint.lock
flint pkg add <path|url>   # record a dependency, then install
flint pkg update [name]    # re-resolve git pins, reinstall
flint pkg list        # show what flint.lock installed
```

`pkg add /tmp/libfoo` records `libfoo = { path = "/tmp/libfoo" }` and
installs it. `pkg add file:///tmp/gitlib` records
`gitlib = { git = "file:///tmp/gitlib" }` and pins its commit.

## flint.lock and flint_modules layout

`flint.lock` pins what was installed:

```toml
[[packages]]
name = "libfoo"
version = "0.1.0"
source = "/tmp/libfoo"

[[packages]]
name = "gitlib"
version = "0.2.0"
source = "file:///tmp/gitlib"
rev = ""
commit = "e62409edc3793e95d58fe7b75ab7ad48cf72472e"
```

Each dependency lands in `flint_modules/<name>/` with its sources and
its own `flint.toml`:

```text
flint_modules/
  libfoo/
    flint.toml
    foo.fl
```

## imports and resolution

```flint
import "libfoo/foo.fl"
print(foo.hello())

import "gitlib/g.fl"
print(g.greet())
```

A quoted path with a `/` or a `.fl` extension is a file: relative to the
importing file, except `flint_modules/` is also searched, and packages
resolve before the standard library. A bare name (`import math`) is the
standard library.

## what is NOT supported

No registry, no version solving across packages, no transitive version
ranges beyond the recorded pin. The lockfile is the source of truth;
delete `flint_modules/` and run `pkg install` to rebuild it.
