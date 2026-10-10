/* SPDX-License-Identifier: MIT */
/*
 * Internal hash table: open addressing, linear probing, tombstones.
 *
 * Keys are interned ObjString, so lookup is a pointer compare. Table here is
 * not the same thing as a flint-level table; see object.h.
 */
#ifndef FL_TABLE_H
#define FL_TABLE_H

#include "common.h"
#include "value.h"

typedef struct VM VM;
typedef struct ObjString ObjString;

/*
 * An empty slot is key == NULL and value == nil. A tombstone is key == NULL
 * and value == true. It keeps the probe sequence intact after a delete.
 *
 * is_const lives here rather than in the compiler because the constraint is
 * on the *binding*, not on any particular assignment. A global can be
 * assigned from another file, from inside a function, or from a loop the
 * compiler never saw as one, so the only place that reliably knows whether
 * this name is constant is the table that holds it.
 */
typedef struct {
	ObjString *key;
	Value value;
	bool is_const; /* set by const, never cleared by assignment */
	/*
	 * Marked by `export` in a module.
	 *
	 * The flag is on the binding rather than in a side table because it
	 * is a property of the binding, exactly as is_const is: a module can
	 * re-bind a name and the question "is this one exported" has to have
	 * the same answer every time it is asked.
	 *
	 * An entry that is exported keeps the flag through a re-bind, so
	 * `export let x = 1` followed by `x = 2` is still exported. A later
	 * `let x` in the same module is not an export, because `export` was
	 * not said -- which is the point of saying it.
	 */
	bool is_exported;
} Entry;

typedef struct Table {
	int count;
	int capacity; /* always a power of two, so index wraps with & */
	Entry *entries;
} Table;

void table_init(Table *table);
void table_free(VM *vm, Table *table);

bool table_get(Table *table, ObjString *key, Value *value);

/*
 * Lookup with a fallback table.
 *
 * This exists for one caller and one reason: a module's own globals must not
 * have to contain `print`, `len` and the rest just to be able to call them.
 *
 * When every module got its own table -- which is what module isolation
 * requires -- a fresh one had no builtins in it, so `import math` produced a
 * module that could not call anything. Copying the builtins into each module
 * would work and would be wrong: every module would then be able to assign to
 * `len`, and a shadow in one file would be invisible in another.
 *
 * So a module table is consulted first and the builtins second. A name the
 * module defines wins, which is correct shadowing, and a name it does not
 * define resolves to the builtin, which is correct lookup. Assignment still
 * goes only to the module's own table, so a module can shadow a builtin for
 * itself without affecting anything else.
 *
 * `value` may be NULL when the caller only wants to know whether the name
 * resolves at all.
 */
bool table_get_with_fallback(
        Table *table, Table *fallback, ObjString *key, Value *value);

/*
 * Insert or overwrite. Never touches the const flag, so overwriting a const
 * leaves it const. Returns true if the key was new.
 */
bool table_set(VM *vm, Table *table, ObjString *key, Value value);

/*
 * Bind a name with const. Returns false if the name is already const, which
 * the caller reports as a redeclaration.
 *
 * Separate from table_set() because the two want opposite things when they
 * meet an existing const: an ordinary write must leave the flag alone, and
 * this must refuse. Folding them into one function with a flag is how a
 * const quietly degrades back into a let.
 */
bool table_define_const(VM *vm, Table *table, ObjString *key, Value value);

/*
 * Define a binding the module marked `export`.
 *
 * Refuses to overwrite an existing name: re-exporting is a mistake, and
 * silently replacing a public binding is exactly the class of bug 0.6.0's
 * module work exists to remove.
 */
bool table_define_exported(
        VM *vm, Table *table, ObjString *key, Value value, bool is_const);

/* Mark an existing binding exported without changing its value. */
void table_mark_exported(VM *vm, Table *table, ObjString *key);

/*
 * Is this name bound with const? Used by the VM to refuse an assignment, and
 * by the compiler to reject it at compile time when the name is already
 * known. Returns false for an unknown name, which the caller has to treat as
 * "not const" rather than "invalid" -- `x = 1` on an undeclared name is
 * caught elsewhere.
 */
bool table_is_const(Table *table, ObjString *key);

bool table_delete(Table *table, ObjString *key);

void table_add_all(VM *vm, Table *from, Table *to);

/* lookup by content. used by the intern table before a string is allocated. */
ObjString *table_find_string(
        Table *table, const char *chars, int length, uint32_t hash);

/* drops keys whose string was not marked. must run before the sweep. */
void table_remove_white(Table *table);

#endif /* FL_TABLE_H */
