# architecture

notes on the internals, for people reading the source. if something here is
wrong, the source is right and this file is a bug.

## values

every value is 64 bits. numbers are doubles. anything with the top 13 bits set
is a boxed tag with a 48-bit payload.

```
double:     [ sign:1 ][ exponent:11 ][ mantissa:52 ]
boxed:      [ 1111 1111 1111 1000 ][ tag:3 ][ payload:48 ]
```

| tag | meaning |
|---|---|
| 1 | nil |
| 2 | false |
| 3 | true |
| 4 | heap pointer to `Obj` |

a 64-bit virtual address uses at most 48 bits, so it fits in the payload. this
is the only reason flint requires a 64-bit host.

arithmetic that produces a NaN gets canonicalized to
`0x7FF8000000000000` before boxing. a NaN with an arbitrary payload could
otherwise collide with a tag.

## compiler

single-pass pratt parser. no ast, no tree walk, no second pass. tokens are
scanned on demand and bytecode goes straight into the chunk buffer.

scopes are a fixed-size stack of `Local` descriptors in the `Compiler` struct,
chained to the enclosing compiler through `enclosing`. slot 0 is always the
callee, so a local index doubles as a stack offset from `frame->slots`.

upvalues are resolved at compile time. `resolve_upvalue()` walks the enclosing
compilers looking for a captured local, marks it captured, and records an
`(index, is_local)` pair. the pair is emitted after `OP_CLOSURE` as two bytes
per upvalue.

constants start as a 1-byte operand. once a chunk passes 256, emission
switches to `OP_CONSTANT_LONG` with a 3-byte index.

### `as`

`OP_CAST` takes one byte, an `FlType` tag, and does nothing but compare. the
value is already on the stack and stays exactly where it is; there is no
variant of this opcode that rewrites anything, because an assertion has nothing
to rewrite.

the interesting part is where the work happens. the type *name* is resolved at
compile time in `as_()`, so `1 as frobnicate` never makes it to the run loop.
the type *value* cannot be: it may be a parameter, a list element, or something
a module supplied, and none of that is knowable while compiling. so the opcode
is a one-byte switch in the dispatch loop, and a mismatch is a runtime error.

the tags in `chunk.h` and the names in `flint_type_name()` are kept in step by
hand, which is the one fragile thing here. `type()` now calls
`flint_type_name()` too, so there is a single list of names and a cast's error
message cannot drift from what `type()` says. the tags are a separate list
because they are the bytecode encoding, and a rename there is a format change.

`as` sits between `PREC_FACTOR` and `PREC_UNARY`, which makes it bind tighter
than arithmetic and looser than unary minus. `-1 as number` therefore asserts
on the `1`. that is the same rule every C-like language uses for a cast, and
it is why `1 + 2 as number` needs no parentheses to mean what it looks like.

### const

a local `const` is decided at compile time. `named_variable()` sees the
`OP_GET_LOCAL` it is about to emit and the `is_const` flag on the local, and
refuses. no runtime cost and no extra instruction.

a global `const` cannot work that way, so the flag lives on the table entry
instead. `OP_DEFINE_GLOBAL_CONST` is `OP_DEFINE_GLOBAL` with one difference:
it calls `table_define_const()`, which marks the entry. the check then happens
in `OP_SET_GLOBAL`, via `table_is_const()`.

the reason it has to be the table and not the compiler is ordering. a name can
be made const by a module that this file imported, and the import runs at
runtime, long after this file was compiled. nothing the compiler saw when it
read the assignment says anything about whether the binding is const. the
table is the only place that knows.

`let` on an existing const is refused in `OP_DEFINE_GLOBAL` as well, and that
one is not obvious. redeclaring an ordinary global is allowed and overwrites.
Doing it to a const would leave the entry's flag set, so the "new" binding
would be unwritable too: the source would read as a plain `let` and the
language would disagree.

redeclaring a const with a *different* value is also refused, for the same
reason: one of the two declarations would be a lie. the same statement
re-executed with the same value is allowed, because importing a module runs
its top level and the second import is not a contradiction.

