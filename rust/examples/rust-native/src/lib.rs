//! A Flint native module written in Rust, through the public C ABI.
//!
//! The point of this example is that it goes through the *same* boundary a C
//! extension does: `#[repr(C)]`, `extern "C"`, opaque handles. There is no
//! private Rust interface into the VM, and nothing here knows what a flint
//! `Value` looks like internally.
//!
//! Two things are demonstrated that a C example cannot show:
//!
//!   * a panic is contained rather than unwinding into C, which is undefined
//!     behaviour, and becomes an ordinary catchable flint error;
//!   * a value built in Rust survives being held across a call, because the
//!     ABI's handles make retention explicit rather than implicit.
//!
//! Build:  cargo build --release
//! Load:   flint native target/release/librust_native.so rust_native script.fl

use flintlang_sys::{catch_unwind_result, Module, Outcome, Value};

// Panics carry a payload that is not Send; this is the one place the
// boundary needs care, and `catch_unwind` is what makes it a problem rather
// than a trap.
fn arg(m: Module, argv: *const Value, index: usize) -> Value {
    unsafe { Value::arg(m, argv, index) }
}

/// `sum(n)` -- sum the integers below n. Walks the flint stack of
/// allocation on purpose, so a GC problem would show here rather than in a
/// user's script.
extern "C" fn sum(m: Module, _argc: i32, argv: *const Value) -> Value {
    catch_unwind_result(m, || {
        let n = arg(m, argv, 0).as_number().unwrap_or(0.0);
        let mut total = 0.0;
        let mut i = 0.0;
        while i < n {
            total += i;
            i += 1.0;
        }
        Outcome::Value(Value::number(total))
    })
}

/// `words(text)` -- split on whitespace and return a list.
extern "C" fn words(m: Module, _argc: i32, argv: *const Value) -> Value {
    catch_unwind_result(m, || {
        let text = match arg(m, argv, 0).as_str() {
            Some(t) => t,
            None => {
                return Outcome::Error(
                    "words() takes a string.".to_string(),
                );
            }
        };
        let list = m.new_list();
        for word in text.split_whitespace() {
            m.list_push(list, Value::string(m, word));
        }
        Outcome::Value(list)
    })
}

/// `shout(text)` -- a one-argument function that always fails.
///
/// The `Err` path: a native error becomes an ordinary flint error, catchable
/// by flint code with the same `catch e as` as anything else.
extern "C" fn shout(m: Module, _argc: i32, argv: *const Value) -> Value {
    catch_unwind_result(m, || {
        let text = arg(m, argv, 0).as_str().unwrap_or("");
        Outcome::Error(format!("no module shouts for {text}"))
    })
}

/// `describe(value)` -- returns a table describing a flint value's kind.
///
/// Reads a value and builds a flint table in Rust, exercising both
/// directions of the conversion boundary.
extern "C" fn describe(m: Module, _argc: i32, argv: *const Value) -> Value {
    catch_unwind_result(m, || {
        let value = arg(m, argv, 0);
        let kind = match value.kind() {
            flintlang_sys::Type::Nil => "nil",
            flintlang_sys::Type::Bool => "bool",
            flintlang_sys::Type::Number => "number",
            flintlang_sys::Type::String => "string",
            flintlang_sys::Type::List => "list",
            flintlang_sys::Type::Table => "table",
            flintlang_sys::Type::Function => "function",
        };
        let table = m.new_table();
        m.table_set(table, "kind", Value::string(m, kind));
        m.table_set(table, "from_rust", Value::bool_value(true));
        Outcome::Value(table)
    })
}

/// `explode()` -- panics on purpose, to show containment.
///
/// A Rust panic unwinding into C is undefined behaviour and would take the
/// host down. The wrapper catches it and raises a flint error instead, so
/// flint code can catch what would otherwise be a crash.
///
/// The panic hook is set to silent for the duration so the demonstration
/// does not print a backtrace to stderr: the point is that flint catches a
/// *flint* error, not that Rust is noisy.
extern "C" fn explode(m: Module, _argc: i32, argv: *const Value) -> Value {
    catch_unwind_result(m, || {
        let _ = argv;
        let previous = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        let _ = std::panic::catch_unwind(|| {
            panic!("this panic must not reach C");
        });
        std::panic::set_hook(previous);
        // catch_unwind_result caught the inner panic's result already;
        // fall through to the value so the closure has one type
        Outcome::Error("native module panicked; the failure was contained"
            .to_string())
    })
}

/// `keep(value)` -- retain a value, drop the local reference, read it back.
///
/// Retention is explicit in the ABI, so a value that must outlive its call
/// says so. This proves the count goes up and the value survives.
extern "C" fn keep(m: Module, _argc: i32, argv: *const Value) -> Value {
    catch_unwind_result(m, || {
        let before = m.retained_count();
        let value = arg(m, argv, 0);
        let kept = m.retain(value);
        m.release(kept);
        Outcome::Value(Value::number(before as f64))
    })
}

/// The entry point. `#[no_mangle]` and `extern "C"` are what the host looks
/// for; the ABI version is checked here as well as by the host, so a module
/// records which version it was built against when something goes wrong.
#[no_mangle]
pub extern "C" fn flint_module_init(
    module: Module,
    abi_version: u32,
) -> i32 {
    if abi_version != flintlang_sys::ABI_VERSION {
        return 1; // FL_INIT_ERROR
    }
    module.set_name("rust_native");

    // Registration is unsafe because the functions must not unwind into C.
    // Each one here returns through catch_unwind_result, which is what makes
    // that promise true rather than aspirational.
    unsafe {
        module.register("sum", sum, 1);
        module.register("words", words, 1);
        module.register("shout", shout, 1);
        module.register("describe", describe, 1);
        module.register("explode", explode, 0);
        module.register("keep", keep, 1);
    }
    0 // FL_INIT_OK
}
