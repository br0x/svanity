#ifndef LIBBASE58_H
#define LIBBASE58_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool (*b58_sha256_impl)(void *, const void *, size_t);

extern ssize_t base58_decode(unsigned char *out, const char *in);
extern ssize_t base58_decode_len(unsigned char *out, const char *in, size_t len);
extern size_t base58_encode(char* out, const unsigned char* in, size_t in_sz);

#ifdef __cplusplus
}
#endif

#endif