/**
 * axlk-bootconfig-server.c — second axl-kernel SoftBMC-shape port.
 *
 * Read-only subset of SoftBMC's BootConfig module: serves boot
 * configuration over HTTP by reading UEFI NVRAM variables through
 * axl_nvstore_get. State-heavier than the HwInfo port because it
 * parses binary UEFI variable formats (BootOrder is a u16 array,
 * Boot#### entries are EFI_LOAD_OPTION records with embedded UCS-2
 * descriptions).
 *
 * Endpoints:
 *   GET /            → overview (BootOrder, BootNext, Timeout, SecureBoot)
 *   GET /entries     → array of boot-entry IDs with descriptions
 *   GET /secureboot  → SecureBoot flag only
 *
 * Purpose (see AXL-Kernel-Design.md §9 K6 follow-up): validate that
 * the sequential-process shape holds up on a second, distinctly-
 * shaped SoftBMC module. HwInfo was stateless SMBIOS snapshots;
 * this one reads UEFI variables, parses binary records, and
 * formats arrays — different enough workload to pressure-test.
 */

#include <axl.h>
#include "axl-kernel.h"

#define BC_PORT            8081
#define BC_RECV_BUFSZ      1024
#define BC_MAX_CLIENTS     5
#define BC_MAX_BOOT_ORDER  32

// ---------------------------------------------------------------------------
// UEFI variable readers
// ---------------------------------------------------------------------------

/* Read BootOrder into @p ids (up to @p cap entries). Returns count. */
static int
read_boot_order(uint16_t *ids, int cap)
{
    uint16_t buf[BC_MAX_BOOT_ORDER];
    size_t   sz = sizeof(buf);

    if (axl_nvstore_get("global", "BootOrder", buf, &sz) != 0) {
        return 0;
    }
    int count = (int)(sz / sizeof(uint16_t));
    if (count > cap) count = cap;
    for (int i = 0; i < count; i++) {
        ids[i] = buf[i];
    }
    return count;
}

static int
read_boot_next(uint16_t *out)
{
    size_t sz = sizeof(*out);
    return axl_nvstore_get("global", "BootNext", out, &sz) == 0 ? 0 : -1;
}

static int
read_boot_timeout(uint16_t *out)
{
    size_t sz = sizeof(*out);
    return axl_nvstore_get("global", "Timeout", out, &sz) == 0 ? 0 : -1;
}

static int
read_secure_boot(uint8_t *out)
{
    size_t sz = sizeof(*out);
    return axl_nvstore_get("global", "SecureBoot", out, &sz) == 0 ? 0 : -1;
}

/* Read Boot#### variable, extract UCS-2 description into UTF-8 out.
 * Returns 0 on success, -1 on not-found / malformed. */
static int
read_boot_entry_description(uint16_t id, char *out, size_t out_cap)
{
    char    name[16];
    uint8_t buf[1024];
    size_t  sz = sizeof(buf);

    axl_snprintf(name, sizeof(name), "Boot%04X", (unsigned)id);
    if (axl_nvstore_get("global", name, buf, &sz) != 0) {
        return -1;
    }

    /* EFI_LOAD_OPTION layout:
     *   u32 Attributes
     *   u16 FilePathListLength
     *   CHAR16 Description[]   (null-terminated)
     *   ...FilePathList, OptionalData ignored
     */
    if (sz < 6) return -1;

    const uint16_t *desc = (const uint16_t *)(buf + 6);
    const uint16_t *end  = (const uint16_t *)(buf + sz);

    /* UCS-2 → naive UTF-8 (ASCII only — enough for QEMU boot-entry
     * names like "UEFI QEMU HARDDISK ..."; non-ASCII becomes '?'). */
    size_t w = 0;
    while (desc < end && *desc != 0 && w + 1 < out_cap) {
        uint16_t ch = *desc++;
        if (ch == '"' || ch == '\\') {
            if (w + 2 >= out_cap) break;
            out[w++] = '\\';
            out[w++] = (char)ch;
        } else if (ch >= 0x20 && ch < 0x7f) {
            out[w++] = (char)ch;
        } else {
            out[w++] = '?';
        }
    }
    out[w] = '\0';
    return 0;
}

// ---------------------------------------------------------------------------
// Endpoint builders
// ---------------------------------------------------------------------------

static void
endpoint_overview(AxlJsonWriter *w)
{
    uint16_t order[BC_MAX_BOOT_ORDER];
    int      n = read_boot_order(order, BC_MAX_BOOT_ORDER);
    uint16_t next, timeout;
    uint8_t  sb;
    bool     have_next = (read_boot_next(&next) == 0);
    bool     have_to   = (read_boot_timeout(&timeout) == 0);
    bool     have_sb   = (read_secure_boot(&sb) == 0);

    axl_json_obj_begin(w);
        axl_json_key(w, "boot_order");
        axl_json_arr_begin(w);
        for (int i = 0; i < n; i++) {
            char id[16];
            axl_snprintf(id, sizeof(id), "Boot%04X", (unsigned)order[i]);
            axl_json_str(w, id);
        }
        axl_json_arr_end(w);

        if (have_next) {
            char id[16];
            axl_snprintf(id, sizeof(id), "Boot%04X", (unsigned)next);
            axl_json_kv_str(w, "boot_next", id);
        } else {
            axl_json_kv_null(w, "boot_next");
        }

        if (have_to) {
            axl_json_kv_uint(w, "timeout_s", (uint64_t)timeout);
        } else {
            axl_json_kv_null(w, "timeout_s");
        }

        if (have_sb) {
            axl_json_kv_bool(w, "secure_boot", sb != 0);
        } else {
            axl_json_kv_null(w, "secure_boot");
        }
    axl_json_obj_end(w);
}

