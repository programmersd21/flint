/* SPDX-License-Identifier: MIT */
/*
 * Internal hash table: open addressing with linear probing.
 *
 * Keys are interned strings, so a hit is a pointer compare. Capacity is
 * always a power of two, which turns the wraparound into a mask instead of a
 * modulo.
 */
#include "table.h"
#include "memory.h"
#include "object.h"
#include "stdint.h"
#include "value.h"

#include <string.h>

/* table_define_const() below needs this, and it is public anyway. */
bool table_is_const(Table *table, ObjString *key);
/*
 * Grow at 75%. Higher means shorter probe chains and wasted slots. Lower
 * means less memory. For a table holding globals this is not a decision
 * anyone should spend time on.
 */
#define TABLE_MAX_LOAD 0.75

void table_init(Table *table)
{
	table->count = 0;
	table->capacity = 0;
	table->entries = NULL;
}

void table_free(VM *vm, Table *table)
{
	FREE_ARRAY(vm, Entry, table->entries, table->capacity);
	table_init(table);
}

/*
 * Find the slot for a key, or the slot it should go in.
 *
 * An empty slot is key == NULL and value == nil. A tombstone is key == NULL
 * and value == true. Probing continues past tombstones, because the key we
 * want may be further along the same chain, but the first tombstone is
 * remembered and returned if we reach the end without a hit. Without that
 * you would insert a duplicate and quietly lose one of the two.
 */
static Entry *find_entry(Entry *entries, int capacity, ObjString *key)
{
	uint32_t index = key->hash & (capacity - 1);
	Entry *tombstone = NULL;

	for (;;) {
		Entry *entry = &entries[index];
		if (entry->key == NULL) {
			if (IS_NIL(entry->value))
				return tombstone != NULL ? tombstone : entry;
			else if (tombstone == NULL)
				tombstone = entry;
		} else if (entry->key == key) {
			return entry;
		}

		index = (index + 1) & (capacity - 1);
	}
}

/*
 * Allocate a new array and rehash into it. Called on growth and nowhere
 * else; a table full of tombstones but few live keys is not reclaimed, which
 * is a known limitation and has not mattered so far.
 */
static void adjust_capacity(VM *vm, Table *table, int capacity)
{
	Entry *entries = ALLOCATE(vm, Entry, capacity);
	for (int i = 0; i < capacity; i++) {
		entries[i].key = NULL;
		entries[i].value = NIL_VAL;
		entries[i].is_const = false;
		entries[i].is_exported = false;
	}

	/*
	 * Rehash from scratch rather than copying, because dropping
	 * tombstones means the surviving count is not the old count.
	 */
	table->count = 0;
	for (int i = 0; i < table->capacity; i++) {
		Entry *entry = &table->entries[i];
		if (entry->key == NULL)
			continue;

		Entry *dest = find_entry(entries, capacity, entry->key);
		dest->key = entry->key;
		dest->value = entry->value;
		/* const has to survive a rehash, or a table that grows
		 * after a const was defined would silently make it writable.
		 * The same is true of the export flag: a module that defines
		 * thirty exports and then grows its table would otherwise lose
		 * whichever ones happened to move, and the importer would see a
		 * module missing exports it plainly has. */
		dest->is_const = entry->is_const;
		dest->is_exported = entry->is_exported;
		table->count++;
	}

	FREE_ARRAY(vm, Entry, table->entries, table->capacity);
	table->entries = entries;
	table->capacity = capacity;
}

bool table_get(Table *table, ObjString *key, Value *value)
{
	if (table->count == 0)
		return false;

	Entry *entry = find_entry(table->entries, table->capacity, key);
	if (entry->key == NULL)
		return false;

	if (value != NULL)
		*value = entry->value;
	return true;
}

bool table_get_with_fallback(Table *table,
                             Table *fallback,
                             ObjString *key,
                             Value *value)
{
	if (table_get(table, key, value))
		return true;
	if (fallback == NULL)
		return false;
	return table_get(fallback, key, value);
}

/*
 * Find the entry for a write, growing first if the table is at its load
 * factor. Reports whether the key was absent, which is what tells `let` from
 * assignment.
 *
 * `is_new` may be NULL when the caller does not care -- the export path, for
 * one, has already checked whether the name exists and only wants the slot.
 *
 * The growth and the count bookkeeping live here rather than in table_set()
 * because there are two ways to write and both have to do them identically.
 */
static Entry *entry_for_write(
        VM *vm, Table *table, ObjString *key, bool *is_new)
{
	if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
		int capacity = GROW_CAPACITY(table->capacity);
		adjust_capacity(vm, table, capacity);
	}

	Entry *entry = find_entry(table->entries, table->capacity, key);

	/* reusing a tombstone does not increase the count */
	bool fresh = entry->key == NULL;
	if (is_new != NULL)
		*is_new = fresh;
	if (fresh && IS_NIL(entry->value))
		table->count++;

	return entry;
}

/*
 * Insert or overwrite, without touching the const flag.
 *
 * An existing const entry keeps its flag, so `const x = 1; let x = 2` cannot
 * quietly downgrade the binding and leave the earlier const meaningless. That
 * downgrade is the whole reason the flag lives on the entry rather than being
 * inferred from the bytecode.
 *
 * The test for "existing" is key == NULL, not "is this a tombstone": a reused
 * tombstone still holds is_const from whatever lived there before it was
 * deleted, so a wider test would resurrect the flag of a dead binding.
 *
 * Returns true if the key was not already present.
 */
