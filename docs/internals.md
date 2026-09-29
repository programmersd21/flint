# VM internals

The compiler is a single-pass Pratt parser. It scans tokens on demand and
emits bytecode into a `Chunk`; there is no syntax tree to keep alive. The VM
runs that chunk with a value stack and a fixed array of call frames.

## values

`Value` is a 64-bit word. Most IEEE 754 doubles fit unchanged. Boxed values
use a reserved NaN bit pattern, a three-bit tag, and a 48-bit payload. The
tags cover `nil`, booleans, and object pointers. Arithmetic NaNs are
canonicalized so their payload cannot be mistaken for a tag.

This depends on pointers fitting in 48 bits. `common.h` rejects non-64-bit
hosts, and `OBJ_VAL` asserts the payload fits. A wider-address machine needs a
different representation, not a looser assertion.

## roots and collection

The collector is non-moving mark and sweep. The value stack, active frame
closures, open upvalues, globals, intern table references, and active compiler
state keep objects alive. Marking uses an explicit gray stack, so a deeply
nested object graph does not recurse through the C stack.

The rooting rule that catches people: **put a new object somewhere the
collector scans before the next allocation.** Allocation can collect. A C
local is invisible to the collector, however convincing its name looks.

The intern table is weak. `collect_garbage()` removes white strings from it
before sweeping objects; reversing those steps makes the table inspect freed
memory. `make stress` collects on every allocation and catches missing roots
before they become intermittent failures.

## closures and upvalues

The compiler resolves captures while compiling nested functions. An upvalue
records either a local slot in an enclosing frame or an upvalue in the
enclosing closure. Open upvalues are shared, so two closures capturing one
local see the same writes.

When a captured local leaves scope, `OP_CLOSE_UPVALUE` copies its value into
the upvalue object and redirects the pointer. A plain pop would leave a
closure pointing into a stack slot that later calls reuse. The closure tests
exercise this lifetime boundary.

## modules

`import_file_native()` resolves a path, records it as in flight, and reenters
`vm_interpret()`. A successful path stays in the VM's module table. An
in-flight path signals a cycle. Failed modules lose the cache entry, but
globals created before the failure remain.

The source directory is VM-wide state. The native saves it around nested
imports and restores it afterward; otherwise a nested import changes the
base used by the next import in its caller. Path identity is the resolved
string, not the filesystem's canonical identity. Symlink aliases can load a
file twice.

## things that hurt

- A value held only in a C local can die at the next allocating call.
- Open upvalues point into the value stack. Reallocation is not an option.
- The intern table is weak. Its cleanup must precede sweep.
- A nested import reenters the interpreter and temporarily changes module
  path state.
- The disassembler must use the same operand widths as the compiler. A wrong
  width makes every instruction after it look like nonsense.

Run `make stress` for rooting failures. The closure cases in
`tests/language/functions/` cover captured-local lifetime; module cases under
`tests/language/modules/` cover nested imports and const bindings.
