/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * axbench.c — real-hardware AP-pool benchmark tool.
 *
 * Measures AxlTaskPool (AP offload) vs BSP across 8 scenarios:
 *   1. ENV          — CPU topology, worker count, firmware date
 *   2. SPINUP       — one-time pool-creation wall time
 *   3. DISPATCH LAT — submit→done round-trip ns/op (1000 iterations)
 *   4. COMPUTE      — compute-bound break-even sweep (rounds vs W)
 *   5. GRANULARITY  — fixed-total-work chunk-count sweep
 *   6. BLUR         — bandwidth-bound tiled box-blur sweep
 *   7. BSP-PART     — BSP-participates vs orchestrate-only
 *   8. SUMMARY      — break-even, peak speedup, dispatch lat, verdict
 *
 * Usage:
 *   axbench            -> report to stdout
 *   axbench <outfile>  -> report to <outfile> (fallback to stdout on error)
 *   axbench a b ...    -> usage error, return 2
 *
 * Progress lines always go to the console; the report goes to the sink.
 * AP kernels are pure arithmetic (AP-safe: no Boot Services, no alloc,
 * no print, no protocol calls).
 *
 * Run: scripts/run-qemu.sh --qemu-arg -smp --qemu-arg N out/native-x64/axbench.efi
 */

#include <axl.h>

/* =========================================================================
 * Output sink — file or stdout
 * ========================================================================= */

static AxlFileWriter *g_writer;   /* NULL = stdout */
static bool           g_write_error;   /* set if a sink write ever failed */

/* Write callback: emit to console char-by-char (no len-aware console API). */
static void
console_write_fn(const char *data, size_t len, void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        axl_printf("%c", data[i]);
    }
}

/* Write callback: emit to file writer (ctx unused — uses g_writer global). */
static void
file_write_fn(const char *data, size_t len, void *ctx)
{
    (void)ctx;
    /* Sink callback (void return) — a write error can't be propagated through
       this contract, so capture it into g_write_error; the finalize path
       reports it alongside the close status. */
    if (axl_file_writer_write(g_writer, data, len) != AXL_OK) {
        g_write_error = true;
    }
}

/* rep: report line — goes to sink (file or stdout). */
static void
rep(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
rep(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_writer) {
        axl_vformat(file_write_fn, NULL, fmt, ap);
    } else {
        axl_vformat(console_write_fn, NULL, fmt, ap);
    }
    va_end(ap);
}

/* progress: always to console. */
static void
progress(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
progress(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    axl_vformat(console_write_fn, NULL, fmt, ap);
    va_end(ap);
}

/* =========================================================================
 * Ctrl-C abort
 *
 * axbench is CPU-bound with no event loop, so the shell's Ctrl-C
 * (ExecutionBreak) is only observed when something calls axl_yield() — the
 * runtime polls the shell break flag there (it is NOT an async notify).
 * The BSP-side orchestration loops therefore call check_interrupt(), which
 * yields (poll + set the interrupted flag) and, on Ctrl-C, prints a line and
 * exits via axl_exit(). The poll is rate-limited to ~10 Hz so a CheckEvent
 * per wave cycle doesn't distort the timed measurements. Checks live only in
 * orchestration loops, never in the measured kernels.
 *
 * The AP workers spin in worker_proc — code inside this image — so they must
 * be stopped before the image unloads or the firmware faults; an axl_atexit
 * hook (cleanup_atexit, defined with the resource globals just before main)
 * frees the pool AND the bench buffers on every exit path (Ctrl-C abort,
 * error return, or normal completion).
 * ========================================================================= */

/* Poll for Ctrl-C (rate-limited); on request abort cleanly — axl_exit runs
 * the atexit hook (stops the AP workers, frees buffers) then gBS->Exit.
 * 130 = 128 + SIGINT, the conventional interrupted code. */
static void
check_interrupt(void)
{
    static uint64_t last_poll_us;
    uint64_t now = axl_time_get_us();
    if (now - last_poll_us < 100000ull) {   /* ~10 Hz */
        return;
    }
    last_poll_us = now;

    axl_yield();   /* polls the shell break flag; sets the interrupted flag */
    if (axl_interrupted()) {
        progress("\r\n[axbench] interrupted (Ctrl-C) - aborting cleanly\r\n");
        axl_exit(130);
    }
}

/* =========================================================================
 * Compute kernel — AP-safe pure FNV mix over a word buffer.
 * ========================================================================= */

typedef struct {
    const uint32_t *in;
    size_t          len;
    uint32_t        rounds;
    uint64_t        out;
} BenchChunk;

static void
bench_mix(AxlArena *arena, void *arg)
{
    (void)arena;
    BenchChunk *c = (BenchChunk *)arg;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t r = 0; r < c->rounds; r++) {
        for (size_t i = 0; i < c->len; i++) {
            h ^= c->in[i];
            h *= 1099511628211ull;
            h ^= (h >> 23);
            h += (h << 17);
        }
    }
    c->out = h;
}

/* Run all chunks serially on BSP; return XOR accumulator. */
static uint64_t
run_bsp(BenchChunk *chunks, size_t n)
{
    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        bench_mix(NULL, &chunks[i]);
        acc ^= chunks[i].out;
    }
    return acc;
}

/* No-progress watchdog threshold: if no task completes for this long, abort
 * the scenario instead of hanging the machine. 60 s gives QEMU (which runs AP
 * threads slowly under heavy KVM oversubscription) plenty of room while still
 * catching genuinely dead/stuck workers. */
#define WATCHDOG_STALL_US  60000000ull   /* 60 s */

/* Shared wave-loop driver for the pool scenarios.
 *
 * Submits up to the pool's currently-available slots (gated on available()
 * so we never hammer a full pool into its "workers busy" WARN), then polls
 * completions; repeats until all n tasks finish.
 *
 * IMPORTANT: available() can momentarily over-report free slots under the
 * worker claim/complete race, so a submit may still be rejected even right
 * after available() said there was room. A rejected submit must NOT advance
 * the task index — otherwise that task is silently dropped and the wave can
 * never reach n (the bug that hung the R6725). We only count accepted submits
 * and re-poll/retry on rejection.
 *
 * elems/elem_size address the task arg array generically (BenchChunk or
 * BlurTile). Returns true on completion, false if the watchdog fires (the
 * caller then records STALLED). */
