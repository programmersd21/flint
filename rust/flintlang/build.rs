//! Link the Flint C runtime as a static library.
//!
//! The runtime builds with the same Makefile the CLI uses, so the flags
//! and sources cannot drift: `make libflint.a` is the single definition
//! of how the C code compiles. This script only finds the archive and
//! hands it to rustc.
//!
//! Two environment overrides, for setups where building in place is wrong:
//!
//! * `FLINT_LIB_DIR`: a directory already containing `libflint.a`.
//!   The Makefile is not run.
//! * `FLINT_NO_BUILD=1`: fail rather than running `make`. Combine with
//!   `FLINT_LIB_DIR` for fully hermetic builds.

use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    // rust/flint -> rust -> flint
    let root = manifest.join("../..");
    println!("cargo:rerun-if-changed=../../include/flint.h");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=FLINT_LIB_DIR");
    println!("cargo:rerun-if-env-changed=FLINT_NO_BUILD");

    let dir: PathBuf = match std::env::var("FLINT_LIB_DIR") {
        Ok(dir) => PathBuf::from(dir),
        Err(_) => {
            let namely = root.join("libflint.a");
            if !namely.exists() {
                if std::env::var("FLINT_NO_BUILD").is_ok() {
                    panic!(
                        "flint: libflint.a not found and FLINT_NO_BUILD is set; \
                         set FLINT_LIB_DIR to a directory containing it, or build \
                         it with `make libflint.a` in the repository root"
                    );
                }
                let status = std::process::Command::new("make")
                    .arg("-C")
                    .arg(&root)
                    .arg("libflint.a")
                    .status();
                match status {
                    Ok(ok) if ok.success() => {}
                    _ => panic!(
                        "flint: `make libflint.a` failed; the crate needs a C11 \
                         toolchain and make, or a prebuilt library via FLINT_LIB_DIR"
                    ),
                }
            }
            root.clone()
        }
    };
    assert!(
        dir.join("libflint.a").exists(),
        "flint: no libflint.a in {}",
        dir.display()
    );
    println!("cargo:rustc-link-search=native={}", dir.display());
    println!("cargo:rustc-link-lib=static=flint");
    // libm, for the runtime's math natives. Every supported target has it;
    // the name differs on MSVC, which is why this is cfg-gated rather than
    // assumed.
    if !cfg!(target_env = "msvc") {
        println!("cargo:rustc-link-lib=m");
    }
}
