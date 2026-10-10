/* SPDX-License-Identifier: MIT */
/*
 * SHA-256 known-answer tests (FIPS 180-4 vectors).
 *
 * The package manager trusts this for lockfile content hashes, so the
 * vectors are the whole point: an implementation that merely runs is not
 * one that hashes correctly.
 */
#include "sha256.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void digest_hex(const void *data, size_t length, char out[65])
{
	FlSha256 ctx;
	uint8_t digest[32];
	fl_sha256_init(&ctx);
	fl_sha256_update(&ctx, data, length);
	fl_sha256_final(&ctx, digest);
	for (int i = 0; i < 32; i++)
		snprintf(out + (size_t)i * 2, 3, "%02x", digest[i]);
	out[64] = '\0';
}

static void check_vector(
        const char *what, const void *data, size_t length, const char *want)
{
	char got[65];
	digest_hex(data, length, got);
	checks++;
	if (strcmp(got, want) != 0) {
		failures++;
		printf("FAIL: %s\n  want %s\n  got  %s\n", what, want, got);
	}
}

/* the same message fed one byte at a time must hash identically: the
 * tree walker updates per read chunk, not per file. */
static void check_chunked(void)
{
	const char *message = "abcdbcdecdefdefgefghfghighijhijkijkljk";
	size_t length = strlen(message);
	FlSha256 whole;
	uint8_t d1[32];
	fl_sha256_init(&whole);
	fl_sha256_update(&whole, message, length);
	fl_sha256_final(&whole, d1);
	FlSha256 parts;
	uint8_t d2[32];
	fl_sha256_init(&parts);
	for (size_t i = 0; i < length; i++)
		fl_sha256_update(&parts, message + i, 1);
	fl_sha256_final(&parts, d2);
	checks++;
	if (memcmp(d1, d2, 32) != 0) {
		failures++;
		printf("FAIL: byte-at-a-time update differs\n");
	}
}

int main(void)
{
	check_vector("empty",
	        "",
	        0,
	        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b8"
	        "55");
	check_vector("abc",
	        "abc",
	        3,
	        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015"
	        "ad");
	check_vector("448-bit message (two blocks after padding)",
	        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
	        56,
	        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06"
	        "c1");
	check_vector("112-byte message (two blocks)",
	        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
	        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
	        112,
	        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9"
	        "d1");
	check_chunked();
	if (failures != 0) {
		printf("%d of %d sha256 checks failed\n", failures, checks);
		return 1;
	}
	printf("sha256: %d checks passed\n", checks);
	return 0;
}
