/* sha256.h -- minimal SHA-256 (FIPS 180-4) for the engine's download
 * verification. Written for this project; eng_ prefix keeps the symbols
 * out of anyone else's namespace. Both the Windows and the POSIX builds
 * use this (it replaced models.c's BCrypt call so the digest code path is
 * identical on every platform).
 */
#ifndef ANIME_SHA256_H
#define ANIME_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t buflen;
} eng_sha256_ctx;

void eng_sha256_init(eng_sha256_ctx *c);
void eng_sha256_update(eng_sha256_ctx *c, const void *data, size_t len);
void eng_sha256_final(eng_sha256_ctx *c, uint8_t out[32]);

/* One-shot: digest `data` and write the lowercase hex string (65 bytes
 * including the NUL) into `out`. */
void eng_sha256_hex(const void *data, size_t len, char out[65]);

#endif /* ANIME_SHA256_H */
