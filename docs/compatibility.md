# Flint 1.0 Compatibility Guarantees

Flint 1.0 establishes strict compatibility guarantees for production stability:

1. **Language Syntax and Semantics**: The 1.0 syntax (`let`, `const`, `fn`, `match`, `try`/`catch`/`throw`, ranges, destructuring) is stable. Any future language evolution will adhere to strict semantic versioning rules.
2. **Module and Package Format**: `flint.toml` and `flint.lock` formats as well as module import resolution paths are stable.
3. **Native ABI and Bytecode**: The stable C ABI in `flint.h` and versioned bytecode bundles maintain backward compatibility across minor releases.
4. **Deprecation Process**: Features slated for removal will pass through at least one minor release cycle with deprecation warnings before removal.
