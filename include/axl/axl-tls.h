/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-tls.h
 *
 * TLS support using mbedTLS, compiled into every build. What activates
 * it is calling axl_tls_init() at startup, not a build flag;
 * --gc-sections keeps the stack out of any image that never does.
 *
 * Provides:
 *   - Self-signed ECDSA P-256 certificate generation
 *   - TLS 1.2 server and client contexts
 *   - Transparent integration with AxlTcp, AxlHttpServer, AxlHttpClient
 *
 * @code
 * if (axl_tls_init() == AXL_OK) {
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
 * @brief Outcome of a non-blocking TLS handshake or read.
 *
 * Unlike a plain error enum this carries a third, non-error state:
 * `AXL_TLS_WANT_MORE` (positive) means the operation isn't finished and
 * needs more transport data — re-arm the recv and call again. `AXL_TLS_OK`
 * is 0 and the error is negative, so `status < 0` reads as failure and
 * `status == AXL_TLS_WANT_MORE` as would-block (mirrors OpenSSL's
 * WANT_READ idiom). Values match the legacy 0/1/-1 codes.
 */
typedef enum {
    AXL_TLS_OK        =  0,  ///< operation complete / success
    AXL_TLS_WANT_MORE =  1,  ///< more transport data needed; call again after recv
    AXL_TLS_ERR       = -1   ///< error
} AxlTlsStatus;

/**
 * @brief Whether TLS is present in this build.
 *
 * Always true, guaranteed. mbedTLS is an unconditional dependency and
 * no build omits it, so this cannot answer no — do not branch on it.
 * It stays public API only so existing consumer source keeps compiling.
 *
 * The question worth asking at runtime is whether axl_tls_init()
 * succeeded; that is what gates https://. Note this predicate also does
 * not report whether the TLS stack survived `--gc-sections` in a given
 * image, and never did.
 *
 * @return always true.
 */
bool
axl_tls_available(void);

/**
 * @brief Initialize the TLS subsystem. Call once at startup.
 * @return AXL_OK on success, AXL_ERR if TLS not compiled in or init failed.
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
 * @return AXL_OK on success, AXL_ERR on failure.
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
 * @return AXL_OK on success, AXL_ERR on failure.
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
 * @return AXL_TLS_OK when the handshake is complete, AXL_TLS_WANT_MORE if
 *     more data is needed (call again after recv), AXL_TLS_ERR on error.
 */
AxlTlsStatus
axl_tls_handshake(
    AxlTlsContext *ctx   ///< TLS context
);

/**
 * @brief Perform or continue a TLS handshake, sending output asynchronously.
 *
 * Like axl_tls_handshake(), but any handshake output (e.g. the ServerHello
 * flight) is flushed via an asynchronous send on @p loop instead of a
 * blocking send. The blocking variant nests an ephemeral loop to complete
 * the send, which cannot make progress when the handshake is itself being
 * driven from inside a callback dispatched by a resident event loop (an
 * AxlService driver). Pair this with an asynchronous recv on the same loop
 * (stage the received ciphertext with axl_tls_stage_data() before calling)
 * so the whole handshake runs on the caller's loop.
 *
 * @return AXL_TLS_OK when the handshake is complete, AXL_TLS_WANT_MORE if
 *     more data is needed (re-arm the recv and call again), AXL_TLS_ERR on error.
 */
AxlTlsStatus
axl_tls_handshake_async(
    AxlTlsContext *ctx,   ///< TLS context
    AxlLoop       *loop   ///< loop to send handshake output on
);

/**
 * @brief Read decrypted data from a TLS connection.
 *
 * @return AXL_TLS_OK on success, AXL_TLS_WANT_MORE if more data is needed,
 *     AXL_TLS_ERR on error.
 */
AxlTlsStatus
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
 * @return AXL_OK on success, AXL_ERR on error.
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
 * @return AXL_OK if initiated, AXL_ERR on failure.
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
 * @brief Whether decrypted/decryptable TLS application data is already
 *        buffered, so the caller must drain it before idling on the
 *        transport.
 *
 * One TCP segment can carry several TLS records. After axl_tls_read()
 * returns one record's plaintext, more records from the same segment may
 * remain staged — re-arming a transport recv then would block forever on
 * bytes that have already arrived. Returns true when a further
 * axl_tls_read() can make progress without new transport input.
 *
 * @return true if buffered TLS application data is pending.
 */
bool
axl_tls_pending(
    AxlTlsContext *ctx   ///< TLS context
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
