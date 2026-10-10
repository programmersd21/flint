# flintlang

Safe, idiomatic embedding of the [Flint scripting language](https://github.com/programmersd21/flint) runtime in Rust programs.

```toml
[dependencies]
flintlang = "0.14.0"
```

```rust
let engine = flintlang::Engine::new().expect("out of memory");
engine.run(r#"print("hello from flint")"#, Some("hello.fl")).unwrap();
```

One `Engine` owns one VM: create it, run source on it, drop it. That is
the whole of what is sound today — run source, read the result. Values
are not exchanged across the boundary and Flint functions are not called
by handle, because each of those needs a lifetime story the engine side
does not yet have. A small honest API beats a wide speculative one; the
C header (`include/flint.h` in the Flint repository) states the same
contract.

## Errors

`run` reports the CLI's exit codes, so a host and a script agree: `0`
for success, `64` for a bad call (a NUL byte in the source), `65` for a
compile error, `70` for a runtime error. Diagnostics go to stderr in the
CLI's rendering.

## Threading

`Engine` is `!Send` and `!Sync`, and the crate additionally serializes
every engine operation behind a process-wide lock, because the C runtime
keeps process-global compiler and scanner state. One thread for Flint,
period; a multithreaded host serializes all engine use. A host that
needs parallel execution needs process isolation, not threads.

## Building

The Flint C runtime (minus `main.c`) is vendored under `c-src/` and
compiled by the build script with the system C compiler — C11, no make,
no network, no extra crates. MSVC is refused with a message; Windows
builds target mingw-w64, matching the rest of the project.

## Versions

This crate versions in lockstep with the language: `flintlang 0.14.0`
embeds Flint 0.14.0. The `flintlang-sys` version tracks the wrapper, and
the ABI version (`FL_ABI_VERSION`, currently 1) tracks the C header
independently — a new function does not change it, a changed layout
does.

## License

MIT. Full embedding, lifecycle, and platform documentation lives in the
[Flint repository](https://github.com/programmersd21/flint), start at
`docs/rust.md`.
