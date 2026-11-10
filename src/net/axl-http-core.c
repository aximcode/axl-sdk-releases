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
#include <axl/axl-string.h>   /* axl_asprintf: joining Expect lines */
#include <axl/axl-http-core.h>
#include "axl-net-internal.h"
#include "axl-http-core-internal.h"

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
_axl_http_parse_request_line_version(
    const char *line,
    size_t      line_len,
    char      **method,
    char      **path,
    char      **query,
    unsigned   *major,
    unsigned   *minor)
{
    if (line == NULL || method == NULL || path == NULL || query == NULL
        || major == NULL || minor == NULL) {
        return AXL_ERR;
    }

    *method = NULL;
    *path   = NULL;
    *query  = NULL;
    *major  = 0;
    *minor  = 0;

    /* No control character anywhere in the request line: its parts are
       separated by SP alone, and a CR, LF, HTAB or NUL inside it is no
       part of any of them (RFC 9112 2.2, 3). */
    for (size_t i = 0; i < line_len; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c < 0x20 || c == 0x7f) {
            axl_error("malformed request line: a control character");
            return AXL_ERR;
        }
    }

    /* method SP request-target SP HTTP-version: exactly three parts, one
       SP between each (RFC 9112 3), none empty. A missing version, or a
       target that only ends like one (`GET /HTTP/1.0`), is malformed --
       never a version guessed from the line's last bytes. */
    const char *sp1 = axl_memchr(line, ' ', line_len);
    const char *end = line + line_len;
    const char *sp2 = (sp1 != NULL) ? axl_memchr(sp1 + 1, ' ', (size_t)(end - sp1 - 1)) : NULL;
    if (sp1 == NULL || sp2 == NULL || sp1 == line || sp2 == sp1 + 1) {
        axl_error("malformed request line: not three parts");
        return AXL_ERR;
    }
    /* HTTP-version = "HTTP" "/" DIGIT "." DIGIT, case-sensitive (2.3): the
       last part is exactly those 8 bytes, so a fourth part (a third SP)
       cannot follow it. */
    const char *v = sp2 + 1;
    if (end - v != 8 || axl_memcmp(v, "HTTP/", 5) != 0
        || v[5] < '0' || v[5] > '9' || v[6] != '.' || v[7] < '0' || v[7] > '9') {
        axl_error("malformed request line: no HTTP-version");
        return AXL_ERR;
    }

    const char *target = sp1 + 1;
    const char *qmark  = axl_memchr(target, '?', (size_t)(sp2 - target));
    const char *pend   = (qmark != NULL) ? qmark : sp2;

    AXL_AUTO_FREE char *m_local = axl_strndup(line, (size_t)(sp1 - line));
    AXL_AUTO_FREE char *p_local = axl_strndup(target, (size_t)(pend - target));
    AXL_AUTO_FREE char *q_local = NULL;
    if (qmark != NULL && qmark + 1 < sp2) {
        q_local = axl_strndup(qmark + 1, (size_t)(sp2 - qmark - 1));
        if (q_local == NULL) {
            return AXL_NO_RESOURCES;
        }
    }
    if (m_local == NULL || p_local == NULL) {
        return AXL_NO_RESOURCES;
    }

    *method = axl_steal_pointer(&m_local);
    *path   = axl_steal_pointer(&p_local);
    *query  = axl_steal_pointer(&q_local);
    *major  = (unsigned)(v[5] - '0');
    *minor  = (unsigned)(v[7] - '0');
    return AXL_OK;
}

