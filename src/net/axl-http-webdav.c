/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-http-webdav.c
    WebDAV class-1 + MOVE/COPY adjunct to AxlHttpServer (RFC 4918 §9).

    Verb scope: OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL,
    MOVE, COPY. PROPPATCH, LOCK, UNLOCK, and If-header conditionals
    are out of scope for v1 — clients (Windows Explorer, macOS
    Finder, davfs2, cadaver) work without them when the server
    doesn't advertise DAV: 2 / class 2.

    This file owns all the WebDAV protocol bits. The consumer's
    filesystem ops are wired in via AxlWebDavOps.
**/

#include "axl-http-server-internal.h"
#include <axl/axl-string.h>
#include <axl/axl-xml.h>

AXL_LOG_DOMAIN("webdav");

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static int on_options (AxlHttpRequest *req, AxlHttpResponse *resp, void *data);
static int on_mkcol   (AxlHttpRequest *req, AxlHttpResponse *resp, void *data);
static int on_delete  (AxlHttpRequest *req, AxlHttpResponse *resp, void *data);
static int on_move    (AxlHttpRequest *req, AxlHttpResponse *resp, void *data);
static int on_copy    (AxlHttpRequest *req, AxlHttpResponse *resp, void *data);
static int on_propfind(AxlHttpRequest *req, AxlHttpResponse *resp, void *data);
static int on_get_or_head(AxlHttpRequest *req, AxlHttpResponse *resp, void *data);
static int on_dav_upload (AxlHttpRequest *req, AxlHttpResponse *resp,
                          const void *chunk, size_t chunk_size,
                          void *data, bool aborted);

// ---------------------------------------------------------------------------
// Lifecycle helpers
// ---------------------------------------------------------------------------

void
webdav_ctx_free(AxlWebDavCtx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    axl_free(ctx->prefix);
    axl_free(ctx);
}

/* Strip the registered prefix off the request URL, returning the
   path the consumer's ops table sees ("/" for root, "/foo/bar" for
   nested). Returns NULL if the request URL doesn't start with the
   prefix (shouldn't happen — the router already matched, but be
   defensive). */
static const char *
strip_prefix(const AxlWebDavCtx *ctx, const char *path)
{
    if (path == NULL) {
        return NULL;
    }
    if (axl_strncmp(path, ctx->prefix, ctx->prefix_len) != 0) {
        return NULL;
    }
    const char *rel = path + ctx->prefix_len;
    if (*rel == '\0') {
        return "/";
    }
    return rel;
}

// ---------------------------------------------------------------------------
// Response helpers — all WebDAV responses get DAV: 1 + Content-Type
// (when applicable) via this path so individual verb handlers can
// stay short.
// ---------------------------------------------------------------------------

/* Lazy-allocate r->headers under the full-ownership contract that
   axl_http_response_set_content_range / dispatch_request expect.
   Mirrors the pattern in axl-http-response.c. Returns true on
   success (table now exists with the right destroy funcs).

   When r->headers is pre-allocated by some other path with the
   wrong destroy-func contract, we'd silently leak strdup'd
   key/value bytes per insert — which would also drop the DAV: 1
   header consumers depend on. Warn loudly so the cause shows up
   in logs instead of as a head-scratching missing-header. */
static bool
ensure_headers(AxlHttpResponse *r)
{
    if (r->headers != NULL) {
        if (axl_hash_table_owns_entries(r->headers)) {
            return true;
        }
        axl_warning("webdav: r->headers pre-allocated with wrong "
                    "destroy-func contract; DAV / Allow / "
                    "Content-Range headers will NOT be inserted "
                    "(would leak key+value strdups). Allocate via "
                    "axl_hash_table_new_full(..., axl_free_impl, "
                    "axl_free_impl) or leave NULL for lazy alloc.");
        return false;
    }
    r->headers = axl_hash_table_new_full(
        axl_str_hash, axl_str_equal,
        axl_free_impl, axl_free_impl);
    return r->headers != NULL;
}

static void
insert_header(AxlHttpResponse *r, const char *key, const char *value)
{
    if (!ensure_headers(r)) {
        return;
    }
    char *k = axl_strdup(key);
    char *v = axl_strdup(value);
    if (k == NULL || v == NULL) {
        axl_free(k);
        axl_free(v);
        return;
    }
    axl_hash_table_replace(r->headers, k, v);
}

/* RFC 4918 §10.1: every WebDAV-method response MUST include a DAV
   header. Class 1 = "DAV: 1". */
static void
set_dav_class_header(AxlHttpResponse *r)
{
    insert_header(r, "DAV", "1");
}

// ---------------------------------------------------------------------------
// Header parsers — Depth, Destination, Overwrite (RFC 4918 §10.{2,3,6})
// ---------------------------------------------------------------------------

/* Depth: "0" | "1" | "infinity" — RFC 4918 §10.2. Returns 0, 1, or
   -1 (infinity / treat as deep walk). Default per verb varies; the
   caller passes a default for the missing-header case. */
static int
parse_depth(const AxlHttpRequest *req, int default_depth)
{
    if (req == NULL || req->headers == NULL) {
        return default_depth;
    }
    const char *v = (const char *)axl_hash_table_lookup(req->headers, "depth");
    if (v == NULL) {
        return default_depth;
    }
    if (axl_strcmp(v, "0") == 0) return 0;
    if (axl_strcmp(v, "1") == 0) return 1;
    if (axl_strcasecmp(v, "infinity") == 0) return -1;
    return default_depth;
}

/* Overwrite: "T" | "F" — RFC 4918 §10.6. Default per RFC is T (true). */
static bool
parse_overwrite(const AxlHttpRequest *req)
{
    if (req == NULL || req->headers == NULL) {
        return true;
    }
    const char *v = (const char *)axl_hash_table_lookup(req->headers,
                                                        "overwrite");
    if (v == NULL) {
        return true;
    }
    return v[0] != 'F' && v[0] != 'f';
}

