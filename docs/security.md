# Flint 1.0 Security Model

Flint 1.0 provides robust security boundaries:
1. **Process Execution**: Process execution (`os.run` or equivalent) never passes arguments implicitly through a shell. Argument boundaries are strictly preserved.
2. **Package Security**: Remote package sources require HTTPS, checksum verification before use, and protection against path traversal and archive bomb attacks.
3. **Bytecode Verification**: All compiled bytecode passes through a rigorous bytecode verifier before execution. Malformed or unsafe bytecode is rejected safely.
4. **TLS & Cryptography**: Network connections use TLS with certificate and hostname verification enabled by default. Cryptographic primitives rely on mature external libraries without home-built implementations.
5. **Filesystem and Inputs**: Unrusted input and filesystem operations use structured error handling and safe path normalization.