the flag has to survive a table rehash, since globals are rehashed as a script
defines more names. `adjust_capacity()` copies it for that reason; without
that, a script that declared a const and then defined twenty more names would
quietly find the const writable. `table_add_all()` carries it across module
copy for the same reason.

## call frames and upvalues

the call stack is a fixed array of `CallFrame`, 256 entries. each frame holds
the running `ObjClosure`, the instruction pointer, and `slots` - a pointer to
the callee value on the value stack. argument 0 is `slots[1]`, which is why
`call()` can set `slots = stack_top - argc - 1`.

`capture_upvalue()` keeps the open upvalue list sorted by descending stack
address. when a function captures a local, the list is walked until an entry
points at the same slot, and the existing one is reused. two closures sharing a
variable share the upvalue, which is what makes by-reference capture work.

`close_upvalues()` copies the value out of the stack slot into the upvalue's
own storage and repoints the upvalue at that storage. the VM emits
`OP_CLOSE_UPVALUE` for any local leaving scope that was captured. a plain
`OP_POP` would leave the upvalue pointing at a slot that the next call reuses.

## modules

every module gets its own `Table` of globals. `vm->globals` is a pointer into
a heap array of them, and the bytecode is unchanged: `OP_DEFINE_GLOBAL` means
"whatever table is running". that is why adding isolation needed no new opcode
in the hot path.

an `ObjClosure` records the environment it was created in. without that, a
function called after its module's import returned would resolve its names
against whatever module happened to be running -- so `area` would read
`display`'s `scale` instead of its own.

environments are allocated and never reused. two imports at the same nesting
depth taking the same slot is how two unrelated modules ended up reporting
"cannot redefine constant" against each other's names.

a module's environment is kept even after the module fails. it may have handed
out a closure the importer still holds, and freeing it is a use-after-free on
every later call. the collector decides instead.

`export` is a flag on the table entry, set by `OP_DEFINE_GLOBAL_EXPORT` at
compile time. The loader copies only flagged bindings into the table the
importer receives. Finding exports by diffing the global table -- what v0.5.0
did -- cannot tell a helper from a public function.

## bytecode verification

every chunk is verified before it runs. the pass checks that each opcode is a
member of the enum, that its operand width is known, that constant indices and
local slots are in range, and that every jump lands inside the code *on an
instruction boundary*. it recurses into closure constants.

the boundary check is the one that earns the rest. a jump into the middle of a
two-byte operand reads that operand byte as an opcode, and that bug can survive
a very long time before it crashes.

this exists because the bytecode is now trusted by something other than the
interpreter -- the instruction-width table, the disassembler, and any future
compiler pass all index off the same encoding. one table, in
`chunk_instruction_size()`, consulted by all of them.

it deliberately does **not** check stack depth. a linear walk cannot model a
branch merge, because `OP_JUMP_IF_FALSE` leaves its condition on the stack for
the branch to pop, so the two edges out of every `if` arrive at different
depths. approximating that rejects valid bytecode, which is worse than not
checking: it is a new source of false crashes on code that works. an
earlier version of the verifier did exactly that and was wrong about ordinary
programs for an afternoon.

writing it found three compiler bugs, which is the argument for having it:
`OP_EXPORT` was used as the top of the opcode range while sitting in the middle
of the enum; the backward jump target was measured from the wrong end of the
instruction; and the local-slot high-water mark was assigned rather than raised,
so a shallower later scope lowered it and every slot an earlier scope had used
then looked out of range.

## garbage collection

non-moving, stop-the-world, mark and sweep. collection triggers when
`bytes_allocated` passes `next_gc`, which is set to twice the heap size after
each cycle, with a floor so startup does not collect against a live set that
does not exist yet.

all the thresholds are in `src/core/config.h`. the reason they are not inline at
their use sites is that a number spread over a dozen call sites is a number
nobody can change coherently when the workload changes.