int
axl_http_parse_request_line(
    const char *line,
    size_t      line_len,
    char      **method,
    char      **path,
    char      **query)
{
    unsigned major = 0;
    unsigned minor = 0;
    return _axl_http_parse_request_line_version(line, line_len, method, path, query,
                                                &major, &minor);
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

    /* No control character but HTAB (the reason phrase may hold one, RFC
       9112 4): a bare CR, NUL or another control inside the line is no
       part of it, as in the request line. */
    for (size_t i = 0; i < line_len; i++) {
        unsigned char c = (unsigned char)line[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            axl_error("malformed status line: a control character");
            return AXL_ERR;
        }
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

/* Whose rules a header block is read with: a server's reading a request,
   or a user agent's reading a response (RFC 9112 2.2, 5.1, 5.2, 6.3). */
typedef enum {
    HTTP_FIELDS_REQUEST,
    HTTP_FIELDS_RESPONSE,
} HttpFieldsMode;

/* RFC 9110 5.6.2 tchar: what a field name (a token) may hold. */
static bool
http_is_tchar(
    unsigned char c
    )
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return true;
    }
    return c != '\0' && axl_strchr("!#$%&'*+-.^_`|~", (int)c) != NULL;
}

/* A byte a field line may hold. A request's value: VCHAR, obs-text, SP or
   HTAB (RFC 9110 5.5) -- no other control character. A response's line:
   anything but CR, LF and NUL, which 5.5 says a recipient MUST NOT keep;
   the other controls it MAY retain. Either way a line holds no CR or LF,
   so it can only end where the tokenizer says. */
static bool
http_is_field_byte(
    unsigned char  c,
    HttpFieldsMode mode
    )
{
    if (mode == HTTP_FIELDS_RESPONSE) {
        return c != '\0' && c != '\r' && c != '\n';
    }
    return c == '\t' || (c >= 0x20 && c != 0x7f);
}

/* The one line tokenizer of a header block. A request's line ends at the
   next CRLF and only there; a response's at the next LF, a CR before it
   being part of the end (RFC 9112 2.2 lets a user agent read a bare LF as
   one). @return true with @p line_end (where the line's own bytes stop)
   and @p next (the next line's start); false when no line end follows --
   the block is cut. */
static bool
http_next_line(
    const char     *data,
    size_t          len,
    size_t          pos,
    HttpFieldsMode  mode,
    size_t         *line_end,
    size_t         *next
    )
{
    for (size_t i = pos; i < len; i++) {
        if (data[i] != '\n') {
            continue;
        }
        if (i > pos && data[i - 1] == '\r') {
            *line_end = i - 1;
        } else if (mode == HTTP_FIELDS_RESPONSE) {
            *line_end = i;
        } else {
            return false;   /* a request's bare LF: not a line end, and
                               refused (its byte check fails on it) */
        }
        *next = i + 1;
        return true;
    }
    return false;
}

/* Is the line at @p pos an obs-fold -- a continuation of the field above
   it (RFC 9112 5.2)? Only a response has them; a request's is refused by
   its name check. */
static bool
http_is_fold(
    const char *data,
    size_t      pos,
    size_t      end
    )
{
    return pos < end && (data[pos] == ' ' || data[pos] == '\t');
}

static bool
http_is_ows(
    char c
    )
{
    return c == ' ' || c == '\t';
}

/* A response's Content-Length: its lines, and the members of a list in
   one (`5, 5`), are one length when all are identical, and refused when
   any differs (RFC 9110 8.6, RFC 9112 6.3). @p prior is the value already
   kept (NULL for the first line); @p val becomes the single member.
   @return AXL_OK, AXL_ERR for a member that differs, AXL_NO_RESOURCES. */
