# rust

Two crates, two directions across the same C ABI (`include/flint.h`):

| crate | direction | what it is |
|---|---|---|
| [`flint-sys`](../rust/flint-sys) | guest | write a native module in Rust, loaded by `flint native` |
| [`flint`](../rust/flint) | host | embed the Flint runtime in a Rust program |

Nothing is rewritten in Rust. Both crates call the C runtime; the
difference is which side of the boundary they stand on.

## embedding: the `flint` crate

```rust
let engine = flint::Engine::new().expect("out of memory");
engine.run(r#"print("hello from flint")"#, Some("hello.fl")).unwrap();
```

`Engine::new` creates one VM. `run` compiles and runs source on it, as
if it were a file named `name` (`None` reads as `"<source>"`). State
persists across calls: a `let` in one run is visible to the next,
because an engine is one VM, not one execution.

Errors are the CLI's exit codes as an enum: `Compile` (65), `Runtime`
(70), `OutOfMemory`, `NulInSource` for an interior NUL byte, `Usage`
(64, unreachable through this API), and `Unknown(code)` for anything a
future runtime adds. Diagnostics go to stderr in the CLI's rendering;
the return carries the code.

What the crate does not do is exchange values with the host. The C ABI
has no value handles on the engine side yet, and a wrapper that
invented its own handle lifetime would be unsound. Run source, read the
result.

### building

```sh
cd rust && cargo test --workspace
```

The crate links the C runtime statically. Its build script runs
`make libflint.a` in the repository root, so a C11 toolchain and `make`
are build requirements. `FLINT_LIB_DIR` points at a prebuilt
`libflint.a` instead; `FLINT_NO_BUILD=1` refuses to run `make`.

```sh
cargo run --example embed -- 'print(2 + 2)'
```

The `embed` example mirrors exit codes: 0, 65, 70.

### threading

`Engine` is `!Send` and `!Sync`: one engine per thread, never moved
between threads. Two engines on two threads are additionally
serialized inside the crate by a process-wide lock, because the C
runtime keeps compiler and scanner state outside any engine. Engines
stay independent in state; they just never run at the same time. A
host that needs parallel Flint execution needs process isolation, not
threads.

## guest modules: the `flint-sys` crate

`flint-sys` is the module side: opaque `Value` and `Module` handles,
checked conversions, retain/release, and `catch_unwind_result`, which
turns a Rust panic into an ordinary catchable Flint error instead of
unwinding into C (undefined behaviour).

```rust
use flint_sys::{catch_unwind_result, Module, Outcome, Value};

extern "C" fn double(m: Module, _argc: i32, argv: *const Value) -> Value {
    catch_unwind_result(m, || {
        let n = unsafe { Value::arg(m, argv, 0) }.as_number().unwrap_or(0.0);
        Outcome::Value(Value::number(n * 2.0))
    })
}
```

Values are valid for the duration of the call they arrived in; keep
one longer with `Module::retain` and `Module::release` it when done.
The full example is in
[`rust/examples/rust-native`](../rust/examples/rust-native); the
terminal-UI example using `ratatui` is in
[`rust/examples/tui-native`](../rust/examples/tui-native).

## versioning

The `flint` crate's version tracks the language release (0.13.0); the
`flint-sys` version tracks the wrapper, and the ABI version
(`fl_abi_version`, currently 1) is checked independently of both. A
module built against a different ABI version is refused before its
entry point runs.