one large-allocation exemption: a single allocation above 1 MiB does not count
toward the threshold. one big buffer says nothing about how many small objects
are alive, and counting it makes the next small allocation collect a heap that
has nothing to do with it.

`vm_free()` frees objects *before* the tables. the reverse order looks harmless
and is not: freeing an object can trigger a collection, that collection reads
`vm->globals`, and if the globals table has already been freed the mark phase
walks freed memory. found by ASan on a new JSON benchmark.

roots:

- the value stack, from `vm->stack` to `vm->stack_top`
- the closure in every live `CallFrame`
- the open upvalue list
- every module environment, not just the running one. a module's bindings stay
  reachable for as long as the VM lives -- a closure may hold a pointer into one
  -- so marking only `vm->globals` would sweep a module still in use
- every `Compiler` on the chain the compiler hands over. the four parser
  globals (`parser`, `current`, `vm`, `loop`) live in one `CompilerState`
  struct, and `compile()` saves it on entry and restores it on exit, so a
  nested compile starts clean and cannot clobber the outer one. the GC walks
  the current chain during the compile that is live right now.

`mark_object()` sets the mark bit and pushes onto an explicit gray stack. the
gray stack grows with plain `realloc`, not `fl_reallocate`, because allocating
during marking must not re-enter the collector. `trace_references()` drains it
iteratively, so object graphs of any depth do not blow the C stack.

`collect_garbage()` order matters:

1. `mark_roots()`
2. `trace_references()`
3. `table_remove_white()` - drops unmarked keys from the interning table
4. `sweep()`

step 3 must happen before step 4, because the intern table holds weak
references. sweep first and the collector reads freed memory to decide what to
free.

## indices and printing

every double-to-index conversion goes through `value_to_index()`. it checks
finiteness, integrality with `floor()` rather than a cast, and bounds by the
container length before converting, which is what makes the negative branch
safe: `|d| <= count` means `count + idx` cannot overflow. the same helper
serves read and write, because one of them checked and the other not is worse
than neither.

integral doubles print with `%ld` via `fl_double_is_printable_int()`. the
range test runs before the cast, because `1e21` overflows `int64_t` and the
check that rejects it has to come first. one helper in `value.h`, used by the
VM, the natives, `str()`, and the debug printer, so the four copies of the
expression cannot drift.

string concatenation checks `a->length > INT_MAX - b->length` before adding,
and `concatenate()` reports through its return value instead of letting
`OP_ADD` fall through and blame the operand types.

## natives and the stack

a native receives its arguments as a pointer into the value stack and returns
one value. it runs on the C stack inside the caller's frame, and if it calls
`vm_interpret()` the whole thing reenters: that is how `import` works.

a native that fails reports through `vm_runtime_error()` and returns, exactly
like one that succeeded. `OP_CALL` therefore checks the frame count after a
native returns, because `vm_runtime_error()` has already unwound everything
and the loop would otherwise keep executing with no frames at all.

## object types

| type | representation |
|---|---|
| `OBJ_STRING` | `ObjString` with a trailing `char chars[]` |
| `OBJ_LIST` | `Value *items` with `count`/`capacity` |
| `OBJ_TABLE` | parallel `ObjString **keys` and `Value *values` |
| `OBJ_FUNCTION` | `Chunk` plus `arity` and `name` |
| `OBJ_CLOSURE` | `ObjFunction *` plus an upvalue pointer array |
| `OBJ_NATIVE` | C function pointer plus `arity` |
| `OBJ_UPVALUE` | `Value *location`, or `closed` once closed |

`ObjString` uses a flexible array member so the header and the bytes are one
allocation. with thousands of interned strings, two allocations per string is
a measurable difference.

`ObjTable` is what flint code sees as a table. `Table` in `table.h` is the
internal C-level hash table used for globals and interning. they share no code
and the similarity is entirely accidental.

`Entry` carries an `is_const` flag alongside the key and the value. it is
almost always false, and it is there for one case: a global bound with `const`,
which has to be a property of the binding rather than of any one assignment.
see the const section above for why that cannot be a compiler-side check.
