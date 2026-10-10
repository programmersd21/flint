/* SPDX-License-Identifier: MIT */
/*
 * Allocation and the garbage collector.
 *
 * All memory the VM manages goes through fl_reallocate(). A vm argument of
 * NULL means "no collector, just malloc", which is what the unit tests and
 * any allocation made before vm_init() use.
 */
#ifndef FL_MEMORY_H
#define FL_MEMORY_H

#include "common.h"
#include "value.h"

typedef struct VM VM;
typedef struct Obj Obj;

/*
 * Doubling, minimum 8. The minimum matters: a list built by push() would
 * otherwise reallocate on every single element for the first few.
 */
#define GROW_CAPACITY(capacity) ((capacity) < 8 ? 8 : (capacity) * 2)

#define GROW_ARRAY(vm, type, pointer, old_count, new_count)                    \
	((type *)fl_reallocate((vm),                                           \
	        (pointer),                                                     \
	        sizeof(type) * (old_count),                                    \
	        sizeof(type) * (new_count)))

#define FREE_ARRAY(vm, type, pointer, old_count)                               \
	fl_reallocate((vm), (pointer), sizeof(type) * (old_count), 0)

#define ALLOCATE(vm, type, count)                                              \
	((type *)fl_reallocate((vm), NULL, 0, sizeof(type) * (count)))

#define FREE(vm, type, pointer) fl_reallocate((vm), (pointer), sizeof(type), 0)

/*
 * The only allocator. new_size == 0 frees. Accounting happens before the
 * realloc so a collection can be triggered by the allocation itself.
 */
void *fl_reallocate(VM *vm, void *ptr, size_t old_size, size_t new_size);

/* mark and sweep. called automatically; call it directly only in tests. */
void collect_garbage(VM *vm);

/*
 * Mark one object and queue it for tracing. Exported because the compiler
 * has roots the collector cannot see, and a root is only a root if it goes
 * through here. Setting is_marked by hand keeps the object alive but leaves
 * everything it points at unmarked.
 */
void mark_object(VM *vm, Obj *object);

void free_object(VM *vm, Obj *object);
void free_objects(VM *vm);

#endif /* FL_MEMORY_H */
