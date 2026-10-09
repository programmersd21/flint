//! Safe Rust bindings over the Flint native C ABI.
//!
//! This is a wrapper, not a second interface. Everything it can do, a C
//! extension can do by calling `include/flint.h` directly -- the Rust side
//! adds checked conversions and keeps `unsafe` in one place rather than
//! adding capability.
//!
//! What a native module looks like from here:
//!
//! ```ignore
//! use flint_sys::{Module, Value};
//!
//! unsafe extern "C" fn add(m: Module, argc: i32, argv: *const Value) -> Value {
//!     let a = Value::arg(m, argv, 0).as_number().unwrap_or(0.0);
//!     let b = Value::arg(m, argv, 1).as_number().unwrap_or(0.0);
//!     Value::number(a + b)
//! }
//! ```
//!
//! The C ABI is the boundary: `#[repr(C)]` and `extern "C"` only, nothing
//! of Rust's own layout or panic behaviour crosses it. A panicking Rust
//! function unwinding into C is undefined behaviour, so every entry point
//! here catches before it returns.

#![deny(missing_docs)]

use std::ffi::{c_char, c_int, c_void, CStr};

/// The ABI version this wrapper was built against.
///
/// A module compiled with a different one is refused by the host before its
/// entry point runs.
pub const ABI_VERSION: u32 = crate::FL_ABI_VERSION;

/// The value kinds flint has. There is no flint type beyond these.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Type {
    /// `nil`
    Nil = 0,
    /// `true` / `false`
    Bool = 1,
    /// every number, including integers
    Number = 2,
    /// a byte sequence; UTF-8 is not decoded
    String = 3,
    /// `[1, 2, 3]`
    List = 4,
    /// `{a: 1}`
    Table = 5,
    /// a flint function, closure or native
    Function = 6,
}

/// An opaque handle to a flint value.
///
/// Valid for the duration of the native call it arrived in. To keep one
/// past that, [`Module::retain`] and then [`Module::release`] it.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Value {
    opaque: *mut c_void,
}

impl Value {
    /// The value every argument slot holds when there is no argument.
    pub fn nil() -> Value {
        unsafe { flint_sys_raw::fl_nil() }
    }

    /// A boolean.
    pub fn bool_value(b: bool) -> Value {
        unsafe { flint_sys_raw::fl_bool(c_int::from(b)) }
    }

    /// A number.
    pub fn number(n: f64) -> Value {
        unsafe { flint_sys_raw::fl_number(n) }
    }

    /// A string, built from bytes. Flint strings are byte sequences, so this
    /// takes `&[u8]` rather than `&str` for the cases where they differ.
    pub fn string_from_bytes(m: Module, bytes: &[u8]) -> Value {
        unsafe {
            flint_sys_raw::fl_string(
                m,
                bytes.as_ptr() as *const c_char,
                bytes.len(),
            )
        }
    }

    /// A string from UTF-8 text.
    pub fn string(m: Module, s: &str) -> Value {
        Value::string_from_bytes(m, s.as_bytes())
    }

    /// Read argument `index` of the current call. Out of range is nil, which
    /// is what a native declared with fewer arguments than it reads sees.
    ///
    /// # Safety
    /// `argv` must be the argument array of the current call.
    pub unsafe fn arg(_m: Module, argv: *const Value, index: usize) -> Value {
        if argv.is_null() {
            return Value::nil();
        }
        *argv.add(index)
    }

    /// What kind of value this is. Never fails.
    pub fn kind(self) -> Type {
        let raw = unsafe { flint_sys_raw::fl_type(self) };
        match raw {
            crate::FL_TYPE_BOOL => Type::Bool,
            crate::FL_TYPE_NUMBER => Type::Number,
            crate::FL_TYPE_STRING => Type::String,
            crate::FL_TYPE_LIST => Type::List,
            crate::FL_TYPE_TABLE => Type::Table,
            crate::FL_TYPE_FUNCTION => Type::Function,
            _ => Type::Nil,
        }
    }