static int
http_cl_merge(
    const char  *prior,
    char       **val
    )
{
    const char *p     = *val;
    const char *first = NULL;
    size_t      flen  = 0;
    for (;;) {
        while (http_is_ows(*p)) {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        const char *end = p;
        while (end > start && http_is_ows(end[-1])) {
            end--;
        }
        size_t n = (size_t)(end - start);
        if (first == NULL) {
            first = start;
            flen  = n;
        } else if (n != flen || axl_memcmp(start, first, n) != 0) {
            return AXL_ERR;
        }
        if (*p == '\0') {
            break;
        }
        p++;
    }
    if (prior != NULL && (axl_strlen(prior) != flen || axl_memcmp(prior, first, flen) != 0)) {
        return AXL_ERR;
    }
    char *one = axl_strndup(first, flen);
    if (one == NULL) {
        return AXL_NO_RESOURCES;
    }
    axl_free(*val);
    *val = one;
    return AXL_OK;
}

/* Keep one field in @p table: @p name (lowercased) and @p val (trimmed),
   both owned by the table on AXL_OK and freed otherwise. A second
   Transfer-Encoding fails in either mode: the table keeps one, and which
   one framed the body would be a guess. A request's second Content-Length
   or Host does too (RFC 9112 3.2, 6.3); a response's Content-Lengths
   merge or fail in http_cl_merge, and its Host means nothing. A request's
   Expect lines join into one list (RFC 9110 5.3), so a second line cannot
   replace an expectation the server would refuse. */
static int
http_field_keep(
    AxlHashTable   *table,
    char           *name,
    char           *val,
    HttpFieldsMode  mode
    )
{
    AXL_AUTO_FREE char *n = name;
    AXL_AUTO_FREE char *v = val;
    const char *prior = axl_hash_table_lookup(table, n);
    bool        is_cl = axl_strcmp(n, "content-length") == 0;
    if (prior != NULL
        && (axl_strcmp(n, "transfer-encoding") == 0
            || (mode == HTTP_FIELDS_REQUEST && (is_cl || axl_strcmp(n, "host") == 0)))) {
        return AXL_ERR;
    }
    if (mode == HTTP_FIELDS_RESPONSE && is_cl) {
        int rc = http_cl_merge(prior, &v);
        if (rc != AXL_OK) {
            return rc;
        }
    }
    if (mode == HTTP_FIELDS_REQUEST && prior != NULL && axl_strcmp(n, "expect") == 0) {
        char *joined = axl_asprintf("%s, %s", prior, v);
        if (joined == NULL) {
            return AXL_NO_RESOURCES;
        }
        axl_free(v);
        v = joined;
    }
    if (axl_hash_table_replace(table, n, v) == AXL_HASH_TABLE_ERR) {
        return AXL_NO_RESOURCES;
    }
    axl_steal_pointer(&n);
    axl_steal_pointer(&v);
    return AXL_OK;
}

/* Read the field starting at the line [@p pos, @p end) -- and, in a
   response, the obs-fold lines after it -- into @p table. @p next is the
   line after the field's last on return. */
static int
http_field_read(
    const char     *data,
    size_t          len,
    size_t          pos,
    size_t          end,
    size_t         *next,
    HttpFieldsMode  mode,
    AxlHashTable   *table
    )
{
    /* The name. A request's: tchars, then the colon at once -- no colon,
       an empty name, whitespace in or after it (5.1; every obs-fold line
       too, 5.2) or any other byte is refused. A response's: the bytes
       before the first colon, the whitespace before it trimmed (5.1: what
       a proxy would deliver). */
    size_t colon = pos;
    size_t name_end;
    if (mode == HTTP_FIELDS_REQUEST) {
        while (colon < end && http_is_tchar((unsigned char)data[colon])) {
            colon++;
        }
        name_end = colon;
    } else {
        while (colon < end && data[colon] != ':') {
            colon++;
        }
        name_end = colon;
        while (name_end > pos && http_is_ows(data[name_end - 1])) {
            name_end--;
        }
    }
    if (name_end == pos || colon == end || data[colon] != ':') {
        return AXL_ERR;
    }
    /* A response's whole line, name included, a request's value: no NUL
       and no bare CR or LF in either, and no other control but HTAB in a
       request's. */
    for (size_t i = (mode == HTTP_FIELDS_RESPONSE) ? pos : colon + 1; i < end; i++) {
        if (!http_is_field_byte((unsigned char)data[i], mode)) {
            return AXL_ERR;
        }
    }

    /* The fold lines after it (a response's), checked the same way. */
    size_t span_end = end;
    size_t after    = *next;
    while (mode == HTTP_FIELDS_RESPONSE) {
        size_t fend = 0;
        size_t fnext = 0;
        if (!http_next_line(data, len, after, mode, &fend, &fnext)) {
            return AXL_ERR;   /* cut */
        }
        if (!http_is_fold(data, after, fend)) {
            break;
        }
        for (size_t i = after; i < fend; i++) {
            if (!http_is_field_byte((unsigned char)data[i], mode)) {
                return AXL_ERR;
            }
        }
        span_end = fend;
        after    = fnext;
    }

    size_t name_len = name_end - pos;
    AXL_AUTO_FREE char *name = axl_malloc(name_len + 1);
    AXL_AUTO_FREE char *val  = axl_malloc(span_end - colon);   /* the span bounds the value */
    if (name == NULL || val == NULL) {
        return AXL_NO_RESOURCES;
    }
    for (size_t i = 0; i < name_len; i++) {
        name[i] = (char)axl_tolower((unsigned char)data[pos + i]);
    }
    name[name_len] = '\0';

    /* The value: the first line's bytes after the colon, then each fold's,
       with the fold itself -- OWS CRLF RWS (RFC 9112 5.2) -- as one SP. */
    size_t vlen = end - (colon + 1);
    axl_memcpy(val, data + colon + 1, vlen);
    size_t at   = *next;
    size_t fend = 0;
    size_t fnext = 0;
    while (at < after && http_next_line(data, len, at, mode, &fend, &fnext)) {
        size_t from = at;
        while (from < fend && http_is_ows(data[from])) {
            from++;
        }
        while (vlen > 0 && http_is_ows(val[vlen - 1])) {
            vlen--;
        }
        val[vlen++] = ' ';
        axl_memcpy(val + vlen, data + from, fend - from);
        vlen += fend - from;
        at = fnext;
    }
    /* Without its optional whitespace on either side (RFC 9110 5.5):
       whitespace inside it is the value's own. */
    size_t vs = 0;
    while (vs < vlen && http_is_ows(val[vs])) {
        vs++;
    }
    while (vlen > vs && http_is_ows(val[vlen - 1])) {
        vlen--;
    }
    axl_memmove(val, val + vs, vlen - vs);
    val[vlen - vs] = '\0';

    *next = after;
    char *n = axl_steal_pointer(&name);
    char *v = axl_steal_pointer(&val);
    return http_field_keep(table, n, v, mode);
}

/* The one tokenizer for a header block, a request's or a response's: the
   WHOLE block or nothing. Every line up to the empty one must be a field
   line (or, in a response, a fold of one), or the block fails. A parser
   that stopped at the first odd byte (it once did, at a bare CR) would
   hand back a table missing the headers behind it -- a Content-Length or
   Transfer-Encoding nobody saw -- with no error. */
static int
http_parse_fields(
    const char     *data,
    size_t          data_len,
    HttpFieldsMode  mode,
    AxlHashTable  **headers
    )
{
    if (data == NULL || headers == NULL) {
        return AXL_ERR;
    }
    *headers = NULL;

    AxlHashTable *table = axl_hash_table_new_full(NULL, NULL, axl_free_impl, axl_free_impl);
    if (table == NULL) {
        return AXL_NO_RESOURCES;
    }
    size_t pos = 0;
    for (;;) {
        size_t end  = 0;
        size_t next = 0;
        if (!http_next_line(data, data_len, pos, mode, &end, &next)) {
            axl_hash_table_free(table);
            return AXL_ERR;   /* no line end: the block is cut */
        }
        if (end == pos) {
            break;   /* the empty line: the block is whole */
        }
        /* A field's folds are read with it, so a fold reaching here has no
           field above it. */
        int rc = http_is_fold(data, pos, end)
                 ? AXL_ERR
                 : http_field_read(data, data_len, pos, end, &next, mode, table);
        if (rc != AXL_OK) {
            axl_hash_table_free(table);
            return rc;
        }
        pos = next;
    }
    *headers = table;
    return AXL_OK;
}

int
axl_http_parse_headers(
    const char    *data,
    size_t         data_len,
    AxlHashTable **headers)
{
    return http_parse_fields(data, data_len, HTTP_FIELDS_REQUEST, headers);
}

int
axl_http_parse_response_headers(
    const char    *data,
    size_t         data_len,
    AxlHashTable **headers)
{
    return http_parse_fields(data, data_len, HTTP_FIELDS_RESPONSE, headers);
}

size_t
axl_http_find_response_header_end(
    const char *buf,
    size_t      len)
{
    if (buf == NULL) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != '\n') {
            continue;
        }
        if (i + 1 < len && buf[i + 1] == '\n') {
            return i + 2;
        }
        if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n') {
            return i + 3;
        }
    }
    return 0;
}


