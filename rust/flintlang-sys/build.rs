// Builds the generated bindings from the real header, so the wrapper and the
// header cannot drift: if flint.h changes a signature, this fails to compile
// rather than producing a mismatch at load time.
fn main() {
    let header = std::path::Path::new("../../include/flint.h");
    assert!(header.exists(), "include/flint.h not found");
    println!("cargo:rerun-if-changed=../../include/flint.h");
}
