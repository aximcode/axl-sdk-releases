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
#include <axl/axl-url.h>

AXL_LOG_DOMAIN("webdav");

/* A request path, decoded, with its NUL: what the ops table is handed. */
#define DAV_PATH_MAX  512
/* PROPFIND Depth: 1 asks list_dir for this many children first, doubling
   while a listing fills the array -- with no upper bound. */
#define DAV_PROPFIND_FIRST  64u

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
static bool dav_put_release (AxlWebDavCtx *ctx);
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
    /* Connection teardown runs every in-flight PUT's abort call before the
       mounts are freed, so no PUT is normally in flight here; one that is
       is discarded rather than left open. */
    if (ctx->put_id != 0) {
        if (dav_put_release(ctx)) {
            axl_warning("PUT in flight at teardown was discarded; the "
                        "backend could not restore its target");
        }
    }
    /* Server-owned user_data (e.g. serve_fs's AxlFsRoot) — release it. */
    if (ctx->user_data_free != NULL && ctx->user_data != NULL) {
        ctx->user_data_free(ctx->user_data);
    }
    axl_free(ctx->prefix);
    axl_free(ctx);
}

/* Whether every component of the decoded path @p p is a name the volume
   would look up as spelled. EDK2 FAT trims a leading blank, and trailing
   blanks and periods, from a name before its lookup, so "~AP1A2B3.TMP."
   opens "~AP1A2B3.TMP" -- past every exact-name check (a live PUT's temp
   was served, deleted and replaced that way) -- and FAT cannot store such
   a name anyway. A component starting with ' ' or ending with ' ' or '.'
   is refused; "." and ".." are dot segments, resolved later. */
static bool
dav_path_components_ok(const char *p)
{
    const char *start = p;
    for (;; p++) {
        if (*p != '/' && *p != '\0') {
            continue;
        }
        size_t n = (size_t)(p - start);
        bool   dots = (n == 1 && start[0] == '.')
                      || (n == 2 && start[0] == '.' && start[1] == '.');
        if (n > 0 && !dots
            && (start[0] == ' ' || start[n - 1] == ' ' || start[n - 1] == '.')) {
            return false;
        }
        if (*p == '\0') {
            return true;
        }
        start = p + 1;
    }
}

/* Percent-decode one path (RFC 3986 2.1) into @p out. A '%' must start a
   two-hex-digit escape. Refused (400): a malformed escape; an escape that
   decodes to '/', '\' or NUL -- a separator or terminator hidden inside a
   segment, which mapping would turn into a real one; and a RAW '\', which
   the filesystem takes for a separator inside what HTTP calls one segment,
   past the containment check. "%2e%2e" decodes to "..": it is resolved
   like any "..", inside the mount. A component FAT would trim before its
   lookup is 400 too (dav_path_components_ok). @return 0, 400, or 414 when the
   decoded path does not fit. */
static int
dav_path_decode(const char *raw, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = raw; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') {
            return 400;
        }
        if (c == '%') {
            int hi = axl_hex_nibble(p[1]);
            int lo = (hi >= 0) ? axl_hex_nibble(p[2]) : -1;
            if (hi < 0 || lo < 0) {
                return 400;
            }
            c = (unsigned char)((hi << 4) | lo);
            if (c == '\0' || c == '/' || c == '\\') {
                return 400;
            }
            p += 2;
        }
        if (n + 1 >= cap) {
            return 414;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return dav_path_components_ok(out) ? 0 : 400;
}

/* The path the consumer's ops table sees for request URL path @p path:
   the registered prefix stripped ("/" for root, "/foo/bar" for nested)
   and the rest percent-decoded into @p out. @return 0, or the status to
   answer: 400 when the URL does not start with the prefix (the router
   already matched, so defensive) or decodes to nothing valid, 414 when
   too long. */
