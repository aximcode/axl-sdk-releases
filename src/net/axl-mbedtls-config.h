/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-mbedtls-config.h:
 *
 * Minimal mbedTLS configuration for AXL SDK TLS support.
 * Enables only what's needed for TLS 1.2 server/client with
 * ECDSA P-256 and self-signed certificate generation.
 */

#ifndef AXL_MBEDTLS_CONFIG_H
#define AXL_MBEDTLS_CONFIG_H

/* Clang targeting windows defines _MSC_VER but we're not MSVC.
   Pre-defining MBEDTLS_CHECK_RETURN prevents mbedTLS from including <sal.h>.
   Don't pre-define _TYPICAL / _OPTIONAL — platform_util.h defines those
   unconditionally, so predefining them here produces redefinition warnings. */
#define MBEDTLS_CHECK_RETURN  __attribute__((warn_unused_result))

/* Platform */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_NO_UDBL_DIVISION
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
/* Declare platform functions before mbedTLS uses them as macros */
#include <stddef.h>
void *axl_mbedtls_calloc(size_t n, size_t size);
void  axl_mbedtls_free(void *ptr);
int   axl_mbedtls_snprintf(char *buf, size_t size, const char *fmt, ...);
int   axl_mbedtls_printf(const char *fmt, ...);

#define MBEDTLS_PLATFORM_CALLOC_MACRO   axl_mbedtls_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO     axl_mbedtls_free
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO axl_mbedtls_snprintf
#define MBEDTLS_PLATFORM_PRINTF_MACRO   axl_mbedtls_printf
#define MBEDTLS_PLATFORM_FPRINTF_ALT

/* No filesystem, no networking (we provide BIO callbacks) */
#undef MBEDTLS_FS_IO
#undef MBEDTLS_NET_C

/* TLS protocol */
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ALL_ALERT_MESSAGES
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET

/* Key exchange: ECDHE-ECDSA only (lean, no RSA) */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED
/* RSA-cert variants — required to talk to BMCs and other servers
 * that ship RSA certificates (almost all of them in 2026: Dell
 * iDRAC, HPE iLO, Lenovo XCC, Supermicro, etc.). Without these,
 * the TLS handshake fails with MBEDTLS_ERR_SSL_NO_CIPHER_CHOSEN
 * (-0x7780) on the cipher-suite negotiation. Confirmed against
 * vendor BMC firmware (AMD-EPYC server, 2026-05-05). */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
/* RSASSA-PSS (+ its MGF1) for JOSE PS256 — axl_pk_rsa_pss_sha256_*. */
#define MBEDTLS_PKCS1_V21
/* RSA key generation (axl_pk_keygen(AXL_PK_RSA)) needs prime generation.
 * TLS itself never calls mbedtls_rsa_gen_key; this only compiles in
 * mbedtls_mpi_gen_prime. */
#define MBEDTLS_GENPRIME

/* Force mbedtls to use its own bundled software inet_pton() in
 * x509_crt.c instead of trying to call the platform one. In our
 * freestanding UEFI build there is no <arpa/inet.h> / inet_pton,
 * but the compiler's __has_include picks up <sys/socket.h> from
 * the build host and AF_INET6 ends up defined — which would route
 * to the (missing) platform inet_pton and produce an undefined-
 * symbol link error once --no-undefined is enabled. The bundled
 * fallback is a simple ~30-line C function with no dependencies. */
#define MBEDTLS_TEST_SW_INET_PTON

/* Elliptic curves */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED  /* X25519 ECDH (axl_ecdh / SSH curve25519) */
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C

/* Ciphers */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_MODE_CTR  /* axl_cipher_ctr_* (e.g. SSH aes*-ctr) */
/* AEAD for the axl_aead_* API (ChaCha20-Poly1305; AES-GCM via GCM_C
 * above). Not used by the TLS 1.2 ciphersuites AXL negotiates. */
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

/* Hashes */
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SHA512_SMALLER
#define MBEDTLS_MD_C

/* RNG — we provide our own entropy source, no platform defaults */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* ASN.1 / X.509 / PK */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

/* Error strings (helpful for debugging) */
#define MBEDTLS_ERROR_C
#define MBEDTLS_ERROR_STRERROR_DUMMY

/* Disable mbedtls_ms_time (we provide our own in platform shim) */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* Provide our own mbedtls_platform_zeroize. Upstream's platform_util.c
   detects explicit_bzero via __GLIBC__, which is still defined via
   <string.h> in a -ffreestanding build — but the symbol itself does
   not link. Override with an _ALT implementation that uses a volatile
   memset to defeat optimizer elision. */
#define MBEDTLS_PLATFORM_ZEROIZE_ALT

/* Version */
#define MBEDTLS_VERSION_C

/* Allow private access for direct struct manipulation */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

/* HKDF for TLS 1.2 key derivation */
#define MBEDTLS_HKDF_C

#endif /* AXL_MBEDTLS_CONFIG_H */
