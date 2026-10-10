/* SPDX-License-Identifier: MIT */
#ifndef FL_SHA256_H
#define FL_SHA256_H

/*
 * SHA-256 (FIPS 180-4), for content hashes the package manager records
 * in flint.lock. A lockfile that pins a commit is reproducible against
 * movement of the branch, not against tampering with the object; hashing
 * the installed tree is what makes "the same as recorded" checkable.
 *
 * Small on purpose: init/update/final over bytes, no allocation, no I/O.
 * Callers that hash trees walk the directory themselves (see pkg.c) so
 * the traversal policy -- ordering, filtering -- lives in one place.
 */
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t state[8];
	uint64_t length;
	uint8_t block[64];
	size_t buffered;
} FlSha256;

void fl_sha256_init(FlSha256 *ctx);
void fl_sha256_update(FlSha256 *ctx, const void *data, size_t length);
/* 32 bytes of digest. the context is spent afterwards; init it again. */
void fl_sha256_final(FlSha256 *ctx, uint8_t digest[32]);

#endif
