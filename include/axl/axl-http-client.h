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
 * @return 0 on success, -1 on unknown option.
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
 * @return 0 on success, -1 on failure.
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
 * @return 0 on success, -1 on failure.
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
 * @return 0 on success, -1 on failure.
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
 * @return 0 on success, -1 on failure.
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
 * @return 0 on success, -1 on failure.
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
 * @return 0 on success, -1 on failure.
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