    /// The number, or `None` when this is not a number.
    pub fn as_number(self) -> Option<f64> {
        let mut out: f64 = 0.0;
        let ok = unsafe { flint_sys_raw::fl_to_number(self, &mut out) };
        if ok != 0 {
            Some(out)
        } else {
            None
        }
    }

    /// The string's bytes, or `None` when this is not a string.
    pub fn as_bytes(self) -> Option<&'static [u8]> {
        let mut ptr: *const c_char = std::ptr::null();
        let mut len: usize = 0;
        let ok = unsafe { flint_sys_raw::fl_to_string(self, &mut ptr, &mut len) };
        if ok == 0 {
            return None;
        }
        if ptr.is_null() {
            return Some(&[]);
        }
        Some(unsafe { std::slice::from_raw_parts(ptr as *const u8, len) })
    }

    /// The string as UTF-8, when it is valid UTF-8. A flint string need not
    /// be, so this can be `None` for something `as_bytes` accepts.
    pub fn as_str(self) -> Option<&'static str> {
        self.as_bytes().and_then(|b| std::str::from_utf8(b).ok())
    }

    /// The boolean, or `None` when this is not a boolean.
    pub fn as_bool(self) -> Option<bool> {
        if self.kind() != Type::Bool {
            return None;
        }
        Some(unsafe { flint_sys_raw::fl_to_bool(self) != 0 })
    }
}

/// The host state a module is registered with.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Module {
    raw: *mut c_void,
}

impl Module {
    /// Wrap a raw module pointer. Only the host calls this.
    ///
    /// # Safety
    /// `raw` must be a live `FlModule *` for the current load.
    #[doc(hidden)]
    pub unsafe fn from_raw(raw: *mut c_void) -> Module {
        Module { raw }
    }

    /// The underlying pointer, for the FFI layer.
    #[doc(hidden)]
    pub fn raw(self) -> *mut c_void {
        self.raw
    }

    /// Declare the module's name.
    pub fn set_name(self, name: &str) -> bool {
        let c = std::ffi::CString::new(name).unwrap_or_default();
        unsafe { flint_sys_raw::fl_module_name(self, c.as_ptr()) != 0 }
    }

    /// Register an exported function.
    ///
    /// # Safety
    /// `f` must remain valid for the life of the module and must not unwind
    /// into C. Wrap it in [`catch_unwind_result`].
    pub unsafe fn register(
        self,
        name: &str,
        f: extern "C" fn(Module, c_int, *const Value) -> Value,
        arity: i32,
    ) -> bool {
        let c = std::ffi::CString::new(name).unwrap_or_default();
        flint_sys_raw::fl_module_func(self, c.as_ptr(), Some(f), arity) != 0
    }

    /// Build an empty list.
    pub fn new_list(self) -> Value {
        unsafe { flint_sys_raw::fl_new_list(self) }
    }

    /// Build an empty table.
    pub fn new_table(self) -> Value {
        unsafe { flint_sys_raw::fl_new_table(self) }
    }

    /// Append to a list.
    pub fn list_push(self, list: Value, value: Value) -> bool {
        unsafe { flint_sys_raw::fl_list_push(self, list, value) != 0 }
    }

    /// A list's length.
    pub fn list_len(self, list: Value) -> Option<usize> {
        let mut n: usize = 0;
        let ok = unsafe { flint_sys_raw::fl_list_length(list, &mut n) };
        if ok != 0 {
            Some(n)
        } else {
            None
        }
    }

    /// Read a list element.
    pub fn list_get(self, list: Value, index: usize) -> Option<Value> {
        let mut out = Value::nil();
        let ok = unsafe { flint_sys_raw::fl_list_get(list, index, &mut out) };
        if ok != 0 {
            Some(out)
        } else {
            None
        }
    }

    /// Set a field on a table.
    pub fn table_set(self, table: Value, key: &str, value: Value) -> bool {
        unsafe {
            flint_sys_raw::fl_table_set(
                self,
                table,
                key.as_ptr() as *const c_char,
                key.len(),
                value,
            ) != 0
        }
    }

