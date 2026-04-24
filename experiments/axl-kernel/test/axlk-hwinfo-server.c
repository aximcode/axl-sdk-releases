/**
 * axlk-hwinfo-server.c — the K6 go/no-go gate.
 *
 * Minimal HwInfo HTTP server on axl-kernel, equivalent-in-shape to
 * SoftBMC's HwInfoModule.c. Demonstrates that a real-world service
 * (not a toy echo) fits naturally in the sequential-process model.
 *
 * Endpoints:
 *   GET /         → index (JSON list of endpoints)
 *   GET /system   → BIOS vendor, system manufacturer/product, total RAM
 *   GET /cpu      → processor manufacturer and version from SMBIOS
 *
 * Design comparison point: SoftBMC's equivalent is a module with
 * Init/Cleanup/Poll callbacks, a static route table, stateless void*-
 * sidecar handlers, and a per-module cleanup order hand-coded in the
 * manager. Here, the whole server is one pid1 loop + spawn-per-client,
 * and each handler is a normal C function operating on its own stack.
 * Compare this file's LOC + conceptual complexity against
 * softbmc/SoftBmcPkg/Application/SoftBmc/Modules/Feature/HwInfo/
 * HwInfoModule.c (534 LOC just for the module shell; doesn't include
 * framework, manager, or HTTP server).
 */

#include <axl.h>
#include "axl-kernel.h"

#define HWINFO_PORT        8080
#define HWINFO_RECV_BUFSZ  1024
#define HWINFO_SEND_BUFSZ  4096
#define HWINFO_MAX_CLIENTS 5

// ---------------------------------------------------------------------------
// SMBIOS data lookup helpers
// ---------------------------------------------------------------------------

static const char *
smbios_string_at(AxlSmbiosHeader *hdr, unsigned offset)
{
    if (hdr == NULL) {
        return "";
    }
    /* SMBIOS header is 4 bytes (type, length, handle [2]). String
     * indices are 1-based bytes at offsets into the formatted area. */
    uint8_t idx = ((uint8_t *)hdr)[offset];
    return axl_smbios_get_string_utf8(hdr, idx);
}

// ---------------------------------------------------------------------------
// Endpoint handlers — each builds JSON into a caller-supplied buffer
// and returns the number of bytes written.
// ---------------------------------------------------------------------------

static int
endpoint_index(char *buf, size_t cap)
{
    return axl_snprintf(buf, cap,
        "{"
            "\"endpoints\":["
                "\"/\","
                "\"/system\","
                "\"/cpu\""
            "]"
        "}");
}

static int
endpoint_system(char *buf, size_t cap)
{
    AxlSmbiosHeader *bios   = axl_smbios_find(0);
    AxlSmbiosHeader *sys    = axl_smbios_find(1);

    AxlFirmwareInfo  fw;
    uint64_t         mem_bytes = 0;
    axl_sys_get_firmware_info(&fw);
    axl_sys_get_memory_size(&mem_bytes);

    /* Each axl_smbios_get_string_utf8 returns a pointer to a single
     * static 128-char buffer, so only one call is safe at a time. We
     * compose the JSON by reading one string at a time. */
    char bios_vendor [128]; axl_strlcpy(bios_vendor, smbios_string_at(bios, 4), sizeof(bios_vendor));
    char sys_mfr     [128]; axl_strlcpy(sys_mfr,     smbios_string_at(sys,  4), sizeof(sys_mfr));
    char sys_product [128]; axl_strlcpy(sys_product, smbios_string_at(sys,  5), sizeof(sys_product));

    return axl_snprintf(buf, cap,
        "{"
            "\"firmware\":{"
                "\"vendor\":\"%s\","
                "\"spec\":\"%u.%u\","
                "\"revision\":\"0x%08x\""
            "},"
            "\"bios\":{"
                "\"vendor\":\"%s\""
            "},"
            "\"system\":{"
                "\"manufacturer\":\"%s\","
                "\"product\":\"%s\""
            "},"
            "\"memory_bytes\":%llu"
        "}",
        fw.vendor,
        (unsigned)fw.spec_major, (unsigned)fw.spec_minor,
        (unsigned)fw.firmware_revision,
        bios_vendor,
        sys_mfr, sys_product,
        (unsigned long long)mem_bytes);
}

