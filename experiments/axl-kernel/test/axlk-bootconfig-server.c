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
#define BC_SEND_BUFSZ      8192
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

static int
endpoint_overview(char *buf, size_t cap)
{
    uint16_t order[BC_MAX_BOOT_ORDER];
    int      n = read_boot_order(order, BC_MAX_BOOT_ORDER);
    uint16_t next, timeout;
    uint8_t  sb;
    int      have_next = (read_boot_next(&next) == 0);
    int      have_to   = (read_boot_timeout(&timeout) == 0);
    int      have_sb   = (read_secure_boot(&sb) == 0);

    int w = 0;
    w += axl_snprintf(buf + w, cap - w, "{\"boot_order\":[");
    for (int i = 0; i < n; i++) {
        w += axl_snprintf(buf + w, cap - w, "%s\"Boot%04X\"",
                          i ? "," : "", (unsigned)order[i]);
    }
    w += axl_snprintf(buf + w, cap - w, "],");
    if (have_next) {
        w += axl_snprintf(buf + w, cap - w,
                          "\"boot_next\":\"Boot%04X\",", (unsigned)next);
    } else {
        w += axl_snprintf(buf + w, cap - w, "\"boot_next\":null,");
    }
    if (have_to) {
        w += axl_snprintf(buf + w, cap - w, "\"timeout_s\":%u,", (unsigned)timeout);
    } else {
        w += axl_snprintf(buf + w, cap - w, "\"timeout_s\":null,");
    }
    if (have_sb) {
        w += axl_snprintf(buf + w, cap - w,
                          "\"secure_boot\":%s", sb ? "true" : "false");
    } else {
        w += axl_snprintf(buf + w, cap - w, "\"secure_boot\":null");
    }
    w += axl_snprintf(buf + w, cap - w, "}");
    return w;
}

static int
endpoint_entries(char *buf, size_t cap)
{
    uint16_t order[BC_MAX_BOOT_ORDER];
    int      n = read_boot_order(order, BC_MAX_BOOT_ORDER);

    int w = 0;
    w += axl_snprintf(buf + w, cap - w, "{\"entries\":[");
    for (int i = 0; i < n; i++) {
        char desc[256];
        desc[0] = '\0';
        read_boot_entry_description(order[i], desc, sizeof(desc));

        w += axl_snprintf(buf + w, cap - w,
                          "%s{\"id\":\"Boot%04X\",\"description\":\"%s\"}",
                          i ? "," : "", (unsigned)order[i], desc);
        if (w >= (int)cap - 64) break;
    }
    w += axl_snprintf(buf + w, cap - w, "]}");
    return w;
}

static int
endpoint_secureboot(char *buf, size_t cap)
{
    uint8_t sb;
    int     have = (read_secure_boot(&sb) == 0);
    return axl_snprintf(buf, cap,
        "{\"secure_boot\":%s}",
        have ? (sb ? "true" : "false") : "null");
}

// ---------------------------------------------------------------------------
// HTTP — same pattern as the HwInfo port
// ---------------------------------------------------------------------------

static int
http_read_path(int fd, char *path_out, size_t path_cap)
{
    char   buf[BC_RECV_BUFSZ];
    size_t total = 0;

    while (total < sizeof(buf) - 1) {
        int n = axlk_read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) return -1;
        total += (size_t)n;
        buf[total] = '\0';
        if (axl_strstr(buf, "\r\n\r\n") != NULL) break;
    }

    const char *first  = axl_strchr(buf, ' ');
    if (first == NULL) return -1;
    const char *second = axl_strchr(first + 1, ' ');
    if (second == NULL) return -1;

    size_t path_len = (size_t)(second - (first + 1));
    if (path_len >= path_cap) path_len = path_cap - 1;
    axl_memcpy(path_out, first + 1, path_len);
    path_out[path_len] = '\0';
    return 0;
}

static int
handle_client(int argc, char **argv)
{
    (void)argc;
    int fd = (int)(intptr_t)argv;

    char path[64];
    char body[BC_SEND_BUFSZ];
    int  body_len = 0, status = 200;
    const char *status_text = "OK";

    if (http_read_path(fd, path, sizeof(path)) != 0) {
        status = 400;
        status_text = "Bad Request";
        body_len = axl_snprintf(body, sizeof(body), "{\"error\":\"bad request\"}");
    } else if (axl_streql(path, "/")) {
        body_len = endpoint_overview(body, sizeof(body));
    } else if (axl_streql(path, "/entries")) {
        body_len = endpoint_entries(body, sizeof(body));
    } else if (axl_streql(path, "/secureboot")) {
        body_len = endpoint_secureboot(body, sizeof(body));
    } else {
        status = 404;
        status_text = "Not Found";
        body_len = axl_snprintf(body, sizeof(body),
                                "{\"error\":\"not found\",\"path\":\"%s\"}",
                                path);
    }

    axl_printf("  pid %d %s → %d (%d bytes)\n",
               (int)axlk_getpid(), path, status, body_len);

    char header[256];
    int hlen = axl_snprintf(header, sizeof(header),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, body_len);

    axlk_write(fd, header, (size_t)hlen);
    if (body_len > 0) {
        axlk_write(fd, body, (size_t)body_len);
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
