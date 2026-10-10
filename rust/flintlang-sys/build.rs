// The hand-written bindings in src/lib.rs mirror a header, so the build
// pins which one: the vendored copy in c-api/, refreshed from the
// repository's include/flint.h. In a checkout both exist and should
// agree; in a published crate only the vendored copy does, and the
// bindings were reviewed against exactly it. Drift between the wrapper
// and a newer header is caught the other way instead: the native ABI
// suite builds real modules against the repository header and runs
// them through these bindings in CI.
fn main() {
    let manifest = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    let vendored = std::path::Path::new(&manifest)
        .join("c-api")
        .join("flint.h");
    assert!(
        vendored.exists(),
        "flintlang-sys: c-api/flint.h is missing from the package"
    );
    println!("cargo:rerun-if-changed=c-api/flint.h");
    println!("cargo:rerun-if-changed=build.rs");
}