static int
endpoint_cpu(char *buf, size_t cap)
{
    AxlSmbiosHeader *cpu = axl_smbios_find(4);

    char mfr [128]; axl_strlcpy(mfr, smbios_string_at(cpu,  7), sizeof(mfr));
    char ver [128]; axl_strlcpy(ver, smbios_string_at(cpu, 16), sizeof(ver));

    /* Current speed: WORD at offset 0x16 (22). */
    uint16_t speed_mhz = 0;
    if (cpu != NULL) {
        speed_mhz = *(uint16_t *)((uint8_t *)cpu + 0x16);
    }

    return axl_snprintf(buf, cap,
        "{"
            "\"processor\":{"
                "\"manufacturer\":\"%s\","
                "\"version\":\"%s\","
                "\"current_speed_mhz\":%u"
            "}"
        "}",
        mfr, ver, (unsigned)speed_mhz);
}

// ---------------------------------------------------------------------------
// Tiny HTTP/1.0 request reader — extracts the request path (method is
// always assumed GET for this demo). Returns 0 on success, -1 on error.
// ---------------------------------------------------------------------------

static int
http_read_path(int fd, char *path_out, size_t path_cap)
{
    char buf[HWINFO_RECV_BUFSZ];
    size_t total = 0;

    /* Read until we see \r\n\r\n, or the buffer fills. */
    while (total < sizeof(buf) - 1) {
        int n = axlk_read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
        buf[total] = '\0';
        if (axl_strstr(buf, "\r\n\r\n") != NULL) {
            break;
        }
    }

    /* Request line: METHOD <space> PATH <space> HTTP/1.x\r\n */
    const char *first_space = axl_strchr(buf, ' ');
    if (first_space == NULL) return -1;
    const char *path_start  = first_space + 1;
    const char *second_space = axl_strchr(path_start, ' ');
    if (second_space == NULL) return -1;

    size_t path_len = (size_t)(second_space - path_start);
    if (path_len >= path_cap) path_len = path_cap - 1;
    axl_memcpy(path_out, path_start, path_len);
    path_out[path_len] = '\0';
    return 0;
}

// ---------------------------------------------------------------------------
// Per-client handler — one process per connection. Sequential code.
// ---------------------------------------------------------------------------

static int
handle_client(int argc, char **argv)
{
    (void)argc;
    int fd = (int)(intptr_t)argv;

    char path[64];
    char body[HWINFO_SEND_BUFSZ];
    int  body_len = 0;
    int  status   = 200;
    const char *status_text = "OK";

    if (http_read_path(fd, path, sizeof(path)) != 0) {
        status = 400;
        status_text = "Bad Request";
        body_len = axl_snprintf(body, sizeof(body), "{\"error\":\"bad request\"}");
    } else if (axl_streql(path, "/")) {
        body_len = endpoint_index(body, sizeof(body));
    } else if (axl_streql(path, "/system")) {
        body_len = endpoint_system(body, sizeof(body));
    } else if (axl_streql(path, "/cpu")) {
        body_len = endpoint_cpu(body, sizeof(body));
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
// pid 1 — accept loop, fork-per-connection
// ---------------------------------------------------------------------------

static int
hwinfo_service(int argc, char **argv)
{
    (void)argc; (void)argv;

    int listener = axlk_listen(HWINFO_PORT);
    if (listener < 0) {
        axl_printf("FAIL: axlk_listen\n");
        return 1;
    }
    axl_printf("axlk-hwinfo-server: listening on port %u\n",
               (unsigned)HWINFO_PORT);

    /* Bounded client count so the test exits deterministically. A real
     * service would loop forever. */
    for (int i = 0; i < HWINFO_MAX_CLIENTS; i++) {
        int client = axlk_accept(listener);
        if (client < 0) {
            break;
        }
        axl_printf("axlk-hwinfo-server: accepted fd=%d\n", client);

        AxlkPid h = axlk_spawn(handle_client, 0,
                               (char **)(intptr_t)client, 0);
        if (h < 0) {
            axl_printf("FAIL: axlk_spawn\n");
            axlk_close(client);
            break;
        }
    }

    /* Reap whatever we spawned. */
    for (;;) {
        int st;
        AxlkPid r = axlk_wait(AXLK_PID_ANY, &st);
        if (r < 0) {
            break;
        }
    }

    axlk_close(listener);
    axl_printf("PASS: axlk-hwinfo-server served %d clients\n",
               HWINFO_MAX_CLIENTS);
    return 0;
}

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    axl_printf("axlk-hwinfo-server: starting\n");

    if (axl_net_auto_init(SIZE_MAX, 10) != 0) {
        axl_printf("FAIL: network not available\n");
        return 1;
    }

    if (axlk_init() != 0) {
        axl_printf("FAIL: axlk_init\n");
        return 1;
    }

    int rc = axlk_run(hwinfo_service, 0, NULL);
    axl_printf("axlk-hwinfo-server: kernel exited rc=%d\n", rc);
    return rc;
}