static void
endpoint_entries(AxlJsonWriter *w)
{
    uint16_t order[BC_MAX_BOOT_ORDER];
    int      n = read_boot_order(order, BC_MAX_BOOT_ORDER);

    axl_json_obj_begin(w);
        axl_json_key(w, "entries");
        axl_json_arr_begin(w);
        for (int i = 0; i < n; i++) {
            char desc[256];
            desc[0] = '\0';
            read_boot_entry_description(order[i], desc, sizeof(desc));

            char id[16];
            axl_snprintf(id, sizeof(id), "Boot%04X", (unsigned)order[i]);

            axl_json_obj_begin(w);
                axl_json_kv_str(w, "id",          id);
                axl_json_kv_str(w, "description", desc);
            axl_json_obj_end(w);
        }
        axl_json_arr_end(w);
    axl_json_obj_end(w);
}

static void
endpoint_secureboot(AxlJsonWriter *w)
{
    uint8_t sb;
    bool    have = (read_secure_boot(&sb) == 0);
    axl_json_obj_begin(w);
    if (have) {
        axl_json_kv_bool(w, "secure_boot", sb != 0);
    } else {
        axl_json_kv_null(w, "secure_boot");
    }
    axl_json_obj_end(w);
}

// ---------------------------------------------------------------------------
// Per-client handler
// ---------------------------------------------------------------------------

static int
handle_client(int argc, char **argv)
{
    (void)argc;
    int fd = (int)(intptr_t)argv;

    char method[16];
    char path[64];
    char scratch[BC_RECV_BUFSZ];
    int  status = 200;
    const char *status_text = "OK";

    AXL_AUTOPTR(AxlString) body = axl_string_new(NULL);
    AxlJsonWriter w;
    axl_json_writer_init(&w, body, AXL_JSON_WRITER_DEFAULT);

    if (axlk_http_read_request_line(fd, scratch, sizeof(scratch),
                                    method, sizeof(method),
                                    path, sizeof(path)) != 0) {
        status = 400;
        status_text = "Bad Request";
        axl_json_obj_begin(&w);
            axl_json_kv_str(&w, "error", "bad request");
        axl_json_obj_end(&w);
    } else if (axl_streql(path, "/")) {
        endpoint_overview(&w);
    } else if (axl_streql(path, "/entries")) {
        endpoint_entries(&w);
    } else if (axl_streql(path, "/secureboot")) {
        endpoint_secureboot(&w);
    } else {
        status = 404;
        status_text = "Not Found";
        axl_json_obj_begin(&w);
            axl_json_kv_str(&w, "error", "not found");
            axl_json_kv_str(&w, "path",  path);
        axl_json_obj_end(&w);
    }
    size_t body_len = axl_json_writer_finish(&w);

    axl_printf("  pid %d %s → %d (%zu bytes)\n",
               (int)axlk_getpid(), path, status, body_len);

    char header[256];
    int hlen = axl_snprintf(header, sizeof(header),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, body_len);

    axlk_write(fd, header, (size_t)hlen);
    if (body_len > 0) {
        axlk_write(fd, axl_string_str(body), body_len);
    }
    axlk_close(fd);
    return 0;
}

// ---------------------------------------------------------------------------
// pid 1 — accept loop
// ---------------------------------------------------------------------------

static int
bootconfig_service(int argc, char **argv)
{
    (void)argc; (void)argv;

    int listener = axlk_listen(BC_PORT);
    if (listener < 0) {
        axl_printf("FAIL: axlk_listen\n");
        return 1;
    }
    axl_printf("axlk-bootconfig-server: listening on port %u\n",
               (unsigned)BC_PORT);

    for (int i = 0; i < BC_MAX_CLIENTS; i++) {
        int client = axlk_accept(listener);
        if (client < 0) break;
        axl_printf("axlk-bootconfig-server: accepted fd=%d\n", client);

        AxlkPid h = axlk_spawn(handle_client, 0,
                               (char **)(intptr_t)client, 0);
        if (h < 0) {
            axl_printf("FAIL: axlk_spawn\n");
            axlk_close(client);
            break;
        }
    }

    for (;;) {
        int st;
        AxlkPid r = axlk_wait(AXLK_PID_ANY, &st);
        if (r < 0) break;
    }

    axlk_close(listener);
    axl_printf("PASS: axlk-bootconfig-server served %d clients\n",
               BC_MAX_CLIENTS);
    return 0;
}

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    axl_printf("axlk-bootconfig-server: starting\n");

    if (axl_net_auto_init(SIZE_MAX, 10) != 0) {
        axl_printf("FAIL: network not available\n");
        return 1;
    }

    if (axlk_init() != 0) {
        axl_printf("FAIL: axlk_init\n");
        return 1;
    }

    int rc = axlk_run(bootconfig_service, 0, NULL);
    axl_printf("axlk-bootconfig-server: kernel exited rc=%d\n", rc);
    return rc;
}
