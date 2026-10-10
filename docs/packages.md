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
content = "9f2d8a1b4c5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f8"

[[packages]]
name = "gitlib"
version = "0.2.0"
source = "file:///tmp/gitlib"
rev = ""
commit = "e62409edc3793e95d58fe7b75ab7ad48cf72472e"
content = "1a2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f809"
```

`content` is the SHA-256 of the installed tree: every regular file by
its path relative to the package directory (sorted, `/`-separated) and
its bytes. Dotfiles, symlinks, and file metadata are excluded, so a git
checkout restores the same bytes -- and the same hash -- on every
machine.

What the hash means depends on the source:

- A git pin is immutable, so its hash is enforced. Reinstalling an
  unchanged pin whose tree is already in place verifies it without
  touching the network or the mirror (`verified gitlib 0.2.0`). A fresh
  materialization whose bytes differ from the recorded hash stops the
  install: the source changed under a pin. The old lock is kept, the
  suspect tree is removed, and `pkg update <name>` re-pins deliberately.
- A path is live source, so its hash is recorded, not enforced: it
  documents exactly what bytes were copied, and the next install
  re-copies regardless.

A lock written before content hashes has no `content` keys. It still
installs; the next install records the hashes. `pkg list` output is
unchanged.

Each dependency lands in `flint_modules/<name>/` with its sources and
its own `flint.toml`. A package can expose its default module as
`main.fl`, as a same-named file such as `levenshtein/levenshtein.fl`,
or as a flat `flint_modules/<name>.fl` file.

```text
flint_modules/
  libfoo/
    flint.toml
    main.fl
  levenshtein/
    flint.toml
    levenshtein.fl
```

## imports and resolution

```flint
import libfoo
print(libfoo.hello())

import levenshtein
print(levenshtein.distance("kitten", "sitting"))

import "gitlib/g.fl"
print(g.greet())
```

A bare name first checks installed packages in `flint_modules/`:
`<name>/main.fl`, then `<name>/<name>.fl`, then the flat
`<name>.fl` layout. If no package matches, the standard library is
checked (so `import math` still imports the library). A quoted path with
a `/` or a `.fl` extension can address a file relative to the importing
file; package-internal paths are also resolved from `flint_modules/`.

## transitive dependencies

a dependency's own `[dependencies]` are followed, recursively. a nested
path is written relative to the package that declares it, so in a project
depending on `../mid`, `mid`'s own `base = { path = "../base" }` means
`../base` relative to `../mid` -- not relative to the project. everything
lands in the same flat `flint_modules/`, because imports are a flat name
space; two packages reaching the same one by different routes install it
once, and two *versions* of one package is a conflict, reported rather
than resolved by picking.

the resolver treats anything below a `flint_modules/` directory as being
inside a project rather than being one, so a package's own `flint.toml`
-- which is there because it was copied -- is not mistaken for the root.

## what is NOT supported

No registry, and no version solving across packages: a requirement is
checked against the one version being installed, and two versions of one
package is reported rather than merged. The lockfile is the source of
truth; delete `flint_modules/` and run `pkg install` to rebuild it.
