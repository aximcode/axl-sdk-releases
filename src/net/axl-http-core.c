/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-core.c
    axl-http-core — HTTP/1.1 request/response parser and builder.
    Shared by axl-http-server and axl-http-client.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-http-core.h>
#include "axl-net-internal.h"

AXL_LOG_DOMAIN("http");

// ---------------------------------------------------------------------------
// axl_http_find_header_end
// ---------------------------------------------------------------------------

size_t
axl_http_find_header_end(
    const char *buf,
    size_t      len)
{
    if (len < 4) {
        return 0;
    }

    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i]     == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
        {
            return i + 4;
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// axl_http_parse_request_line
// ---------------------------------------------------------------------------

int
axl_http_parse_request_line(
    const char *line,
    size_t      line_len,
    char      **method,
    char      **path,
    char      **query)
{
    size_t method_end;
    size_t path_start;
    size_t path_end;
    size_t qs_start;
    size_t qs_end;

    if (line == NULL || method == NULL || path == NULL || query == NULL) {
        return AXL_ERR;
    }

    *method = NULL;
    *path   = NULL;
    *query  = NULL;

    //
    // Method: up to first space
    //
    method_end = 0;
    while (method_end < line_len && line[method_end] != ' ') {
        method_end++;
    }

    if (method_end == 0 || method_end >= line_len) {
        axl_error("malformed request line: no method");
        return AXL_ERR;
    }

    //
    // Path: after space, up to space or '?'
    //
    path_start = method_end + 1;
    path_end = path_start;
    while (path_end < line_len && line[path_end] != ' ' && line[path_end] != '?') {
        path_end++;
    }

    //
    // Query string: after '?' up to next space
    //
    qs_start = 0;
    qs_end = 0;
    if (path_end < line_len && line[path_end] == '?') {
        qs_start = path_end + 1;
        qs_end = qs_start;
        while (qs_end < line_len && line[qs_end] != ' ') {
            qs_end++;
        }
    }

    AXL_AUTO_FREE char *m_local = axl_strndup(line, method_end);
    AXL_AUTO_FREE char *p_local = axl_strndup(line + path_start,
                                              path_end - path_start);
    AXL_AUTO_FREE char *q_local = NULL;

    if (qs_start < qs_end) {
        q_local = axl_strndup(line + qs_start, qs_end - qs_start);
        if (q_local == NULL) {
            return AXL_ERR;
        }
    }

    if (m_local == NULL || p_local == NULL) {
        return AXL_ERR;
    }

    *method = axl_steal_pointer(&m_local);
    *path   = axl_steal_pointer(&p_local);
    *query  = axl_steal_pointer(&q_local);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_http_parse_status_line
// ---------------------------------------------------------------------------

int
axl_http_parse_status_line(
    const char *line,
    size_t      line_len,
    size_t     *status_code)
{
    size_t space_pos;

    if (line == NULL || status_code == NULL) {
        return AXL_ERR;
    }

    //
    // Find first space (after "HTTP/1.x")
    //
    space_pos = 0;
    for (size_t j = 0; j < line_len; j++) {
        if (line[j] == ' ') {
            space_pos = j;
            break;
        }
    }

    if (space_pos == 0 || space_pos + 3 >= line_len) {
        axl_error("malformed status line");
        return AXL_ERR;
    }

    //
    // Parse 3-digit status code
    //
    if (line[space_pos + 1] < '0' || line[space_pos + 1] > '9' ||
        line[space_pos + 2] < '0' || line[space_pos + 2] > '9' ||
        line[space_pos + 3] < '0' || line[space_pos + 3] > '9')
    {
        axl_error("malformed status code");
        return AXL_ERR;
    }

    *status_code =
        (size_t)(line[space_pos + 1] - '0') * 100 +
        (size_t)(line[space_pos + 2] - '0') * 10 +
        (size_t)(line[space_pos + 3] - '0');

    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_http_parse_headers
// ---------------------------------------------------------------------------

int
axl_http_parse_headers(
    const char    *data,
    size_t         data_len,
    AxlHashTable **headers)
{
    AxlHashTable *table;
    size_t        pos;

    if (data == NULL || headers == NULL) {
        return AXL_ERR;
    }

    *headers = NULL;

    table = axl_hash_table_new_full(NULL, NULL, axl_free_impl, axl_free_impl);
    if (table == NULL) {
        return AXL_ERR;
    }

    pos = 0;
    while (pos < data_len) {
        //
        // End of headers: empty line
        //
        if (data[pos] == '\r') {
            break;
        }

        //
        // Find end of this header line
        //
        size_t line_end = pos;
        while (line_end < data_len && data[line_end] != '\r') {
            line_end++;
        }

        //
        // Find colon separator
        //
        size_t colon = pos;
        while (colon < line_end && data[colon] != ':') {
            colon++;
        }

        if (colon < line_end) {
            size_t name_len  = colon - pos;
            size_t val_start = colon + 1;

            while (val_start < line_end && data[val_start] == ' ') {
                val_start++;
            }

            size_t val_len = line_end - val_start;

            //
            // Store as lowercase char key, char value copy.
            // AUTO_FREE guards the two buffers: if either alloc
            // fails, or if axl_hash_table_replace rejects the entry,
            // cleanup fires at the end of this block.
            //
            AXL_AUTO_FREE char *name = axl_malloc(name_len + 1);
            AXL_AUTO_FREE char *val  = axl_malloc(val_len + 1);

            if (name != NULL && val != NULL) {
                for (size_t i = 0; i < name_len; i++) {
                    name[i] = (char)axl_tolower((unsigned char)data[pos + i]);
                }

                name[name_len] = '\0';
                axl_memcpy(val, data + val_start, val_len);
                val[val_len] = '\0';

                if (axl_hash_table_replace(table, name, val) != AXL_HASH_TABLE_ERR) {
                    axl_steal_pointer(&name);
                    axl_steal_pointer(&val);
                }
            }
        }

        //
        // Skip past \r\n
        //
        pos = line_end;
        if (pos + 1 < data_len && data[pos] == '\r' && data[pos + 1] == '\n') {
            pos += 2;
        } else {
            break;
        }
    }

    *headers = table;
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_http_get_content_length
// ---------------------------------------------------------------------------

size_t
axl_http_get_content_length(
    AxlHashTable *headers)
{
    const char *val;
    size_t      result;

    if (headers == NULL) {
        return 0;
    }

    val = (const char *)axl_hash_table_lookup(headers, "content-length");
    if (val == NULL) {
        return 0;
    }

    result = 0;
    for (size_t i = 0; val[i] != '\0'; i++) {
        if (val[i] >= '0' && val[i] <= '9') {
            result = result * 10 + (val[i] - '0');
        } else {
            break;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// http_build_request_line
// ---------------------------------------------------------------------------

size_t
http_build_request_line(
    char       *buf,
    size_t      buf_size,
    const char *method,
    const char *path)
{
    return axl_snprintf(buf, buf_size, "%s %s HTTP/1.1\r\n", method, path);
}

// ---------------------------------------------------------------------------
// http_build_status_line
// ---------------------------------------------------------------------------

static const char *
status_reason_phrase(
    size_t code)
{
    switch (code) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

size_t
http_build_status_line(
    char   *buf,
    size_t  buf_size,
    size_t  status_code)
{
    return axl_snprintf(
               buf,
               buf_size,
               "HTTP/1.1 %llu %s\r\n",
               (unsigned long long)status_code,
               status_reason_phrase(status_code));
}

// ---------------------------------------------------------------------------
// IP address parsing
// ---------------------------------------------------------------------------

EFI_STATUS
net_parse_ip_address(
    const char       *string,
    EFI_IPv4_ADDRESS *addr)
{
    size_t octet;
    size_t octet_idx;

    if (string == NULL || addr == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    axl_memset(addr, 0, sizeof(*addr));

    octet = 0;
    octet_idx = 0;

    for (size_t i = 0; ; i++) {
        char ch = string[i];

        if (axl_isdigit((unsigned char)ch)) {
            octet = octet * 10 + (ch - '0');
            if (octet > 255) {
                return EFI_INVALID_PARAMETER;
            }
        } else if (ch == '.' || ch == '\0') {
            if (octet_idx >= 4) {
                return EFI_INVALID_PARAMETER;
            }

            addr->Addr[octet_idx] = (UINT8)octet;
            octet_idx++;
            octet = 0;

            if (ch == '\0') {
                break;
            }
        } else {
            return EFI_INVALID_PARAMETER;
        }
    }

    if (octet_idx != 4) {
        return EFI_INVALID_PARAMETER;
    }

    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// axl_http_parse_range
// ---------------------------------------------------------------------------

static uint64_t
parse_u64(const char *s, const char **endp)
{
    uint64_t val = 0;

    while (*s >= '0' && *s <= '9') {
        if (val > (uint64_t)-1 / 10) {
            /* Overflow — skip remaining digits and return max */
            while (*s >= '0' && *s <= '9') {
                s++;
            }
            *endp = s;
            return (uint64_t)-1;
        }
        val = val * 10 + (uint64_t)(*s - '0');
        s++;
    }
    *endp = s;
    return val;
}

bool
axl_http_parse_range(
    const char   *range_header,
    uint64_t      file_size,
    AxlHttpRange *out)
{
    const char *p;
    uint64_t    start;
    uint64_t    end;

    if (out == NULL) {
        return false;
    }

    out->valid = false;
    out->start = 0;
    out->end = 0;
    out->total = file_size;

    if (range_header == NULL || file_size == 0) {
        return false;
    }

    /* Require "bytes=" prefix (case-insensitive) */
    if (axl_strncasecmp(range_header, "bytes=", 6) != 0) {
        return false;
    }
    p = range_header + 6;

    if (*p == '-') {
        /* Suffix range: bytes=-500 means last 500 bytes */
        p++;
        if (*p < '0' || *p > '9') {
            return false;
        }
        uint64_t suffix = parse_u64(p, &p);
        if (suffix == 0 || suffix > file_size) {
            suffix = file_size;
        }
        start = file_size - suffix;
        end = file_size - 1;
    } else if (*p >= '0' && *p <= '9') {
        /* Start specified */
        start = parse_u64(p, &p);
        if (*p != '-') {
            return false;
        }
        p++;

        if (*p >= '0' && *p <= '9') {
            /* bytes=START-END */
            end = parse_u64(p, &p);
            if (end >= file_size) {
                end = file_size - 1;
            }
        } else {
            /* bytes=START- (open-ended) */
            end = file_size - 1;
        }
    } else {
        return false;
    }

    /* Validate */
    if (start >= file_size || start > end) {
        return false;
    }

    out->start = start;
    out->end = end;
    out->total = file_size;
    out->valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// axl_http_accepts
// ---------------------------------------------------------------------------

bool
axl_http_accepts(const char *accept_header, const char *media_type)
{
    const char *p;
    size_t      mt_len;

    if (accept_header == NULL || media_type == NULL) {
        return false;
    }

    mt_len = axl_strlen(media_type);
    p = accept_header;

    while (*p != '\0') {
        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        /* Check for wildcard */
        if (p[0] == '*' && p[1] == '/' && p[2] == '*') {
            return true;
        }

        /* Compare media type (case-insensitive) */
        if (axl_strncasecmp(p, media_type, mt_len) == 0) {
            char next = p[mt_len];
            /* Must be end of type: delimiter, parameter, or end of string */
            if (next == '\0' || next == ',' || next == ';' || next == ' ') {
                return true;
            }
        }

        /* Advance to next type (past comma) */
        while (*p != '\0' && *p != ',') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
    }

    return false;
}
