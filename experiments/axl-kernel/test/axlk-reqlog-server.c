/**
 * axlk-reqlog-server.c — third axl-kernel SoftBMC-shape port.
 *
 * Pressure-tests the one shape the existing ports don't: live, RAM-
 * resident state mutated on every HTTP request, with bounded memory
 * (ring buffer) and visible cross-request side effects.
 *
 *   HwInfo port   → stateless (rescans SMBIOS each request)
 *   BootConfig    → reads UEFI NVRAM (state lives in firmware)
 *   reqlog (this) → state lives in module-level RAM, mutated by every
 *                   request, observable by subsequent requests
 *
 * SoftBMC's log ring, telemetry counters, and session table all have
 * this exact shape — a global structure that every handler appends to.
 *
 * Why this is safe without locks: child handlers only yield on
 * axlk_read / axlk_write. The append between "compute slot" and "store
 * entry" has no syscall, so the scheduler can't preempt mid-mutation.
 * Counters increment atomically from the scheduler's perspective.
 *
 * Endpoints:
 *   GET /         → {"capacity":N,"received":N,"dropped":N,"head":I}
 *   GET /log      → {"entries":[{"ts_ms":..,"method":"GET","path":".."},...]}
 *                    oldest-first, up to capacity entries
 *   GET /healthz  → {"ok":true}   (varies the recorded path mix)
 *
 * The integration test drives the server past ring capacity to verify
 * wrap-around and that `dropped` increments, then queries `/log` to
 * confirm the buffer holds the most-recent N entries.
 */

#include <axl.h>
#include "axl-kernel.h"

#define RL_PORT          8082
#define RL_RECV_BUFSZ    1024
/* Unbounded by the 16-slot PCB: the accept loop drains zombies inline
 * via axlk_waitpid(AXLK_WNOHANG), so handler slots recycle immediately.
 * Older revisions were capped at 12 for exactly this reason. */
#define RL_MAX_CLIENTS   24
#define RL_RING_CAP      8
#define RL_PATH_MAX      63
#define RL_METHOD_MAX    7

/* Padded to 128 bytes so 8 entries × 128 = 1024 (power-of-2) — the size
 * AxlRingBuf requires for its mask-based wrap. The padding is unused. */
typedef struct {
    uint64_t ts_ms;
    char     method[RL_METHOD_MAX + 1];
    char     path[RL_PATH_MAX + 1];
    char     _pad[128 - 8 - (RL_METHOD_MAX + 1) - (RL_PATH_MAX + 1)];
} ReqLogEntry;

/* The whole point of this port: module-level state shared across all
 * handler processes. No locks — see file header for why. */
static AxlRingBuf  g_ring;
static uint8_t     g_ring_storage[RL_RING_CAP * sizeof(ReqLogEntry)];

static void
ring_init(void)
{
    axl_ring_buf_init_fixed(&g_ring,
                            g_ring_storage, sizeof(g_ring_storage),
                            sizeof(ReqLogEntry),
                            AXL_RING_BUF_OVERWRITE, NULL);
}

static void
ring_append(const char *method, const char *path)
{
    ReqLogEntry e;
    e.ts_ms = axl_time_get_ms();
    axl_strlcpy(e.method, method, sizeof(e.method));
    axl_strlcpy(e.path,   path,   sizeof(e.path));
    axl_memset(e._pad, 0, sizeof(e._pad));
    axl_ring_buf_push_elem(&g_ring, &e);
}

/* Stats accessors that translate AxlRingBuf's byte counters to elements. */
static inline uint32_t
ring_received(void)
{
    return (uint32_t)(axl_ring_buf_pushes_total(&g_ring) / sizeof(ReqLogEntry));
}

static inline uint32_t
ring_dropped(void)
{
    return (uint32_t)(axl_ring_buf_pushes_lost(&g_ring) / sizeof(ReqLogEntry));
}

// ---------------------------------------------------------------------------
// Endpoint builders
// ---------------------------------------------------------------------------

static void
endpoint_overview(AxlJsonWriter *w)
{
    /* "head" stays in the response shape for backward-compat with the
     * integration test; it's the next-write index modulo capacity. */
    uint32_t head = ring_received() % RL_RING_CAP;

    axl_json_obj_begin(w);
        axl_json_kv_uint(w, "capacity", (uint64_t)RL_RING_CAP);
        axl_json_kv_uint(w, "received", (uint64_t)ring_received());
        axl_json_kv_uint(w, "dropped",  (uint64_t)ring_dropped());
        axl_json_kv_uint(w, "head",     (uint64_t)head);
    axl_json_obj_end(w);
}

