# Packages

0.8.0 does not have a package manager, a registry or a lockfile. This file
stays as the place those will be documented once they exist, and describes
only what is true today.

Today the only ways to get code into a script are:

* `import "lib/util.fl"` for a source-relative module file
* a standard-library module by name from the usual lookup path
  (`$FLINT_STDLIB`, `<exe-dir>/lib`, `~/.flint/stdlib`)
* `flint sync` to refresh the installed copy of the standard library

A future `flint.toml` / `flint.lock` / `flint install` workflow is planned,
but nothing here should be quoted in an issue or wiki page as if it exists.
