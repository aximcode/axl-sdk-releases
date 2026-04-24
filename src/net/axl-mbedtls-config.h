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
   Pre-define these to prevent mbedTLS from including <sal.h>. */
#define MBEDTLS_CHECK_RETURN  __attribute__((warn_unused_result))
#define MBEDTLS_CHECK_RETURN_TYPICAL  MBEDTLS_CHECK_RETURN
#define MBEDTLS_CHECK_RETURN_OPTIONAL

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

/* Elliptic curves */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C

/* Ciphers */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC

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
