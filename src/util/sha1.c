/* Small public-domain-style SHA-1, enough to identify ROM dumps. */
#include <string.h>
#include "sha1.h"

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_hex(const void *data, size_t len, char out[41])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    const uint8_t *p = data;
    size_t n = len;
    while (n >= 64) { block(h, p); p += 64; n -= 64; }
    uint8_t tail[128] = { 0 };
    memcpy(tail, p, n);
    tail[n] = 0x80;
    size_t tl = n < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    block(h, tail);
    if (tl == 128) block(h, tail + 64);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        uint8_t b = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
        out[2*i] = hx[b >> 4]; out[2*i+1] = hx[b & 15];
    }
    out[40] = 0;
}
