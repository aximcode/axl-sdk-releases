/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-http-client.h:
 *
 * HTTP client with GET, POST, PUT, DELETE, and file download.
 *
 * Configuration uses string key-value pairs (like librdkafka):
 * @code
 * AxlHttpClient *c = axl_http_client_new();
 * axl_http_client_set(c, "timeout.ms", "30000");
 * axl_http_client_set(c, "keep.alive", "false");
 * axl_http_client_set(c, "max.redirects", "0");
 * axl_http_client_set(c, "header.User-Agent", "MyApp/1.0");
 * @endcode
 *
 * Supported options:
 *   "timeout.ms"      — per-operation timeout in milliseconds (default: "10000")
 *   "keep.alive"      — connection reuse: "true" (default) or "false"
 *   "max.redirects"   — redirect limit (default: "5"), "0" to disable
 *   "tls.verify"      — certificate verification: "true" (default) or "false"
 *   "header.<Name>"   — default header sent with every request
 */

#ifndef AXL_HTTP_CLIENT_H
#define AXL_HTTP_CLIENT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <axl/axl-macros.h>
#include <axl/axl-hash-table.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AxlHttpClient AxlHttpClient;

typedef struct {
    size_t   status_code;
    AxlHashTable  *headers;
    void     *body;
    size_t   body_size;
} AxlHttpClientResponse;

/**
 * @brief Create a new HTTP client with default options.
 *
 * @return client instance, or NULL on failure.
 */
AxlHttpClient *
axl_http_client_new(void);

/**
 * @brief Free an HTTP client and close any open connection.
 */
