/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file Fetch.c
    Fetch — HTTP client tool (curl-like).

    Build with axl-cc:
      axl-cc Fetch.c -o Fetch.efi

    Usage: Fetch.efi [options] <url>
    Run with --help for the full option list.
**/

#include <axl.h>

// ---------------------------------------------------------------------------
// Option definitions
// ---------------------------------------------------------------------------

static const AxlArgDesc flags[] = {
    { .name = "output",      .short_name = 'o', .type = AXL_ARG_STRING,
      .help = "Write response body to FILE" },
    { .name = "remote-name", .short_name = 'O', .type = AXL_ARG_BOOL,
      .help = "Write to file named from URL path" },
    { .name = "upload",      .short_name = 'T', .type = AXL_ARG_STRING,
      .help = "Upload FILE via PUT" },
    { .name = "data",        .short_name = 'd', .type = AXL_ARG_STRING,
      .help = "Send DATA as POST body" },
    { .name = "method",      .short_name = 'X', .type = AXL_ARG_STRING,
      .help = "HTTP method (GET, POST, PUT, DELETE, HEAD)" },
    { .name = "header",      .short_name = 'H', .type = AXL_ARG_MULTI,
      .help = "Add header \"Name: Value\" (repeatable)" },
    { .name = "head",        .short_name = 'I', .type = AXL_ARG_BOOL,
      .help = "HEAD request (show headers only)" },
    { .name = "verbose",     .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Show request/response headers" },
    { .name = "silent",      .short_name = 's', .type = AXL_ARG_BOOL,
      .help = "Suppress status output" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "url", .type = AXL_ARG_STRING, .required = true,
      .help = "URL to fetch" },
    {0}
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Print response headers via axl_hash_table_foreach.
static void
print_header(
    const void  *key,
    void        *value,
    void        *data
    )
{
    (void)data;
    axl_printf("< %s: %s\n", (const char *)key, (const char *)value);
}

/// Extract filename from URL path: "/path/to/file.bin" -> "file.bin"
static const char *
get_url_filename(
    const char  *path
    )
{
    if (path == NULL) {
        return NULL;
    }
    const char *last = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            last = p + 1;
        }
    }
    return (*last != '\0') ? last : NULL;
}