static bool
drive_wave(AxlTaskPool *pool, AxlTaskProc proc, void *elems,
           size_t elem_size, size_t n)
{
    size_t   submitted = 0, done = 0;
    bool     single = axl_task_pool_is_single_core(pool);
    uint64_t last_progress_us = axl_time_get_us();

    while (done < n) {
        check_interrupt();
        if (single) {
            /* Single-core fallback: submit runs the task inline. */
            void *arg = (char *)elems + submitted * elem_size;
            axl_task_pool_submit(pool, proc, arg, NULL, NULL);
            submitted++;
            done++;
            continue;
        }

        size_t free_slots = axl_task_pool_available(pool);
        while (submitted < n && free_slots > 0) {
            void *arg = (char *)elems + submitted * elem_size;
            if (axl_task_pool_submit(pool, proc, arg, NULL, NULL)
                    == AXL_TASK_ID_INVALID) {
                break;   /* over-reported slot: stop, drain, retry next cycle */
            }
            submitted++;
            free_slots--;
        }

        size_t freed = axl_task_pool_poll(pool);
        done += freed;
        if (freed > 0) {
            last_progress_us = axl_time_get_us();
        } else if (axl_time_get_us() - last_progress_us > WATCHDOG_STALL_US) {
            return false;
        }
    }
    return true;
}

/* Compute wave: run all chunks through the pool. Returns the XOR accumulator,
 * or (uint64_t)-1 if the watchdog fired. */
static uint64_t
run_pool(AxlTaskPool *pool, BenchChunk *chunks, size_t n)
{
    if (!drive_wave(pool, bench_mix, chunks, sizeof(chunks[0]), n)) {
        return (uint64_t)-1;
    }
    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc ^= chunks[i].out;
    }
    return acc;
}

/* =========================================================================
 * Blur kernel — AP-safe horizontal box blur.
 * Sized adaptively: IMG_W is fixed; IMG_H is chosen at runtime so the
 * image is at least W*8 rows (meaningful tile sweep range).
 * ========================================================================= */

#define IMG_W    512
#define BLUR_R   4
/* Clamp for dynamic image height (at 512 wide = 4 MB per buffer). */
#define MAX_IMG_H  8192

static uint8_t *g_src;   /* heap-allocated in main() after sizing */
static uint8_t *g_dst;   /* heap-allocated in main() after sizing */
static size_t   g_img_h; /* chosen at startup */

typedef struct {
    const uint8_t *src;
    uint8_t       *dst;
    uint32_t       y0, y1;
} BlurTile;

static BlurTile *g_tiles; /* heap-allocated in main() after sizing */

static void
blur_tile(AxlArena *arena, void *arg)
{
    (void)arena;
    BlurTile *t = (BlurTile *)arg;
    for (uint32_t y = t->y0; y < t->y1; y++) {
        const uint8_t *srow = t->src + (size_t)y * IMG_W;
        uint8_t       *drow = t->dst + (size_t)y * IMG_W;
        for (uint32_t x = 0; x < IMG_W; x++) {
            uint32_t sum = 0, cnt = 0;
            for (int dx = -BLUR_R; dx <= BLUR_R; dx++) {
                int xx = (int)x + dx;
                if (xx >= 0 && xx < IMG_W) {
                    sum += srow[xx];
                    cnt++;
                }
            }
            drow[x] = (uint8_t)(sum / cnt);
        }
    }
}

/* Build tile list; returns tile count. */
static size_t
blur_build_tiles(uint32_t tile_h)
{
    size_t n = 0;
    for (uint32_t y = 0; y < (uint32_t)g_img_h; y += tile_h, n++) {
        g_tiles[n].src = g_src;
        g_tiles[n].dst = g_dst;
        g_tiles[n].y0  = y;
        g_tiles[n].y1  = ((y + tile_h) <= (uint32_t)g_img_h)
                              ? (y + tile_h) : (uint32_t)g_img_h;
    }
    return n;
}

/* Blur run context (tile count may change per sweep step). */
static size_t g_n_blur_tiles;

static uint64_t
blur_run_bsp(void)
{
    for (size_t i = 0; i < g_n_blur_tiles; i++) {
        blur_tile(NULL, &g_tiles[i]);
    }
    uint64_t acc = 0;
    for (size_t i = 0; i < (size_t)IMG_W * g_img_h; i++) {
        acc += g_dst[i];
    }
    return acc;
}

/* Blur wave: run all tiles through the pool. Returns the sum accumulator,
 * or (uint64_t)-1 if the watchdog fired. */
static uint64_t
blur_run_pool(AxlTaskPool *pool)
{
    if (!drive_wave(pool, blur_tile, g_tiles, sizeof(g_tiles[0]),
                    g_n_blur_tiles)) {
        return (uint64_t)-1;
    }
    uint64_t acc = 0;
    for (size_t i = 0; i < (size_t)IMG_W * g_img_h; i++) {
        acc += g_dst[i];
    }
    return acc;
}

/* =========================================================================
 * Median timer (warm-up=1, timed=3)
 * ========================================================================= */

#define WARMUP_RUNS  1
#define TIMED_RUNS   3

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Stall flag: set by thunks when run_pool/blur_run_pool fires the watchdog.
 * median_us returns (uint64_t)-1 (STALLED_US sentinel) when set. */
#define STALLED_US  ((uint64_t)-1)
static bool g_stalled;

typedef void (*TimedFn)(void *ctx);

/* Returns STALLED_US if g_stalled was set during any call to fn. */
static uint64_t
median_us(TimedFn fn, void *ctx)
{
    g_stalled = false;
    for (int w = 0; w < WARMUP_RUNS; w++) {
        check_interrupt();
        fn(ctx);
        if (g_stalled) { return STALLED_US; }
    }
    uint64_t s[TIMED_RUNS];
    for (int i = 0; i < TIMED_RUNS; i++) {
        uint64_t t0 = axl_time_get_us();
        fn(ctx);
        s[i] = axl_time_get_us() - t0;
        if (g_stalled) { return STALLED_US; }
        check_interrupt();   /* outside the t0..t1 window — no timing distortion */
    }
    axl_qsort(s, TIMED_RUNS, sizeof(s[0]), cmp_u64);
    return s[TIMED_RUNS / 2];
}

/* Thunks for compute kernels. */
typedef struct {
    BenchChunk    *chunks;
    size_t         n;
    AxlTaskPool   *pool;
} ComputeCtx;

static void bsp_thunk(void *ctx)
{
    ComputeCtx *c = (ComputeCtx *)ctx;
    run_bsp(c->chunks, c->n);
}

static void pool_thunk(void *ctx)
{
    ComputeCtx *c = (ComputeCtx *)ctx;
    /* Reset outputs before each timed run so results aren't stale. */
    for (size_t i = 0; i < c->n; i++) { c->chunks[i].out = 0; }
    uint64_t r = run_pool(c->pool, c->chunks, c->n);
    if (r == (uint64_t)-1) { g_stalled = true; }
}

/* Thunks for blur kernels. */
typedef struct {
    AxlTaskPool *pool;
    uint32_t     tile_h;
} BlurCtx;

static void blur_bsp_thunk(void *ctx)
{
    BlurCtx *b = (BlurCtx *)ctx;
    g_n_blur_tiles = blur_build_tiles(b->tile_h);
    axl_memset(g_dst, 0, IMG_W * g_img_h);
    blur_run_bsp();
}