void
axl_http_client_free(
    AxlHttpClient *c  ///< client to free (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlHttpClient, axl_http_client_free)
#endif

/**
 * @brief Set a client option.
 *
 * All values are strings, parsed internally. See header comment
 * for the list of supported options.
 *
 * @return AXL_OK on success, AXL_ERR on unknown option.
 */
int
axl_http_client_set(
    AxlHttpClient *c,      ///< client
    const char    *key,    ///< option name
    const char    *value   ///< option value (string)
);

/**
 * @brief Get a client option value.
 *
 * Returns a pointer to the internally stored string. The pointer
 * remains valid until the option is changed or the client is freed.
 *
 * @return option value string, or NULL for unknown options.
 */
const char *
axl_http_client_get(
    AxlHttpClient *c,    ///< client
    const char    *key   ///< option name
);

/**
 * @brief HTTP GET request.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_get(
    AxlHttpClient          *c,         ///< client
    const char             *url,       ///< full URL string
    AxlHttpClientResponse  **out_resp  ///< receives response; free with axl_http_client_response_free()
);

/**
 * @brief HTTP POST request.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_post(
    AxlHttpClient          *c,             ///< client
    const char             *url,           ///< full URL string
    const void             *body,          ///< request body
    size_t                 size,           ///< body size in bytes
    const char             *content_type,  ///< MIME type (e.g. "application/json")
    AxlHttpClientResponse  **out_resp      ///< receives response
);

/**
 * @brief HTTP PUT request.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_put(
    AxlHttpClient          *c,             ///< client
    const char             *url,           ///< full URL string
    const void             *body,          ///< request body
    size_t                 size,           ///< body size in bytes
    const char             *content_type,  ///< MIME type
    AxlHttpClientResponse  **out_resp      ///< receives response
);

/**
 * @brief HTTP DELETE request.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_delete(
    AxlHttpClient          *c,         ///< client
    const char             *url,       ///< full URL string
    AxlHttpClientResponse  **out_resp  ///< receives response
);

/**
 * @brief Generic HTTP request with optional per-request headers.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_request(
    AxlHttpClient          *c,              ///< client
    const char             *method,         ///< HTTP method ("GET", "POST", "PUT", "DELETE", etc.)
    const char             *url,            ///< full URL string
    const void             *body,           ///< request body, or NULL
    size_t                 body_size,       ///< body size in bytes
    const char             *content_type,   ///< MIME type, or NULL
    AxlHashTable           *extra_headers,  ///< optional hash table of additional headers (NULL for none)
    AxlHttpClientResponse  **out_resp       ///< receives response
);

/**
 * @brief Producer callback for streaming PUT/POST request bodies.
 *
 * Called repeatedly by the client to fill the next outgoing
 * chunk. The implementation reads from its backing source (file,
 * buffer, generated content) into @p out_buf and reports the byte
 * count via @p out_size. A return of AXL_OK with @p *out_size == 0
 * signals end-of-body — the client emits the final empty chunk
 * (chunked transfer) or stops sending (Content-Length transfer).
 *
 * @return AXL_OK to continue (including the EOF case);
 *     AXL_ERR to abort the request. The client tears the
 *     connection down on AXL_ERR; the cleanup callback (if any)
 *     still fires.
 */
typedef int (*AxlRequestBodyStreamer)(
    void   *ctx,
    void   *out_buf,
    size_t  out_buf_size,
    size_t *out_size
);

/**
 * @brief Issue an HTTP request with a streaming request body.
 *
 * Mirrors axl_http_request but builds the body via @p streamer
 * rather than a contiguous buffer. Use for multi-chunk uploads
 * where the body isn't materialized in RAM (UEFI Shell `cp` to a
 * mounted volume, large-file PUT, generated content, etc.).
 *
 * When @p total_size is known, the client emits a
 * `Content-Length` header and sends the body as raw bytes; if the
 * streamer signals EOF before @p total_size bytes are produced
 * the request fails with AXL_ERR. Pass `(size_t)-1` to use
 * `Transfer-Encoding: chunked` instead — useful when the producer
 * doesn't know the total length up front.
 *
 * @p cleanup_fn (if non-NULL) fires once after the request
 * completes — success, error, OR streamer abort — so consumers
 * can release the producer state at a single site instead of
 * threading cleanup through every error return.
 *
 * Streaming requests do NOT retry on stale connections or follow
 * redirects: the producer callback can only be consumed once.
 * Callers who need either behavior must re-invoke this function
 * with a fresh streamer state.
 *
 * @return AXL_OK on success, AXL_ERR on failure (connection
 *     reset, streamer returned AXL_ERR, Content-Length mismatch,
 *     etc.).
 */
int
axl_http_request_streaming(
    AxlHttpClient          *c,
    const char             *method,        ///< "PUT", "POST", etc.
    const char             *url,
    AxlRequestBodyStreamer  streamer,      ///< producer callback
    void                   *ctx,           ///< opaque, passed to streamer + cleanup
    void                  (*cleanup_fn)(void *ctx),  ///< optional finalizer (NULL = none)
    size_t                  total_size,    ///< body length in bytes; (size_t)-1 = chunked
    const char             *content_type,
    AxlHashTable           *extra_headers,
    AxlHttpClientResponse **out_resp
);

/**
 * @brief Issue an HTTP request whose body is the contents of a
 *     local file, streamed via AxlStream.
 *
 * Convenience wrapper over axl_http_request_streaming for the
 * common "upload this file" case. Opens @p path read-only via
 * axl_fopen, sizes the body via axl_file_info, and streams the
 * bytes to the wire without materializing the whole file in RAM.
 * The stream is closed when the request completes.
 *
 * Use this for `cp` / upload semantics where the producer is just
 * "the bytes on disk." For producer/consumer patterns where the
 * source isn't a file (ring buffers, generated content), use
 * axl_http_request_streaming with a custom producer callback.
 *
 * @return AXL_OK on success, AXL_ERR on failure (file unreadable,
 *     producer error, connection reset, etc.).
 */
int
axl_http_request_stream_file(
    AxlHttpClient          *c,
    const char             *method,
    const char             *url,
    const char             *path,           ///< local file path (UTF-8)
    const char             *content_type,
    AxlHashTable           *extra_headers,
    AxlHttpClientResponse **out_resp
);

/**
 * @brief Free a client response.
 */
void
axl_http_client_response_free(
    AxlHttpClientResponse *resp  ///< response to free (NULL-safe)
);

#ifdef AXL_HAVE_AUTOPTR
AXL_DEFINE_AUTOPTR_CLEANUP(AxlHttpClientResponse, axl_http_client_response_free)
#endif

/**
 * @brief Download a URL to a local file.
 *
 * @return AXL_OK on success, AXL_ERR on failure.
 */
int
axl_http_download(
    AxlHttpClient *c,           ///< client
    const char    *url,         ///< full URL string
    const char    *local_path   ///< filesystem path to write (UTF-8)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_HTTP_CLIENT_H */
