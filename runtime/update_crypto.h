// SHA-256 and ECDSA P-256 verification without any library (runtime/update_crypto.c), for updating an AppImage: it runs
// on many distributions and can count on none of their crypto libraries. Only checks public data, so nothing here
// needs to be constant time. SW Update on Windows uses Windows' own crypto instead.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t used;
} UcSha256;

void uc_sha256_init(UcSha256 *c);
void uc_sha256_update(UcSha256 *c, const void *data, size_t n);
void uc_sha256_final(UcSha256 *c, uint8_t out[32]);
bool uc_sha256_file(const char *path, uint8_t out[32]);  // false: it can't be read

// hash: the SHA-256 of what was signed; sig: r then s; pub: X then Y (all big-endian)
bool uc_p256_verify(const uint8_t hash[32], const uint8_t sig[64], const uint8_t pub[64]);

#ifdef __cplusplus
}
#endif
