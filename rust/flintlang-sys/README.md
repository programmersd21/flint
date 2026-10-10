# flintlang-sys

Safe Rust bindings over the [Flint](https://github.com/programmersd21/flint)
native C ABI (`include/flint.h`) — the module side. Write a native module
in Rust, and Flint loads it with a plain `import`:

```rust
use flintlang_sys::{Module, Value};

unsafe extern "C" fn add(m: Module, argc: i32, argv: *const Value) -> Value {
    let a = Value::arg(m, argv, 0).as_number().unwrap_or(0.0);
    let b = Value::arg(m, argv, 1).as_number().unwrap_or(0.0);
    Value::number(a + b)
}
```

Build it as a `cdylib` named after the module, drop the `.so`/`.dylib`
where Flint looks (beside the script, `flint_modules/`, or the standard
library), and `import mymod` finds it — the same discovery Python uses
for extension modules on `sys.path`.

## Safety boundary

The C ABI is the boundary: `#[repr(C)]` and `extern "C"` only, nothing
of Rust's layout or panic behaviour crosses it. A panicking Rust function
unwinding into C is undefined behaviour, so every entry point here
catches before it returns — use `catch_unwind_result`, which converts a
panic into an ordinary Flint error.

`Value` handles are valid for the duration of the native call that
received them. To keep one past that, `retain` it and `release` it
later; the host roots retained handles against the collector.

## Versions

`flintlang-sys 0.14.0` binds ABI version 1; `abi_version()` asks the
host at run time, which is the check that decides whether a module
loads. The reviewed header copy lives in `c-api/`.

Most programs want [`flintlang`](https://crates.io/crates/flintlang)
instead — the safe embedding API. This crate is for authors of native
modules.

## License

MIT. The ABI contract, module lifecycle, and platform notes live in the
[Flint repository](https://github.com/programmersd21/flint), start at
`docs/native-abi.md`.