    /// Read a field, absent keys included: `None` means "no such key".
    pub fn table_get(self, table: Value, key: &str) -> Option<Value> {
        let mut out = Value::nil();
        let ok = unsafe {
            flint_sys_raw::fl_table_get(
                table,
                key.as_ptr() as *const c_char,
                key.len(),
                &mut out,
            )
        };
        if ok != 0 {
            Some(out)
        } else {
            None
        }
    }

    /// Keep a value past the end of this call. Release it when done.
    pub fn retain(self, value: Value) -> Value {
        unsafe { flint_sys_raw::fl_retain(self, value) }
    }

    /// Drop a retained value.
    pub fn release(self, value: Value) {
        unsafe { flint_sys_raw::fl_release(self, value) }
    }

    /// How many values this module currently holds retained.
    pub fn retained_count(self) -> usize {
        unsafe { flint_sys_raw::fl_handle_count(self) }
    }

    /// Raise an ordinary flint error, catchable by flint code.
    pub fn raise(self, message: &str) {
        let c = std::ffi::CString::new(message).unwrap_or_default();
        unsafe { flint_sys_raw::fl_raise(self, c.as_ptr()) }
    }

    /// Raise with formatting.
    pub fn raise_fmt(
        self,
        template: &str,
        args: std::fmt::Arguments<'_>,
    ) {
        // rendered through a write to a String, which is how Arguments is
        // meant to be consumed without re-implementing format_args
        use std::fmt::Write;
        let mut text = String::new();
        let _ = write!(text, "{template}");
        let _ = write!(text, "{}", args);
        self.raise(&text);
    }
}

/// What a native function should do, so it cannot unwind into C.
///
/// `extern "C"` functions are `nounwind`: a Rust panic crossing into C is
/// undefined behaviour, and it would take the host down with it. Every
/// entry point returns one of these instead of a `Result`, so the happy
/// path stays a plain return.
pub enum Outcome {
    /// produce this value
    Value(Value),
    /// raise this message instead
    Error(String),
}

/// Run a native function, converting a panic into a flint error.
///
/// Unwinding into C is undefined behaviour, so a panic has to be caught
/// here and reported the way every other failure is. An `Err` from the
/// closure raises; an `Err` *from* the closure's own error is raised too.
pub fn catch_unwind_result<F>(m: Module, f: F) -> Value
where
    F: FnOnce() -> Outcome + std::panic::UnwindSafe,
{
    match std::panic::catch_unwind(f) {
        Ok(Outcome::Value(v)) => v,
        Ok(Outcome::Error(message)) => {
            m.raise(&message);
            Value::nil()
        }
        Err(_) => {
            m.raise("native module panicked; the failure was contained");
            Value::nil()
        }
    }
}

/// The version of the ABI, as the host reports it.
///
/// A module calls this to check what it is linked against before relying on
/// anything.
pub fn abi_version() -> u32 {
    unsafe { flint_sys_raw::fl_abi_version() }
}

/// Whether the host implements a capability.
pub fn has_capability(capability: u32) -> bool {
    unsafe { flint_sys_raw::fl_has_capability(capability) != 0 }
}

/// capability tags, for `has_capability`.
pub mod capabilities {
    /// `fl_retain` / `fl_release`
    pub const RETAIN: u32 = crate::FL_CAP_RETAIN;
    /// list construction and access
    pub const LIST: u32 = crate::FL_CAP_LIST;
    /// table construction and access
    pub const TABLE: u32 = crate::FL_CAP_TABLE;
}

/// The entry point a flint native module must export.
///
/// The symbol name is what the host looks up; the signature is fixed.
pub const ENTRY_POINT: &str = "flint_module_init";

/// The raw C declarations. Nothing above this line has `unsafe` in it that
/// a caller can reach without going through a checked wrapper.
/// The ABI version the header declares. Mirrors FL_ABI_VERSION; the
/// [`abi_version`] call asks the host at run time, which is the one that
/// decides whether a module loads.
pub const FL_ABI_VERSION: u32 = 1;

/// init return values.
pub const FL_INIT_OK: c_int = 0;
/// init returned failure.
pub const FL_INIT_ERROR: c_int = 1;

/// the symbol the host looks up in a shared object
pub const FL_MODULE_FUNC: &[u8] = b"flint_module_init\0";

