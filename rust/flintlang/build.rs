//! Link the Flint C runtime as a static library.
//!
//! The C sources are vendored under `c-src/` (mirroring the repository's
//! `src/` and `include/` minus `main.c`) and compiled here with the
//! system C compiler, so this crate builds anywhere with a C11 toolchain
//! and no other tools: no make, no network, no extra crates. That is what
//! makes the crate publishable -- a `cargo package` contains everything
//! the build needs.
//!
//! Flags mirror the Makefile's release build (`-O2 -DNDEBUG`, POSIX
//! 2008). The vendored tree is a copy, not a fork: refresh it from the
//! repository root and run the full gate before publishing.
//!
//! MSVC is refused with a message: the C runtime targets mingw-w64 on
//! Windows (it needs unistd-style declarations MSVC does not provide),
//! matching the rest of the project.

use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        panic!(
            "flintlang: MSVC is not supported; build with mingw-w64 gcc \
             (the Windows toolchain this runtime targets)"
        );
    }

    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let csrc = manifest.join("c-src");
    assert!(
        csrc.join("include").join("flint.h").exists(),
        "flintlang: c-src/ is missing; refresh it from the repository root"
    );
    println!("cargo:rerun-if-changed=c-src");
    println!("cargo:rerun-if-changed=build.rs");

    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let obj_dir = out.join("objs");
    std::fs::create_dir_all(&obj_dir).unwrap();

    let cc = std::env::var("CC").unwrap_or_else(|_| {
        if cfg!(windows) {
            "gcc".to_string()
        } else {
            "cc".to_string()
        }
    });

    let includes = vec![
        csrc.join("include"),
        csrc.join("src"),
        csrc.join("src").join("core"),
        csrc.join("src").join("frontend"),
        csrc.join("src").join("runtime"),
        csrc.join("src").join("util"),
    ];

    let mut objects = Vec::new();
    let mut sources: Vec<PathBuf> = Vec::new();
    collect_c(&csrc.join("src"), &mut sources);
    sources.sort();
    for src in &sources {
        let rel = src.strip_prefix(&csrc).unwrap();
        let mut obj = obj_dir.join(rel);
        obj.set_extension("o");
        if let Some(parent) = obj.parent() {
            std::fs::create_dir_all(parent).unwrap();
        }
        let mut cmd = Command::new(&cc);
        cmd.arg("-std=c11")
            .arg("-O2")
            .arg("-DNDEBUG")
            .arg("-D_POSIX_C_SOURCE=200809L")
            .arg("-fPIC")
            .arg("-c")
            .arg(src)
            .arg("-o")
            .arg(&obj);
        for inc in &includes {
            cmd.arg("-I").arg(inc);
        }
        let status = cmd
            .status()
            .unwrap_or_else(|e| panic!("flintlang: could not run C compiler '{cc}': {e}"));
        assert!(
            status.success(),
            "flintlang: compiling {} failed",
            src.display()
        );
        objects.push(obj);
    }

    let lib = out.join("libflintlang.a");
    let mut ar = Command::new("ar");
    ar.arg("crus").arg(&lib);
    for obj in &objects {
        ar.arg(obj);
    }
    let status = ar
        .status()
        .unwrap_or_else(|e| panic!("flintlang: could not run archiver 'ar': {e}"));
    assert!(
        status.success(),
        "flintlang: archiving libflintlang.a failed"
    );

    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=flintlang");
    // libm, for the runtime's math natives. Every supported target has
    // it; the name differs on MSVC, which is refused above rather than
    // assumed here.
    println!("cargo:rustc-link-lib=m");
}

fn collect_c(dir: &Path, out: &mut Vec<PathBuf>) {
    let entries = std::fs::read_dir(dir)
        .unwrap_or_else(|e| panic!("flintlang: cannot read {}: {e}", dir.display()));
    for entry in entries {
        let path = entry.unwrap().path();
        if path.is_dir() {
            collect_c(&path, out);
        } else if path.extension().and_then(|e| e.to_str()) == Some("c") {
            out.push(path);
        }
    }
}
