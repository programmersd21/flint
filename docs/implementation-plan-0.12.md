# Flint 0.12.0 Implementation Plan

## Baseline Assessment (Complete)

**Current State:**
- Version: v0.11.0-3-g7710523-dirty
- Build: Clean with -Wpedantic -Werror
- Tests: 137/137 passing
- Unit tests: All passing
- Verifier: 4383 checks passing
- Features implemented: structs, modules with transaction rollback

**Object Types:**
- OBJ_STRING, OBJ_FUNCTION, OBJ_NATIVE, OBJ_CLOSURE
- OBJ_UPVALUE, OBJ_LIST, OBJ_TABLE, OBJ_STRUCT_CTOR

**Package Manager:**
- Basic `flint pkg` exists with path/git dependencies
- Lockfile support
- Missing: transitive resolution, integrity checks, native packages

## Phase B: Design (CURRENT)

### B1. Language Features Design
- [x] Structs - Already implemented
- [ ] Enums with tagged variants
- [ ] Pattern matching over enums and literals
- [ ] Methods on structs (if time permits)

### B2. Native ABI Design
Core requirements:
- Public header: `include/flint.h`
- Opaque handles for GC safety
- Version negotiation
- Registration and initialization
- Error propagation
- Platform abstraction for dynamic loading (dlopen/dlsym)

API surface:
```c
// Version and ABI
uint32_t flint_abi_version(void);
bool flint_abi_compatible(uint32_t requested);

// Value handles (opaque, GC-safe)
typedef struct FlintValue* FlintValue;
typedef struct FlintVM* FlintVM;

// Type queries
typedef enum {
    FLINT_TYPE_NIL,
    FLINT_TYPE_BOOL,
    FLINT_TYPE_NUMBER,
    FLINT_TYPE_STRING,
    FLINT_TYPE_LIST,
    FLINT_TYPE_TABLE,
    FLINT_TYPE_FUNCTION
} FlintType;

FlintType flint_type(FlintValue val);

// Value creation
FlintValue flint_nil(FlintVM* vm);
FlintValue flint_bool(FlintVM* vm, bool value);
FlintValue flint_number(FlintVM* vm, double value);
FlintValue flint_string(FlintVM* vm, const char* str, size_t len);

// Value extraction
bool flint_as_bool(FlintValue val);
double flint_as_number(FlintValue val);
const char* flint_as_string(FlintValue val, size_t* len);

// Collections
FlintValue flint_list_new(FlintVM* vm);
void flint_list_push(FlintValue list, FlintValue item);
FlintValue flint_list_get(FlintValue list, int index);
size_t flint_list_len(FlintValue list);

FlintValue flint_table_new(FlintVM* vm);
void flint_table_set(FlintValue table, const char* key, FlintValue val);
FlintValue flint_table_get(FlintValue table, const char* key);

// Native functions
typedef FlintValue (*FlintNativeFn)(FlintVM* vm, int argc, FlintValue* args);

typedef struct {
    const char* name;
    FlintNativeFn fn;
    int arity; // -1 for variadic
} FlintNativeFunc;

// Module registration
typedef struct {
    const char* name;
    FlintNativeFunc* functions;
    size_t function_count;
} FlintModule;

// Entry point signature
typedef bool (*FlintModuleInit)(FlintVM* vm, uint32_t abi_version);

// Error handling
void flint_error(FlintVM* vm, const char* message);
void flint_error_type(FlintVM* vm, const char* type, const char* message);

// Handle management (for persistent handles)
FlintValue flint_retain(FlintVM* vm, FlintValue val);
void flint_release(FlintVM* vm, FlintValue val);
```

### B3. Package Manager Enhancements
- Transitive dependency resolution algorithm
- Version constraint parsing (^1.0.0, ~1.2.3)
- Content integrity (SHA-256 checksums in lockfile)
- Native package metadata
- Platform/ABI compatibility checks

## Phase C: Runtime Foundations