static void blur_pool_thunk(void *ctx)
{
    BlurCtx *b = (BlurCtx *)ctx;
    g_n_blur_tiles = blur_build_tiles(b->tile_h);
    axl_memset(g_dst, 0, IMG_W * g_img_h);
    uint64_t r = blur_run_pool(b->pool);
    if (r == (uint64_t)-1) { g_stalled = true; }
}

/* =========================================================================
 * Topology helpers
 * ========================================================================= */

/* Maximum processors we track (EPYC 9555 dual-socket = 256 threads). */
#define MAX_PROCS  512

static AxlCpuProcessor g_procs[MAX_PROCS];
static size_t g_total_procs, g_enabled_procs, g_out_n;

/* Count distinct (package, core) pairs among enabled processors. */
static size_t
count_phys_cores(void)
{
    /* Pack (package<<16 | core) pairs; mark seen in a small bitset.
     * EPYC 9555 dual-socket: up to 128 cores per socket = 256 total pairs.
     * Use a simple O(n^2) dedup — n<=512, fine. */
    uint32_t seen[MAX_PROCS];
    size_t nseen = 0;
    for (size_t i = 0; i < g_out_n; i++) {
        if (!g_procs[i].enabled) {
            continue;
        }
        uint32_t key = (g_procs[i].package << 16) | (g_procs[i].core & 0xffff);
        bool found = false;
        for (size_t j = 0; j < nseen; j++) {
            if (seen[j] == key) {
                found = true;
                break;
            }
        }
        if (!found && nseen < MAX_PROCS) {
            seen[nseen++] = key;
        }
    }
    /* If MP enumeration returned nothing (uniprocessor firmware), return 1. */
    return nseen > 0 ? nseen : 1;
}

/* Count distinct package values among enabled processors. */
static size_t
count_sockets(void)
{
    uint32_t seen[16]; /* capped at 16 sockets — sufficient for any realistic target */
    size_t nseen = 0;
    for (size_t i = 0; i < g_out_n; i++) {
        if (!g_procs[i].enabled) {
            continue;
        }
        uint32_t pkg = g_procs[i].package;
        bool found = false;
        for (size_t j = 0; j < nseen; j++) {
            if (seen[j] == pkg) {
                found = true;
                break;
            }
        }
        if (!found && nseen < 16) {
            seen[nseen++] = pkg;
        }
    }
    return nseen > 0 ? nseen : 1;
}

/* Threads per physical core (SMT depth). */
static size_t
threads_per_core(void)
{
    size_t phys = count_phys_cores();
    size_t enabled = g_enabled_procs > 0 ? g_enabled_procs : 1;
    return enabled / phys;
}

/* =========================================================================
 * Calibration — find the largest rounds that keeps a single BSP
 * baseline <= target_us.
 * ========================================================================= */

/* Compute one timed BSP pass with given rounds over n_chunks. */
typedef struct { BenchChunk *chunks; size_t n; } CalCtx;

static void cal_bsp_thunk(void *ctx)
{
    CalCtx *c = (CalCtx *)ctx;
    run_bsp(c->chunks, c->n);
}

static uint32_t
calibrate_rounds(BenchChunk *chunks, size_t n, const uint32_t *sweep,
                 size_t sweep_len, uint64_t target_us)
{
    uint32_t chosen = sweep[0];
    for (size_t k = 0; k < sweep_len; k++) {
        for (size_t i = 0; i < n; i++) {
            chunks[i].rounds = sweep[k];
            chunks[i].out = 0;
        }
        CalCtx ctx = { chunks, n };
        /* Single warm-up, single timing (calibration is rough). */
        cal_bsp_thunk(&ctx);
        uint64_t t0 = axl_time_get_us();
        cal_bsp_thunk(&ctx);
        uint64_t us = axl_time_get_us() - t0;
        if (us <= target_us) {
            chosen = sweep[k];
        } else {
            break;
        }
    }
    return chosen;
}

/* =========================================================================
 * Chunk storage — sized adaptively to W*4.
 * W can be up to 255 on the XE7745, so W*4 = 1020 chunks.
 * ========================================================================= */

#define MAX_CHUNKS  4096
#define WORDS_PER_CHUNK  256    /* small L1/L2-resident working set */

static uint32_t   g_input[WORDS_PER_CHUNK];
static BenchChunk *g_chunks; /* heap-allocated in main() after sizing */

/* =========================================================================
 * Dispatch-latency probe (near-empty task, BSP-side timing).
 * ========================================================================= */

static volatile uint64_t g_lat_dummy;

static void lat_task(AxlArena *arena, void *arg)
{
    (void)arena;
    /* Tiny arithmetic to prevent DCE; minimal work. */
    uint64_t *p = (uint64_t *)arg;
    *p = (*p ^ 0xdeadbeefcafeull) + 1;
}

/* =========================================================================
 * BSP-participates variant: split work into AP portion and BSP portion,
 * run them concurrently so BSP+APs all contribute.
 * ========================================================================= */

/* Run BSP portion of BSP-participates scenario.
 * APs handle chunks [0..ap_end), BSP handles [ap_end..n).
 * Returns (uint64_t)-1 if the watchdog fires. */
static uint64_t
run_bsp_part(AxlTaskPool *pool, BenchChunk *chunks, size_t n)
{
    if (axl_task_pool_is_single_core(pool)) {
        return run_bsp(chunks, n);
    }
    size_t W = axl_task_pool_worker_count(pool);
    /* APs handle first W/(W+1) fraction; BSP handles the rest. */
    size_t ap_end = (n * W) / (W + 1);
    if (ap_end == 0) { ap_end = 1; }
    if (ap_end >= n) { ap_end = n - 1; }

    /* Submit AP portion — gate on available slots to avoid WARN flood. */
    size_t submitted = 0, done = 0;
    uint64_t last_progress_us = axl_time_get_us();
    while (submitted < ap_end) {
        check_interrupt();
        size_t free_slots = axl_task_pool_available(pool);
        size_t batch = ap_end - submitted;
        if (batch > free_slots) { batch = free_slots; }
        for (size_t b = 0; b < batch; b++) {
            /* available() may over-report under the worker race; a rejected
             * submit must not advance the index (else the task is dropped). */
            if (axl_task_pool_submit(pool, bench_mix, &chunks[submitted],
                                     NULL, NULL) == AXL_TASK_ID_INVALID) {
                break;
            }
            submitted++;
        }
        if (submitted < ap_end) {
            /* Pool full — drain a slot then retry. */
            size_t freed = axl_task_pool_poll(pool);
            done += freed;
            if (freed > 0) {
                last_progress_us = axl_time_get_us();
            } else if (axl_time_get_us() - last_progress_us > WATCHDOG_STALL_US) {
                return (uint64_t)-1;
            }
        }
    }

    /* BSP computes its portion while APs run. */
    uint64_t bsp_acc = 0;
    for (size_t i = ap_end; i < n; i++) {
        bench_mix(NULL, &chunks[i]);
        bsp_acc ^= chunks[i].out;
    }

    /* Drain remaining AP tasks. */
    while (done < submitted) {
        check_interrupt();
        size_t freed = axl_task_pool_poll(pool);
        done += freed;
        if (freed > 0) {
            last_progress_us = axl_time_get_us();
        } else if (axl_time_get_us() - last_progress_us > WATCHDOG_STALL_US) {
            return (uint64_t)-1;
        }
    }

    uint64_t ap_acc = 0;
    for (size_t i = 0; i < ap_end; i++) {
        ap_acc ^= chunks[i].out;
    }
    return ap_acc ^ bsp_acc;
}

