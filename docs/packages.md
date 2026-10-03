# Flint 1.0.0 Packages & Toolchain

Flint 1.0.0 features integrated project management and toolchain commands.

## Package Manifest (`flint.toml`)
```toml
[package]
name = "my_app"
version = "1.0.0"

[dependencies]
```

## Toolchain Commands
- `flint run`: Execute scripts or project entry point.
- `flint check`: Run static verification and linting.
- `flint test`: Discover and run test suites.
- `flint fmt`: Format Flint source files deterministically.
- `flint lint`: Report potential bugs, dead code, and style issues.
- `flint sync`: Synchronize standard library files.