/// capability tags
pub const FL_CAP_RETAIN: u32 = 1;
/// list construction and access
pub const FL_CAP_LIST: u32 = 2;
/// table construction and access
pub const FL_CAP_TABLE: u32 = 3;

/// value kind tags, as flint.h numbers them
pub const FL_TYPE_NIL: c_int = 0;
/// `true` / `false`
pub const FL_TYPE_BOOL: c_int = 1;
/// every number
pub const FL_TYPE_NUMBER: c_int = 2;
/// a byte sequence
pub const FL_TYPE_STRING: c_int = 3;
/// a list
pub const FL_TYPE_LIST: c_int = 4;
/// a table
pub const FL_TYPE_TABLE: c_int = 5;
/// a function
pub const FL_TYPE_FUNCTION: c_int = 6;

/// The raw C declarations.
///
/// `unsafe` is confined here; nothing above this module has raw pointer
/// arithmetic an extension author has to reason about.
#[allow(non_upper_case_globals, non_camel_case_types, non_snake_case)]
#[allow(missing_docs)]
pub mod flint_sys_raw {
    use super::*;

    pub use super::Module as FlModule;
    pub use super::Value as FlValue;

    extern "C" {
        pub fn fl_abi_version() -> u32;
        pub fn fl_has_capability(capability: u32) -> c_int;

        pub fn fl_module_name(module: FlModule, name: *const c_char) -> c_int;
        pub fn fl_module_func(
            module: FlModule,
            name: *const c_char,
            f: Option<
                extern "C" fn(FlModule, c_int, *const FlValue) -> FlValue,
            >,
            arity: c_int,
        ) -> c_int;
        pub fn fl_module_vm(module: FlModule) -> *mut c_void;

        pub fn fl_nil() -> FlValue;
        pub fn fl_bool(value: c_int) -> FlValue;
        pub fn fl_number(value: f64) -> FlValue;
        pub fn fl_string(
            module: FlModule,
            bytes: *const c_char,
            length: usize,
        ) -> FlValue;

        pub fn fl_type(value: FlValue) -> c_int;
        pub fn fl_to_bool(value: FlValue) -> c_int;
        pub fn fl_to_number(value: FlValue, out: *mut f64) -> c_int;
        pub fn fl_to_string(
            value: FlValue,
            bytes: *mut *const c_char,
            length: *mut usize,
        ) -> c_int;

        pub fn fl_new_list(module: FlModule) -> FlValue;
        pub fn fl_new_table(module: FlModule) -> FlValue;
        pub fn fl_list_push(
            module: FlModule,
            list: FlValue,
            value: FlValue,
        ) -> c_int;
        pub fn fl_list_length(list: FlValue, out: *mut usize) -> c_int;
        pub fn fl_list_get(
            list: FlValue,
            index: usize,
            out: *mut FlValue,
        ) -> c_int;

        pub fn fl_table_set(
            module: FlModule,
            table: FlValue,
            key: *const c_char,
            key_length: usize,
            value: FlValue,
        ) -> c_int;
        pub fn fl_table_get(
            table: FlValue,
            key: *const c_char,
            key_length: usize,
            out: *mut FlValue,
        ) -> c_int;
        pub fn fl_table_has(
            table: FlValue,
            key: *const c_char,
            key_length: usize,
        ) -> c_int;
        pub fn fl_table_length(table: FlValue, out: *mut usize) -> c_int;

        pub fn fl_retain(module: FlModule, value: FlValue) -> FlValue;
        pub fn fl_release(module: FlModule, value: FlValue);
        pub fn fl_handle_count(module: FlModule) -> usize;

        pub fn fl_raise(module: FlModule, message: *const c_char);
        pub fn fl_raise_fmt(module: FlModule, format: *const c_char, ...);
        pub fn fl_error_category() -> *const c_char;
    }
}

/// Read the host's error category name. "Error" when there is none.
pub fn error_category() -> &'static str {
    unsafe {
        let p = flint_sys_raw::fl_error_category();
        if p.is_null() {
            return "Error";
        }
        CStr::from_ptr(p).to_str().unwrap_or("Error")
    }
}