/// Parse "Name: Value" header string into key and value.
/// Modifies buf in place (inserts NUL at colon).
/// Returns false if no colon found.
static bool
parse_header_str(
    char   *buf,
    char  **key,
    char  **val
    )
{
    char *colon = NULL;
    for (char *p = buf; *p != '\0'; p++) {
        if (*p == ':') {
            colon = p;
            break;
        }
    }
    if (colon == NULL) {
        return false;
    }

    *colon = '\0';
    *key = buf;

    // Skip ": " prefix on value
    char *v = colon + 1;
    while (*v == ' ') {
        v++;
    }
    *val = v;
    return true;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static int
run_fetch(AxlArgs *a)
{
    const char *url = axl_args_get_string(a, "url");

    /* Auto-load NIC drivers + DHCP so Fetch works from a bare UEFI shell. */
    switch (axl_net_ensure_drivers()) {
    case AXL_NET_DRIVERS_OK:
        break;
    case AXL_NET_DRIVERS_NOT_FOUND:
        axl_printf("Fetch: no NIC drivers found in drivers/<arch>/ "
                   "on any mounted volume.\n");
        return 1;
    case AXL_NET_DRIVERS_NO_LINK:
        axl_printf("Fetch: drivers loaded but no NIC came up — "
                   "is a NIC plugged in?\n");
        return 1;
    default:
        axl_printf("Fetch: failed to bring up networking.\n");
        return 1;
    }
    if (axl_net_auto_init(SIZE_MAX, 10) != AXL_OK) {
        axl_printf("Fetch: no IP address — DHCP did not complete.\n");
        return 1;
    }

    bool silent    = axl_args_get_bool(a,"silent");
    bool verbose   = axl_args_get_bool(a,"verbose");
    bool head_only = axl_args_get_bool(a,"head");

    // Determine HTTP method
    const char *method_str = axl_args_get_string(a,"method");
    const char *data_str = axl_args_get_string(a,"data");
    const char *upload_path = axl_args_get_string(a,"upload");
    char method[16] = "GET";
    if (method_str != NULL) {
        axl_strlcpy(method, method_str, sizeof(method));
    } else if (data_str != NULL) {
        axl_strlcpy(method, "POST", sizeof(method));
    } else if (upload_path != NULL) {
        axl_strlcpy(method, "PUT", sizeof(method));
    } else if (head_only) {
        axl_strlcpy(method, "HEAD", sizeof(method));
    }

    // Build extra headers from -H options. The static buffer-pool
    // bound is generous for typical CLI use; warn when the caller
    // passes more so the silent truncation in the loop below is
    // visible.
    #define FETCH_MAX_HEADERS    16
    #define FETCH_MAX_HEADER_LEN 256
    AXL_AUTOPTR(AxlHashTable) extra_headers = NULL;
    size_t hdr_count = axl_args_get_multi_count(a,"header");
    static char hdr_bufs[FETCH_MAX_HEADERS][FETCH_MAX_HEADER_LEN];

    if (hdr_count > FETCH_MAX_HEADERS) {
        axl_printf("warning: %zu headers passed; only the first %d "
                   "will be sent\n",
                   hdr_count, FETCH_MAX_HEADERS);
    }
    if (hdr_count > 0) {
        extra_headers = axl_hash_table_new_str();
        for (size_t i = 0; i < hdr_count && i < FETCH_MAX_HEADERS; i++) {
            const char *hdr = axl_args_get_multi(a,"header", i);
            if (hdr == NULL) {
                continue;
            }
            axl_strlcpy(hdr_bufs[i], hdr, sizeof(hdr_bufs[i]));
            char *key = NULL;
            char *val = NULL;
            if (parse_header_str(hdr_bufs[i], &key, &val)) {
                axl_hash_table_insert(extra_headers, key, val);
            }
        }
    }

    // Load request body
    AXL_AUTO_FREE void *body = NULL;
    size_t      body_size = 0;
    const char *content_type = NULL;

    if (data_str != NULL) {
        body_size = axl_strlen(data_str);
        body = axl_memdup(data_str, body_size);
        content_type = "application/x-www-form-urlencoded";
    } else if (upload_path != NULL) {
        if (axl_file_get_contents(upload_path, &body, &body_size) != AXL_OK) {
            axl_printf("Fetch: cannot read '%s'\n", upload_path);
            return 1;
        }
        content_type = "application/octet-stream";
    }

    // Create HTTP client and send request
    AXL_AUTOPTR(AxlHttpClient) client = axl_http_client_new();
    if (client == NULL) {
        axl_printf("Fetch: out of memory\n");
        return 1;
    }

    if (!silent) {
        axl_printf("> %s %s\n", method, url);
    }

    AXL_AUTOPTR(AxlHttpClientResponse) resp = NULL;
    int rc = axl_http_request(
        client, method, url, body, body_size,
        content_type, extra_headers, &resp);

    if (rc != AXL_OK || resp == NULL) {
        axl_printf("Fetch: request failed\n");
        return 1;
    }

    // Print status
    if (!silent) {
        axl_printf("< HTTP %zu\n", resp->status_code);
    }

    // Print headers (verbose or HEAD mode)
    if (verbose || head_only) {
        if (resp->headers != NULL) {
            axl_hash_table_foreach(resp->headers, print_header, NULL);
        }
        axl_printf("\n");
    }

    // Handle response body output
    int exit_status = 0;

    if (!head_only && resp->body != NULL && resp->body_size > 0) {
        const char *out_file = axl_args_get_string(a,"output");
        bool auto_name = axl_args_get_bool(a,"remote-name");

        if (out_file != NULL) {
            if (!axl_file_set_contents(out_file, resp->body, resp->body_size)) {
                axl_printf("Fetch: write '%s' failed\n", out_file);
            } else if (!silent) {
                axl_printf("Saved %u bytes to %s\n",
                           (unsigned)resp->body_size, out_file);
            }
        } else if (auto_name) {
            AXL_AUTOPTR(AxlUrl) parsed = NULL;
            int url_rc = axl_url_parse(url, &parsed);
            if (url_rc == AXL_OK && parsed != NULL) {
                const char *name = get_url_filename(parsed->path);
                if (name != NULL && *name != '\0') {
                    if (!axl_file_set_contents(name, resp->body,
                                                resp->body_size)) {
                        axl_printf("Fetch: write '%s' failed\n", name);
                    } else if (!silent) {
                        axl_printf("Saved %u bytes to %s\n",
                                   (unsigned)resp->body_size, name);
                    }
                } else {
                    axl_printf("Fetch: -O: cannot determine filename from URL\n");
                }
            } else {
                axl_printf("Fetch: -O: cannot parse URL\n");
            }
        } else {
            for (size_t i = 0; i < resp->body_size; i++) {
                char ch = ((char *)resp->body)[i];
                if (ch == '\n') {
                    axl_printf("\n");
                } else if (ch >= 0x20 && ch < 0x7F) {
                    axl_printf("%c", ch);
                } else {
                    axl_printf(".");
                }
            }
            axl_printf("\n");
        }
    }

    if (resp->status_code >= 400) {
        exit_status = 1;
    }

    return exit_status;
}

int
main(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "Fetch",
        .help         = "HTTP client (curl-like) for UEFI",
        .flags        = flags,
        .positionals  = positional,
        .handler      = run_fetch,
    });
}
