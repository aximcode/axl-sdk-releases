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
#define RL_SEND_BUFSZ    8192
/* Unbounded by the 16-slot PCB: the accept loop drains zombies inline
 * via axlk_waitpid(AXLK_WNOHANG), so handler slots recycle immediately.
 * Older revisions were capped at 12 for exactly this reason. */
#define RL_MAX_CLIENTS   24
#define RL_RING_CAP      8
#define RL_PATH_MAX      63
#define RL_METHOD_MAX    7

typedef struct {
    uint64_t ts_ms;
    char     method[RL_METHOD_MAX + 1];
    char     path[RL_PATH_MAX + 1];
} ReqLogEntry;

/* The whole point of this port: module-level state shared across all
 * handler processes. No locks — see file header for why. */
static struct {
    ReqLogEntry slots[RL_RING_CAP];
    uint32_t    head;       /* next write index */
    uint32_t    received;   /* total appends since start */
    uint32_t    dropped;    /* count of overwritten (oldest) entries */
} g_ring;

static void
ring_append(const char *method, const char *path)
{
    ReqLogEntry *e = &g_ring.slots[g_ring.head];

    e->ts_ms = axl_time_get_ms();

    size_t i = 0;
    while (i < RL_METHOD_MAX && method[i] != '\0') {
        e->method[i] = method[i];
        i++;
    }
    e->method[i] = '\0';

    i = 0;
    while (i < RL_PATH_MAX && path[i] != '\0') {
        e->path[i] = path[i];
        i++;
    }
    e->path[i] = '\0';

    g_ring.head = (g_ring.head + 1) % RL_RING_CAP;
    g_ring.received++;
    if (g_ring.received > RL_RING_CAP) {
        g_ring.dropped = g_ring.received - RL_RING_CAP;
    }
}

// ---------------------------------------------------------------------------
// Endpoint builders
// ---------------------------------------------------------------------------

static int
endpoint_overview(char *buf, size_t cap)
{
    return axl_snprintf(buf, cap,
        "{\"capacity\":%u,\"received\":%u,\"dropped\":%u,\"head\":%u}",
        (unsigned)RL_RING_CAP,
        (unsigned)g_ring.received,
        (unsigned)g_ring.dropped,
        (unsigned)g_ring.head);
}

static int
endpoint_log(char *buf, size_t cap)
{
    /* Walk oldest → newest. If the ring hasn't wrapped yet, oldest is
     * slot 0; otherwise it's the slot at `head` (which holds the next
     * to be overwritten = currently the oldest valid). */
    uint32_t count = g_ring.received < RL_RING_CAP
                     ? g_ring.received
                     : RL_RING_CAP;
    uint32_t start = g_ring.received < RL_RING_CAP ? 0 : g_ring.head;

    int w = 0;
    w += axl_snprintf(buf + w, cap - w, "{\"entries\":[");
    for (uint32_t i = 0; i < count; i++) {
        const ReqLogEntry *e = &g_ring.slots[(start + i) % RL_RING_CAP];
        w += axl_snprintf(buf + w, cap - w,
            "%s{\"ts_ms\":%llu,\"method\":\"%s\",\"path\":\"%s\"}",
            i ? "," : "",
            (unsigned long long)e->ts_ms,
            e->method, e->path);
        if (w >= (int)cap - 128) break;
    }
    w += axl_snprintf(buf + w, cap - w, "]}");
    return w;
}

static int
endpoint_healthz(char *buf, size_t cap)
{
    return axl_snprintf(buf, cap, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// HTTP — same minimal pattern as the HwInfo / BootConfig ports
// ---------------------------------------------------------------------------

static int
http_read_request(int fd, char *method_out, size_t method_cap,
                  char *path_out, size_t path_cap)
{
    char   buf[RL_RECV_BUFSZ];
    size_t total = 0;

    while (total < sizeof(buf) - 1) {
        int n = axlk_read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) return -1;
        total += (size_t)n;
        buf[total] = '\0';
        if (axl_strstr(buf, "\r\n\r\n") != NULL) break;
    }

    const char *first = axl_strchr(buf, ' ');
    if (first == NULL) return -1;
    const char *second = axl_strchr(first + 1, ' ');
    if (second == NULL) return -1;

    size_t method_len = (size_t)(first - buf);
    if (method_len >= method_cap) method_len = method_cap - 1;
    axl_memcpy(method_out, buf, method_len);
    method_out[method_len] = '\0';

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

    char method[16];
    char path[80];
    char body[RL_SEND_BUFSZ];
    int  body_len = 0, status = 200;
    const char *status_text = "OK";

    if (http_read_request(fd, method, sizeof(method),
                          path, sizeof(path)) != 0) {
        status = 400;
        status_text = "Bad Request";
        body_len = axl_snprintf(body, sizeof(body),
                                "{\"error\":\"bad request\"}");
    } else {
        /* Record FIRST so /log and / observations include themselves —
         * this is what makes "every request mutates state" honest. */
        ring_append(method, path);

        if (axl_streql(path, "/")) {
            body_len = endpoint_overview(body, sizeof(body));
        } else if (axl_streql(path, "/log")) {
            body_len = endpoint_log(body, sizeof(body));
        } else if (axl_streql(path, "/healthz")) {
            body_len = endpoint_healthz(body, sizeof(body));
        } else {
            status = 404;
            status_text = "Not Found";
            body_len = axl_snprintf(body, sizeof(body),
                "{\"error\":\"not found\",\"path\":\"%s\"}", path);
        }
    }

    axl_printf("  pid %d %s %s → %d (%d bytes; recv=%u drop=%u)\n",
               (int)axlk_getpid(), method, path, status, body_len,
               (unsigned)g_ring.received, (unsigned)g_ring.dropped);

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
reqlog_service(int argc, char **argv)
{
    (void)argc; (void)argv;

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
               (unsigned)g_ring.received,
               (unsigned)g_ring.dropped);
    return 0;
}

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;

    axl_printf("axlk-reqlog-server: starting\n");

    if (axl_net_auto_init(SIZE_MAX, 10) != 0) {
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
