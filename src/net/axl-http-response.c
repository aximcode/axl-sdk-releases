/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-response.c
    HTTP response building and public response helpers.
**/

#include "axl-http-server-internal.h"

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// Response sending (internal)
// ---------------------------------------------------------------------------

static void
emit_header(
    const void *key,
    void       *value,
    void       *data
    )
{
    HeaderBuildCtx *ctx = (HeaderBuildCtx *)data;

    if (ctx->len < ctx->buf_size) {
        ctx->len += axl_snprintf(
                      ctx->buf + ctx->len,
                      ctx->buf_size - ctx->len,
                      "%s: %s\r\n",
                      (const char *)key,
                      (const char *)value
                      );
    }
}

void
send_response(
    HttpConn        *conn,
    AxlHttpResponse *resp
    )
{
    char   header[2048];
    size_t len;

    len = http_build_status_line(header, sizeof(header), resp->status_code);

    if (resp->content_type != NULL) {
        len += axl_snprintf(header + len, sizeof(header) - len,
                 "Content-Type: %s\r\n", resp->content_type);
    }

    if (resp->body != NULL && resp->body_size > 0) {
        len += axl_snprintf(header + len, sizeof(header) - len,
                 "Content-Length: %llu\r\n", (unsigned long long)resp->body_size);
    }

    len += axl_snprintf(header + len, sizeof(header) - len,
             "Connection: %s\r\n",
             conn->keep_alive ? "keep-alive" : "close");

    //
    // Emit any custom response headers
    //
    if (resp->headers != NULL) {
        HeaderBuildCtx ctx;
        ctx.buf      = header;
        ctx.buf_size = sizeof(header);
        ctx.len      = len;

        axl_hash_table_foreach(resp->headers, emit_header, &ctx);
        len = ctx.len;
    }

    //
    // End of headers
    //
    len += axl_snprintf(header + len, sizeof(header) - len, "\r\n");

    if (conn->tls_ctx != NULL) {
        if (axl_tls_write(conn->tls_ctx, header, len) != AXL_OK) {
            axl_warning("TLS write failed for headers");
            return;
        }
        if (resp->body != NULL && resp->body_size > 0) {
            if (axl_tls_write(conn->tls_ctx, resp->body, resp->body_size) != AXL_OK) {
                axl_warning("TLS write failed for body");
                return;
            }
        }
    } else {
        axl_tcp_send(conn->sock, header, len, 0);
        if (resp->body != NULL && resp->body_size > 0) {
            axl_tcp_send(conn->sock, resp->body, resp->body_size, 0);
        }
    }
}

void
send_error_response(
    HttpConn *conn,
    size_t    status_code
    )
{
    AxlHttpResponse resp;

    axl_memset(&resp, 0, sizeof(resp));
    resp.status_code = status_code;
    send_response(conn, &resp);
}

// ---------------------------------------------------------------------------
// Public response helpers
// ---------------------------------------------------------------------------

static void
set_body_copy(
    AxlHttpResponse *r,
    const void      *data,
    size_t           size
    )
{
    if (r->body != NULL) {
        axl_free(r->body);
    }

    r->body = axl_malloc(size);
    if (r->body != NULL) {
        axl_memcpy(r->body, data, size);
        r->body_size = size;
    } else {
        /* The set_json/set_text/set_range public wrappers all return
         * void, so a failed alloc would otherwise be invisible — the
         * client would see a 200 response with an empty body and no
         * idea why. Surface it via the operator log; the response goes
         * out empty (current behaviour preserved for ABI). */
        axl_warning("response body alloc failed: %zu bytes", size);
        r->body_size = 0;
    }
}

void
axl_http_response_set_json(AxlHttpResponse *r, const char *json)
{
    if (r == NULL || json == NULL) {
        return;
    }

    set_body_copy(r, json, axl_strlen(json));
    r->content_type = "application/json";
    if (r->status_code == 0) {
        r->status_code = 200;
    }
}

void
axl_http_response_set_text(AxlHttpResponse *r, const char *text)
{
    if (r == NULL || text == NULL) {
        return;
    }

    set_body_copy(r, text, axl_strlen(text));
    r->content_type = "text/plain";
    if (r->status_code == 0) {
        r->status_code = 200;
    }
}

void
axl_http_response_set_status(AxlHttpResponse *r, size_t code)
{
    if (r != NULL) {
        r->status_code = code;
    }
}

void
axl_http_response_set_file(AxlHttpResponse *r, const char *path)
{
    if (r == NULL || path == NULL) {
        return;
    }

    {
        void    *tmp  = NULL;
        size_t   len  = 0;
        int      rc;

        rc = axl_file_get_contents(path, &tmp, &len);
        if (rc != AXL_OK) {
            axl_debug("static file not found: %s", path);
            r->status_code = 404;
            return;
        }
        void *buffer = axl_malloc(len);
        if (buffer == NULL) {
            axl_error("response body alloc failed: %zu bytes for %s",
                      len, path);
            axl_free(tmp);
            r->status_code = 500;
            return;
        }
        axl_memcpy(buffer, tmp, len);
        size_t size = (size_t)len;
        axl_free(tmp);

        if (r->body != NULL) {
            axl_free(r->body);
        }

        r->body      = buffer;
        r->body_size = size;
    }

    //
    // Infer content type from extension
    //
    const char *ext = NULL;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '.') {
            ext = p;
        }
    }

    r->content_type = "application/octet-stream";
    if (ext != NULL) {
        if (axl_strcmp(ext, ".html") == 0 || axl_strcmp(ext, ".htm") == 0) {
            r->content_type = "text/html";
        } else if (axl_strcmp(ext, ".css") == 0) {
            r->content_type = "text/css";
        } else if (axl_strcmp(ext, ".js") == 0) {
            r->content_type = "application/javascript";
        } else if (axl_strcmp(ext, ".json") == 0) {
            r->content_type = "application/json";
        } else if (axl_strcmp(ext, ".txt") == 0) {
            r->content_type = "text/plain";
        } else if (axl_strcmp(ext, ".png") == 0) {
            r->content_type = "image/png";
        } else if (axl_strcmp(ext, ".jpg") == 0 || axl_strcmp(ext, ".jpeg") == 0) {
            r->content_type = "image/jpeg";
        } else if (axl_strcmp(ext, ".svg") == 0) {
            r->content_type = "image/svg+xml";
        } else if (axl_strcmp(ext, ".ico") == 0) {
            r->content_type = "image/x-icon";
        }
    }

    if (r->status_code == 0) {
        r->status_code = 200;
    }
}

void
axl_http_response_set_range(AxlHttpResponse *r, const void *data,
                            size_t offset, size_t length,
                            size_t total_size)
{
    if (r == NULL || data == NULL) {
        return;
    }

    set_body_copy(r, (const uint8_t *)data + offset, (size_t)length);
    r->status_code  = 206;
    r->content_type = "application/octet-stream";
}