/* =========================================================================
 * Resource teardown (registered with axl_atexit so it runs on every exit
 * path: normal return, error return, and Ctrl-C via axl_exit). Idempotent —
 * each resource is freed once and nulled — so the explicit calls on the
 * normal/error paths and the atexit backstop never double-free.
 * ========================================================================= */

static AxlTaskPool *g_pool;   /* set in main; the hook frees + nulls it */

static void
cleanup_atexit(void *unused)
{
    (void)unused;
    if (g_pool   != NULL) { axl_task_pool_free(g_pool); g_pool = NULL; }
    if (g_tiles  != NULL) { axl_free(g_tiles);  g_tiles  = NULL; }
    if (g_dst    != NULL) { axl_free(g_dst);    g_dst    = NULL; }
    if (g_src    != NULL) { axl_free(g_src);    g_src    = NULL; }
    if (g_chunks != NULL) { axl_free(g_chunks); g_chunks = NULL; }
    if (g_writer != NULL) {
        if (axl_file_writer_close(g_writer) != AXL_OK || g_write_error) {
            axl_printerr("axbench: warning: benchmark output file did not write/flush cleanly\n");
        }
        g_writer = NULL;
    }
}

/* No-op handler: installing one disables axl_yield()'s default exit-on-break
 * policy so check_interrupt() can print + choose the exit code itself. The
 * runtime still sets the interrupted flag for us. */
static void
on_interrupt(void)
{
}

/* =========================================================================
 * main — AXL_TOOL_MAIN so axbench builds as a first-class tool (standalone
 * tools/axbench.efi + a busybox-safe axl_tool_axbench_main), with the uniform
 * --version / -V handling every other tool gets.
 * ========================================================================= */

