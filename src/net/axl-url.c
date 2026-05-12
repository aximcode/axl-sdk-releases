/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-url.c
    AxlUrl — URL parser and builder.
**/

#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-url.h>

AXL_LOG_DOMAIN("url");

// ---------------------------------------------------------------------------
// File-scope variables
// ---------------------------------------------------------------------------

/* Hex digits for axl_url_encode (`%XX` byte-emission). Uppercase
   per RFC 3986 §2.1. */
static const char hex_chars[] = "0123456789ABCDEF";

// ---------------------------------------------------------------------------
// axl_url_parse
// ---------------------------------------------------------------------------

int
axl_url_parse(const char *url, AxlUrl **out_parsed)
{
    const char             *p;
    const char             *scheme_end;
    const char             *authority_start;
    const char             *userinfo_end;
    const char             *user_end;
    const char             *host_start;
    const char             *host_end;
    const char             *port_start;
    const char             *path_start;
    const char             *query_start;
    const char             *fragment_start;
    AXL_AUTOPTR(AxlUrl)     u = NULL;
    uint16_t                port;

    if (url == NULL || out_parsed == NULL) {
        return AXL_ERR;
    }

    //
    // Find "://"
    //
    scheme_end = NULL;
    for (p = url; *p != '\0'; p++) {
        if (p[0] == ':' && p[1] == '/' && p[2] == '/') {
            scheme_end = p;
            break;
        }
    }

    if (scheme_end == NULL || scheme_end == url) {
        axl_error("malformed URL: no scheme in '%s'", url);
        return AXL_ERR;
    }

    //
    // Authority starts after "://" and ends at the first '/', '?',
    // or '#' (or NUL). Look for an '@' inside the authority — if
    // present, the prefix is the userinfo (`user[:password]`) and
    // the host starts after the '@'. An '@' AFTER the authority
    // boundary (in path or query) is part of that component and
    // must NOT be treated as userinfo separator.
    //
    authority_start = scheme_end + 3;
    if (*authority_start == '\0') {
        axl_error("malformed URL: no host in '%s'", url);
        return AXL_ERR;
    }

    userinfo_end = NULL;
    for (p = authority_start;
         *p != '\0' && *p != '/' && *p != '?' && *p != '#';
         p++)
    {
        if (*p == '@') {
            userinfo_end = p;
            break;
        }
    }

    if (userinfo_end != NULL) {
        host_start = userinfo_end + 1;
        //
        // Userinfo: split on the FIRST ':' into user / password.
        // Subsequent ':' bytes belong to the password (RFC 3986
        // technically deprecates `:` in userinfo, but tolerate it
        // for round-trip fidelity).
        //
        user_end = userinfo_end;
        for (p = authority_start; p < userinfo_end; p++) {
            if (*p == ':') {
                user_end = p;
                break;
            }
        }
    } else {
        host_start = authority_start;
        user_end   = NULL;
    }

    //
    // Find end of host — terminated by ':', '/', '?', '#', or NUL
    //
    host_end = host_start;
    while (*host_end != '\0' && *host_end != ':' && *host_end != '/' &&
           *host_end != '?' && *host_end != '#')
    {
        host_end++;
    }

    if (host_end == host_start) {
        axl_error("malformed URL: empty host in '%s'", url);
        return AXL_ERR;
    }

    //
    // Optional port after ':'. RFC 3986 §3.2.3 allows an empty
    // port (`host:` → scheme default). Non-digit bytes are NOT
    // accepted — they'd otherwise leave the parser staring at
    // garbage and silently mis-parse the rest of the URL. We also
    // bound at uint16_t so `host:99999` doesn't silently truncate.
    //
    port = 0;
    port_start = host_end;
    if (*port_start == ':') {
        port_start++;
        uint32_t  port32    = 0;
        bool      any_digit = false;
        while (axl_isdigit((unsigned char)*port_start)) {
            port32 = port32 * 10 + (uint32_t)(*port_start - '0');
            if (port32 > 65535) {
                axl_error("malformed URL: port out of range in '%s'", url);
                return AXL_ERR;
            }
            any_digit = true;
            port_start++;
        }
        // After the digit run we must be at a path/query/fragment
        // terminator or NUL — anything else means the port had
        // trailing non-digit bytes (e.g. `host:80abc`) and the
        // input is malformed.
        if (*port_start != '\0' && *port_start != '/' &&
            *port_start != '?' && *port_start != '#')
        {
            axl_error("malformed URL: non-digit port in '%s'", url);
            return AXL_ERR;
        }
        if (any_digit) {
            port = (uint16_t)port32;
        }
        // else: empty port (`host:/`), fall through with port = 0
        // so the default-port logic below picks the scheme default.

        p = port_start;
    } else {
        p = host_end;
    }

    //
    // Path starts at '/' or is empty. Stop at '?' (query) or '#'
    // (fragment).
    //
    path_start = NULL;
    if (*p == '/') {
        path_start = p;
        while (*p != '\0' && *p != '?' && *p != '#') {
            p++;
        }
    }

    //
    // Query starts after '?', ends at '#' or NUL.
    //
    query_start = NULL;
    if (*p == '?') {
        query_start = p + 1;
        p++;
        while (*p != '\0' && *p != '#') {
            p++;
        }
    }

    //
    // Fragment starts after '#', runs to NUL.
    //
    fragment_start = NULL;
    if (*p == '#') {
        fragment_start = p + 1;
    }

    //
    // Default ports
    //
    if (port == 0) {
        size_t scheme_len = (size_t)(scheme_end - url);

        if (scheme_len == 4 && axl_strncmp(url, "http", 4) == 0) {
            port = 80;
        } else if (scheme_len == 5 && axl_strncmp(url, "https", 5) == 0) {
            port = 443;
        }
    }

    //
    // Allocate and populate
    //
    u = axl_calloc(1, sizeof(AxlUrl));
    if (u == NULL) {
        return AXL_ERR;
    }

    u->scheme = axl_strndup(url, (size_t)(scheme_end - url));
    u->host   = axl_strndup(host_start, (size_t)(host_end - host_start));
    u->port   = port;

    //
    // Userinfo (user + optional password). Distinguish "absent" vs
    // "present but empty": NULL means no `@` in authority; empty
    // string ("") means the slot was present with no bytes (e.g.
    // `user:@host` → password = "").
    //
    if (userinfo_end != NULL) {
        u->user = axl_strndup(authority_start,
                              (size_t)(user_end - authority_start));
        if (u->user == NULL) {
            return AXL_ERR;
        }
        if (user_end < userinfo_end) {
            // user_end points at the ':' between user and password
            u->password = axl_strndup(user_end + 1,
                                      (size_t)(userinfo_end - (user_end + 1)));
            if (u->password == NULL) {
                return AXL_ERR;
            }
        }
    }

    if (path_start != NULL) {
        // Path ends at the first '?' / '#' / NUL after path_start
        const char *path_end = path_start;
        while (*path_end != '\0' && *path_end != '?' && *path_end != '#') {
            path_end++;
        }
        u->path = axl_strndup(path_start, (size_t)(path_end - path_start));
    } else {
        u->path = axl_strdup("/");
    }

    if (query_start != NULL) {
        // Query ends at '#' or NUL
        const char *query_end = query_start;
        while (*query_end != '\0' && *query_end != '#') {
            query_end++;
        }
        u->query = axl_strndup(query_start, (size_t)(query_end - query_start));
        if (u->query == NULL) {
            return AXL_ERR;
        }
    }

    if (fragment_start != NULL) {
        u->fragment = axl_strdup(fragment_start);
        if (u->fragment == NULL) {
            return AXL_ERR;
        }
    }

    if (u->scheme == NULL || u->host == NULL || u->path == NULL) {
        return AXL_ERR;
    }

    *out_parsed = axl_steal_pointer(&u);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// axl_url_free
// ---------------------------------------------------------------------------

void
axl_url_free(AxlUrl *url)
{
    if (url == NULL) {
        return;
    }

    axl_free(url->scheme);
    axl_free(url->user);
    axl_free(url->password);
    axl_free(url->host);
    axl_free(url->path);
    axl_free(url->query);
    axl_free(url->fragment);
    axl_free(url);
}

// ---------------------------------------------------------------------------
// axl_url_build
// ---------------------------------------------------------------------------

char *
axl_url_build(const char *scheme, const char *host, uint16_t port,
              const char *path)
{
    char         buf[512];
    size_t       len;
    char        *result;
    uint16_t     default_port;
    const char  *use_path;

    if (scheme == NULL || host == NULL) {
        return NULL;
    }

    use_path = (path != NULL) ? path : "/";

    //
    // Determine default port for scheme
    //
    default_port = 0;
    if (axl_strcmp(scheme, "http") == 0) {
        default_port = 80;
    } else if (axl_strcmp(scheme, "https") == 0) {
        default_port = 443;
    }

    //
    // Build: scheme://host[:port]path
    //
    if (port != 0 && port != default_port) {
        len = axl_snprintf(buf, sizeof(buf), "%s://%s:%u%s", scheme, host, port, use_path);
    } else {
        len = axl_snprintf(buf, sizeof(buf), "%s://%s%s", scheme, host, use_path);
    }

    result = axl_malloc(len + 1);
    if (result != NULL) {
        axl_memcpy(result, buf, len + 1);
    }

    return result;
}

// ---------------------------------------------------------------------------
// axl_url_encode / axl_url_decode
// ---------------------------------------------------------------------------

static bool
is_unreserved(char c)
{
    return axl_isalnum((unsigned char)c)
        || c == '-' || c == '.' || c == '_' || c == '~';
}

int
axl_url_encode(const char *src, char *out, size_t size)
{
    size_t pos = 0;

    if (src == NULL || out == NULL || size == 0) {
        return -1;
    }

    while (*src != '\0') {
        char c = *src++;

        if (is_unreserved(c) || c == '/') {
            /* Unreserved + '/' pass through (path-safe encoding) */
            if (pos + 1 >= size) {
                return -1;
            }
            out[pos++] = c;
        } else {
            /* Encode as %XX */
            if (pos + 3 >= size) {
                return -1;
            }
            out[pos++] = '%';
            out[pos++] = hex_chars[((unsigned char)c >> 4) & 0x0F];
            out[pos++] = hex_chars[(unsigned char)c & 0x0F];
        }
    }

    out[pos] = '\0';
    return (int)pos;
}

int
axl_url_decode(const char *src, char *out, size_t size)
{
    size_t pos = 0;

    if (src == NULL || out == NULL || size == 0) {
        return -1;
    }

    while (*src != '\0') {
        if (pos + 1 >= size) {
            return -1;
        }

        if (src[0] == '%' && src[1] != '\0' && src[2] != '\0') {
            int hi = axl_hex_nibble(src[1]);
            int lo = axl_hex_nibble(src[2]);
            if (hi >= 0 && lo >= 0) {
                out[pos++] = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        }

        out[pos++] = *src++;
    }

    out[pos] = '\0';
    return (int)pos;
}
