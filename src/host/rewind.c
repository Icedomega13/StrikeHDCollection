/* Rewind buffer. See rewind.h. */
#include "rewind.h"
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 8192

static size_t ssize, limit, total;
static uint8_t *cur, *tmp;           /* newest state; scratch for the XOR */
static int have_cur;
static uint8_t *ent[MAX_ENTRIES];    /* ring of compressed deltas, oldest at head */
static size_t ent_n[MAX_ENTRIES];
static int head, count;

static uint8_t *put_varint(uint8_t *p, size_t v)
{
    while (v >= 0x80) { *p++ = (uint8_t)(v | 0x80); v >>= 7; }
    *p++ = (uint8_t)v;
    return p;
}
static const uint8_t *get_varint(const uint8_t *p, size_t *v)
{
    size_t r = 0;
    int s = 0;
    for (;;) { uint8_t b = *p++; r |= (size_t)(b & 0x7F) << s; if (!(b & 0x80)) break; s += 7; }
    *v = r;
    return p;
}

/* XOR difference as (zero run, literal run, literals)... */
static uint8_t *encode(const uint8_t *d, size_t n, size_t *out_n)
{
    uint8_t *buf = malloc(n + n / 64 + 64), *p = buf;
    if (!buf) return NULL;
    size_t i = 0;
    while (i < n) {
        size_t z = i;
        while (z < n && d[z] == 0) z++;
        size_t l = z;
        /* a literal run ends at 8 zeros in a row */
        while (l < n) {
            if (d[l] == 0) {
                size_t k = l;
                while (k < n && k < l + 8 && d[k] == 0) k++;
                if (k - l >= 8 || k == n) break;
                l = k;
            } else l++;
        }
        p = put_varint(p, z - i);
        p = put_varint(p, l - z);
        memcpy(p, d + z, l - z);
        p += l - z;
        i = l;
    }
    *out_n = (size_t)(p - buf);
    uint8_t *fit = realloc(buf, *out_n ? *out_n : 1);
    return fit ? fit : buf;
}

static void apply(uint8_t *s, const uint8_t *e, size_t en)
{
    const uint8_t *p = e, *end = e + en;
    size_t i = 0;
    while (p < end && i < ssize) {
        size_t z, l;
        p = get_varint(p, &z);
        p = get_varint(p, &l);
        i += z;
        for (size_t k = 0; k < l && i < ssize; k++) s[i++] ^= *p++;
    }
}

static void drop_oldest(void)
{
    total -= ent_n[head];
    free(ent[head]);
    ent[head] = NULL;
    head = (head + 1) % MAX_ENTRIES;
    count--;
}

void rw_init(size_t state_size, size_t max_bytes)
{
    rw_clear();
    free(cur); free(tmp);
    ssize = state_size;
    limit = max_bytes;
    cur = malloc(ssize);
    tmp = malloc(ssize);
}

void rw_clear(void)
{
    while (count) drop_oldest();
    head = 0;
    total = 0;
    have_cur = 0;
}

void rw_push(const uint8_t *state)
{
    if (!cur || !tmp) return;
    if (have_cur) {
        for (size_t i = 0; i < ssize; i++) tmp[i] = cur[i] ^ state[i];
        size_t n;
        uint8_t *e = encode(tmp, ssize, &n);
        if (e) {
            if (count == MAX_ENTRIES) drop_oldest();
            int at = (head + count) % MAX_ENTRIES;
            ent[at] = e;
            ent_n[at] = n;
            count++;
            total += n;
            while (count > 1 && total > limit) drop_oldest();
        }
    }
    memcpy(cur, state, ssize);
    have_cur = 1;
}

int rw_pop(uint8_t *out)
{
    if (!count || !have_cur) return 0;
    int at = (head + count - 1) % MAX_ENTRIES;
    apply(cur, ent[at], ent_n[at]);
    total -= ent_n[at];
    free(ent[at]);
    ent[at] = NULL;
    count--;
    memcpy(out, cur, ssize);
    return 1;
}

int rw_count(void) { return count; }
size_t rw_bytes(void) { return total; }
