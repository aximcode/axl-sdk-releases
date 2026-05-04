/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file rfbrowse.c
    Redfish browser — connect to a BMC and browse Redfish resources.

    Build with axl-cc:
      axl-cc rfbrowse.c -o rfbrowse.efi

    Usage: rfbrowse.efi <host-or-url> [options] [path|shortcut ...]
    See -h for full option list.
**/

#include <axl.h>

// ---------------------------------------------------------------------------
// Option definitions
// ---------------------------------------------------------------------------

static const AxlArgDesc flags[] = {
    { .name = "user",     .short_name = 'u', .type = AXL_ARG_STRING,
      .help = "Username" },
    { .name = "password", .short_name = 'p', .type = AXL_ARG_STRING,
      .help = "Password" },
    { .name = "basic",    .short_name = 'b', .type = AXL_ARG_BOOL,
      .help = "Use HTTP Basic auth (default: session)" },
    { .name = "members",  .short_name = 'm', .type = AXL_ARG_BOOL,
      .help = "List collection Members URIs" },
    { .name = "expand",   .short_name = 'e', .type = AXL_ARG_BOOL,
      .help = "With -m, GET each member" },
    { .name = "raw",      .short_name = 'r', .type = AXL_ARG_BOOL,
      .help = "Raw JSON output (no colors)" },
    { .name = "verbose",  .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Show HTTP status and headers" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "host",  .type = AXL_ARG_STRING, .required = true,
      .help = "Redfish service host (or full URL)" },
    { .name = "paths", .type = AXL_ARG_MULTI,
      .help = "Optional Redfish paths or shortcut names (default: service root)" },
    {0}
};

// ---------------------------------------------------------------------------
// Shortcut table
// ---------------------------------------------------------------------------