/* Destination header: a URL — usually absolute ("http://host:port/dav/foo")
   but may be a path. Returns the path component RELATIVE to the
   webdav prefix, or NULL on parse failure / cross-mount destination.
   Writes into @p out (caller-supplied buffer). */
static int
parse_destination(const AxlWebDavCtx *ctx,
                  const AxlHttpRequest *req,
                  char *out,
                  size_t out_size)
{
    if (req == NULL || req->headers == NULL || out == NULL || out_size == 0) {
        return AXL_ERR;
    }
    const char *v = (const char *)axl_hash_table_lookup(
        req->headers, "destination");
    if (v == NULL) {
        return AXL_ERR;
    }

    /* Skip scheme + authority if present. "http://host[:port]/path"
       → start at the first "/" after the "://". */
    const char *path = v;
    const char *scheme_end = axl_strstr(v, "://");
    if (scheme_end != NULL) {
        path = axl_strchr(scheme_end + 3, '/');
        if (path == NULL) {
            return AXL_ERR;
        }
    }

    /* Must be inside our prefix. */
    if (axl_strncmp(path, ctx->prefix, ctx->prefix_len) != 0) {
        return AXL_ERR;
    }
    const char *rel = path + ctx->prefix_len;
    if (*rel == '\0') {
        rel = "/";
    } else if (*rel != '/') {
        /* prefix matched as substring but not at a path boundary
           ("/dav" prefix vs "/davother/foo") — reject. */
        return AXL_ERR;
    }

    if (axl_strlen(rel) >= out_size) {
        return AXL_ERR;
    }
    axl_memcpy(out, rel, axl_strlen(rel) + 1);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// PROPFIND helpers — RFC 1123 date formatting + <D:response> entry
// builder. Element escaping is delegated to AxlXmlWriter; this file
// used to carry a hand-rolled `<>&"'` escaper that's now obsolete.
// ---------------------------------------------------------------------------

/* unix_to_rfc1123: epoch seconds → "Sun, 06 Nov 1994 08:49:37 GMT".
   Civil-from-days algorithm (Howard Hinnant, public domain) — works
   for any year in the proleptic Gregorian calendar. ~30 lines vs
   pulling in a libc dependency. */
static void
unix_to_rfc1123(uint64_t epoch_secs, char *out, size_t out_size)
{
    static const char *DOW[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char *MON[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec" };
    int64_t  days  = (int64_t)(epoch_secs / 86400);
    uint32_t s_day = (uint32_t)(epoch_secs % 86400);
    uint32_t hour  = s_day / 3600;
    uint32_t minu  = (s_day / 60) % 60;
    uint32_t sec   = s_day % 60;

    /* Day of week: 1970-01-01 was a Thursday (dow=4). */
    int dow = (int)((days + 4) % 7);
    if (dow < 0) dow += 7;

    /* Civil-from-days: shift epoch to internal era starting 0000-03-01. */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int64_t  y   = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365*yoe + yoe/4 - yoe/100);
    uint32_t mp  = (5*doy + 2) / 153;
    uint32_t d   = doy - (153*mp + 2)/5 + 1;
    uint32_t m   = (mp < 10) ? mp + 3 : mp - 9;
    if (m <= 2) y += 1;

    axl_snprintf(out, out_size, "%s, %02u %s %04llu %02u:%02u:%02u GMT",
                 DOW[dow], (unsigned)d, MON[m - 1],
                 (unsigned long long)y, hour, minu, sec);
}

/* Compose the absolute href "<prefix>/<rel>[/]" into @p out. Caller
   passes a stack buffer; on overflow returns false. The trailing
   slash on directories is a strong RFC 4918 §5.2 hint clients
   follow when building child URLs. */
static bool
compose_href(const AxlWebDavCtx *ctx, const char *rel, bool is_dir,
             char *out, size_t out_size)
{
    /* prefix is e.g. "/dav" (no trailing slash); rel starts with
       "/" so the join is correct. Root rel ("/") becomes the bare
       prefix; the is_dir branch below appends the trailing slash. */
    int n;
    if (axl_strcmp(rel, "/") == 0) {
        n = axl_snprintf(out, out_size, "%s/", ctx->prefix);
    } else {
        n = axl_snprintf(out, out_size, "%s%s", ctx->prefix, rel);
    }
    if (n < 0 || (size_t)n >= out_size) {
        return false;
    }
    size_t len = (size_t)n;
    if (is_dir && len > 0 && out[len - 1] != '/') {
        if (len + 1 >= out_size) {
            return false;
        }
        out[len++] = '/';
        out[len]   = '\0';
    }
    return true;
}

/* Emit one <D:response> for @p e living at @p rel within ctx via
   the AxlXmlWriter. */
static void
emit_entry(AxlXmlWriter *w, const AxlWebDavCtx *ctx,
           const char *rel, const AxlFsEntry *e)
{
    axl_xml_writer_start_element(w, "D:response");

    /* href */
    char href[1024];
    if (compose_href(ctx, rel, axl_fs_entry_is_dir(e), href, sizeof(href))) {
        axl_xml_writer_start_element(w, "D:href");
        axl_xml_writer_text(w, href);
        axl_xml_writer_end_element(w);
    }
    /* On href overflow we silently drop the href element. The
       parent compose path's 512-byte cap on @p rel makes this
       effectively unreachable; the writer's sticky-error flag
       would surface a real OOM at finish. */

    axl_xml_writer_start_element(w, "D:propstat");
    axl_xml_writer_start_element(w, "D:prop");

    /* resourcetype */
    axl_xml_writer_start_element(w, "D:resourcetype");
    if (axl_fs_entry_is_dir(e)) {
        axl_xml_writer_start_element(w, "D:collection");
        axl_xml_writer_end_element(w);
    }
    axl_xml_writer_end_element(w);

    /* displayname */
    axl_xml_writer_start_element(w, "D:displayname");
    axl_xml_writer_text(w, e->name);
    axl_xml_writer_end_element(w);

    /* getcontentlength — files only; directories don't carry a
       meaningful size per RFC 4918 §15.4. */
    if (!axl_fs_entry_is_dir(e)) {
        char clen[32];
        axl_snprintf(clen, sizeof(clen), "%llu",
                     (unsigned long long)e->size);
        axl_xml_writer_start_element(w, "D:getcontentlength");
        axl_xml_writer_text(w, clen);
        axl_xml_writer_end_element(w);
    }

    /* getlastmodified — omit if unknown (mtime_unix == 0). */
    if (e->mtime_unix != 0) {
        char date[64];
        unix_to_rfc1123(e->mtime_unix, date, sizeof(date));
        axl_xml_writer_start_element(w, "D:getlastmodified");
        axl_xml_writer_text(w, date);
        axl_xml_writer_end_element(w);
    }

    axl_xml_writer_end_element(w);  /* D:prop */

    axl_xml_writer_start_element(w, "D:status");
    axl_xml_writer_text(w, "HTTP/1.1 200 OK");
    axl_xml_writer_end_element(w);

    axl_xml_writer_end_element(w);  /* D:propstat */
    axl_xml_writer_end_element(w);  /* D:response */
}

// ---------------------------------------------------------------------------
// Per-verb handlers
// ---------------------------------------------------------------------------

static int
on_propfind(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    AxlWebDavCtx *ctx = data;
    set_dav_class_header(resp);

    const char *rel_raw = strip_prefix(ctx, req->path);
    if (rel_raw == NULL) {
        resp->status_code = 400;
        return 0;
    }
    /* Normalize: clients commonly request collections with a
       trailing slash ("/dav/sub/" — Finder, cadaver, rclone all do
       this). Strip the slash unless rel is just "/" (the mount
       root) so list_dir's parent-match and child-path concat both
       see the canonical form. Without this:
         - test_dav_list_collect's "path[parent_len] != '/'" reject
           silently drops every child.
         - emit_entry concats rel + "/" + name → "/sub//x" hrefs
           that violate RFC 3986. */
    char rel_buf[512];
    size_t rel_len = axl_strlen(rel_raw);
    if (rel_len >= sizeof(rel_buf)) {
        resp->status_code = 414;  /* URI Too Long */
        return 0;
    }
    axl_memcpy(rel_buf, rel_raw, rel_len + 1);
    while (rel_len > 1 && rel_buf[rel_len - 1] == '/') {
        rel_buf[--rel_len] = '\0';
    }
    const char *rel = rel_buf;

    if (ctx->ops.stat == NULL) {
        resp->status_code = 405;
        return 0;
    }

    /* Stat the resource itself first — drives both the Depth=0
       case AND the parent <D:response> in Depth=1. RFC 4918 §9.1
       says a missing resource returns 404 (not a 207 envelope). */
    AxlFsEntry self;
    axl_memset(&self, 0, sizeof(self));
    if (ctx->ops.stat(ctx->user_data, rel, &self) != AXL_OK) {
        resp->status_code = 404;
        return 0;
    }

    /* Depth header: default per RFC 4918 §9.1 is "infinity" but
       infinity-recursive PROPFIND is expensive AND many servers
       (including SoftBMC's existing impl) cap at 1. v1 treats
       infinity the same as 1 — clients that care about deep walks
       can recurse themselves. */
    int depth = parse_depth(req, 1);
    /* depth values: 0 = self only, 1 = self + immediate children,
       -1 = infinity (RFC 4918). v1 treats infinity as 1 — clients
       that need deep walks recurse themselves. */
    bool include_children = (depth != 0);

    AxlString *out = axl_string_new_size(4096);
    if (out == NULL) {
        resp->status_code = 500;
        return 0;
    }

    AxlXmlWriter xw;
    axl_xml_writer_init(&xw, out, AXL_XML_WRITER_DEFAULT);
    axl_xml_writer_prologue(&xw);
    axl_xml_writer_start_element(&xw, "D:multistatus");
    axl_xml_writer_attribute(&xw, "xmlns:D", "DAV:");

    /* Self entry. The displayname is the basename of rel; for the
       mount root that's "" — emit "/" so clients have something
       sensible to show. */
    if (axl_strcmp(rel, "/") == 0 && self.name[0] == '\0') {
        axl_strlcpy(self.name, "/", sizeof(self.name));
    }
    emit_entry(&xw, ctx, rel, &self);

    /* Children — only when Depth > 0 AND the resource is a dir
       AND the consumer wired list_dir. */
    if (include_children && axl_fs_entry_is_dir(&self) && ctx->ops.list_dir != NULL) {
        /* Cap: enough for typical UEFI volumes. Consumers with
           larger directories should paginate at a higher layer
           (out of scope for v1). */
        AxlFsEntry kids[256];
        size_t kids_max = sizeof(kids) / sizeof(kids[0]);
        size_t count = 0;
        if (ctx->ops.list_dir(ctx->user_data, rel, kids,
                              kids_max, &count) == AXL_OK) {
            if (count == kids_max) {
                axl_warning("propfind: directory '%s' has >= %zu "
                            "entries; truncating PROPFIND result. "
                            "v1 does not paginate; consumers with "
                            "very large directories must batch at "
                            "a higher layer.",
                            rel, kids_max);
            }
            for (size_t i = 0; i < count; i++) {
                /* Build the child's path = (rel == "/" ? "" : rel)
                   + "/" + kids[i].name. rel is normalized (no
                   trailing slash) so this never produces "//". */
                char child_path[512];
                bool root = (axl_strcmp(rel, "/") == 0);
                int n = axl_snprintf(child_path, sizeof(child_path),
                                     "%s/%s",
                                     root ? "" : rel,
                                     kids[i].name);
                if (n < 0 || (size_t)n >= sizeof(child_path)) {
                    axl_warning("propfind: child path overflow "
                                "(rel='%s', name='%s'); skipping",
                                rel, kids[i].name);
                    continue;
                }
                emit_entry(&xw, ctx, child_path, &kids[i]);
            }
        }
    }

    axl_xml_writer_end_element(&xw);  /* D:multistatus */
    axl_xml_writer_finish(&xw);
    if (axl_xml_writer_error(&xw)) {
        axl_string_free(out);
        resp->status_code = 500;
        return 0;
    }

    /* Hand the body off as a copy via set_text + status 207. We
       free the AxlString after — set_text already strdup'd.
       axl_string_str always returns a valid pointer (empty string at
       worst); the real OOM was caught by axl_xml_writer_error above. */
    axl_http_response_set_text(resp, axl_string_str(out));
    axl_string_free(out);
    /* set_text defaults content_type to text/plain; RFC 4918 §9.1
       requires application/xml for PROPFIND responses. */
    resp->content_type = "application/xml; charset=\"utf-8\"";
    resp->status_code  = 207;
    return 0;
}

// ---------------------------------------------------------------------------
// GET / HEAD — single handler covers both. HEAD skips the streaming
// body and just emits the headers (Content-Length, Content-Type,
// Last-Modified). RFC 7233 Range support uses the existing
// axl_http_parse_range + axl_http_response_set_streamer +
// axl_http_response_set_content_range primitives.
// ---------------------------------------------------------------------------

typedef struct {
    AxlWebDavCtx *ctx;
    void         *read_ctx;
} DavGetCtx;

static int
dav_read_streamer(void *vctx, void *out_buf, size_t out_buf_size,
                  size_t *out_size)
{
    DavGetCtx *gs = vctx;
    return gs->ctx->ops.read_chunk(gs->read_ctx, out_buf,
                                   out_buf_size, out_size);
}

static void
dav_read_cleanup(void *vctx)
{
    DavGetCtx *gs = vctx;
    gs->ctx->ops.read_close(gs->read_ctx);
    axl_free(gs);
}

static int
on_get_or_head(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    AxlWebDavCtx *ctx = data;
    set_dav_class_header(resp);

    bool head_only = (axl_strcmp(req->method, "HEAD") == 0);

    const char *rel = strip_prefix(ctx, req->path);
    if (rel == NULL) {
        resp->status_code = 400;
        return 0;
    }
    if (ctx->ops.stat == NULL) {
        resp->status_code = 405;
        return 0;
    }

    AxlFsEntry e;
    axl_memset(&e, 0, sizeof(e));
    if (ctx->ops.stat(ctx->user_data, rel, &e) != AXL_OK) {
        resp->status_code = 404;
        return 0;
    }
    if (axl_fs_entry_is_dir(&e)) {
        /* GET on a collection — could redirect to a directory
           listing, render HTML, or return PROPFIND-equivalent.
           v1 returns 405; clients use PROPFIND for directory
           contents. */
        resp->status_code = 405;
        return 0;
    }

    /* Content-Type from optional consumer hint, else octet-stream. */
    const char *ct = NULL;
    if (ctx->ops.content_type != NULL) {
        ct = ctx->ops.content_type(ctx->user_data, rel);
    }
    if (ct == NULL) {
        ct = "application/octet-stream";
    }
    resp->content_type = ct;

    /* Last-Modified header (RFC 7232 §2.2) when known. */
    if (e.mtime_unix != 0) {
        char date[64];
        unix_to_rfc1123(e.mtime_unix, date, sizeof(date));
        insert_header(resp, "Last-Modified", date);
    }

    /* Want-Digest / Digest emission (RFC 3230). Only fires when
       the consumer wired ops.digest AND the client sent a
       `Want-Digest:` request header. Walks the algorithm list in
       client-listed order (q-values deferred — they'd let us
       prefer one algo over another, but for now first-supported
       wins, which matches what every WebDAV mount client we've
       seen asks for in single-algo terms). */
    const char *want_digest = (req->headers != NULL)
        ? (const char *)axl_hash_table_lookup(req->headers, "want-digest")
        : NULL;
    if (ctx->ops.digest != NULL && want_digest != NULL) {
        /* SHA-512 hex is 128 chars + NUL, so 256 fits everything
           through SHA-3-512 with room for the `<algo>=` prefix
           when we format the response header below. */
        char hex[256];
        char algo[32];
        const char *p = want_digest;
        while (*p != '\0') {
            /* Skip leading commas + whitespace. */
            while (*p == ',' || *p == ' ' || *p == '\t') p++;
            if (*p == '\0') break;
            /* Capture the algorithm token: legal chars are letters,
               digits, '-'. Lowercase as we go (HTTP tokens are
               case-insensitive per RFC 3230 §4.3.1). */
            size_t a = 0;
            while (((*p >= 'A' && *p <= 'Z') ||
                    (*p >= 'a' && *p <= 'z') ||
                    (*p >= '0' && *p <= '9') ||
                    *p == '-') && a < sizeof(algo) - 1)
            {
                char c = *p++;
                if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
                algo[a++] = c;
            }
            algo[a] = '\0';
            /* Skip the rest of this entry (q-value, etc.) up to the
               next comma. */
            while (*p != '\0' && *p != ',') p++;
            if (a == 0) continue;
            /* Try this algorithm. First success wins. */
            hex[0] = '\0';
            if (ctx->ops.digest(ctx->user_data, rel, algo,
                                hex, sizeof(hex)) == AXL_OK &&
                hex[0] != '\0')
            {
                char header_val[320];
                axl_snprintf(header_val, sizeof(header_val),
                             "%s=%s", algo, hex);
                insert_header(resp, "Digest", header_val);
                break;
            }
        }
    }

    /* Range parsing — single-range only (the SDK's
       axl_http_parse_range matches RFC 7233's bytes=START-END /
       START- / -SUFFIX shapes and clamps to file size).

       parse_range returns false for both malformed input AND
       well-formed-but-unsatisfiable (start >= file_size). RFC
       7233 section 4.4 distinguishes: unsatisfiable MUST be 416
       with Content-Range: bytes (asterisk)/(size); malformed
       MAY be ignored (200 with full body). Cheap pre-check
       picks unsatisfiable out of the false bucket. */
    AxlHttpRange range;
    bool is_partial = false;
    const char *range_hdr = (req->headers != NULL)
        ? (const char *)axl_hash_table_lookup(req->headers, "range")
        : NULL;
    if (range_hdr != NULL &&
        axl_strncasecmp(range_hdr, "bytes=", 6) == 0)
    {
        if (axl_http_parse_range(range_hdr, e.size, &range) &&
            range.valid)
        {
            is_partial = true;
        } else {
            /* Walk the start byte. Suffix form (bytes=-N) is
               always satisfiable post-clamp, so only check the
               bytes=START[-END] case. */
            const char *p = range_hdr + 6;
            if (*p >= '0' && *p <= '9') {
                uint64_t start_check = 0;
                while (*p >= '0' && *p <= '9') {
                    start_check = start_check * 10 +
                                  (uint64_t)(*p - '0');
                    p++;
                }
                if (*p == '-' && start_check >= e.size) {
                    /* RFC 7233 section 4.4 -- 416 with the
                       Content-Range form `bytes (asterisk)/(size)`. */
                    char cr[64];
                    axl_snprintf(cr, sizeof(cr), "bytes */%llu",
                                 (unsigned long long)e.size);
                    insert_header(resp, "Content-Range", cr);
                    resp->status_code = 416;
                    return 0;
                }
            }
            /* Otherwise: malformed Range, fall through to full
               200 per RFC 7233 §3.1 (server MAY ignore). */
        }
    }

    uint64_t start = is_partial ? range.start : 0;
    uint64_t end   = is_partial ? range.end   : (e.size > 0 ? e.size - 1 : 0);
    uint64_t slice = (e.size == 0) ? 0 : (end - start + 1);

    if (head_only) {
        /* Headers-only response. Set Content-Length explicitly via
           the headers table (send_response only auto-emits
           Content-Length when body != NULL). */
        char clen[32];
        axl_snprintf(clen, sizeof(clen), "%llu",
                     (unsigned long long)slice);
        insert_header(resp, "Content-Length", clen);
        if (is_partial) {
            axl_http_response_set_content_range(resp, start, end, e.size);
            resp->status_code = 206;
        } else {
            resp->status_code = 200;
        }
        /* Leave body == NULL so send_response sends headers only.
           The auto-Content-Length only fires when body != NULL, so
           our explicit one wins. */
        return 0;
    }

    /* GET — open + stream. */
    if (ctx->ops.read_open  == NULL || ctx->ops.read_chunk == NULL ||
        ctx->ops.read_close == NULL) {
        resp->status_code = 405;
        return 0;
    }

    DavGetCtx *gs = axl_calloc(1, sizeof(*gs));
    if (gs == NULL) {
        resp->status_code = 500;
        return 0;
    }
    gs->ctx = ctx;
    if (ctx->ops.read_open(ctx->user_data, rel, start, &gs->read_ctx) != AXL_OK) {
        axl_free(gs);
        resp->status_code = 500;
        return 0;
    }

    /* set_streamer sets Content-Length when total_size != (size_t)-1.
       We always know the slice size up front (Range or full file)
       so use that — chunked transfer-encoding isn't needed. */
    axl_http_response_set_streamer(resp, dav_read_streamer, gs,
                                   dav_read_cleanup, slice, ct);
    if (is_partial) {
        axl_http_response_set_content_range(resp, start, end, e.size);
        resp->status_code = 206;
    } else {
        resp->status_code = 200;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// PUT — drives the AxlUploadHandler chunk callback. Single in-flight
// PUT per mount; concurrent uploads on the same WebDAV mount are
// not supported in v1.
// ---------------------------------------------------------------------------

/* Lowercase a single ASCII byte. Idempotent on lowercase / non-alpha. */
static inline char
ascii_tolower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* On clean PUT EOF (called when resp->status_code is 201), verify a
   client-supplied `Content-Digest: <algo>=<hex>` request header
   against the just-uploaded file's actual digest. Reuses ops.digest
   — the same callback that powers response-side Digest emission.
   The header value is plain hex (axl-webfs's chosen symmetric
   format; RFC 9530's `:base64:` wrapper form is not parsed in v1).

   Failure modes:
     - Header absent → no-op (consumer opted out of validation).
     - ops.digest not wired → no-op (SDK can't verify; pass through).
     - Digest callback returns AXL_ERR → no-op (consumer can't supply
       hex for this algo; pass through rather than fail valid uploads).
     - Hex mismatch → respond 400, best-effort delete via ops.remove
       so the corrupted bytes don't linger.

   The mismatch + cleanup path is best-effort: if ops.remove is not
   wired or returns an error, the bad file may remain. Either way
   the 400 status tells the client the upload failed. */
static void
content_digest_validate(AxlWebDavCtx *ctx, AxlHttpRequest *req,
                        AxlHttpResponse *resp)
{
    if (ctx->ops.digest == NULL || req == NULL || req->headers == NULL) {
        return;
    }
    const char *header = (const char *)axl_hash_table_lookup(
        req->headers, "content-digest");
    if (header == NULL) {
        return;
    }

    /* Parse <algo>=<hex>. Skip leading whitespace, capture algo
       (lowercase), require `=`, capture hex (lowercase) — RFC 9530
       structured-field colon wrappers and q-values not supported. */
    const char *p = header;
    while (*p == ' ' || *p == '\t') p++;
    char algo[32];
    size_t a = 0;
    while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-') &&
           a < sizeof(algo) - 1)
    {
        algo[a++] = ascii_tolower(*p++);
    }
    algo[a] = '\0';
    if (a == 0 || *p != '=') {
        return;  /* malformed — silently skip */
    }
    p++;  /* consume '=' */

    /* Hex body up to whitespace / comma / end. Normalize case as we
       walk so the comparison is byte-exact against ops.digest's
       lowercase output. */
    char claimed[256];
    size_t h = 0;
    while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t' &&
           h < sizeof(claimed) - 1)
    {
        claimed[h++] = ascii_tolower(*p++);
    }
    claimed[h] = '\0';
    if (h == 0) {
        return;  /* empty hex body */
    }

    const char *rel = strip_prefix(ctx, req->path);
    if (rel == NULL) {
        return;
    }

    char actual[256];
    if (ctx->ops.digest(ctx->user_data, rel, algo,
                        actual, sizeof(actual)) != AXL_OK)
    {
        return;  /* consumer can't supply — pass through */
    }

    /* Byte-exact compare. axl_strcmp is fine since both are NUL-
       terminated lowercase hex. */
    if (axl_strcmp(actual, claimed) == 0) {
        return;  /* match — leave the 201 in place */
    }

    /* Mismatch — corruption detected. Tear down + 400. */
    if (ctx->ops.remove != NULL) {
        ctx->ops.remove(ctx->user_data, rel);
    }
    axl_warning("webdav: PUT Content-Digest mismatch for %s "
                "(client claimed %s=%s, server got %s)",
                rel, algo, claimed, actual);
    resp->status_code = 400;
}

static int
on_dav_upload(AxlHttpRequest *req, AxlHttpResponse *resp,
              const void *chunk, size_t chunk_size,
              void *data, bool aborted)
{
    AxlWebDavCtx *ctx = data;

    if (aborted) {
        /* Connection died mid-PUT. Release any open write ctx if it
           belongs to this request. resp is NOT transmitted on the
           abort path per the AxlUploadHandler contract. */
        if (ctx->put_req == req && ctx->put_ctx != NULL &&
            ctx->ops.write_close != NULL)
        {
            ctx->ops.write_close(ctx->put_ctx, true);
        }
        if (ctx->put_req == req) {
            ctx->put_ctx    = NULL;
            ctx->put_req    = NULL;
            ctx->put_failed = false;
        }
        return 0;
    }

    if (chunk == NULL && chunk_size == 0) {
        /* Clean EOF — finalize and respond. Single exit at the end
           of this block so the before_response hook fires once per
           PUT, regardless of which sub-case (normal / empty / no
           ops) populated the status. */
        set_dav_class_header(resp);

        if (ctx->put_req == req) {
            /* Normal case: at least one chunk arrived. Close the
               write ctx and report 201 (or 500 if write_chunk
               errored mid-stream). */
            if (ctx->put_ctx != NULL && ctx->ops.write_close != NULL) {
                ctx->ops.write_close(ctx->put_ctx, false);
            }
            resp->status_code = ctx->put_failed ? 500 : 201;
            ctx->put_ctx    = NULL;
            ctx->put_req    = NULL;
            ctx->put_failed = false;
        } else {
            /* Empty PUT body — no chunk ever arrived
               (Content-Length: 0). Open + close to materialize an
               empty file, mirroring POSIX `: > file` semantics. */
            const char *rel = strip_prefix(ctx, req->path);
            if (rel == NULL || ctx->ops.write_open == NULL ||
                ctx->ops.write_close == NULL)
            {
                resp->status_code = 405;
            } else {
                void *empty_ctx = NULL;
                if (ctx->ops.write_open(ctx->user_data, rel, &empty_ctx)
                    != AXL_OK)
                {
                    resp->status_code = 409;
                } else {
                    ctx->ops.write_close(empty_ctx, false);
                    resp->status_code = 201;
                }
            }
        }

        /* End-to-end integrity check (symmetric to Want-Digest). If
           the client sent a `Content-Digest: <algo>=<hex>` header
           and ops.digest is wired, verify the just-uploaded bytes
           against the claim. Hoisted out of the if/else above so
           empty-body PUTs are validated too — an empty file has a
           well-known digest (e3b0c44... for sha-256) and a client
           sending the wrong hex with Content-Length:0 shouldn't
           silently succeed. */
        if (resp->status_code == 201) {
            content_digest_validate(ctx, req, resp);
        }

        /* Last-call mutation hook — symmetric with dav_route_dispatch
           on the verb routes, but inlined here because the upload-
           route handler fires per chunk and only the EOF branch
           builds a real response. */
        if (ctx->ops.before_response != NULL) {
            ctx->ops.before_response(ctx->user_data, req, resp);
        }
        return 0;
    }

    /* Chunk arrival. */
    if (ctx->put_req == NULL) {
        /* First chunk — open. */
        const char *rel = strip_prefix(ctx, req->path);
        if (rel == NULL || ctx->ops.write_open == NULL) {
            ctx->put_failed = true;
            return AXL_ERR;  /* aborts the upload — sends 500 */
        }
        if (ctx->ops.write_open(ctx->user_data, rel, &ctx->put_ctx) != AXL_OK) {
            ctx->put_failed = true;
            return AXL_ERR;
        }
        ctx->put_req = req;
    } else if (ctx->put_req != req) {
        /* Concurrent PUT — refuse this one; do NOT touch the
           in-flight upload (req A is mid-stream on its own
           connection; aborting it here AND clearing put_req
           would let A's next chunk masquerade as a fresh start
           and silently produce a partial file the consumer
           accepted as clean). v1 limitation: consumers must
           serialize PUTs to the same mount; the second-arriving
           request gets 500 + connection close while the first
           continues unaffected. */
        return AXL_ERR;
    }

    if (ctx->put_failed) {
        return AXL_ERR;
    }

    if (ctx->ops.write_chunk == NULL ||
        ctx->ops.write_chunk(ctx->put_ctx, chunk, chunk_size) != AXL_OK)
    {
        ctx->put_failed = true;
        return AXL_ERR;
    }
    return 0;
}

static int
on_options(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    (void)req;
    (void)data;
    set_dav_class_header(resp);
    insert_header(resp, "Allow",
        "OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, MOVE, COPY");
    /* Body is empty for OPTIONS — set_text("") would emit
       Content-Length: 0 anyway. send_response handles no-body
       responses fine. */
    resp->status_code = 200;
    return 0;
}

static int
on_mkcol(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    AxlWebDavCtx *ctx = data;
    set_dav_class_header(resp);

    /* RFC 4918 §9.3.1: MKCOL with a request body is 415
       Unsupported Media Type. We'd need an XML parser to handle
       the rare extended-MKCOL case. Reject bodies. */
    if (req->body_size > 0) {
        resp->status_code = 415;
        return 0;
    }

    const char *rel = strip_prefix(ctx, req->path);
    if (rel == NULL) {
        resp->status_code = 400;
        return 0;
    }
    /* MKCOL / is meaningless — the mount root always exists.
       Refuse with 405 for symmetry with the DELETE/MOVE-root
       refusals just below; clients shouldn't be asking. */
    if (axl_strcmp(rel, "/") == 0) {
        resp->status_code = 405;
        return 0;
    }
    if (ctx->ops.mkdir == NULL) {
        resp->status_code = 405;  /* Method Not Allowed */
        return 0;
    }

    int rc = ctx->ops.mkdir(ctx->user_data, rel);
    /* Map mkdir failure to 409 Conflict (RFC 4918 §9.3.1: parent
       doesn't exist) for v1. Distinguishing 405 (collection already
       exists) requires an extra stat round-trip; leave to the
       consumer to surface via a dedicated rc later. */
    resp->status_code = (rc == AXL_OK) ? 201 : 409;
    return 0;
}

static int
on_delete(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    AxlWebDavCtx *ctx = data;
    set_dav_class_header(resp);

    const char *rel = strip_prefix(ctx, req->path);
    if (rel == NULL) {
        resp->status_code = 400;
        return 0;
    }
    if (ctx->ops.remove == NULL) {
        resp->status_code = 405;
        return 0;
    }
    /* Refuse DELETE / — destroying the entire mount is almost
       certainly a client mistake. */
    if (axl_strcmp(rel, "/") == 0) {
        resp->status_code = 403;
        return 0;
    }

    int rc = ctx->ops.remove(ctx->user_data, rel);
    /* RFC 4918 §9.6.1: 204 No Content on success. 404 on missing.
       v1 doesn't distinguish — both rc paths to 204 vs 404. */
    resp->status_code = (rc == AXL_OK) ? 204 : 404;
    return 0;
}

static int
on_move(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    AxlWebDavCtx *ctx = data;
    set_dav_class_header(resp);

    const char *src = strip_prefix(ctx, req->path);
    if (src == NULL) {
        resp->status_code = 400;
        return 0;
    }
    if (ctx->ops.move == NULL) {
        resp->status_code = 405;
        return 0;
    }
    /* Refuse MOVE / — same reasoning as DELETE /. */
    if (axl_strcmp(src, "/") == 0) {
        resp->status_code = 403;
        return 0;
    }

    char dst[512];
    if (parse_destination(ctx, req, dst, sizeof(dst)) != AXL_OK) {
        /* RFC 4918 §9.9: bad / missing Destination → 400. */
        resp->status_code = 400;
        return 0;
    }

    bool overwrite = parse_overwrite(req);
    int rc = ctx->ops.move(ctx->user_data, src, dst, overwrite);
    /* Map success to 201 (created at destination) — we don't yet
       distinguish 204 (overwrote existing). Map failure to 412 if
       overwrite=false and destination exists, else 409. v1 lumps
       failures into 409; consumer-side ops can return AXL_ERR for
       any case and the client will retry. */
    resp->status_code = (rc == AXL_OK) ? 201 : 409;
    return 0;
}

/* COPY-specific Depth parser. RFC 4918 §9.8.3: only "0" or
   "infinity" are valid; "1" or any other token → 400. Missing
   header defaults to infinity (the spec default for COPY on a
   collection). Writes 0 (collection-only) or -1 (infinity) into
   @p out_depth on success. Returns false on invalid Depth so the
   caller can emit 400 — a separate signal from "depth -1" which
   the int return would conflict with (AXL_ERR == -1 in axl-macros.h). */
static bool
parse_copy_depth(const AxlHttpRequest *req, int *out_depth)
{
    if (req == NULL || req->headers == NULL) {
        *out_depth = -1;
        return true;
    }
    const char *v = (const char *)axl_hash_table_lookup(req->headers,
                                                        "depth");
    if (v == NULL) {
        *out_depth = -1;
        return true;
    }
    if (axl_strcmp(v, "0") == 0) {
        *out_depth = 0;
        return true;
    }
    if (axl_strcasecmp(v, "infinity") == 0) {
        *out_depth = -1;
        return true;
    }
    return false;  /* "1" or anything else */
}

static int
on_copy(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    AxlWebDavCtx *ctx = data;
    set_dav_class_header(resp);

    const char *src = strip_prefix(ctx, req->path);
    if (src == NULL) {
        resp->status_code = 400;
        return 0;
    }
    if (ctx->ops.copy == NULL) {
        resp->status_code = 405;
        return 0;
    }
    /* Refuse COPY / — same reasoning as DELETE / and MOVE /. */
    if (axl_strcmp(src, "/") == 0) {
        resp->status_code = 403;
        return 0;
    }

    int depth = -1;
    if (!parse_copy_depth(req, &depth)) {
        /* RFC 4918 §9.8.3: Depth must be "0" or "infinity"; anything
           else (notably "1") is 400 Bad Request. */
        resp->status_code = 400;
        return 0;
    }

    char dst[512];
    if (parse_destination(ctx, req, dst, sizeof(dst)) != AXL_OK) {
        /* RFC 4918 §9.8.4: bad / missing Destination → 400. */
        resp->status_code = 400;
        return 0;
    }

    /* Pre-stat the source so a missing-source error returns 404
       (the normal client expectation per RFC 4918 §9.8.5) rather
       than the generic 409 the ops.copy = AXL_ERR path would map
       to. MOVE doesn't do this today; it's a known follow-up. */
    if (ctx->ops.stat != NULL) {
        AxlFsEntry self;
        axl_memset(&self, 0, sizeof(self));
        if (ctx->ops.stat(ctx->user_data, src, &self) != AXL_OK) {
            resp->status_code = 404;
            return 0;
        }
    }

    bool overwrite = parse_overwrite(req);
    int rc = ctx->ops.copy(ctx->user_data, src, dst, overwrite, depth);
    /* Success → 201 Created (we don't yet distinguish 204 when the
       COPY overwrote an existing resource). Failure → 409 (lumps
       412 precondition-failed-when-overwrite-false-and-dest-exists
       into the same bucket as other ops failures; matches MOVE,
       same typed-return follow-up). */
    resp->status_code = (rc == AXL_OK) ? 201 : 409;
    return 0;
}

// ---------------------------------------------------------------------------
// Public API: axl_http_server_add_webdav
// ---------------------------------------------------------------------------

/* Generic verb dispatcher. Calls the per-verb inner handler (which
   still receives ctx as `data`), then fires ops.before_response so
   the consumer can mutate the response (add ETag / Cache-Control /
   etc. headers) before the dispatcher sends it on the wire. */
static int
dav_route_dispatch(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    const DavRouteSlot *slot = data;
    int rc = slot->inner(req, resp, slot->parent);
    if (slot->parent->ops.before_response != NULL) {
        slot->parent->ops.before_response(slot->parent->user_data,
                                          req, resp);
    }
    return rc;
}

/* Register one verb route under <prefix>/<wildcard> via
   dav_route_dispatch so the before_response hook fires automatically.
   Errors (table full, slot table exhausted) surface as AXL_ERR. */
static int
register_verb(AxlHttpServer *s, const char *method,
              const char *path_pattern,
              AxlHttpHandler handler, AxlWebDavCtx *ctx)
{
    if (ctx->route_slot_count >=
        sizeof(ctx->route_slots) / sizeof(ctx->route_slots[0]))
    {
        axl_error("webdav: route slot table exhausted (cap %zu)",
                  sizeof(ctx->route_slots) / sizeof(ctx->route_slots[0]));
        return AXL_ERR;
    }
    DavRouteSlot *slot = &ctx->route_slots[ctx->route_slot_count++];
    slot->parent = ctx;
    slot->inner  = handler;
    HttpRoute *r = add_route_internal(s, method, path_pattern,
                                      dav_route_dispatch, slot);
    if (r == NULL) {
        axl_error("webdav: failed to register %s %s",
                  method, path_pattern);
        return AXL_ERR;
    }
    return AXL_OK;
}

int
axl_http_server_add_webdav(AxlHttpServer *s, const char *prefix,
                           const AxlWebDavOps *ops, void *user_data)
{
    if (s == NULL || prefix == NULL || prefix[0] != '/' ||
        ops == NULL) {
        return AXL_ERR;
    }
    if (s->webdav_ctx_count >=
        sizeof(s->webdav_ctxs) / sizeof(s->webdav_ctxs[0])) {
        axl_error("webdav: server already has the max %zu mounts",
                  sizeof(s->webdav_ctxs) / sizeof(s->webdav_ctxs[0]));
        return AXL_ERR;
    }

    /* Normalize prefix: strip a trailing slash if present (we
       want "/dav", not "/dav/", because we'll register
       "/dav/<wildcard>" as the prefix route). */
    size_t plen = axl_strlen(prefix);
    while (plen > 1 && prefix[plen - 1] == '/') {
        plen--;
    }
    if (plen == 0) {
        return AXL_ERR;  /* prefix was just "/" — too broad */
    }

    AxlWebDavCtx *ctx = axl_calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return AXL_ERR;
    }
    ctx->prefix = axl_malloc(plen + 1);
    if (ctx->prefix == NULL) {
        axl_free(ctx);
        return AXL_ERR;
    }
    axl_memcpy(ctx->prefix, prefix, plen);
    ctx->prefix[plen] = '\0';
    ctx->prefix_len   = plen;
    ctx->server       = s;
    ctx->ops          = *ops;
    ctx->user_data    = user_data;

    /* Pattern is "<prefix>/<wildcard>" — covers all paths under
       the mount. (Exact "<prefix>" without trailing slash isn't
       matched by this; modern WebDAV clients append the slash.
       Edge case acceptable for v1.) */
    char pattern[256];
    if ((size_t)axl_snprintf(pattern, sizeof(pattern), "%s/*",
                             ctx->prefix) >= sizeof(pattern)) {
        webdav_ctx_free(ctx);
        return AXL_ERR;
    }

    /* Stash the ctx in webdav_ctxs[] BEFORE registering routes.
       If a register_verb call fails partway through (OOM, route
       table full), already-registered routes still hold r->data
       = ctx. By having the server own the ctx via webdav_ctxs[]
       at this point, axl_http_server_free will clean it up — no
       use-after-free dangling against freed-here ctx. The caller
       sees AXL_ERR meaning "not all verbs got registered" and
       should free the server. */
    s->webdav_ctxs[s->webdav_ctx_count++] = ctx;

    if (register_verb(s, "OPTIONS",  pattern, on_options,     ctx) != AXL_OK ||
        register_verb(s, "PROPFIND", pattern, on_propfind,    ctx) != AXL_OK ||
        register_verb(s, "GET",      pattern, on_get_or_head, ctx) != AXL_OK ||
        register_verb(s, "HEAD",     pattern, on_get_or_head, ctx) != AXL_OK ||
        register_verb(s, "MKCOL",    pattern, on_mkcol,       ctx) != AXL_OK ||
        register_verb(s, "DELETE",   pattern, on_delete,      ctx) != AXL_OK ||
        register_verb(s, "MOVE",     pattern, on_move,        ctx) != AXL_OK ||
        register_verb(s, "COPY",     pattern, on_copy,        ctx) != AXL_OK)
    {
        return AXL_ERR;
    }

    /* PUT goes through the streaming upload route so multi-GB
       uploads bypass body_limit and never materialize the whole
       request body in RAM. */
    if (axl_http_server_add_upload_route(s, "PUT", pattern,
                                         on_dav_upload, ctx) != AXL_OK)
    {
        axl_error("webdav: failed to register PUT upload route");
        return AXL_ERR;
    }

    return AXL_OK;
}