static void
endpoint_log(AxlJsonWriter *w)
{
    uint32_t count = axl_ring_buf_get_length(&g_ring);

    axl_json_obj_begin(w);
        axl_json_key(w, "entries");
        axl_json_arr_begin(w);
        for (uint32_t i = 0; i < count; i++) {
            ReqLogEntry e;
            if (axl_ring_buf_peek_nth_elem(&g_ring, i, &e) != AXL_OK) {
                break;
            }
            axl_json_obj_begin(w);
                axl_json_kv_uint(w, "ts_ms",  e.ts_ms);
                axl_json_kv_str (w, "method", e.method);
                axl_json_kv_str (w, "path",   e.path);
            axl_json_obj_end(w);
        }
        axl_json_arr_end(w);
    axl_json_obj_end(w);
}

static void
endpoint_healthz(AxlJsonWriter *w)
{
    axl_json_obj_begin(w);
        axl_json_kv_bool(w, "ok", true);
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
    char path[80];
    char scratch[RL_RECV_BUFSZ];
    int  status = 200;
    const char *status_text = "OK";

    AXL_AUTOPTR(AxlString) body = axl_string_new(NULL);
    AxlJsonWriter jw;
    axl_json_writer_init(&jw, body, AXL_JSON_STRICT);

    if (axlk_http_read_request_line(fd, scratch, sizeof(scratch),
                                    method, sizeof(method),
                                    path, sizeof(path)) != 0) {
        status = 400;
        status_text = "Bad Request";
        axl_json_obj_begin(&jw);
            axl_json_kv_str(&jw, "error", "bad request");
        axl_json_obj_end(&jw);
    } else {
        /* Record FIRST so /log and / observations include themselves —
         * this is what makes "every request mutates state" honest. */
        ring_append(method, path);

        if (axl_streql(path, "/")) {
            endpoint_overview(&jw);
        } else if (axl_streql(path, "/log")) {
            endpoint_log(&jw);
        } else if (axl_streql(path, "/healthz")) {
            endpoint_healthz(&jw);
        } else {
            status = 404;
            status_text = "Not Found";
            axl_json_obj_begin(&jw);
                axl_json_kv_str(&jw, "error", "not found");
                axl_json_kv_str(&jw, "path",  path);
            axl_json_obj_end(&jw);
        }
    }
    size_t body_len = axl_json_writer_finish(&jw);

    axl_printf("  pid %d %s %s → %d (%zu bytes; recv=%u drop=%u)\n",
               (int)axlk_getpid(), method, path, status, body_len,
               (unsigned)ring_received(), (unsigned)ring_dropped());

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
reqlog_service(int argc, char **argv)
{
    (void)argc; (void)argv;

    ring_init();

    int listener = axlk_listen(RL_PORT);
    if (listener < 0) {
        axl_printf("FAIL: axlk_listen\n");
        return 1;
    }
    axl_printf("axlk-reqlog-server: listening on port %u\n",
               (unsigned)RL_PORT);

    for (int i = 0; i < RL_MAX_CLIENTS; i++) {
        /* Drain any zombies from prior connections so their PCB slots
         * recycle. Without this the accept-then-spawn pattern caps
         * out at ~14 total connections (PCB has 16 slots; pid 1 + up
         * to ~14 zombies accumulated until the post-loop wait). */
        while (axlk_waitpid(AXLK_PID_ANY, NULL, AXLK_WNOHANG) > 0) {
        }

        int client = axlk_accept(listener);
        if (client < 0) break;
        axl_printf("axlk-reqlog-server: accepted fd=%d\n", client);

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
    axl_printf("PASS: axlk-reqlog-server served %d clients (recv=%u drop=%u)\n",
               RL_MAX_CLIENTS,
               (unsigned)ring_received(),
               (unsigned)ring_dropped());
    return 0;
}

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    axl_printf("axlk-reqlog-server: starting\n");

    if (axl_net_auto_init(SIZE_MAX, 10) != AXL_OK) {
        axl_printf("FAIL: network not available\n");
        return 1;
    }

    if (axlk_init() != 0) {
        axl_printf("FAIL: axlk_init\n");
        return 1;
    }

    int rc = axlk_run(reqlog_service, 0, NULL);
    axl_printf("axlk-reqlog-server: kernel exited rc=%d\n", rc);
    return rc;
}
