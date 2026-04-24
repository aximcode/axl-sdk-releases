/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-tls.h:
 *
 * TLS support using mbedTLS. Optional — requires AXL_TLS=1 at build time.
 * When not available, all functions return -1/NULL/false.
 *
 * Provides:
 *   - Self-signed ECDSA P-256 certificate generation
 *   - TLS 1.2 server and client contexts
 *   - Transparent integration with AxlTcp, AxlHttpServer, AxlHttpClient
 *
 * @code
 * if (axl_tls_init() == 0) {
 *     void *cert, *key;
 *     size_t cert_len, key_len;
 *     axl_tls_generate_self_signed("MyServer", NULL, 0,
 *                                  &cert, &cert_len, &key, &key_len);
 *     axl_tls_server_set_cert(cert, cert_len, key, key_len);
 *     axl_free(cert);
 *     axl_free(key);
 *     // HTTP server now supports HTTPS
 * }
 * @endcode
 */

#ifndef AXL_TLS_H
#define AXL_TLS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <axl/axl-tcp.h>   /* AxlTcpCallback for axl_tls_write_async */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlTlsContext AxlTlsContext;
typedef struct AxlTcp AxlTcp;
typedef struct AxlLoop AxlLoop;

/**
 * @brief Check if TLS support was compiled in.
 * @return true if AXL_TLS=1 was set at build time.
 */
bool
axl_tls_available(void);

/**
 * @brief Initialize the TLS subsystem. Call once at startup.
 * @return 0 on success, -1 if TLS not compiled in or init failed.
 */
int
axl_tls_init(void);

/**
 * @brief Shut down the TLS subsystem and free global resources.
 */
void
axl_tls_cleanup(void);

/**
 * @brief Generate a self-signed ECDSA P-256 certificate.
 *
 * The certificate includes SubjectAltName entries for localhost
 * and any provided IP addresses. Valid for 10 years.
 * Caller frees @a cert_der and @a key_der with axl_free().
 *
 * @return 0 on success, -1 on failure.
 */
int
axl_tls_generate_self_signed(
    const char *cn,                    ///< Common Name (e.g. "MyServer")
    const AxlIPv4Address *ips,  ///< IP addresses for SAN (may be NULL)
    size_t      ip_count,              ///< number of IP addresses
    void      **cert_der,              ///< [out] DER-encoded certificate
    size_t     *cert_len,              ///< [out] certificate length
    void      **key_der,               ///< [out] DER-encoded private key
    size_t     *key_len                ///< [out] key length
);

/**
 * @brief Load a server certificate and private key for TLS.
 *
 * After this call, TLS accept operations use this certificate.
 *
 * @return 0 on success, -1 on failure.
 */
int
axl_tls_server_set_cert(
    const void *cert_der,   ///< DER-encoded certificate
    size_t      cert_len,   ///< certificate length
    const void *key_der,    ///< DER-encoded private key
    size_t      key_len     ///< key length
);

/**
 * @brief Create a TLS server context for an accepted TCP connection.
 *
 * Wraps the socket for TLS. Call axl_tls_handshake() after to
 * complete the TLS handshake.
 *
 * @return context, or NULL on failure.
 */
AxlTlsContext *
axl_tls_accept(
    AxlTcp *sock   ///< accepted TCP socket
);

/**
 * @brief Create a TLS client context for an outbound TCP connection.
 *
 * @a hostname is used for SNI (Server Name Indication).
 * Call axl_tls_handshake() after to complete the handshake.
 *
 * @return context, or NULL on failure.
 */
AxlTlsContext *
axl_tls_connect(
    AxlTcp *sock,       ///< connected TCP socket
    const char    *hostname    ///< server hostname for SNI
);

/**
 * @brief Perform or continue a TLS handshake.
 *
 * May need to be called multiple times if data isn't available yet.
 *
 * @return 0 on success (handshake complete),
 *         1 if more data needed (call again after recv),
 *         -1 on error.
 */
int
axl_tls_handshake(
    AxlTlsContext *ctx   ///< TLS context
);

/**
 * @brief Read decrypted data from a TLS connection.
 *
 * @return 0 on success, -1 on error, 1 if more data needed.
 */
int
axl_tls_read(
    AxlTlsContext *ctx,       ///< TLS context
    void          *buf,       ///< output buffer
    size_t         size,      ///< buffer capacity
    size_t        *out_len    ///< [out] bytes read
);

/**
 * @brief Write data over a TLS connection.
 *
 * Encrypts and sends data via the underlying TCP socket.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_tls_write(
    AxlTlsContext *ctx,       ///< TLS context
    const void    *data,      ///< plaintext data
    size_t         len        ///< data length
);

/**
 * @brief Async write over a TLS connection.
 *
 * Encrypts data synchronously, then sends the ciphertext
 * asynchronously via axl_tcp_send_async on the given loop.
 * The callback fires when the send completes. One-shot —
 * the callback's bool return is ignored (send owns its buffer;
 * each write needs a fresh call).
 *
 * @return 0 if initiated, -1 on failure.
 */
int
axl_tls_write_async(
    AxlTlsContext *ctx,                                    ///< TLS context
    const void    *data,                                   ///< plaintext data
    size_t         len,                                    ///< data length
    AxlLoop       *loop,                                   ///< event loop
    AxlTcpCallback cb,                                     ///< completion callback
    void          *cb_data                                 ///< opaque context
);

/**
 * @brief Stage received TCP data for TLS processing.
 *
 * Points the TLS BIO recv callback at the provided buffer (zero-copy).
 * Call before axl_tls_handshake() or axl_tls_read().
 */
void
axl_tls_stage_data(
    AxlTlsContext *ctx,   ///< TLS context
    const void    *data,  ///< received TCP data
    size_t         len    ///< data length
);

/**
 * @brief Free a TLS context. Sends close_notify. NULL-safe.
 */
void
axl_tls_free(
    AxlTlsContext *ctx   ///< context to free
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlTlsContext, axl_tls_free)
#endif

#ifdef __cplusplus
}
#endif

#endif /* AXL_TLS_H */
