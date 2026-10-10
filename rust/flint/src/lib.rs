//! Safe Rust embedding of the Flint scripting language.
//!
//! An [`Engine`] owns one Flint VM: create it, run source on it, drop it.
//! Diagnostics go to stderr in the same rendering as the CLI; the result
//! tells you which of the CLI's exit codes the run produced.
//!
//! ```rust
//! let engine = flint::Engine::new().expect("out of memory");
//! engine.run(r#"print("hello from flint")"#, Some("hello.fl")).unwrap();
//! ```
//!
//! What this crate does not do is exchange values with the host: run
//! source, read the result. The C ABI has no value handles on the engine
//! side yet, and a wrapper that invented its own handle lifetime would be
//! unsound. A small honest API beats a wide speculative one.
//!
//! ## Threading
//!
//! [`Engine`] is `!Send` and `!Sync`, on purpose: one engine per thread,
//! and an engine never moves between threads. The C header requires that,
//! and the type enforces it.
//!
//! Two engines on two threads are additionally serialized inside this
//! crate by a process-wide lock. That lock exists because the C runtime
//! keeps process-global compiler and scanner state (`CompilerState` and
//! the scanner in `src/frontend/`), so two threads compiling at once --
//! even on independent engines -- would race. Independent engines stay
//! independent in *state*; they just never run at the same *time*. A host
//! that needs parallel Flint execution needs process isolation, not
//! threads.
//!
//! ## Linking
//!
//! The crate links the C runtime statically. Building it runs
//! `make libflint.a` in the repository root, so a C11 toolchain and
//! `make` are build requirements; `FLINT_LIB_DIR` points at a prebuilt
//! `libflint.a` instead, and `FLINT_NO_BUILD=1` refuses to run `make`.

#![deny(missing_docs)]

use std::ffi::{c_char, c_int, c_void, CString};
use std::ptr::NonNull;

/// The ABI version this crate was built against, taken from `flint-sys`
/// rather than repeated here so the two cannot disagree.
pub const ABI_VERSION: u32 = flint_sys::ABI_VERSION;

/// The runtime process-global lock. See the crate documentation: the C
/// side keeps compiler and scanner state outside any engine, so every
/// engine operation -- create, run, free -- holds this while it calls
/// in. Contention is a non-issue in practice: engine calls are coarse
/// (a whole compilation, a whole run), and correctness is not for sale.
static PROCESS_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// Hold the process lock. Poisoning is recovered rather than propagated:
/// nothing inside the critical section can panic (the FFI calls return
/// error codes; there is no unwinding across them), so a poisoned lock
/// means a bug elsewhere, not a violated invariant here.
fn hold_process_lock() -> std::sync::MutexGuard<'static, ()> {
    PROCESS_LOCK
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

/// The ABI version the linked runtime reports.
///
/// Equal to [`flint_sys::ABI_VERSION`]; a mismatch means the static
/// library and the headers disagree, and nothing should run.
pub fn abi_version() -> u32 {
    unsafe { raw::fl_abi_version() }
}

/// Whether the linked runtime implements a capability tag.
///
/// See [`flint_sys::capabilities`].
pub fn has_capability(capability: u32) -> bool {
    unsafe { raw::fl_has_capability(capability) != 0 }
}

/// What running source can report.
///
/// The variants mirror the CLI's exit codes, so a host and a script
/// agree on what happened.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// `fl_engine_new` returned NULL: the process is out of memory.
    OutOfMemory,
    /// Source or name contained an interior NUL byte, which C strings
    /// cannot carry. The engine was not touched.
    NulInSource,
    /// The engine or source pointer was NULL (exit code 64). Unreachable
    /// through this API, which never passes NULL; kept so a future host
    /// that does is diagnosable rather than silent.
    Usage,
    /// The source did not compile (exit code 65). The diagnostic is on
    /// stderr, rendered as the CLI renders it.
    Compile,
    /// The program compiled and then failed at run time (exit code 70).
    Runtime,
    /// Any other exit code. A future runtime may add one; matching on a
    /// number you do not know beats a panic in a version check.
    Unknown(c_int),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::OutOfMemory => write!(f, "out of memory creating the engine"),
            Error::NulInSource => {
                write!(f, "source or name contains a NUL byte")
            }
            Error::Usage => write!(f, "invalid engine or source (exit 64)"),
            Error::Compile => write!(f, "compile error (exit 65)"),
            Error::Runtime => write!(f, "runtime error (exit 70)"),
            Error::Unknown(code) => {
                write!(f, "flint exited with code {code}")
            }
        }
    }
}

