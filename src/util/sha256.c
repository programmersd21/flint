/* SPDX-License-Identifier: MIT */
#include "sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t rotr(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32 - n));
}

static const uint32_t k[64] = {
        0x428a2f98U,
        0x71374491U,
        0xb5c0fbcfU,
        0xe9b5dba5U,
        0x3956c25bU,
        0x59f111f1U,
        0x923f82a4U,
        0xab1c5ed5U,
        0xd807aa98U,
        0x12835b01U,
        0x243185beU,
        0x550c7dc3U,
        0x72be5d74U,
        0x80deb1feU,
        0x9bdc06a7U,
        0xc19bf174U,
        0xe49b69c1U,
        0xefbe4786U,
        0x0fc19dc6U,
        0x240ca1ccU,
        0x2de92c6fU,
        0x4a7484aaU,
        0x5cb0a9dcU,
        0x76f988daU,
        0x983e5152U,
        0xa831c66dU,
        0xb00327c8U,
        0xbf597fc7U,
        0xc6e00bf3U,
        0xd5a79147U,
        0x06ca6351U,
        0x14292967U,
        0x27b70a85U,
        0x2e1b2138U,
        0x4d2c6dfcU,
        0x53380d13U,
        0x650a7354U,
        0x766a0abbU,
        0x81c2c92eU,
        0x92722c85U,
        0xa2bfe8a1U,
        0xa81a664bU,
        0xc24b8b70U,
        0xc76c51a3U,
        0xd192e819U,
        0xd6990624U,
        0xf40e3585U,
        0x106aa070U,
        0x19a4c116U,
        0x1e376c08U,
        0x2748774cU,
        0x34b0bcb5U,
        0x391c0cb3U,
        0x4ed8aa4aU,
        0x5b9cca4fU,
        0x682e6ff3U,
        0x748f82eeU,
        0x78a5636fU,
        0x84c87814U,
        0x8cc70208U,
        0x90befffaU,
        0xa4506cebU,
        0xbef9a3f7U,
        0xc67178f2U,
};

static void compress(FlSha256 *ctx, const uint8_t block[64])
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		size_t o = (size_t)i * 4;
		w[i] = ((uint32_t)block[o] << 24) |
		       ((uint32_t)block[o + 1] << 16) |
		       ((uint32_t)block[o + 2] << 8) | (uint32_t)block[o + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^
		              (w[i - 15] >> 3);
		uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^
		              (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = ctx->state[0];
	uint32_t b = ctx->state[1];
	uint32_t c = ctx->state[2];
	uint32_t d = ctx->state[3];
	uint32_t e = ctx->state[4];
	uint32_t f = ctx->state[5];
	uint32_t g = ctx->state[6];
	uint32_t h = ctx->state[7];
	for (int i = 0; i < 64; i++) {
		uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + s1 + ch + k[i] + w[i];
		uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = s0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

void fl_sha256_init(FlSha256 *ctx)
{
	ctx->state[0] = 0x6a09e667U;
	ctx->state[1] = 0xbb67ae85U;
	ctx->state[2] = 0x3c6ef372U;
	ctx->state[3] = 0xa54ff53aU;
	ctx->state[4] = 0x510e527fU;
	ctx->state[5] = 0x9b05688cU;
	ctx->state[6] = 0x1f83d9abU;
	ctx->state[7] = 0x5be0cd19U;
	ctx->length = 0;
	ctx->buffered = 0;
}

void fl_sha256_update(FlSha256 *ctx, const void *data, size_t length)
{
	const uint8_t *p = (const uint8_t *)data;
	ctx->length += (uint64_t)length;
	while (length > 0) {
		size_t room = 64 - ctx->buffered;
		size_t take = length < room ? length : room;
		memcpy(ctx->block + ctx->buffered, p, take);
		ctx->buffered += take;
		p += take;
		length -= take;
		if (ctx->buffered == 64) {
			compress(ctx, ctx->block);
			ctx->buffered = 0;
		}
	}
}

void fl_sha256_final(FlSha256 *ctx, uint8_t digest[32])
{
	/* length in bits, big-endian, in the last eight bytes. the 0x80
	 * byte starts the padding; if it does not fit beside the length,
	 * a whole extra block goes out first. */
	uint64_t bits = ctx->length * 8;
	uint8_t pad = 0x80;
	fl_sha256_update(ctx, &pad, 1);
	uint8_t zero = 0x00;
	while (ctx->buffered != 56)
		fl_sha256_update(ctx, &zero, 1);
	uint8_t tail[8];
	for (int i = 0; i < 8; i++)
		tail[i] = (uint8_t)(bits >> (56 - 8 * i));
	/* bypass update's length accounting: the padding is framing, not
	 * message, and update would count it into a length already spent. */
	memcpy(ctx->block + 56, tail, 8);
	compress(ctx, ctx->block);
	ctx->buffered = 0;
	for (int i = 0; i < 8; i++) {
		size_t o = (size_t)i * 4;
		digest[o] = (uint8_t)(ctx->state[i] >> 24);
		digest[o + 1] = (uint8_t)(ctx->state[i] >> 16);
		digest[o + 2] = (uint8_t)(ctx->state[i] >> 8);
		digest[o + 3] = (uint8_t)ctx->state[i];
	}
}
