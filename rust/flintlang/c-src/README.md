# vendored C runtime

Copies of the repository's `include/` and `src/` (minus `main.c`),
so the `flintlang` crate builds anywhere with a C11 toolchain: no
make, no network, no repo layout required. That self-containment is
what makes the crate publishable.

Refresh from the repository root before any release:

    rm -rf rust/flintlang/c-src
    mkdir -p rust/flintlang/c-src
    cp -r include rust/flintlang/c-src/
    ... (per-directory copies of src/*, minus main.c)

then run the full gate (`make validate` plus `cargo test` in `rust/`)
so a drifted copy fails loudly instead of shipping silently.
