/* Trimmed from OpenSSH's crypto_api.h (public domain -- "Assembled from
   generated headers and source files by Markus Friedl. Placed in the
   public domain."), openssh-portable V_10_5_P1, commit b3f73442.

   Reduced to what ed25519.c actually uses: the crypto_int/uint typedefs,
   the SHA-512 digest size, the Ed25519 sizes, and the entry points.
   Upstream's `#include "includes.h"` is dropped -- it pulls OpenSSH's
   autoconf world, which does not exist here -- and `randombytes` is left
   as a prototype for src/net/axl-ed25519-glue.c to define rather than
   macro-mapped to arc4random_buf.

   Upstream's crypto_hash_sha512 is a static inline over OpenSSL's
   EVP_Digest or a bundled SHA2; here it is a prototype, supplied by
   src/net/axl-ed25519-glue.c over mbedTLS's one-shot mbedtls_sha512().
   ed25519.c ignores its result, so the glue counts a failure and the
   provider refuses the operation -- see that file for why zeroing the
   digest is not enough.

   Everything else upstream declares -- SNTRUP761, ML-KEM, ML-DSA, the
   SHA-3 utilities, small_random32 -- is dropped: ed25519.c references
   none of it. No other change. See deps/ed25519-ref10/README.md. */

#ifndef crypto_api_h
#define crypto_api_h

#include <stdint.h>
#include <stdlib.h>

typedef int8_t   crypto_int8;
typedef uint8_t  crypto_uint8;
typedef int16_t  crypto_int16;
typedef uint16_t crypto_uint16;
typedef int32_t  crypto_int32;
typedef uint32_t crypto_uint32;
typedef int64_t  crypto_int64;
typedef uint64_t crypto_uint64;

#define crypto_hash_sha512_BYTES 64U

int crypto_hash_sha512(unsigned char *, const unsigned char *,
    unsigned long long);

#define crypto_sign_ed25519_SECRETKEYBYTES 64U
#define crypto_sign_ed25519_PUBLICKEYBYTES 32U
#define crypto_sign_ed25519_SEEDBYTES      32U
#define crypto_sign_ed25519_BYTES          64U

int crypto_sign_ed25519(unsigned char *, unsigned long long *,
    const unsigned char *, unsigned long long, const unsigned char *);
int crypto_sign_ed25519_open(unsigned char *, unsigned long long *,
    const unsigned char *, unsigned long long, const unsigned char *);
int crypto_sign_ed25519_keypair(unsigned char *, unsigned char *);
int crypto_sign_ed25519_keypair_from_seed(unsigned char *, unsigned char *,
    const unsigned char *);

void randombytes(unsigned char *, unsigned long long);
void explicit_bzero(void *, size_t);

#endif /* crypto_api_h */