bool table_set(VM *vm, Table *table, ObjString *key, Value value)
{
	bool is_new;
	Entry *entry = entry_for_write(vm, table, key, &is_new);

	entry->key = key;
	entry->value = value;
	return is_new;
}

/*
 * Bind a name with const, or refuse.
 *
 * Returns false if the name is already const, because redeclaring a constant
 * would make the first declaration a lie: the value it held is gone and the
 * promise that nothing writes it is gone with it. The caller reports it.
 *
 * Upgrading is allowed, because `let x = 1; const x = 2` narrows the binding
 * rather than contradicting it, and refusing that would be a rule with no
 * principle behind it.
 */
/*
 * Define a global that the module marked `export`.
 *
 * Separate from table_set() rather than a flag parameter, for the same reason
 * table_define_const() is: the two have opposite opinions about what happens
 * to an existing binding, and folding that into a mode flag is how a const
 * quietly degrades back into a let. This one refuses to overwrite an existing
 * name, because re-exporting is a mistake rather than an intention.
 *
 * Marking is separate again, because a re-bind of an already-exported name
 * stays exported -- `export let x` then `x = 2` is still exported -- and that
 * is table_mark_exported().
 */
bool table_define_exported(VM *vm,
                           Table *table,
                           ObjString *key,
                           Value value,
                           bool is_const)
{
	if (table_get(table, key, NULL))
		return false;
	Entry *entry = entry_for_write(vm, table, key, NULL);
	entry->key = key;
	entry->value = value;
	entry->is_const = is_const;
	entry->is_exported = true;
	return true;
}

/*
 * Mark an existing binding exported without changing its value.
 *
 * This is the path for `export fn`, where the function's closure is already
 * on the stack and the binding may already exist from an earlier declaration.
 */
void table_mark_exported(VM *vm, Table *table, ObjString *key)
{
	(void)vm;
	for (int i = 0; i < table->capacity; i++) {
		if (table->entries[i].key == key) {
			table->entries[i].is_exported = true;
			return;
		}
	}
}

bool table_define_const(VM *vm, Table *table, ObjString *key, Value value)
{
	if (table_is_const(table, key))
		return false;

	bool is_new;
	Entry *entry = entry_for_write(vm, table, key, &is_new);

	entry->key = key;
	entry->value = value;
	/* set unconditionally, including on an overwrite: this is the call
	 * that establishes the binding, and a let being promoted to const
	 * is the only way to reach an existing entry here */
	entry->is_const = true;
	(void)is_new;

	return true;
}

/*
 * Is this name bound with const?
 *
 * An unknown name answers false, and the caller has to treat that as "not
 * const" rather than as an error: `x = 1` for an undeclared x is a different
 * diagnostic, reported by the VM's own undefined-variable path.
 */
bool table_is_const(Table *table, ObjString *key)
{
	if (table->count == 0)
		return false;

	Entry *entry = find_entry(table->entries, table->capacity, key);
	return entry->key != NULL && entry->is_const;
}

bool table_delete(Table *table, ObjString *key)
{
	if (table->count == 0)
		return false;

	Entry *entry = find_entry(table->entries, table->capacity, key);
	if (entry->key == NULL)
		return false;

	/*
	 * A tombstone, not an empty slot. Emptying the slot would cut the
	 * probe chain in half and any key that probed past it would become
	 * unreachable. Tombstones still count toward the load factor, so a
	 * delete-heavy table grows instead of degrading, and only a rehash
	 * reclaims them.
	 */
	entry->key = NULL;
	entry->value = TRUE_VAL;
	return true;
}

/*
 * Copy every live entry from one table into another, preserving const.
 *
 * Const has to come along. A table that copied names without it would turn an
 * imported constant into an ordinary binding in the importing table, and the
 * copy is exactly the case where nobody is watching the flag.
 */
void table_add_all(VM *vm, Table *from, Table *to)
{
	for (int i = 0; i < from->capacity; i++) {
		Entry *entry = &from->entries[i];
		if (entry->key == NULL)
			continue;

		if (entry->is_const)
			table_define_const(vm, to, entry->key, entry->value);
		else
			table_set(vm, to, entry->key, entry->value);
	}
}

/*
 * Lookup by content rather than by pointer. Only the interning path needs
 * this: to intern a string you have not allocated yet, so you cannot have
 * its pointer. Compares length and hash first to keep memcmp off the hot
 * path.
 */
ObjString *table_find_string(
        Table *table, const char *chars, int length, uint32_t hash)
{
	if (table->count == 0)
		return NULL;

	uint32_t index = hash & (table->capacity - 1);
	for (;;) {
		Entry *entry = &table->entries[index];
		if (entry->key == NULL) {
			if (IS_NIL(entry->value))
				return NULL;
		} else if (entry->key->length == length &&
		           entry->key->hash == hash &&
		           memcmp(entry->key->chars, chars, length) == 0) {
			return entry->key;
		}

		index = (index + 1) & (table->capacity - 1);
	}
}

/*
 * Drop keys whose string was not marked. Runs between tracing and sweeping,
 * because the intern table holds weak references: once the sweep has run the
 * keys are freed memory and touching them is undefined behaviour.
 */
void table_remove_white(Table *table)
{
	for (int i = 0; i < table->capacity; i++) {
		Entry *entry = &table->entries[i];
		if (entry->key != NULL && !entry->key->obj.is_marked)
			table_delete(table, entry->key);
	}
}