### C1. GC Handle System
Create a handle table for native extensions:
- Separate handle table per VM
- Handles survive GC by being roots
- Handle validation
- Automatic cleanup on VM shutdown

### C2. Module System Verification
- Test transaction rollback thoroughly
- Add tests for nested imports
- Verify cleanup on initialization failure

## Phase D: Language Implementation

### D1. Enums
```flint
enum Result {
    Ok(value),
    Err(message),
}

enum Status {
    Pending,
    Running,
    Complete,
}
```

Implementation:
- New OBJ_ENUM_TYPE for enum declarations
- New OBJ_ENUM_VALUE for enum instances
- Variant with/without payload
- Pattern matching integration

### D2. Pattern Matching
```flint
match result {
    Ok(val) => print("Success: " + val),
    Err(msg) => print("Error: " + msg),
}

match status {
    Pending => print("waiting"),
    Running => print("in progress"),
    Complete => print("done"),
}
```

Implementation:
- New TOKEN_MATCH, TOKEN_CASE keywords (or use existing constructs)
- Compile to jump table for exhaustiveness
- Payload binding
- Guards support

## Phase E: Native ABI

### E1. Public Header
- Create `include/flint.h`
- Opaque handle types
- Version macros
- Function declarations

### E2. Implementation
- Handle table in VM
- Type conversion functions
- Error propagation
- Registration system

### E3. Dynamic Loader
- Platform abstraction: `src/runtime/loader.h`, `src/runtime/loader_posix.c`
- Symbol resolution
- Version checking
- Initialization protocol

### E4. ABI Tests
- Build test extension in `tests/native/test_extension.c`
- Test all API surface
- GC stress tests with native calls
- Version mismatch tests

## Phase F: Package Manager

### F1. Dependency Resolution
- Build dependency graph
- Detect cycles
- Version constraint solver
- Conflict detection

### F2. Integrity
- SHA-256 hashing
- Checksum verification
- Lockfile checksums

### F3. Native Package Support
- Platform detection
- ABI version in manifest
- Shared library loading

## Phase G: Rust Integration

### G1. Rust Wrapper Crate
Create `rust/flint-sys/` with:
- `build.rs` to find `flint.h`
- FFI bindings (bindgen or manual)
- Safe Rust wrappers

### G2. Example Extension
Create `examples/rust_extension/`:
- Simple Rust native module
- Demonstrates API usage
- Build script integration

### G3. Ratatui Integration
Create `examples/ratatui_tui/`:
- Terminal UI module
- Event handling
- Widget rendering
- Resource cleanup

## Phase H: Standard Library

### H1. Consistency Audit
- Error handling conventions
- Argument validation
- Return value patterns

### H2. Documentation
- Update `docs/library.md`
- Document every function
- Error behavior

## Phase I: Testing

### I1. Language Tests
- Enum construction and matching
- Pattern exhaustiveness
- Error cases

### I2. ABI Tests
- C extension build
- Rust extension build
- GC stress with native calls

### I3. Package Tests
- Transitive dependencies
- Version conflicts
- Integrity failures

### I4. Regression Suite
- Run full test suite
- Stress tests
- Sanitizer runs

## Phase J: Release Preparation

### J1. Documentation
- Update SPEC.md
- Update docs/language.md
- Create docs/native-abi.md
- Migration guide
- Changelog

### J2. Version Metadata
- Update to 0.12.0
- Generate version header
- Tag preparation

### J3. Final Validation
- Clean build
- All tests pass
- Benchmarks recorded
- Examples work

## Success Criteria

- [ ] Enums implemented and tested
- [ ] Pattern matching implemented and tested
- [ ] Native C ABI complete with documentation
- [ ] C extension example works
- [ ] Rust wrapper crate builds
- [ ] Rust extension example works
- [ ] Ratatui TUI example works
- [ ] Package manager handles transitive deps
- [ ] Package manager validates integrity
- [ ] Native packages load with ABI checks
- [ ] All tests pass (language, unit, ABI, package)
- [ ] Documentation complete and accurate
- [ ] No regressions from 0.11.0
