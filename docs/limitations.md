# Flint 1.0 Known Limitations

1. **Platform Support**: Fully tested and optimized on Linux (x86_64, aarch64). macOS and Windows builds are supported via portable C11 code, but platform-specific system integrations (such as terminal raw mode or advanced OS signals) may have restricted semantics compared to Linux.
2. **Concurrency Model**: Flint utilizes an explicit OS thread and channel concurrency model with a managed GC safepoint architecture. Shared mutable state between threads requires explicit synchronization primitives (mutexes, atomics).
3. **Unicode Awareness**: String operations handle UTF-8 byte sequences and character lengths; advanced full Unicode grapheme-cluster breaking is limited to standard UTF-8 validation and traversal.
