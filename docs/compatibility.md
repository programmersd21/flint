# Flint 0.8.0 Compatibility Notes

0.8.0 is a pre-1.0 release. The guarantees here are the ones the current
test suite and documentation support; they are weaker than a 1.0 policy on
purpose.

1. **Language syntax and semantics**: the constructs listed in SPEC.md are
   implemented and covered by tests. Anything that SPEC.md does not cover is
   subject to change before 1.0.
2. **Modules**: quoted imports resolve relative to the importing file, never
   to the process working directory. This behavior is tested.
3. **Native ABI**: the internal ABI between the compiler and VM is not
   stable. There is no public `flint.h` ABI yet, so native extensions are
   not supported at this stage.
4. **Bytecode**: the in-memory bytecode format is internal and may change
   without a version bump. There is no serialized bytecode format yet.
5. **Breaking changes**: expected before 1.0; most recent ones are listed in
   the commit history (recoverable errors, stepped ranges, anonymous
   functions, language identifiers in diagnostic output).