impl std::error::Error for Error {}

/// One Flint VM, with its globals, collector, and loaded modules.
///
/// Created with [`Engine::new`], used with [`Engine::run`], released by
/// dropping. Not `Send` or `Sync`: see the crate documentation.
pub struct Engine {
    // Never NULL after `new` checks; owned, freed exactly once by Drop.
    // `NonNull<c_void>` is `!Send + !Sync`, which is the confinement the
    // C header requires, expressed in the type rather than in prose.
    raw: NonNull<c_void>,
}

impl Engine {
    /// Create an engine.
    ///
    /// One heap allocation and one VM initialisation. Fails only when
    /// out of memory.
    pub fn new() -> Result<Engine, Error> {
        // Safe: `fl_engine_new` has no preconditions, and NULL is its
        // only failure signal, which is checked before wrapping. The
        // process lock is held because initialisation shares the C
        // runtime's global state with every other engine operation.
        let _held = hold_process_lock();
        let raw = unsafe { raw::fl_engine_new() };
        match NonNull::new(raw) {
            Some(raw) => Ok(Engine { raw }),
            None => Err(Error::OutOfMemory),
        }
    }

    /// Compile and run `source`, as if it were a file named `name`.
    ///
    /// `name` appears in diagnostics; `None` reads as `"<source>"`.
    /// State persists across calls -- a `let` in one run is visible to
    /// the next -- because an engine is one VM, not one execution.
    ///
    /// Diagnostics go to stderr; the return carries the exit code as an
    /// [`Error`]. Ordinary use needs no `unsafe`: the only failure this
    /// function can produce before touching the engine is an interior
    /// NUL byte, reported as [`Error::NulInSource`].
    pub fn run(&self, source: &str, name: Option<&str>) -> Result<(), Error> {
        let source = CString::new(source).map_err(|_| Error::NulInSource)?;
        let name = name
            .map(CString::new)
            .transpose()
            .map_err(|_| Error::NulInSource)?;
        let name_ptr = name.as_ref().map_or(std::ptr::null(), |n| n.as_ptr());
        // Safe: `raw` is a live engine this struct owns (Drop has not
        // run: `&self` borrows it), both pointers are non-NULL and
        // NUL-terminated, and the call takes no callback -- no Rust
        // state crosses into C, so nothing can unwind or dangle.
        // `&self` rather than `&mut self` is sound because aliasing is
        // confined by `!Sync` within a thread and by the process lock
        // across threads: no two engine calls overlap in time.
        let _held = hold_process_lock();
        let code = unsafe { raw::fl_engine_run(self.raw.as_ptr(), source.as_ptr(), name_ptr) };
        match code {
            0 => Ok(()),
            64 => Err(Error::Usage),
            65 => Err(Error::Compile),
            70 => Err(Error::Runtime),
            other => Err(Error::Unknown(other)),
        }
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        // Safe: `raw` came from a successful `fl_engine_new`, is freed
        // exactly once here (the struct owns it and is not Clone), and
        // the C function is safe on any engine it created. Locked like
        // every other engine operation: shutdown shares global state.
        let _held = hold_process_lock();
        unsafe { raw::fl_engine_free(self.raw.as_ptr()) };
    }
}

/// The engine side of the C ABI. Nothing else in this crate touches C.
///
/// Kept minimal on purpose: the three engine functions and the version
/// query. Everything module-side lives in `flint-sys`, and the two do
/// not share declarations, so a change on one side cannot silently
/// redeclare the other.
mod raw {
    use super::*;

    extern "C" {
        pub fn fl_abi_version() -> u32;
        pub fn fl_has_capability(capability: u32) -> c_int;
        pub fn fl_engine_new() -> *mut c_void;
        pub fn fl_engine_free(engine: *mut c_void);
        pub fn fl_engine_run(
            engine: *mut c_void,
            source: *const c_char,
            name: *const c_char,
        ) -> c_int;
    }
}