static const struct {
    const char *name;
    const char *path;
} shortcuts[] = {
    { "root",     "/redfish/v1/" },
    { "systems",  "/redfish/v1/Systems" },
    { "system",   "/redfish/v1/Systems/1" },
    { "sys",      "/redfish/v1/Systems/1" },
    { "chassis",  "/redfish/v1/Chassis" },
    { "chassis1", "/redfish/v1/Chassis/1" },
    { "managers", "/redfish/v1/Managers" },
    { "manager",  "/redfish/v1/Managers/1" },
    { "mgr",      "/redfish/v1/Managers/1" },
    { "thermal",  "/redfish/v1/Chassis/1/Thermal" },
    { "power",    "/redfish/v1/Chassis/1/Power" },
    { "bios",     "/redfish/v1/Systems/1/Bios" },
    { "nics",     "/redfish/v1/Systems/1/EthernetInterfaces" },
    { "storage",  "/redfish/v1/Systems/1/Storage" },
    { "accounts", "/redfish/v1/AccountService/Accounts" },
    { "sessions", "/redfish/v1/SessionService/Sessions" },
    { "logs",     "/redfish/v1/Managers/1/LogServices" },
    { NULL, NULL }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Resolve a shortcut name to a Redfish path.
/// Returns the input unchanged if not a known shortcut.
static const char *
resolve_shortcut(
    const char *name
    )
{
    for (int i = 0; shortcuts[i].name != NULL; i++) {
        if (axl_strcasecmp(name, shortcuts[i].name) == 0) {
            return shortcuts[i].path;
        }
    }
    return name;
}

/// Build a full URL from base URL and Redfish path.
/// If path starts with '/', append it directly.
/// Otherwise prepend /redfish/v1/.
static void
build_url(
    char       *buf,
    size_t      size,
    const char *base_url,
    const char *path
    )
{
    if (path[0] == '/') {
        axl_snprintf(buf, size, "%s%s", base_url, path);
    } else {
        axl_snprintf(buf, size, "%s/redfish/v1/%s", base_url, path);
    }
}

/// Build a base URL from a host-or-url argument.
/// If it contains "://", use as-is. Otherwise prepend "https://".
static void
build_base_url(
    char       *buf,
    size_t      size,
    const char *host
    )
{
    if (axl_strstr(host, "://") != NULL) {
        axl_strlcpy(buf, host, size);
    } else {
        axl_snprintf(buf, size, "https://%s", host);
    }

    // Strip trailing slash
    size_t len = axl_strlen(buf);
    if (len > 0 && buf[len - 1] == '/') {
        buf[len - 1] = '\0';
    }
}

/// Print a response header (for verbose mode).
static void
print_header(
    const void *key,
    void       *value,
    void       *data
    )
{
    (void)data;
    axl_printf("  %s: %s\n", (const char *)key, (const char *)value);
}

/// Print Redfish error from JSON response body (best-effort).
static void
print_redfish_error(
    const void *body,
    size_t      body_size,
    size_t      status_code
    )
{
    if (body == NULL || body_size == 0) {
        axl_printf("rfbrowse: HTTP %zu (no body)\n", status_code);
        return;
    }

    AxlJsonReader ctx;
    if (!axl_json_parse((const char *)body, body_size, &ctx)) {
        axl_printf("rfbrowse: HTTP %zu\n", status_code);
        return;
    }

    // Try "error" → "message" nested path.
    // Redfish errors: {"error":{"message":"...","code":"..."}}
    // Flat fallback: {"message":"..."}
    char msg[256];
    if (axl_json_get_string(&ctx, "message", msg, sizeof(msg))) {
        axl_printf("rfbrowse: HTTP %zu — %s\n", status_code, msg);
    } else {
        axl_printf("rfbrowse: HTTP %zu\n", status_code);
    }
    axl_json_free(&ctx);
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

/// Authenticate via Redfish session creation.
/// Returns 0 on success, -1 on failure.
static int
session_login(
    AxlHttpClient *client,
    const char    *base_url,
    const char    *user,
    const char    *password,
    char          *session_uri,
    size_t         session_uri_size
    )
{
    // Build login JSON
    AXL_AUTOPTR(AxlString) json_str = axl_string_new(NULL);
    AxlJsonWriter jw;
    axl_json_writer_init(&jw, json_str, AXL_JSON_WRITER_DEFAULT);
    axl_json_obj_begin(&jw);
    axl_json_kv_str(&jw, "UserName", user);
    axl_json_kv_str(&jw, "Password", password);
    axl_json_obj_end(&jw);
    size_t json_len = axl_json_writer_finish(&jw);

    if (axl_json_writer_error(&jw)) {
        axl_printf("rfbrowse: credentials too long\n");
        return -1;
    }
    const char *json_buf = axl_string_str(json_str);

    // POST to session service
    char url[512];
    axl_snprintf(url, sizeof(url),
                 "%s/redfish/v1/SessionService/Sessions", base_url);

    AXL_AUTOPTR(AxlHttpClientResponse) resp = NULL;
    int rc = axl_http_post(client, url, json_buf, json_len,
                           "application/json", &resp);

    if (rc != AXL_OK || resp == NULL) {
        axl_printf("rfbrowse: login request failed\n");
        return -1;
    }

    if (resp->status_code != 200 && resp->status_code != 201) {
        print_redfish_error(resp->body, resp->body_size, resp->status_code);
        return -1;
    }

    // Extract token from response headers (lowercase)
    const char *token = NULL;
    if (resp->headers != NULL) {
        token = (const char *)axl_hash_table_lookup(
            resp->headers, "x-auth-token");
    }

    if (token == NULL) {
        axl_printf("rfbrowse: login succeeded but no X-Auth-Token in response\n");
        return -1;
    }

    // Set persistent auth header
    axl_http_client_set(client, "header.X-Auth-Token", token);

    // Store session URI for logout
    if (session_uri != NULL) {
        const char *loc = (const char *)axl_hash_table_lookup(
            resp->headers, "location");
        if (loc != NULL) {
            axl_strlcpy(session_uri, loc, session_uri_size);
        } else {
            session_uri[0] = '\0';
        }
    }

    return 0;
}

/// Set up HTTP Basic authentication.
static int
setup_basic_auth(
    AxlHttpClient *client,
    const char    *user,
    const char    *password
    )
{
    // Build "user:password"
    char creds[256];
    int n = axl_snprintf(creds, sizeof(creds), "%s:%s", user, password);
    if (n < 0 || (size_t)n >= sizeof(creds)) {
        axl_printf("rfbrowse: credentials too long\n");
        return -1;
    }

    AXL_AUTO_FREE char *b64 = axl_base64_encode(creds, (size_t)n);
    if (b64 == NULL) {
        axl_printf("rfbrowse: base64 encoding failed\n");
        return -1;
    }

    char header_val[384];
    axl_snprintf(header_val, sizeof(header_val), "Basic %s", b64);
    axl_http_client_set(client, "header.Authorization", header_val);
    return 0;
}

/// Logout by DELETEing the session. Best-effort, errors ignored.
static void
session_logout(
    AxlHttpClient *client,
    const char    *base_url,
    const char    *session_uri
    )
{
    if (session_uri[0] == '\0') {
        return;
    }

    char url[512];
    build_url(url, sizeof(url), base_url, session_uri);

    AXL_AUTOPTR(AxlHttpClientResponse) resp = NULL;
    axl_http_delete(client, url, &resp);
}

// ---------------------------------------------------------------------------
// Resource display
// ---------------------------------------------------------------------------

/// GET a Redfish resource and display it.
/// Returns 0 on success, -1 on failure.
static int
get_resource(
    AxlHttpClient *client,
    const char    *url,
    bool           raw,
    bool           verbose
    )
{
    AXL_AUTOPTR(AxlHttpClientResponse) resp = NULL;
    int rc = axl_http_get(client, url, &resp);

    if (rc != AXL_OK || resp == NULL) {
        axl_printf("rfbrowse: GET %s failed\n", url);
        return -1;
    }

    if (verbose) {
        axl_printf("< HTTP %zu\n", resp->status_code);
        if (resp->headers != NULL) {
            axl_hash_table_foreach(resp->headers, print_header, NULL);
        }
        axl_printf("\n");
    }

    if (resp->status_code >= 400) {
        print_redfish_error(resp->body, resp->body_size, resp->status_code);
        return -1;
    }

    if (resp->body != NULL && resp->body_size > 0) {
        if (raw) {
            axl_printf("%.*s\n", (int)resp->body_size, (const char *)resp->body);
        } else {
            axl_json_console_print(resp->body, resp->body_size);
        }
        axl_printf("\n");
    }

    return 0;
}

/// GET a collection and list or expand its Members.
/// Returns 0 on success, -1 on failure.
static int
get_members(
    AxlHttpClient *client,
    const char    *url,
    const char    *base_url,
    bool           expand,
    bool           raw,
    bool           verbose
    )
{
    AXL_AUTOPTR(AxlHttpClientResponse) resp = NULL;
    int rc = axl_http_get(client, url, &resp);

    if (rc != AXL_OK || resp == NULL) {
        axl_printf("rfbrowse: GET %s failed\n", url);
        return -1;
    }

    if (verbose) {
        axl_printf("< HTTP %zu\n", resp->status_code);
        if (resp->headers != NULL) {
            axl_hash_table_foreach(resp->headers, print_header, NULL);
        }
        axl_printf("\n");
    }

    if (resp->status_code >= 400) {
        print_redfish_error(resp->body, resp->body_size, resp->status_code);
        return -1;
    }

    if (resp->body == NULL || resp->body_size == 0) {
        axl_printf("rfbrowse: empty response\n");
        return -1;
    }

    AxlJsonReader ctx;
    if (!axl_json_parse(resp->body, resp->body_size, &ctx)) {
        axl_printf("rfbrowse: failed to parse JSON response\n");
        return -1;
    }

    // Print collection name if present
    char name[128];
    if (axl_json_get_string(&ctx, "Name", name, sizeof(name))) {
        axl_printf("=== %s ===\n", name);
    }

    // Iterate Members array
    AxlJsonArrayIter iter;
    if (!axl_json_array_begin(&ctx, "Members", &iter)) {
        axl_printf("rfbrowse: no Members array in response\n");
        axl_json_free(&ctx);
        return -1;
    }

    int result = 0;
    int count = 0;
    AxlJsonReader element;

    while (axl_json_array_next(&iter, &element)) {
        char odata_id[256];
        if (!axl_json_get_string(&element, "@odata.id",
                                 odata_id, sizeof(odata_id))) {
            continue;
        }

        count++;

        if (expand) {
            axl_printf("\n--- %s ---\n", odata_id);
            char member_url[512];
            build_url(member_url, sizeof(member_url), base_url, odata_id);
            if (get_resource(client, member_url, raw, verbose) != 0) {
                result = -1;
            }
        } else {
            axl_printf("  %s\n", odata_id);
        }
    }

    axl_printf("\n%d member(s)\n", count);
    axl_json_free(&ctx);
    return result;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static int
run_rfbrowse(AxlArgs *a)
{
    const char *host = axl_args_get_string(a, "host");

    /* Auto-load NIC drivers + DHCP so rfbrowse works from a bare UEFI
     * shell. TLS is freestanding mbedtls — no firmware TlsDxe needed. */
    switch (axl_net_ensure_drivers()) {
    case AXL_NET_DRIVERS_OK:
        break;
    case AXL_NET_DRIVERS_NOT_FOUND:
        axl_printf("rfbrowse: no NIC drivers found in drivers/<arch>/ "
                   "on any mounted volume.\n");
        return 1;
    case AXL_NET_DRIVERS_NO_LINK:
        axl_printf("rfbrowse: drivers loaded but no NIC came up — "
                   "is a NIC plugged in?\n");
        return 1;
    default:
        axl_printf("rfbrowse: failed to bring up networking.\n");
        return 1;
    }
    if (axl_net_auto_init(SIZE_MAX, 10) != AXL_OK) {
        axl_printf("rfbrowse: no IP address — DHCP did not complete.\n");
        return 1;
    }

    const char *user     = axl_args_get_string(a, "user");
    const char *password = axl_args_get_string(a, "password");
    bool basic   = axl_args_get_bool(a, "basic");
    bool members = axl_args_get_bool(a, "members");
    bool expand  = axl_args_get_bool(a, "expand");
    bool raw     = axl_args_get_bool(a, "raw");
    bool verbose = axl_args_get_bool(a, "verbose");

    // Build base URL
    char base_url[256];
    build_base_url(base_url, sizeof(base_url), host);

    // Create and configure HTTP client
    AXL_AUTOPTR(AxlHttpClient) client = axl_http_client_new();
    if (client == NULL) {
        axl_printf("rfbrowse: out of memory\n");
        return 1;
    }

    axl_http_client_set(client, "tls.verify", "false");
    axl_http_client_set(client, "timeout.ms", "30000");
    axl_http_client_set(client, "header.Accept", "application/json");
    axl_http_client_set(client, "header.OData-Version", "4.0");

    // Authenticate
    char session_uri[256] = "";
    bool have_session = false;

    if (basic && user != NULL && password != NULL) {
        if (setup_basic_auth(client, user, password) != 0) {
            return 1;
        }
    } else if (user != NULL && password != NULL) {
        if (session_login(client, base_url, user, password,
                          session_uri, sizeof(session_uri)) != 0) {
            return 1;
        }
        have_session = true;
        if (verbose) {
            axl_printf("Logged in (session: %s)\n\n",
                       session_uri[0] ? session_uri : "unknown");
        }
    }

    // Determine which paths to fetch
    int  path_count = axl_args_get_pos_count(a);
    int  result     = 0;

    if (path_count == 0) {
        // No path args — fetch service root
        char url[512];
        build_url(url, sizeof(url), base_url, "/redfish/v1/");

        if (members) {
            result = get_members(client, url, base_url, expand, raw, verbose);
        } else {
            result = get_resource(client, url, raw, verbose);
        }
    } else {
        // Process each path argument
        for (int i = 0; i < path_count; i++) {
            const char *arg = axl_args_get_pos(a, i);
            if (arg == NULL) {
                continue;
            }

            const char *path = resolve_shortcut(arg);
            char url[512];
            build_url(url, sizeof(url), base_url, path);

            if (path_count > 1 && !raw) {
                axl_printf("=== %s ===\n", arg);
            }

            int rc;
            if (members) {
                rc = get_members(client, url, base_url, expand, raw, verbose);
            } else {
                rc = get_resource(client, url, raw, verbose);
            }

            if (rc != 0) {
                result = 1;
            }
        }
    }

    // Logout
    if (have_session) {
        session_logout(client, base_url, session_uri);
    }

    return result;
}

int
main(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "rfbrowse",
        .help         = "Redfish REST API browser",
        .flags        = flags,
        .positionals  = positional,
        .handler      = run_rfbrowse,
    });
}