static int
dav_rel(const AxlWebDavCtx *ctx, const char *path, char *out, size_t cap)
{
    if (path == NULL || axl_strncmp(path, ctx->prefix, ctx->prefix_len) != 0) {
        return 400;
    }
    const char *rel = path + ctx->prefix_len;
    return dav_path_decode((*rel == '\0') ? "/" : rel, out, cap);
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
        /* log-level: this bool IS checked -- by insert_header(), which returns
           void. So the status stops one frame up and reaches nobody, while the
           response still goes out successfully, just missing DAV / Allow /
           Content-Range. A caller cannot discover that from any return value,
           and a WebDAV client failing on absent headers gives no clue why. */
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

/* Length of the authority @p a (@p n bytes) without a trailing port that
   is the default for @p scheme (@p scheme_len bytes), so
   "host:80" and "host" compare equal for http. */
static size_t
authority_len_sans_default_port(const char *a, size_t n,
                                const char *scheme, size_t scheme_len)
{
    const char *port = NULL;
    if (scheme_len == 4 && axl_strncasecmp(scheme, "http", 4) == 0) {
        port = ":80";
    } else if (scheme_len == 5 && axl_strncasecmp(scheme, "https", 5) == 0) {
        port = ":443";
    }
    size_t pl = (port != NULL) ? axl_strlen(port) : 0;
    if (port != NULL && n > pl && axl_strncmp(a + n - pl, port, pl) == 0) {
        return n - pl;
    }
    return n;
}

/* Does the authority of an absolute Destination URL (@p scheme up to
   "://", then @p auth for @p auth_len bytes) name the server the request
   was sent to -- its Host header? A request with no Host header (HTTP/1.0)
   cannot be checked and is taken as this server. Userinfo ("user@") is not
   part of the server's name. Names compare case-insensitively. */
static bool
destination_is_this_server(const AxlHttpRequest *req,
                           const char *scheme, size_t scheme_len,
                           const char *auth, size_t auth_len)
{
    const char *host = (const char *)axl_hash_table_lookup(req->headers,
                                                           "host");
    if (host == NULL) {
        return true;
    }
    for (size_t i = auth_len; i > 0; i--) {
        if (auth[i - 1] == '@') {
            auth_len -= i;
            auth     += i;
            break;
        }
    }
    size_t al = authority_len_sans_default_port(auth, auth_len,
                                                scheme, scheme_len);
    size_t hl = authority_len_sans_default_port(host, axl_strlen(host),
                                                scheme, scheme_len);
    return al == hl && axl_strncasecmp(auth, host, al) == 0;
}

/* Destination header: a URL — usually absolute ("http://host:port/dav/foo")
   but may be a path. Writes the path component RELATIVE to the webdav
   prefix into @p out (caller-supplied buffer).

   @return AXL_OK; AXL_UNSUPPORTED for an absolute URL naming another
       server (RFC 4918 §9.8.5 / §9.9.4: 502); AXL_ERR on a missing or
       unparsable header or a destination outside this mount. */
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
        if (!destination_is_this_server(req, v, (size_t)(scheme_end - v),
                                        scheme_end + 3,
                                        (size_t)(path - (scheme_end + 3)))) {
            return AXL_UNSUPPORTED;
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

    /* Decoded as the request path is: one rule for both. */
    return (dav_path_decode(rel, out, out_size) == 0) ? AXL_OK : AXL_ERR;
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

/* The absolute href "<prefix>/<rel>[/]" for @p rel, on the heap (axl_free
   it), or NULL when it cannot be allocated. @p rel is the DECODED path
   (names as stored); the href is a URL, so every byte outside RFC 3986's
   unreserved set but '/' is encoded -- a space as %20, UTF-8 byte by byte.
   The prefix is the registered URL path, used as given. The trailing slash
   on directories is a strong RFC 4918 §5.2 hint clients follow when
   building child URLs. No length limit: an href is never cut. */
static char *
compose_href(const AxlWebDavCtx *ctx, const char *rel, bool is_dir)
{
    bool   root = axl_strcmp(rel, "/") == 0;
    size_t cap  = axl_strlen(rel) * 3u + 1u;   /* every byte as %XX */
    char  *enc  = root ? NULL : axl_malloc(cap);
    if (!root && (enc == NULL || axl_url_encode(rel, enc, cap) < 0)) {
        axl_free(enc);
        return NULL;
    }
    const char *tail  = root ? "/" : enc;
    size_t      tlen  = axl_strlen(tail);
    bool        slash = is_dir && tail[tlen - 1] != '/';
    size_t      plen  = ctx->prefix_len;
    char       *out   = axl_malloc(plen + tlen + (slash ? 1u : 0u) + 1u);
    if (out != NULL) {
        axl_memcpy(out, ctx->prefix, plen);
        axl_memcpy(out + plen, tail, tlen);
        if (slash) {
            out[plen + tlen++] = '/';
        }
        out[plen + tlen] = '\0';
    }
    axl_free(enc);
    return out;
}

/* Emit one <D:response> for @p e living at @p rel within ctx via
   the AxlXmlWriter. false when its href cannot be built (no memory): the
   caller fails the whole listing rather than send an entry without one. */
static bool
emit_entry(AxlXmlWriter *w, const AxlWebDavCtx *ctx,
           const char *rel, const AxlFsEntry *e)
{
    char *href = compose_href(ctx, rel, axl_fs_entry_is_dir(e));
    if (href == NULL) {
        return false;
    }
    axl_xml_writer_start_element(w, "D:response");
    axl_xml_writer_start_element(w, "D:href");
    axl_xml_writer_text(w, href);
    axl_xml_writer_end_element(w);
    axl_free(href);

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
    return true;
}

// ---------------------------------------------------------------------------
// Per-verb handlers
// ---------------------------------------------------------------------------

static int
on_propfind(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    AxlWebDavCtx *ctx = data;
    set_dav_class_header(resp);

    char rel_raw[DAV_PATH_MAX];
    int  rst = dav_rel(ctx, req->path, rel_raw, sizeof(rel_raw));
    if (rst != 0) {
        resp->status_code = rst;
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
    axl_xml_writer_init(&xw, out, AXL_XML_DEFAULT);
    axl_xml_writer_prologue(&xw);
    axl_xml_writer_start_element(&xw, "D:multistatus");
    axl_xml_writer_attribute(&xw, "xmlns:D", "DAV:");

    /* Self entry. The displayname is the basename of rel; for the
       mount root that's "" — emit "/" so clients have something
       sensible to show. */
    if (axl_strcmp(rel, "/") == 0 && self.name[0] == '\0') {
        axl_strlcpy(self.name, "/", sizeof(self.name));
    }
    bool whole  = emit_entry(&xw, ctx, rel, &self);
    bool listed = true;   /* list_dir did not fail */

    /* Children — only when Depth > 0 AND the resource is a dir
       AND the consumer wired list_dir. */
    if (whole && include_children && axl_fs_entry_is_dir(&self) && ctx->ops.list_dir != NULL) {
        /* On the HEAP, never the stack: an AxlFsEntry is hundreds of
           bytes. list_dir takes no cursor, so a listing that fills the
           array is asked again with twice the room until it does not --
           with no cap: the listing is the volume's own, and a 207 that
           leaves entries out reads as the whole truth to a sync client,
           which then deletes what it did not see. When the room cannot
           be had, the PROPFIND fails (507) rather than answer in part. */
        AxlFsEntry *kids     = NULL;
        size_t      kids_max = DAV_PROPFIND_FIRST;
        size_t      count    = 0;
        int         lrc      = AXL_OK;
        for (;;) {
            axl_free(kids);
            kids = (kids_max <= SIZE_MAX / sizeof(*kids))
                   ? axl_calloc(kids_max, sizeof(*kids)) : NULL;
            if (kids == NULL) {
                whole = false;
                break;
            }
            count = 0;
            lrc   = ctx->ops.list_dir(ctx->user_data, rel, kids, kids_max, &count);
            if (lrc != AXL_OK || count < kids_max) {
                break;
            }
            if (kids_max > SIZE_MAX / 2u) {
                axl_free(kids);
                kids  = NULL;
                whole = false;
                break;
            }
            kids_max *= 2u;
        }
        listed = lrc == AXL_OK;
        for (size_t i = 0; whole && listed && i < count; i++) {
            /* The child's path = (rel == "/" ? "" : rel) + "/" + name, on
               the heap: a long name under a deep collection is listed, not
               skipped. rel is normalized (no trailing slash). */
            bool   root = (axl_strcmp(rel, "/") == 0);
            size_t rl   = root ? 0u : axl_strlen(rel);
            size_t nl   = axl_strlen(kids[i].name);
            char  *cp   = axl_malloc(rl + nl + 2u);
            if (cp == NULL) {
                whole = false;
                break;
            }
            axl_memcpy(cp, rel, rl);
            cp[rl] = '/';
            axl_memcpy(cp + rl + 1u, kids[i].name, nl + 1u);
            whole = emit_entry(&xw, ctx, cp, &kids[i]);
            axl_free(cp);
        }
        axl_free(kids);
    }

    axl_xml_writer_end_element(&xw);  /* D:multistatus */
    axl_xml_writer_finish(&xw);
    if (!listed) {
        /* A listing that failed is not an empty one. */
        axl_error("PROPFIND '%s': the directory could not be listed; 500", rel);
        axl_string_free(out);
        resp->status_code = 500;
        return 0;
    }
    if (!whole) {
        /* Never part of a listing: the memory for the whole of it -- the
           children, a path, an href -- could not be had. */
        axl_error("PROPFIND '%s': the listing does not fit in memory; 507, "
                  "nothing listed", rel);
        axl_string_free(out);
        axl_http_response_set_text(resp, "PROPFIND: the listing does not fit "
                                   "in memory\n");
        resp->status_code = 507;
        return 0;
    }
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

    char rel_buf[DAV_PATH_MAX];
    int  rst = dav_rel(ctx, req->path, rel_buf, sizeof(rel_buf));
    if (rst != 0) {
        resp->status_code = rst;
        return 0;
    }
    const char *rel = rel_buf;
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
// PUT — drives the AxlUploadHandler chunk callback. One PUT in flight
// per mount: another PUT that arrives meanwhile is 503 with
// Retry-After, and touches nothing.
// ---------------------------------------------------------------------------

/* Lowercase a single ASCII byte. Idempotent on lowercase / non-alpha. */
static inline char
ascii_tolower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* One Content-Digest entry's value, @p len bytes at @p v, as lowercase hex
   in @p hex: RFC 9530's byte-sequence form `:<base64>:`, or the bare hex
   an earlier client sends. A value that is neither leaves "", which
   matches no digest -- a claim the SDK cannot read is a mismatch, not a
   pass. */
static void
put_digest_value_hex(
    const char *v,
    size_t      len,
    char       *hex,
    size_t      cap
    )
{
    static const char digits[] = "0123456789abcdef";

    hex[0] = '\0';
    if (len >= 2 && v[0] == ':' && v[len - 1] == ':') {
        char   b64[AXL_WEBDAV_DIGEST_HEX_MAX];
        size_t blen = len - 2;
        void  *raw  = NULL;
        size_t rlen = 0;
        if (blen + 3 >= sizeof(b64)) {
            return;
        }
        axl_memcpy(b64, v + 1, blen);
        /* RFC 8941 §4.2.7 lets a sender drop the padding; the decoder
           wants it, so restore it. */
        while (blen % 4 != 0) {
            b64[blen++] = '=';
        }
        b64[blen] = '\0';
        if (axl_base64_decode(b64, &raw, &rlen) != AXL_OK) {
            return;
        }
        if (rlen * 2 < cap) {
            const uint8_t *b = raw;
            for (size_t i = 0; i < rlen; i++) {
                hex[2 * i]     = digits[b[i] >> 4];
                hex[2 * i + 1] = digits[b[i] & 0x0f];
            }
            hex[2 * rlen] = '\0';
        }
        axl_free(raw);
        return;
    }
    if (len == 0 || len >= cap) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        char c = ascii_tolower(v[i]);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            hex[0] = '\0';
            return;
        }
        hex[i] = c;
    }
    hex[len] = '\0';
}

/* Read a client's `Content-Digest` request header (RFC 9530): a list of
   `<algorithm>=<value>` entries, each value `:<base64>:` or (as earlier
   clients send) bare hex, any `;parameters` ignored. EVERY entry is
   scanned and the strongest algorithm the SDK hashes is the one checked
   -- sha-512, else sha-256, the two RFC 9530 registers as active. An entry
   for any other algorithm is ignored, and a header with none the SDK
   hashes checks nothing (false): the PUT proceeds unverified. On true,
   @p claimed holds the value as lowercase hex ("" when unreadable or
   absent, which then matches nothing). */
static bool
put_digest_claim(
    const AxlHttpRequest *req,
    AxlChecksumType      *type,
    char                 *claimed,
    size_t                cap
    )
{
    if (req == NULL || req->headers == NULL) {
        return false;
    }
    const char *p = (const char *)axl_hash_table_lookup(req->headers,
                                                        "content-digest");
    if (p == NULL) {
        return false;
    }
    int best = 0;   /* 0 none, 1 sha-256, 2 sha-512 */
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char   algo[32];
        size_t a = 0;
        while (*p != '\0' && *p != '=' && *p != ',' && *p != ';'
               && *p != ' ' && *p != '\t') {
            if (a < sizeof(algo) - 1) {
                algo[a++] = ascii_tolower(*p);
            }
            p++;
        }
        algo[a] = '\0';
        const char *v    = NULL;
        size_t      vlen = 0;
        if (*p == '=') {
            v = ++p;
            while (*p != '\0' && *p != ',' && *p != ';'
                   && *p != ' ' && *p != '\t') {
                p++;
            }
            vlen = (size_t)(p - v);
        }
        while (*p != '\0' && *p != ',') {
            p++;   /* parameters, or whatever else the entry carries */
        }
        int rank = (axl_strcmp(algo, "sha-512") == 0) ? 2
                 : (axl_strcmp(algo, "sha-256") == 0) ? 1 : 0;
        if (rank > best) {
            /* An entry with no value claims nothing that can match. */
            best = rank;
            put_digest_value_hex((v != NULL) ? v : "", vlen, claimed, cap);
        }
    }
    if (best == 0) {
        return false;
    }
    *type = (best == 2) ? AXL_CHECKSUM_SHA512 : AXL_CHECKSUM_SHA256;
    return true;
}

/* Finish a running digest and compare it with the claimed hex. */
static bool
put_digest_matches(
    AxlChecksum *sum,
    const char  *claimed
    )
{
    const char *actual = axl_checksum_get_string(sum);
    return actual != NULL && axl_strcmp(actual, claimed) == 0;
}

/* The status a refused PUT answers, by the code its open or commit
   returned: 403 for a refusal by policy (AXL_DENIED); 409 for a target of
   the wrong kind or one the mount may not replace (AXL_IS_DIR,
   AXL_EXISTS); 507 for a volume without the room (AXL_NO_RESOURCES);
   @p other for any other failure. */
static int
put_refusal_status(
    int rc,
    int other
    )
{
    switch (rc) {
    case AXL_DENIED:
        return 403;
    case AXL_IS_DIR:
    case AXL_EXISTS:
        return 409;
    case AXL_NO_RESOURCES:
        return 507;
    case AXL_IO_ERROR:
        return 500;   /* the volume could not answer */
    default:
        return other;
    }
}

/* Answer a PUT that arrived while another holds the mount's slot. */
static void
put_busy(
    AxlHttpResponse *resp
    )
{
    resp->status_code = 503;
    insert_header(resp, "Retry-After", "1");
}

/* End the in-flight PUT without storing it: close its write as aborted --
   the backend discards it -- and free the mount's single PUT slot. true
   when the write was declared IN PLACE at write_open and its discard says
   it could not restore the resource: the resource holds what was written.
   For an atomic write the discard's return is ignored, as it always was. */
static bool
dav_put_release(
    AxlWebDavCtx *ctx
    )
{
    bool kept = false;
    if (ctx->put_ctx != NULL && ctx->ops.write_close != NULL) {
        int rc = ctx->ops.write_close(ctx->put_ctx, true);
        kept = ctx->put_in_place && rc != AXL_OK;
    }
    ctx->put_in_place = false;
    axl_checksum_free(ctx->put_sum);
    ctx->put_sum = NULL;
    ctx->put_ctx = NULL;
    ctx->put_id  = 0;
    return kept;
}

/* The body length a PUT states: its Content-Length (0 when it has none
   and is not chunked -- HTTP/1.1 framing gives it no body), or
   AXL_WEBDAV_LENGTH_UNKNOWN for a chunked body. The request parser has
   already refused (400) a Content-Length that is not a number or does not
   fit below SIZE_MAX, so the value read here is the one the body is framed
   by, and never reaches UINT64_MAX: the two cannot collide. */
static uint64_t
put_body_length(
    const AxlHttpRequest *req
    )
{
    const char *te = (req->headers != NULL)
                     ? axl_hash_table_lookup(req->headers, "transfer-encoding") : NULL;
    if (te != NULL && axl_strcasecmp(te, "chunked") == 0) {
        return AXL_WEBDAV_LENGTH_UNKNOWN;
    }
    return (uint64_t)axl_http_get_content_length(req->headers);
}

/* Open the write a PUT needs and claim the mount's slot for it, with the
   status a refusal answers: 405 for a mount that cannot write, 403 for a
   refusal by policy (AXL_DENIED), 503 for a backend out of room for
   another write (AXL_BUSY), 409 for any other (a missing parent
   collection, an existing file on a no-overwrite mount). A Content-Digest
   the SDK can check starts its running digest here. */
static int
dav_put_begin(
    AxlWebDavCtx    *ctx,
    AxlHttpRequest  *req,
    AxlHttpResponse *resp
    )
{
    if (ctx->ops.write_open == NULL) {
        resp->status_code = 405;
        return AXL_ERR;
    }
    char rel[DAV_PATH_MAX];
    int  rst = dav_rel(ctx, req->path, rel, sizeof(rel));
    if (rst != 0) {
        resp->status_code = rst;
        return AXL_ERR;
    }
    if (req->request_id == 0) {
        /* 0 is the slot's "no PUT in flight": a PUT keyed on it would be
           taken for no PUT at all. The server never hands it out. */
        resp->status_code = 500;
        return AXL_ERR;
    }
    AxlChecksumType type;
    AxlChecksum    *sum = NULL;
    if (put_digest_claim(req, &type, ctx->put_claim, sizeof(ctx->put_claim))) {
        sum = axl_checksum_new(type);
        if (sum == NULL) {
            resp->status_code = 500;
            return AXL_ERR;
        }
    }
    void *wctx = NULL;
    bool  in_place = false;
    int   rc   = ctx->ops.write_open(ctx->user_data, rel, put_body_length(req),
                                     &wctx, &in_place);
    if (rc != AXL_OK) {
        axl_checksum_free(sum);
        if (rc == AXL_BUSY) {
            put_busy(resp);
        } else {
            resp->status_code = put_refusal_status(rc, 409);
        }
        return AXL_ERR;
    }
    ctx->put_ctx      = wctx;
    ctx->put_in_place = in_place;
    ctx->put_sum      = sum;
    ctx->put_id  = req->request_id;
    return AXL_OK;
}

/* Clean EOF of the PUT holding the slot: check its Content-Digest, then
   commit. A mismatch is decided BEFORE the commit, so the write is
   discarded (write_close aborted) and a backend that replaces atomically
   keeps the old resource; only then is the status 400. The commit's own
   status chooses the rest: 201 once stored; 409 for AXL_IS_DIR or
   AXL_EXISTS (the target changed while the body streamed -- a collection
   appeared there, or a file on a no-overwrite mount); 403 for AXL_DENIED;
   507 for AXL_NO_RESOURCES; 500 for any other failure -- the final flush
   is where the write becomes durable, and 201 for bytes that did not land
   would be a lie. */
static void
dav_put_finish(
    AxlWebDavCtx    *ctx,
    AxlHttpResponse *resp
    )
{
    if (ctx->put_sum != NULL && !put_digest_matches(ctx->put_sum,
                                                    ctx->put_claim)) {
        /* Hashed before the release frees the running digest. */
        char got[129];   /* sha-512 hex and the NUL */
        axl_strlcpy(got, axl_checksum_get_string(ctx->put_sum), sizeof(got));
        if (dav_put_release(ctx)) {
            axl_error("PUT Content-Digest mismatch (client claimed %s, body "
                      "hashed to %s); the resource was written in place and "
                      "now holds that body", ctx->put_claim, got);
            axl_http_response_set_text(resp, "Content-Digest mismatch: the "
                                       "resource now holds the body that did "
                                       "not match\n");
        } else {
            axl_warning("PUT Content-Digest mismatch (client claimed %s, "
                        "body hashed to %s); nothing stored",
                        ctx->put_claim, got);
        }
        resp->status_code = 400;
        return;
    }
    int rc = (ctx->ops.write_close == NULL)
             ? AXL_OK : ctx->ops.write_close(ctx->put_ctx, false);
    if (rc != AXL_OK && ctx->put_in_place) {
        /* A commit in place that fails (the final flush) leaves the
           resource as partial as a write that failed mid-body. */
        axl_error("PUT %s: the write in place failed at its commit; the "
                  "resource is partially written", ctx->prefix);
        axl_http_response_set_text(resp, "PUT failed part-way: the resource "
                                   "is partially written\n");
        resp->status_code = 500;
    } else if (rc == AXL_BUSY) {
        put_busy(resp);   /* the target went in-place busy while this streamed */
    } else {
        resp->status_code = (rc == AXL_OK) ? 201 : put_refusal_status(rc, 500);
    }
    ctx->put_in_place = false;
    axl_checksum_free(ctx->put_sum);
    ctx->put_sum = NULL;
    ctx->put_ctx = NULL;
    ctx->put_id  = 0;
}

static int
on_dav_upload(AxlHttpRequest *req, AxlHttpResponse *resp,
              const void *chunk, size_t chunk_size,
              void *data, bool aborted)
{
    AxlWebDavCtx *ctx  = data;
    bool          mine = ctx->put_id != 0 && ctx->put_id == req->request_id;

    if (aborted) {
        /* The PUT ended without its body: the connection died, or the
           server refused the body's framing. Release the write if this
           request holds the slot. resp is sent only for the server's own
           refusal (AxlUploadHandler); a write in place that the discard
           could not undo says so in it, as every in-place failure does. */
        if (mine && dav_put_release(ctx)) {
            axl_http_response_set_text(resp, "PUT failed part-way: the "
                                       "resource is partially written\n");
        }
        return 0;
    }

    if (chunk == NULL && chunk_size == 0) {
        /* Clean EOF — finalize and respond. Single exit at the end
           of this block so the before_response hook fires once per
           PUT, regardless of which sub-case set the status. */
        set_dav_class_header(resp);

        bool finish = mine;
        if (!mine) {
            /* No chunk ever arrived (Content-Length: 0): open and commit
               an empty write, mirroring POSIX `: > file`, through the
               same digest check and commit as a body. */
            if (ctx->put_id != 0) {
                /* Another PUT is in flight: this one would replace the
                   very file that one is writing. */
                put_busy(resp);
            } else if (ctx->ops.write_close == NULL) {
                /* Without a close the empty file cannot be made. */
                resp->status_code = 405;
            } else {
                finish = dav_put_begin(ctx, req, resp) == AXL_OK;
            }
        }
        if (finish) {
            dav_put_finish(ctx, resp);
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

    /* Chunk arrival. Every AXL_ERR below is this PUT's LAST call: the
       upload framework sends the response and calls the handler no more
       for it -- no abort call follows -- so nothing this PUT holds may
       outlive the return. State left behind is what once wedged a
       mount: every later PUT was refused, and the first of them had
       already emptied its target. */
    if (ctx->put_id == 0) {
        /* First chunk — open. A refusal claims no slot. */
        if (dav_put_begin(ctx, req, resp) != AXL_OK) {
            return AXL_ERR;
        }
    } else if (!mine) {
        /* Another PUT holds the slot -- refuse this one, and do NOT touch
           the one in flight: it is mid-stream on its own connection. */
        put_busy(resp);
        return AXL_ERR;
    }

    if (ctx->ops.write_chunk == NULL ||
        ctx->ops.write_chunk(ctx->put_ctx, chunk, chunk_size) != AXL_OK)
    {
        /* The PUT ends here (500): discard the write and free the slot
           for the next PUT. A backend that wrote in place cannot discard
           it: the response says what is left. */
        if (dav_put_release(ctx)) {
            axl_error("PUT %s: a write in place failed part-way; the "
                      "resource is partially written", ctx->prefix);
            resp->status_code = 500;
            axl_http_response_set_text(resp, "PUT failed part-way: the "
                                       "resource is partially written\n");
        }
        return AXL_ERR;
    }
    if (ctx->put_sum != NULL) {
        axl_checksum_update(ctx->put_sum, chunk, chunk_size);
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

    char rel_buf[DAV_PATH_MAX];
    int  rst = dav_rel(ctx, req->path, rel_buf, sizeof(rel_buf));
    if (rst != 0) {
        resp->status_code = rst;
        return 0;
    }
    const char *rel = rel_buf;
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

    char rel_buf[DAV_PATH_MAX];
    int  rst = dav_rel(ctx, req->path, rel_buf, sizeof(rel_buf));
    if (rst != 0) {
        resp->status_code = rst;
        return 0;
    }
    const char *rel = rel_buf;
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
    /* RFC 4918 §9.6.1: 204 No Content on success; 423 Locked for a
       resource a PUT is writing in place (AXL_BUSY); 404 otherwise. */
    resp->status_code = (rc == AXL_OK) ? 204 : (rc == AXL_BUSY) ? 423
                        : (rc == AXL_IO_ERROR) ? 500 : 404;
    return 0;
}

/* Are @p a and @p b the same request path, a trailing '/' aside? */
static bool
same_dav_path(const char *a, const char *b)
{
    size_t na = axl_strlen(a);
    size_t nb = axl_strlen(b);
    while (na > 1 && a[na - 1] == '/') {
        na--;
    }
    while (nb > 1 && b[nb - 1] == '/') {
        nb--;
    }
    return na == nb && axl_strncmp(a, b, na) == 0;
}

/* Do @p a and @p b differ in letter case only, a trailing '/' aside? */
static bool
case_variant_dav_path(const char *a, const char *b)
{
    size_t na = axl_strlen(a);
    size_t nb = axl_strlen(b);
    while (na > 1 && a[na - 1] == '/') {
        na--;
    }
    while (nb > 1 && b[nb - 1] == '/') {
        nb--;
    }
    return na == nb && axl_strncasecmp(a, b, na) == 0;
}

/* What MOVE and COPY check before the op is called (RFC 4918 §9.8.5,
   §9.9.4), in this order: a missing or unparsable Destination (400); one on
   another server (502); a destination that is the source itself, or the
   mount root -- overwriting that is DELETE / (403); a missing source (404,
   when stat is wired); an existing destination with Overwrite: F (412),
   unless it differs from the source in letter case only and the backend
   declares AXL_WEBDAV_CAP_IDENTITY.

   @return true with @p dst, @p overwrite and @p existed filled when the op
       should run; false with resp->status_code set when it must not. */
static bool
transfer_precheck(const AxlWebDavCtx *ctx, const AxlHttpRequest *req,
                  AxlHttpResponse *resp, const char *src,
                  char *dst, size_t dst_size,
                  bool *overwrite, bool *existed)
{
    int prc = parse_destination(ctx, req, dst, dst_size);
    if (prc != AXL_OK) {
        resp->status_code = (prc == AXL_UNSUPPORTED) ? 502 : 400;
        return false;
    }
    if (same_dav_path(src, dst) || axl_strcmp(dst, "/") == 0) {
        resp->status_code = 403;
        return false;
    }

    *overwrite = parse_overwrite(req);
    *existed   = false;
    /* Without stat the SDK cannot tell these apart: every failure is then
       the op's, and every success 201. */
    if (ctx->ops.stat != NULL) {
        AxlFsEntry e;
        axl_memset(&e, 0, sizeof(e));
        if (ctx->ops.stat(ctx->user_data, src, &e) != AXL_OK) {
            resp->status_code = 404;
            return false;
        }
        *existed = ctx->ops.stat(ctx->user_data, dst, &e) == AXL_OK;
        /* A destination differing from the source in letter case only may
           be the source itself (case-insensitive FAT) or another entry
           (a case-sensitive volume); only the backend can tell. One that
           declares it decides identity gets it, with overwrite false and
           the obligation to refuse another entry; any other is refused
           here, as for any existing destination. */
        bool forward = (ctx->ops.caps & AXL_WEBDAV_CAP_IDENTITY) != 0
                       && case_variant_dav_path(src, dst);
        if (*existed && !*overwrite && !forward) {
            resp->status_code = 412;
            return false;
        }
    }
    return true;
}

/* The status for a MOVE or COPY op's return: 201 when it created the
   destination, 204 when it replaced one (RFC 4918 §9.8.5, §9.9.4). A
   failure maps by its code: 403 for a refusal by policy (AXL_DENIED) or an
   impossible request (AXL_INVALID -- onto the source itself, into its own
   subtree); 412 for a destination that may not be overwritten
   (AXL_EXISTS); 409 when the destination cannot be reached or its kind
   conflicts (AXL_NOT_FOUND, AXL_NOT_DIR, AXL_IS_DIR, AXL_NOT_EMPTY); 502
   for a move only a copy could make, which the backend cannot
   (AXL_UNSUPPORTED); 507 for a volume without the room (AXL_NO_RESOURCES);
   500 for anything else. */
static int
transfer_status(int rc, bool existed)
{
    switch (rc) {
    case AXL_OK:
        return existed ? 204 : 201;
    case AXL_DENIED:
    case AXL_INVALID:
        return 403;
    case AXL_EXISTS:
        return 412;
    case AXL_NOT_FOUND:
    case AXL_NOT_DIR:
    case AXL_IS_DIR:
    case AXL_NOT_EMPTY:
        return 409;
    case AXL_UNSUPPORTED:
        return 502;
    case AXL_NO_RESOURCES:
        return 507;
    case AXL_BUSY:
        return 423;   /* a PUT is writing it in place */
    default:
        return 500;   /* AXL_ERR, AXL_CANCELLED, ... */
    }
}

static int
on_move(AxlHttpRequest *req, AxlHttpResponse *resp, void *data)
{
    AxlWebDavCtx *ctx = data;
    set_dav_class_header(resp);

    char src_buf[DAV_PATH_MAX];
    int  rst = dav_rel(ctx, req->path, src_buf, sizeof(src_buf));
    if (rst != 0) {
        resp->status_code = rst;
        return 0;
    }
    const char *src = src_buf;
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
    bool overwrite = true;
    bool existed   = false;
    if (!transfer_precheck(ctx, req, resp, src, dst, sizeof(dst),
                           &overwrite, &existed)) {
        return 0;
    }
    int rc = ctx->ops.move(ctx->user_data, src, dst, overwrite);
    resp->status_code = transfer_status(rc, existed);
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

    char src_buf[DAV_PATH_MAX];
    int  rst = dav_rel(ctx, req->path, src_buf, sizeof(src_buf));
    if (rst != 0) {
        resp->status_code = rst;
        return 0;
    }
    const char *src = src_buf;
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
    bool overwrite = true;
    bool existed   = false;
    if (!transfer_precheck(ctx, req, resp, src, dst, sizeof(dst),
                           &overwrite, &existed)) {
        return 0;
    }
    int rc = ctx->ops.copy(ctx->user_data, src, dst, overwrite, depth);
    resp->status_code = transfer_status(rc, existed);
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
              AxlHttpHandler handler, AxlWebDavCtx *ctx,
              uint32_t auth_flags)
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
    r->auth_flags = auth_flags;
    return AXL_OK;
}

int
axl_http_server_add_webdav(AxlHttpServer *s, const char *prefix,
                           const AxlWebDavOps *ops, void *user_data)
{
    return axl_http_server_add_webdav_auth(s, prefix, ops, user_data,
                                           AXL_ROUTE_NO_AUTH);
}

int
axl_http_server_add_webdav_auth(AxlHttpServer *s, const char *prefix,
                                const AxlWebDavOps *ops, void *user_data,
                                uint32_t auth_flags)
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

    if (register_verb(s, "OPTIONS",  pattern, on_options,     ctx, auth_flags) != AXL_OK ||
        register_verb(s, "PROPFIND", pattern, on_propfind,    ctx, auth_flags) != AXL_OK ||
        register_verb(s, "GET",      pattern, on_get_or_head, ctx, auth_flags) != AXL_OK ||
        register_verb(s, "HEAD",     pattern, on_get_or_head, ctx, auth_flags) != AXL_OK ||
        register_verb(s, "MKCOL",    pattern, on_mkcol,       ctx, auth_flags) != AXL_OK ||
        register_verb(s, "DELETE",   pattern, on_delete,      ctx, auth_flags) != AXL_OK ||
        register_verb(s, "MOVE",     pattern, on_move,        ctx, auth_flags) != AXL_OK ||
        register_verb(s, "COPY",     pattern, on_copy,        ctx, auth_flags) != AXL_OK)
    {
        return AXL_ERR;
    }

    /* PUT goes through the streaming upload route so multi-GB
       uploads bypass body_limit and never materialize the whole
       request body in RAM. It carries the same auth_flags so the
       mount is gated uniformly (the upload path enforces auth before
       any body byte — see axl-http-request.c). */
    if (axl_http_server_add_upload_route_auth(s, "PUT", pattern,
                                              on_dav_upload, ctx,
                                              auth_flags) != AXL_OK)
    {
        axl_error("webdav: failed to register PUT upload route");
        return AXL_ERR;
    }

    return AXL_OK;
}
