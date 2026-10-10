//! The safe API against the real runtime. Ordinary use is `unsafe`-free
//! by construction: this file has no `unsafe` block, and would not
//! compile with one that did anything the API forbids.
//!
//! Output goes to the process's stdout/stderr, so assertions are on the
//! `Result`, not on printed text. A run that prints the right thing and
//! returns the wrong code is a failure, and these tests say so.

use flintlang::{Engine, Error};

#[test]
fn runs_clean_source() {
    let engine = Engine::new().expect("engine");
    assert_eq!(engine.run("let x = 1 + 2", Some("ok.fl")), Ok(()));
}

#[test]
fn unnamed_source_runs() {
    let engine = Engine::new().expect("engine");
    assert_eq!(engine.run("let x = 1", None), Ok(()));
}

#[test]
fn compile_error_is_compile() {
    let engine = Engine::new().expect("engine");
    assert_eq!(engine.run("let =", Some("bad.fl")), Err(Error::Compile));
}

#[test]
fn runtime_error_is_runtime() {
    let engine = Engine::new().expect("engine");
    assert_eq!(
        engine.run("nosuchfn()", Some("bad.fl")),
        Err(Error::Runtime)
    );
}

#[test]
fn interior_nul_never_reaches_c() {
    let engine = Engine::new().expect("engine");
    assert_eq!(
        engine.run("print(\"a\0b\")", Some("nul.fl")),
        Err(Error::NulInSource)
    );
    assert_eq!(
        engine.run("let x = 1", Some("a\0b.fl")),
        Err(Error::NulInSource)
    );
}

#[test]
fn state_persists_across_runs() {
    // One engine is one VM: a `let` in one run is visible to the next.
    let engine = Engine::new().expect("engine");
    assert_eq!(engine.run("let carried = 41", None), Ok(()));
    assert_eq!(engine.run("let next = carried + 1", None), Ok(()));
}

#[test]
fn engines_are_independent() {
    let first = Engine::new().expect("first engine");
    let second = Engine::new().expect("second engine");
    assert_eq!(first.run("let only_here = 1", None), Ok(()));
    // `second` never saw the declaration: reading it is a runtime error.
    assert_eq!(second.run("only_here", None), Err(Error::Runtime));
    drop(first);
    drop(second);
}

#[test]
fn repeated_creation_and_destruction() {
    // A hundred engines in a row: creation and shutdown must not leak
    // enough to matter, nor leave global state behind. (Leak detection
    // itself is ASan's job in CI; here the bar is that it runs green.)
    for _ in 0..100 {
        let engine = Engine::new().expect("engine");
        assert_eq!(engine.run("let x = [1, 2, 3]", None), Ok(()));
    }
}

#[test]
fn concurrent_engines_do_not_race() {
    // Two threads, two engines, at the same time. The C runtime keeps
    // compiler and scanner state outside any engine, so without the
    // crate's process-wide lock this segfaults. With it, both threads
    // simply take turns.
    std::thread::scope(|scope| {
        let left = scope.spawn(|| {
            let engine = Engine::new().expect("left engine");
            for i in 0..50 {
                engine
                    .run(&format!("let left{i} = {i} * {i}"), None)
                    .unwrap();
            }
        });
        let right = scope.spawn(|| {
            let engine = Engine::new().expect("right engine");
            for i in 0..50 {
                engine
                    .run(&format!("let right{i} = {i} + {i}"), None)
                    .unwrap();
            }
        });
        left.join().expect("left thread");
        right.join().expect("right thread");
    });
}

#[test]
fn errors_display() {
    assert_eq!(Error::Compile.to_string(), "compile error (exit 65)");
    assert_eq!(Error::Runtime.to_string(), "runtime error (exit 70)");
    assert_eq!(
        Error::OutOfMemory.to_string(),
        "out of memory creating the engine"
    );
    assert_eq!(Error::Unknown(99).to_string(), "flint exited with code 99");
}

#[test]
fn abi_version_agrees_with_sys() {
    assert_eq!(flintlang::abi_version(), flintlang_sys::ABI_VERSION);
    assert!(flintlang::abi_version() >= 1);
}