AXL_TOOL_MAIN(axbench)
{
    /* -h/--help via the shared hook (this also fixes the old wart where "-h"
       was opened as an OUTPUT FILE — the hook intercepts it first). --version/-V
       is handled one layer up by AXL_TOOL_MAIN. axbench keeps its own tiny
       [outfile] parse rather than the axl_args framework on purpose: it aborts
       via axl_exit() on Ctrl-C, which would leak an axl_args_run parse state. */
    if (axl_help_handle("axbench",
            "AP task-pool micro-benchmark",
            "axbench [outfile]",
            argc, argv)) {
        return 0;
    }

    /* --- Argument parsing --- */
    if (argc > 2) {
        axl_printf("Usage: axbench [outfile]\r\n");
        return 2;
    }
    const char *outfile = (argc == 2) ? argv[1] : NULL;

    g_writer = NULL;
    if (outfile != NULL) {
        g_writer = axl_file_writer_open(outfile, 0);
        if (!g_writer) {
            axl_printf("[axbench] WARNING: could not open '%s' for writing;"
                       " falling back to stdout\r\n", outfile);
        }
    }

    /* --- Topology --- */
    axl_cpu_topology(&g_total_procs, &g_enabled_procs,
                     g_procs, MAX_PROCS, &g_out_n);
    size_t sockets    = count_sockets();
    size_t phys_cores = count_phys_cores();
    size_t smt_depth  = threads_per_core();

    /* --- Pool creation (timed — scenario 2: SPINUP) --- */
    progress("[1/8] Creating task pool (may take a moment on many-core systems)...\r\n");
    uint64_t spinup_t0 = axl_time_get_us();
    AxlTaskPool *pool = axl_task_pool_new();
    uint64_t spinup_us = axl_time_get_us() - spinup_t0;

    /* Stop the AP workers on any exit path (Ctrl-C abort or normal return)
     * before the image unloads — see check_interrupt(). */
    g_pool = pool;
    axl_atexit(cleanup_atexit, NULL);
    axl_signal_install(on_interrupt);   /* let check_interrupt() own the exit */

    size_t W = axl_task_pool_worker_count(pool);
    bool   single = axl_task_pool_is_single_core(pool);

    /* --- Initialise input buffer --- */
    for (size_t i = 0; i < WORDS_PER_CHUNK; i++) {
        g_input[i] = (uint32_t)(i * 2654435761u + 1u);
    }

    /* -----------------------------------------------------------------------
     * Scenario 1: ENV
     * --------------------------------------------------------------------- */
    progress("[1/8] ENV...\r\n");

    AxlRealtime rt;
    axl_memset(&rt, 0, sizeof(rt));
    axl_time_realtime(&rt);

    rep("==========================================================\r\n");
    rep(" axbench - AP-pool benchmark\r\n");
    rep("==========================================================\r\n");
    rep("Date:            %04u-%02u-%02u %02u:%02u:%02u\r\n",
        rt.year, rt.month, rt.day, rt.hour, rt.minute, rt.second);
    rep("Logical total:   %zu\r\n", g_total_procs);
    rep("Logical enabled: %zu\r\n", g_enabled_procs);
    rep("Sockets:         %zu\r\n", sockets);
    rep("Physical cores:  %zu\r\n", phys_cores);
    rep("SMT depth:       %zu threads/core\r\n", smt_depth);
    rep("AP workers (W):  %zu\r\n", W);
    rep("Single-core:     %s\r\n", single ? "yes" : "no");
    rep("\r\n");

    if (single) {
        rep("Single-core mode: no APs available. AP-vs-BSP comparison N/A.\r\n");
        rep("==========================================================\r\n");
        cleanup_atexit(NULL);   /* closes g_writer + frees pool (idempotent) */
        return 0;
    }

    /* -----------------------------------------------------------------------
     * Allocate bench buffers now that W and sizing are known.
     * Single-core path returned above, so these are only reachable with APs.
     * AP kernels only read/write through pointers in their structs — no
     * axl_malloc/axl_free on the AP path.
     * --------------------------------------------------------------------- */
    size_t actual_img_h = (size_t)W * 8;
    if (actual_img_h < 64)        { actual_img_h = 64; }
    if (actual_img_h > MAX_IMG_H) { actual_img_h = MAX_IMG_H; }

    g_chunks = axl_malloc(MAX_CHUNKS * sizeof(*g_chunks));
    if (!g_chunks) {
        axl_printf("[axbench] ERROR: out of memory allocating g_chunks\r\n");
        cleanup_atexit(NULL);
        return 1;
    }

    g_src = axl_malloc(IMG_W * actual_img_h);
    if (!g_src) {
        axl_printf("[axbench] ERROR: out of memory allocating g_src\r\n");
        cleanup_atexit(NULL);
        return 1;
    }
    g_dst = axl_malloc(IMG_W * actual_img_h);
    if (!g_dst) {
        axl_printf("[axbench] ERROR: out of memory allocating g_dst\r\n");
        cleanup_atexit(NULL);
        return 1;
    }
    /* Max tiles = one row per tile (worst case). */
    g_tiles = axl_malloc(actual_img_h * sizeof(*g_tiles));
    if (!g_tiles) {
        axl_printf("[axbench] ERROR: out of memory allocating g_tiles\r\n");
        cleanup_atexit(NULL);
        return 1;
    }

    /* -----------------------------------------------------------------------
     * Scenario 2: SPINUP
     * --------------------------------------------------------------------- */
    progress("[2/8] SPINUP (already timed)...\r\n");

    rep("----------------------------------------------------------\r\n");
    rep("[2] SPINUP\r\n");
    rep("----------------------------------------------------------\r\n");
    rep("Pool creation: %llu us  (one-time; amortized to ~0 in steady state)\r\n",
        (unsigned long long)spinup_us);
    rep("\r\n");

    /* -----------------------------------------------------------------------
     * Scenario 3: DISPATCH LATENCY
     * --------------------------------------------------------------------- */
    progress("[3/8] Dispatch latency (1000 iterations)...\r\n");

    rep("----------------------------------------------------------\r\n");
    rep("[3] DISPATCH LATENCY\r\n");
    rep("----------------------------------------------------------\r\n");

    #define LAT_ITERS  1000
    g_lat_dummy = 42;
    bool lat_stalled = false;

    /* Warm-up: submit one task at a time; pool has at least 1 free slot. */
    for (int w = 0; w < 5 && !lat_stalled; w++) {
        check_interrupt();
        AxlTaskId lat_id = axl_task_pool_submit(pool, lat_task,
                                                (void *)&g_lat_dummy,
                                                NULL, NULL);
        if (lat_id != AXL_TASK_ID_INVALID) {
            uint64_t wdog = axl_time_get_us();
            while (axl_task_pool_poll(pool) == 0) {
                if (axl_time_get_us() - wdog > WATCHDOG_STALL_US) {
                    lat_stalled = true;
                    break;
                }
            }
        }
    }
    uint64_t lat_t0 = axl_time_get_us();
    for (int i = 0; i < LAT_ITERS && !lat_stalled; i++) {
        AxlTaskId lat_id = axl_task_pool_submit(pool, lat_task,
                                                (void *)&g_lat_dummy,
                                                NULL, NULL);
        if (lat_id != AXL_TASK_ID_INVALID) {
            uint64_t wdog = axl_time_get_us();
            while (axl_task_pool_poll(pool) == 0) {
                if (axl_time_get_us() - wdog > WATCHDOG_STALL_US) {
                    lat_stalled = true;
                    break;
                }
            }
        }
    }
    uint64_t lat_us = axl_time_get_us() - lat_t0;
    /* ns per iteration */
    uint64_t lat_ns = (lat_us * 1000ull) / LAT_ITERS;
    if (lat_stalled) {
        const char *msg = "WATCHDOG: dispatch-latency scenario stalled - "
                          "workers may not be live; skipping\r\n";
        progress("%s", msg);
        rep("%s", msg);
        lat_ns = 0;
    }
    rep("dispatch latency (mean over %d ops): %llu ns\r\n",
        LAT_ITERS, (unsigned long long)lat_ns);
    rep("(QEMU inflates this to 100-600 us/op; expect sub-us on real HW)\r\n");
    rep("\r\n");

    /* -----------------------------------------------------------------------
     * Calibration for compute scenarios
     * --------------------------------------------------------------------- */
    progress("[4/8] Calibrating compute workload...\r\n");

    size_t n_chunks = (size_t)(W * 4);
    if (n_chunks < 8) { n_chunks = 8; }
    if (n_chunks > MAX_CHUNKS) { n_chunks = MAX_CHUNKS; }

    for (size_t i = 0; i < n_chunks; i++) {
        g_chunks[i].in  = g_input;
        g_chunks[i].len = WORDS_PER_CHUNK;
        g_chunks[i].rounds = 1;
        g_chunks[i].out = 0;
    }

    /* Sweep candidates — adaptive runtime bounding. */
    static const uint32_t cal_sweep[] = { 1, 4, 16, 64, 256, 1024, 4096 };
    /* Target: BSP baseline per data point <= 4s. */
    uint32_t cal_rounds = calibrate_rounds(g_chunks, n_chunks,
                                           cal_sweep,
                                           sizeof(cal_sweep) / sizeof(cal_sweep[0]),
                                           4000000ull);

    /* Build a compute sweep around the calibrated max. */
    /* We want ~5 points spanning a 256x range ending at cal_rounds. */
    uint32_t comp_sweep[6];
    size_t comp_sweep_len = 0;
    {
        uint32_t r = cal_rounds;
        /* Start from 1/256th of max (or 1, whichever is larger). */
        uint32_t lo = r / 256;
        if (lo < 1) { lo = 1; }
        comp_sweep[comp_sweep_len++] = lo;
        for (int s = 1; s <= 5; s++) {
            uint32_t v = lo;
            for (int q = 0; q < s; q++) { v *= 4; }
            if (v > r) { v = r; }
            if (v != comp_sweep[comp_sweep_len - 1]) {
                comp_sweep[comp_sweep_len++] = v;
            }
            if (v >= r) { break; }
        }
        if (comp_sweep[comp_sweep_len - 1] != r) {
            comp_sweep[comp_sweep_len++] = r;
        }
    }

    axl_printf("[axbench] W=%zu workers; estimated runtime ~3-6 min (serial baselines dominate)\n", W);

    /* -----------------------------------------------------------------------
     * Scenario 4: COMPUTE break-even sweep
     * --------------------------------------------------------------------- */
    progress("[4/8] Compute break-even sweep (%zu chunks, W=%zu)...\r\n",
             n_chunks, W);

    rep("----------------------------------------------------------\r\n");
    rep("[4] COMPUTE-BOUND BREAK-EVEN  (chunks=%zu, words/chunk=%d)\r\n",
        n_chunks, WORDS_PER_CHUNK);
    rep("----------------------------------------------------------\r\n");
    rep("  rounds  bsp_us   pool_us  speedup  eff%%/W  eff%%/phys mismatch\r\n");

    uint32_t break_even_rounds = 0;
    uint64_t peak_compute_speedup_x100 = 0;

    ComputeCtx cctx = { g_chunks, n_chunks, pool };

    for (size_t k = 0; k < comp_sweep_len; k++) {
        uint32_t rounds = comp_sweep[k];
        for (size_t i = 0; i < n_chunks; i++) {
            g_chunks[i].rounds = rounds;
            g_chunks[i].out = 0;
        }

        /* Correctness gate. */
        uint64_t bsp_chk = run_bsp(g_chunks, n_chunks);
        for (size_t i = 0; i < n_chunks; i++) { g_chunks[i].out = 0; }
        uint64_t ap_chk  = run_pool(pool, g_chunks, n_chunks);
        if (ap_chk == (uint64_t)-1) {
            const char *msg = "WATCHDOG: scenario 4 stalled - only partial"
                              " tasks completed in 60s (workers may not all be"
                              " live); skipping\r\n";
            progress("%s", msg);
            rep("%s", msg);
            break;
        }
        bool mismatch = (bsp_chk != ap_chk);

        /* Timed. */
        for (size_t i = 0; i < n_chunks; i++) { g_chunks[i].out = 0; }
        uint64_t bsp_us  = median_us(bsp_thunk, &cctx);
        for (size_t i = 0; i < n_chunks; i++) { g_chunks[i].out = 0; }
        uint64_t pool_us = median_us(pool_thunk, &cctx);

        if (pool_us == STALLED_US) {
            const char *msg = "WATCHDOG: scenario 4 stalled during timed run"
                              " (workers may not all be live); skipping\r\n";
            progress("%s", msg);
            rep("%s", msg);
            break;
        }

        uint64_t sx100 = (pool_us > 0) ? (bsp_us * 100ull / pool_us) : 0;
        uint64_t eff_w  = (W > 0) ? (sx100 / (uint64_t)W) : 0;
        uint64_t eff_p  = (phys_cores > 0) ? (sx100 / (uint64_t)phys_cores) : 0;

        if (sx100 >= 100 && break_even_rounds == 0) {
            break_even_rounds = rounds;
        }
        if (sx100 > peak_compute_speedup_x100) {
            peak_compute_speedup_x100 = sx100;
        }

        rep("  %6u  %7llu  %7llu  %4llu.%02llu  %4llu%%  %5llu%%    %s\r\n",
            rounds,
            (unsigned long long)bsp_us,
            (unsigned long long)pool_us,
            (unsigned long long)(sx100 / 100),
            (unsigned long long)(sx100 % 100),
            (unsigned long long)eff_w,
            (unsigned long long)eff_p,
            mismatch ? "MISMATCH" : "ok");

        if (mismatch) {
            axl_printf("[axbench] MISMATCH at rounds=%u!\r\n", rounds);
        }
    }
    rep("\r\n");

    /* -----------------------------------------------------------------------
     * Scenario 5: GRANULARITY sweep (fixed total work)
     * --------------------------------------------------------------------- */
    progress("[5/8] Granularity sweep...\r\n");

    rep("----------------------------------------------------------\r\n");
    rep("[5] GRANULARITY SWEEP  (fixed total work, rounds=%u)\r\n",
        cal_rounds);
    rep("----------------------------------------------------------\r\n");
    rep("  chunks  bsp_us   pool_us  speedup  eff%%/W  eff%%/phys mismatch\r\n");

    /* Chunk counts: {W, W*2, W*4, W*8, W*16}, clamped to MAX_CHUNKS. */
    size_t gran_sweep[5];
    size_t gran_sweep_len = 0;
    for (int m = 1; m <= 16; m *= 2) {
        size_t nc = (size_t)W * (size_t)m;
        if (nc < 1) { nc = 1; }
        if (nc > MAX_CHUNKS) { nc = MAX_CHUNKS; }
        gran_sweep[gran_sweep_len++] = nc;
        if (nc >= MAX_CHUNKS || gran_sweep_len >= 5) { break; }
    }

    for (size_t k = 0; k < gran_sweep_len; k++) {
        size_t nc = gran_sweep[k];
        for (size_t i = 0; i < nc; i++) {
            g_chunks[i].in     = g_input;
            g_chunks[i].len    = WORDS_PER_CHUNK;
            g_chunks[i].rounds = cal_rounds;
            g_chunks[i].out    = 0;
        }
        ComputeCtx gctx = { g_chunks, nc, pool };

        /* Correctness. */
        uint64_t bsp_chk = run_bsp(g_chunks, nc);
        for (size_t i = 0; i < nc; i++) { g_chunks[i].out = 0; }
        uint64_t ap_chk  = run_pool(pool, g_chunks, nc);
        if (ap_chk == (uint64_t)-1) {
            const char *msg = "WATCHDOG: scenario 5 stalled - workers may not"
                              " all be live; skipping\r\n";
            progress("%s", msg);
            rep("%s", msg);
            break;
        }
        bool mismatch = (bsp_chk != ap_chk);

        for (size_t i = 0; i < nc; i++) { g_chunks[i].out = 0; }
        uint64_t bsp_us  = median_us(bsp_thunk, &gctx);
        for (size_t i = 0; i < nc; i++) { g_chunks[i].out = 0; }
        uint64_t pool_us = median_us(pool_thunk, &gctx);

        if (pool_us == STALLED_US) {
            const char *msg = "WATCHDOG: scenario 5 stalled during timed run"
                              " (workers may not all be live); skipping\r\n";
            progress("%s", msg);
            rep("%s", msg);
            break;
        }

        uint64_t sx100 = (pool_us > 0) ? (bsp_us * 100ull / pool_us) : 0;
        uint64_t eff_w = (W > 0) ? (sx100 / (uint64_t)W) : 0;
        uint64_t eff_p = (phys_cores > 0) ? (sx100 / (uint64_t)phys_cores) : 0;

        rep("  %6zu  %7llu  %7llu  %4llu.%02llu  %4llu%%  %5llu%%    %s\r\n",
            nc,
            (unsigned long long)bsp_us,
            (unsigned long long)pool_us,
            (unsigned long long)(sx100 / 100),
            (unsigned long long)(sx100 % 100),
            (unsigned long long)eff_w,
            (unsigned long long)eff_p,
            mismatch ? "MISMATCH" : "ok");

        if (mismatch) {
            axl_printf("[axbench] MISMATCH at chunks=%zu!\r\n", nc);
        }
    }
    rep("\r\n");

    /* -----------------------------------------------------------------------
     * Scenario 6: BANDWIDTH-BOUND blur sweep
     * --------------------------------------------------------------------- */
    progress("[6/8] Bandwidth blur sweep (calibrating image size)...\r\n");

    /* Choose image height: already computed as actual_img_h above. */
    g_img_h = actual_img_h;

    /* Initialise source image. */
    for (size_t i = 0; i < (size_t)IMG_W * g_img_h; i++) {
        g_src[i] = (uint8_t)(i * 31u + (i >> 8));
    }

    /* Tile height sweep: want tiles spanning from <W to several*W.
     * tile_h = img_h / target_tiles; vary target_tiles. */
    size_t blur_targets[] = {
        (size_t)(W / 2 > 1 ? W / 2 : 1),    /* half W tiles → small, fine-grained */
        (size_t)W,
        (size_t)W * 2,
        (size_t)W * 4,
        (size_t)W * 8,
    };
    size_t blur_ntargets = sizeof(blur_targets) / sizeof(blur_targets[0]);

    rep("----------------------------------------------------------\r\n");
    rep("[6] BANDWIDTH-BOUND BLUR  (img=%zux%zu, BLUR_R=%d)\r\n",
        (size_t)IMG_W, g_img_h, BLUR_R);
    rep("----------------------------------------------------------\r\n");
    rep("  tile_h  tiles  bsp_us   pool_us  speedup  eff%%/W  eff%%/phys mismatch\r\n");

    uint64_t peak_blur_speedup_x100 = 0;
    uint32_t best_blur_tile_h = 0;

    for (size_t k = 0; k < blur_ntargets; k++) {
        size_t target_tiles = blur_targets[k];
        if (target_tiles < 1) { target_tiles = 1; }
        uint32_t tile_h = (uint32_t)(g_img_h / target_tiles);
        if (tile_h < 1) { tile_h = 1; }

        BlurCtx bctx = { pool, tile_h };
        g_n_blur_tiles = blur_build_tiles(tile_h);
        size_t actual_tiles = g_n_blur_tiles;

        /* Correctness gate. */
        axl_memset(g_dst, 0, IMG_W * g_img_h);
        g_n_blur_tiles = blur_build_tiles(tile_h);
        uint64_t bblur = blur_run_bsp();

        axl_memset(g_dst, 0, IMG_W * g_img_h);
        g_n_blur_tiles = blur_build_tiles(tile_h);
        uint64_t pblur = blur_run_pool(pool);

        if (pblur == (uint64_t)-1) {
            const char *msg = "WATCHDOG: scenario 6 stalled - workers may not"
                              " all be live; skipping\r\n";
            progress("%s", msg);
            rep("%s", msg);
            break;
        }
        bool mismatch = (bblur != pblur);

        /* Timed. */
        uint64_t bsp_us  = median_us(blur_bsp_thunk, &bctx);
        uint64_t pool_us = median_us(blur_pool_thunk, &bctx);

        if (pool_us == STALLED_US) {
            const char *msg = "WATCHDOG: scenario 6 stalled during timed run"
                              " (workers may not all be live); skipping\r\n";
            progress("%s", msg);
            rep("%s", msg);
            break;
        }

        uint64_t sx100 = (pool_us > 0) ? (bsp_us * 100ull / pool_us) : 0;
        uint64_t eff_w = (W > 0) ? (sx100 / (uint64_t)W) : 0;
        uint64_t eff_p = (phys_cores > 0) ? (sx100 / (uint64_t)phys_cores) : 0;

        if (sx100 > peak_blur_speedup_x100) {
            peak_blur_speedup_x100 = sx100;
            best_blur_tile_h = tile_h;
        }

        rep("  %6u  %5zu  %7llu  %7llu  %4llu.%02llu  %4llu%%  %5llu%%    %s\r\n",
            tile_h, actual_tiles,
            (unsigned long long)bsp_us,
            (unsigned long long)pool_us,
            (unsigned long long)(sx100 / 100),
            (unsigned long long)(sx100 % 100),
            (unsigned long long)eff_w,
            (unsigned long long)eff_p,
            mismatch ? "MISMATCH" : "ok");

        if (mismatch) {
            axl_printf("[axbench] BLUR MISMATCH at tile_h=%u!\r\n", tile_h);
        }
    }
    rep("\r\n");

    /* -----------------------------------------------------------------------
     * Scenario 7: BSP-PARTICIPATES
     * --------------------------------------------------------------------- */
    progress("[7/8] BSP-participates comparison...\r\n");

    rep("----------------------------------------------------------\r\n");
    rep("[7] BSP-PARTICIPATES  (rounds=%u, chunks=%zu)\r\n",
        cal_rounds, n_chunks);
    rep("----------------------------------------------------------\r\n");

    for (size_t i = 0; i < n_chunks; i++) {
        g_chunks[i].in     = g_input;
        g_chunks[i].len    = WORDS_PER_CHUNK;
        g_chunks[i].rounds = cal_rounds;
        g_chunks[i].out    = 0;
    }

    /* Correctness for both variants. */
    uint64_t bsp_ref = run_bsp(g_chunks, n_chunks);
    bool s7_stalled  = false;

    for (size_t i = 0; i < n_chunks; i++) { g_chunks[i].out = 0; }
    uint64_t pool_ref = run_pool(pool, g_chunks, n_chunks);
    if (pool_ref == (uint64_t)-1) { s7_stalled = true; }
    bool pool_ok = !s7_stalled && (pool_ref == bsp_ref);

    uint64_t part_ref = bsp_ref; /* default if stalled */
    bool part_ok = false;
    if (!s7_stalled) {
        for (size_t i = 0; i < n_chunks; i++) { g_chunks[i].out = 0; }
        part_ref = run_bsp_part(pool, g_chunks, n_chunks);
        if (part_ref == (uint64_t)-1) { s7_stalled = true; }
        part_ok = !s7_stalled && (part_ref == bsp_ref);
    }

    /* Timed. */
    ComputeCtx c7 = { g_chunks, n_chunks, pool };
    for (size_t i = 0; i < n_chunks; i++) { g_chunks[i].out = 0; }
    uint64_t bsp_us7  = median_us(bsp_thunk, &c7);

    uint64_t pool_us7 = STALLED_US;
    uint64_t part_us7 = STALLED_US;

    if (!s7_stalled) {
        for (size_t i = 0; i < n_chunks; i++) { g_chunks[i].out = 0; }
        pool_us7 = median_us(pool_thunk, &c7);
        if (pool_us7 == STALLED_US) { s7_stalled = true; }
    }

    /* BSP-participates: inline timing. */
    if (!s7_stalled) {
        /* Warm-up. */
        for (int w = 0; w < WARMUP_RUNS && !s7_stalled; w++) {
            for (size_t i = 0; i < n_chunks; i++) { g_chunks[i].out = 0; }
            uint64_t r = run_bsp_part(pool, g_chunks, n_chunks);
            if (r == (uint64_t)-1) { s7_stalled = true; }
        }
        uint64_t part_times[TIMED_RUNS];
        for (int t = 0; t < TIMED_RUNS && !s7_stalled; t++) {
            for (size_t i = 0; i < n_chunks; i++) { g_chunks[i].out = 0; }
            uint64_t t0 = axl_time_get_us();
            uint64_t r  = run_bsp_part(pool, g_chunks, n_chunks);
            part_times[t] = axl_time_get_us() - t0;
            if (r == (uint64_t)-1) { s7_stalled = true; }
        }
        if (!s7_stalled) {
            axl_qsort(part_times, TIMED_RUNS, sizeof(part_times[0]), cmp_u64);
            part_us7 = part_times[TIMED_RUNS / 2];
        }
    }

    if (s7_stalled) {
        const char *msg = "WATCHDOG: scenario 7 stalled - workers may not all"
                          " be live; skipping\r\n";
        progress("%s", msg);
        rep("%s", msg);
        rep("\r\n");
    } else {
        uint64_t pool_sx100 = (pool_us7 > 0) ? (bsp_us7 * 100ull / pool_us7) : 0;
        uint64_t part_sx100 = (part_us7 > 0) ? (bsp_us7 * 100ull / part_us7) : 0;

        rep("  Mode                    bsp_us   pool_us  speedup  mismatch\r\n");
        rep("  BSP only (baseline)  %7llu       N/A      N/A      N/A\r\n",
            (unsigned long long)bsp_us7);
        rep("  AP pool (W workers)  %7llu  %7llu  %4llu.%02llu  %s\r\n",
            (unsigned long long)bsp_us7, (unsigned long long)pool_us7,
            (unsigned long long)(pool_sx100 / 100),
            (unsigned long long)(pool_sx100 % 100),
            pool_ok ? "ok" : "MISMATCH");
        rep("  AP + BSP (W+1 eff.)  %7llu  %7llu  %4llu.%02llu  %s\r\n",
            (unsigned long long)bsp_us7, (unsigned long long)part_us7,
            (unsigned long long)(part_sx100 / 100),
            (unsigned long long)(part_sx100 % 100),
            part_ok ? "ok" : "MISMATCH");

        bool bsp_helps = (part_us7 < pool_us7);
        rep("  BSP-participation %s (delta %+lld us)\r\n",
            bsp_helps ? "HELPS" : "does NOT help",
            (long long)pool_us7 - (long long)part_us7);
        rep("\r\n");
    }

    /* -----------------------------------------------------------------------
     * Scenario 8: SUMMARY
     * --------------------------------------------------------------------- */
    progress("[8/8] Writing summary...\r\n");

    rep("==========================================================\r\n");
    rep("[8] SUMMARY\r\n");
    rep("==========================================================\r\n");
    rep("Topology:\r\n");
    rep("  %zu logical / %zu enabled / %zu sockets / %zu phys cores / %zu SMT\r\n",
        g_total_procs, g_enabled_procs, sockets, phys_cores, smt_depth);
    rep("  AP workers (W): %zu\r\n", W);
    rep("\r\n");
    rep("Dispatch latency:   %llu ns/op  (QEMU pessimistic; real HW target: sub-us)\r\n",
        (unsigned long long)lat_ns);
    rep("Pool spin-up:       %llu us  (one-time)\r\n",
        (unsigned long long)spinup_us);
    rep("\r\n");
    rep("Compute-bound:\r\n");
    if (break_even_rounds > 0) {
        rep("  Break-even rounds: %u\r\n", break_even_rounds);
    } else {
        rep("  Break-even: not reached (increase rounds or core count)\r\n");
    }
    rep("  Peak speedup:      %llu.%02llux  (eff/W=%llu%%  eff/phys=%llu%%)\r\n",
        (unsigned long long)(peak_compute_speedup_x100 / 100),
        (unsigned long long)(peak_compute_speedup_x100 % 100),
        (W > 0) ? (unsigned long long)(peak_compute_speedup_x100 / (uint64_t)W) : 0,
        (phys_cores > 0) ? (unsigned long long)(peak_compute_speedup_x100 / (uint64_t)phys_cores) : 0);
    rep("\r\n");
    rep("Bandwidth-bound (NUMA sensitivity):\r\n");
    rep("  Best tile_h: %u  Peak speedup: %llu.%02llux  (eff/W=%llu%%  eff/phys=%llu%%)\r\n",
        best_blur_tile_h,
        (unsigned long long)(peak_blur_speedup_x100 / 100),
        (unsigned long long)(peak_blur_speedup_x100 % 100),
        (W > 0) ? (unsigned long long)(peak_blur_speedup_x100 / (uint64_t)W) : 0,
        (phys_cores > 0) ? (unsigned long long)(peak_blur_speedup_x100 / (uint64_t)phys_cores) : 0);
    rep("  (Bandwidth-limited speedup vs phys_cores reveals NUMA ceiling;\r\n");
    rep("   compute-bound efficiency vs phys_cores reveals SMT headroom.)\r\n");
    rep("\r\n");
    rep("BSP-participation:  %s\r\n",
        s7_stalled ? "STALLED (no data)" :
        (part_us7 < pool_us7) ? "BSP computing reduces latency (use W+1 model)" :
                                 "BSP orchestration is better (pure AP model)");
    rep("\r\n");
    rep("Recommendation:\r\n");
    if (peak_compute_speedup_x100 >= 100ull * (uint64_t)W / 2) {
        rep("  Compute-bound workloads benefit strongly from AP offload.\r\n");
    } else {
        rep("  Compute-bound gains are modest; dispatch overhead dominates.\r\n");
    }
    if (peak_blur_speedup_x100 < peak_compute_speedup_x100 / 2) {
        rep("  Bandwidth-bound (blur) speedup is significantly lower than\r\n");
        rep("  compute - cross-socket NUMA bandwidth contention is likely.\r\n");
    } else {
        rep("  Blur and compute speedups are comparable - memory bandwidth\r\n");
        rep("  is not the primary bottleneck at this working-set size.\r\n");
    }
    rep("==========================================================\r\n");

    /* --- Finalize --- */
    int report_rc = 0;
    if (g_writer) {
        int rc = axl_file_writer_close(g_writer);
        if (rc != AXL_OK) {
            axl_printf("[axbench] WARNING: file close/flush error (rc=%d)\r\n", rc);
            /* The close is where the report becomes durable. A green exit
               status here would tell a script the report is on disk when
               it is not. */
            report_rc = 1;
        } else {
            axl_printf("[axbench] Report written to: %s\r\n", outfile);
        }
        g_writer = NULL;   /* closed here for the message; don't double-close */
    }

    cleanup_atexit(NULL);   /* frees pool + buffers (idempotent w/ atexit) */
    progress("[axbench] Done.\r\n");
    return report_rc;
}
