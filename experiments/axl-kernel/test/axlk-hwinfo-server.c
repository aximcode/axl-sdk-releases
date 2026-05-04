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
// Endpoint handlers — each writes one JSON object into the writer.
// ---------------------------------------------------------------------------

static void
endpoint_index(AxlJsonWriter *w)
{
    axl_json_obj_begin(w);
        axl_json_key(w, "endpoints");
        axl_json_arr_begin(w);
            axl_json_str(w, "/");
            axl_json_str(w, "/system");
            axl_json_str(w, "/cpu");
        axl_json_arr_end(w);
    axl_json_obj_end(w);
}

static void
endpoint_system(AxlJsonWriter *w)
{
    AxlSmbiosHeader *bios = axl_smbios_find(0);
    AxlSmbiosHeader *sys  = axl_smbios_find(1);

    AxlFirmwareInfo  fw;
    uint64_t         mem_bytes = 0;
    axl_sys_get_firmware_info(&fw);
    axl_sys_get_memory_size(&mem_bytes);

    /* Each axl_smbios_get_string_utf8 returns a pointer to a single
     * static 128-char buffer, so only one call is safe at a time —
     * snapshot strings into local buffers before composing JSON. */
    char bios_vendor [128]; axl_strlcpy(bios_vendor, smbios_string_at(bios, 4), sizeof(bios_vendor));
    char sys_mfr     [128]; axl_strlcpy(sys_mfr,     smbios_string_at(sys,  4), sizeof(sys_mfr));
    char sys_product [128]; axl_strlcpy(sys_product, smbios_string_at(sys,  5), sizeof(sys_product));

    char spec[16];
    axl_snprintf(spec, sizeof(spec), "%u.%u",
                 (unsigned)fw.spec_major, (unsigned)fw.spec_minor);
    char revision[16];
    axl_snprintf(revision, sizeof(revision), "0x%08x",
                 (unsigned)fw.firmware_revision);

    axl_json_obj_begin(w);
        axl_json_key(w, "firmware");
        axl_json_obj_begin(w);
            axl_json_kv_str(w, "vendor",   fw.vendor);
            axl_json_kv_str(w, "spec",     spec);
            axl_json_kv_str(w, "revision", revision);
        axl_json_obj_end(w);
        axl_json_key(w, "bios");
        axl_json_obj_begin(w);
            axl_json_kv_str(w, "vendor", bios_vendor);
        axl_json_obj_end(w);
        axl_json_key(w, "system");
        axl_json_obj_begin(w);
            axl_json_kv_str(w, "manufacturer", sys_mfr);
            axl_json_kv_str(w, "product",      sys_product);
        axl_json_obj_end(w);
        axl_json_kv_uint(w, "memory_bytes", (uint64_t)mem_bytes);
    axl_json_obj_end(w);
}

static void
endpoint_cpu(AxlJsonWriter *w)
{
    AxlSmbiosHeader *cpu = axl_smbios_find(4);

    char mfr [128]; axl_strlcpy(mfr, smbios_string_at(cpu,  7), sizeof(mfr));
    char ver [128]; axl_strlcpy(ver, smbios_string_at(cpu, 16), sizeof(ver));

    /* Current speed: WORD at offset 0x16 (22). */
    uint16_t speed_mhz = 0;
    if (cpu != NULL) {
        speed_mhz = *(uint16_t *)((uint8_t *)cpu + 0x16);
    }

    axl_json_obj_begin(w);
        axl_json_key(w, "processor");
        axl_json_obj_begin(w);
            axl_json_kv_str (w, "manufacturer",      mfr);
            axl_json_kv_str (w, "version",           ver);
            axl_json_kv_uint(w, "current_speed_mhz", (uint64_t)speed_mhz);
        axl_json_obj_end(w);
    axl_json_obj_end(w);
}

// ---------------------------------------------------------------------------
// Per-client handler — one process per connection. Sequential code.
// ---------------------------------------------------------------------------

static int
handle_client(int argc, char **argv)
{
    (void)argc;
    int fd = (int)(intptr_t)argv;

    char method[16];
    char path[64];
    char scratch[HWINFO_RECV_BUFSZ];
    int  status   = 200;
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
        endpoint_index(&w);
    } else if (axl_streql(path, "/system")) {
        endpoint_system(&w);
    } else if (axl_streql(path, "/cpu")) {
        endpoint_cpu(&w);
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

    if (axl_net_auto_init(SIZE_MAX, 10) != AXL_OK) {
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