// ---------------------------------------------------------------------------
// axl_http_get_content_length
// ---------------------------------------------------------------------------

int
axl_http_parse_content_length(
    AxlHashTable *headers,
    size_t       *out)
{
    const char *v = (headers != NULL)
                    ? (const char *)axl_hash_table_lookup(headers, "content-length") : NULL;
    size_t      n = 0;

    if (out == NULL) {
        return AXL_INVALID;
    }
    *out = 0;
    if (v == NULL) {
        return AXL_OK;
    }
    if (*v == '\0') {
        return AXL_INVALID;
    }
    for (const char *p = v; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return AXL_INVALID;
        }
        size_t d = (size_t)(*p - '0');
        if (n > (SIZE_MAX - 1 - d) / 10) {
            return AXL_INVALID;   /* would reach SIZE_MAX or wrap */
        }
        n = n * 10 + d;
    }
    *out = n;
    return AXL_OK;
}

size_t
axl_http_get_content_length(
    AxlHashTable *headers)
{
    size_t n = 0;
    return (axl_http_parse_content_length(headers, &n) == AXL_OK) ? n : SIZE_MAX;
}

// ---------------------------------------------------------------------------
// Message framing (axl-http-core-internal.h)
// ---------------------------------------------------------------------------

void
_axl_http_codings_scan(
    const char     *te,
    AxlHttpCodings *out)
{
    axl_memset(out, 0, sizeof(*out));
    const char *p = te;
    for (;;) {
        while (http_is_ows(*p)) {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        const char *end = p;
        while (end > start && http_is_ows(end[-1])) {
            end--;
        }
        size_t n = (size_t)(end - start);
        if (n == 0) {
            out->empty = true;
        }
        out->last_chunked = n == 7 && axl_strncasecmp(start, "chunked", 7) == 0;
        if (out->last_chunked) {
            out->chunked++;
        } else if (n > 0) {
            out->other++;
        }
        if (*p == '\0') {
            break;
        }
        p++;
    }
}

int
_axl_http_response_framing(
    AxlHashTable   *headers,
    const char     *method,
    size_t          status,
    AxlHttpFraming *out)
{
    const char *te = axl_hash_table_lookup(headers, "transfer-encoding");
    bool        cl = axl_hash_table_lookup(headers, "content-length") != NULL;
    out->length   = 0;
    out->reusable = true;
    /* Rule 1: no body, whatever the fields say -- a response to HEAD, a
       1xx, 204 or 304, and a 2xx to CONNECT (a tunnel follows, not a body). */
    bool head    = method != NULL && axl_strcmp(method, "HEAD") == 0;
    bool connect = method != NULL && axl_strcmp(method, "CONNECT") == 0;
    if (head || status / 100 == 1 || status == 204 || status == 304
        || (connect && status / 100 == 2)) {
        out->kind = AXL_HTTP_BODY_NONE;
        return AXL_OK;
    }
    if (te != NULL) {
        AxlHttpCodings cod;
        _axl_http_codings_scan(te, &cod);
        bool chunked  = cod.last_chunked && cod.chunked == 1 && !cod.empty;
        out->kind     = chunked ? AXL_HTTP_BODY_CHUNKED : AXL_HTTP_BODY_TO_CLOSE;
        out->reusable = chunked && !cl;
        return AXL_OK;
    }
    if (!cl) {
        /* Rule 8: neither -- the body runs to the close, and the
           connection ends with it. */
        out->kind     = AXL_HTTP_BODY_TO_CLOSE;
        out->reusable = false;
        return AXL_OK;
    }
    out->kind = AXL_HTTP_BODY_LENGTH;
    return (axl_http_parse_content_length(headers, &out->length) == AXL_OK) ? AXL_OK : AXL_ERR;
}

int
_axl_http_response_head_parse(
    const char    *buf,
    size_t         head_len,
    size_t        *status_code,
    AxlHashTable **headers)
{
    *headers = NULL;
    const char *lf = axl_memchr(buf, '\n', head_len);
    if (lf == NULL) {
        return AXL_ERR;
    }
    size_t next     = (size_t)(lf - buf) + 1;
    size_t line_len = next - 1;
    if (line_len > 0 && buf[line_len - 1] == '\r') {
        line_len--;
    }
    if (axl_http_parse_status_line(buf, line_len, status_code) != AXL_OK) {
        return AXL_ERR;
    }
    return axl_http_parse_response_headers(buf + next, head_len - next, headers);
}

/* The status a parser's failure answers: running out of memory is the
   server's failure (500), anything else the request's (400). */
static size_t
http_head_refusal(
    int rc
    )
{
    return (rc == AXL_NO_RESOURCES) ? 500 : 400;
}

size_t
_axl_http_request_head_parse(
    const char     *buf,
    size_t          head_len,
    AxlHttpReqHead *out)
{
    axl_memset(out, 0, sizeof(*out));

    /* The request line ends at the first CRLF (axl_http_find_header_end
       found the head's end, so there is one). */
    const char *crlf = axl_memmem(buf, head_len, "\r\n", 2);
    if (crlf == NULL) {
        return 400;
    }
    size_t   line_len = (size_t)(crlf - buf);
    unsigned major    = 0;
    unsigned minor    = 0;
    int      rc       = _axl_http_parse_request_line_version(buf, line_len, &out->method,
                                                             &out->path, &out->query,
                                                             &major, &minor);
    if (rc != AXL_OK) {
        return http_head_refusal(rc);
    }
    /* HTTP/1.x only: another major version is one this server does not
       speak (RFC 9110 15.6.6). */
    if (major != 1) {
        return 505;
    }
    out->http10 = minor == 0;

    /* axl_http_parse_headers parses the whole block or fails it: a bare
       CR or LF, a control character, a name that is not a token, a line
       without a colon (obs-fold included) or a second Content-Length /
       Transfer-Encoding / Host line is 400 here. */
    size_t start = line_len + 2;
    rc = axl_http_parse_headers(buf + start, head_len - start, &out->headers);
    if (rc != AXL_OK) {
        return http_head_refusal(rc);
    }
    /* An HTTP/1.1 request names its host (RFC 9112 3.2); HTTP/1.0 may
       leave it out. */
    if (!out->http10 && axl_hash_table_lookup(out->headers, "host") == NULL) {
        return 400;
    }
    return 0;
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
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
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
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 423: return "Locked";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    case 507: return "Insufficient Storage";
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

// ---------------------------------------------------------------------------
// _axl_http_expect_check
// ---------------------------------------------------------------------------

AxlHttpExpect
_axl_http_expect_check(
    const char *value,
    bool        http10)
{
    static const char continue_token[] = "100-continue";
    const size_t      token_len = sizeof(continue_token) - 1;

    if (value == NULL) {
        return AXL_HTTP_EXPECT_NONE;
    }
    /* The field value without its OWS (RFC 9110 5.5). */
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    size_t len = axl_strlen(value);
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t')) {
        len--;
    }
    if (len == 0) {
        return AXL_HTTP_EXPECT_NONE;
    }
    if (len == token_len && axl_strncasecmp(value, continue_token, token_len) == 0) {
        /* A 1.0 client may not understand a 1xx: MUST ignore (10.1.1). */
        return http10 ? AXL_HTTP_EXPECT_NONE : AXL_HTTP_EXPECT_CONTINUE;
    }
    return AXL_HTTP_EXPECT_FAILED;
}
