#ifndef DS_SHA1_H
#define DS_SHA1_H
#include <stddef.h>
#include <stdint.h>
/* SHA-1 of a buffer as 40 lowercase hex chars plus NUL. Used to identify ROMs. */
void sha1_hex(const void *data, size_t len, char out[41]);
#endif
