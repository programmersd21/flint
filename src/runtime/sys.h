/* SPDX-License-Identifier: MIT */
/*
 * Natives for writing small unix programs.
 *
 * args, env, exit, and the file and process primitives. They live here rather
 * than in native.c because they are a different kind of thing: native.c is the
 * language's core vocabulary (len, push, type), and this is the standard
 * library a shell script reaches for. Same calling convention, same rules
 * about rooting, just a different reason to exist.
 *
 * Two rules run through all of it and are worth stating once:
 *
 *   - every value that crosses into C is rooted for the duration of any call
 *     that can allocate, because the collector can fire inside it
 *   - every length is checked before it is added to another, because a script
 *     can build a string as fast as it likes and the arithmetic is done in int
 */
#ifndef FL_SYS_H
#define FL_SYS_H

#include "vm.h"

/*
 * Hand the script's command line to the runtime, once, at startup.
 *
 * owned_args is a single malloc'd block: a vector of pointers into it, then
 * the bytes. One allocation rather than argc+1, and the free is a single
 * call. The runtime does not copy the strings; the block lives as long as the
 * process, which is as long as the VM.
 */
void sys_set_args(int argc, char **argv);

/* free the argv block. for vm_free(). */
void sys_free_args(void);

/* register every native in this file. called from register_natives(). */
void register_sys_natives(VM *vm);

/*
 * The directory an import is resolved against, pushed by the VM when it
 * starts running a file. module resolution is in native.c because import_file
 * lives there, but the path logic is here with the rest of the filesystem
 * knowledge.
 */
void sys_set_source_dir(const char *dir);

/*
 * Point the import resolver at the directory holding `file`, so `import
 * "y.fl"` from inside project/lib/x.fl finds project/lib/y.fl. A file with
 * no slash has no directory and leaves the resolver on the working directory.
 */
void sys_set_source_dir_for_file(const char *file);

/*
 * Resolve a module path against the directory of the importing file.
 *
 * Relative paths join the source directory; absolute paths come back
 * unchanged. The caller frees the result. This is what makes `import
 * "lib/math.fl"` mean the same thing from any working directory, which is
 * the point of the whole exercise.
 */
char *sys_resolve_module(const char *path);

/*
 * True when a module file exists for this bare name.
 *
 * Checks the standard library first (`<stdlib>/<name>.fl`), then the
 * importing file's directory (`<source_dir>/<name>.fl`). Used to suggest an
 * import when a script uses a name it never defined: if a file with the
 * right shape exists, forgetting the import line is the likely story.
 *
 * Only bare names: anything with a slash or a `.fl` suffix is a path the
 * user spelled themselves, and suggesting an import for it would be guessing
 * at a file nobody asked about.
 */
bool sys_module_file_exists(const char *name);

#endif /* FL_SYS_H */
