/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * console-device-smoke.c — DEBUG-OVMF smoke for axl-console-device (output-only).
 *
 * A resident `load`-ed consumer driver that installs the take-over device and
 * binds its AxlConsoleOps straight into a crude GOP cell grid. If the take-over
 * works, the Shell (already at the prompt when we load) fans its console output
 * through our device -> the emit engine -> these ops -> our grid, which we blit to
 * the framebuffer. This is the productized-device counterpart of the proven cv2
 * spike: cv2 fed its grid from its OWN SimpleTextOut; here the grid is fed by the
 * DEVICE's ops, exercising the real axl_console_device_install path.
 *
 * Verification ritual (from cv2, see AXL-Console-Device-Design.md):
 *  - our 80x25 grid occupies x in [0,640), y in [0,400); the region at x>640 is a
 *    clean-surface test. After a one-time full-framebuffer wipe, if eviction worked
 *    GraphicsConsole is silent and x>640 stays PURE BLACK through both connect
 *    storms (load's ConnectAllEfi + our ConnectController). If eviction FAILED,
 *    GraphicsConsole co-paints the shell there.
 *  - status bars (x<640, below the grid) report liveness: install OK, and "ops
 *    received" (green once the shell's output reaches our grid through the device).
 *  - Instrument ONLY with axl_gfx_fill_rect GOP bars, NEVER axl_printf: a
 *    taken-over console swallows printf.
 *
 * Run (release OVMF has no asserts; DEBUG OVMF is the real gate):
 *   OVMF_CODE=$HOME/uefi/Build/OvmfX64/DEBUG_GCC5/FV/OVMF_CODE.fd \
 *   OVMF_VARS=$HOME/uefi/Build/OvmfX64/DEBUG_GCC5/FV/OVMF_VARS.fd \
 *   SHOT_WAIT=18 timeout 120 scripts/run-qemu.sh --arch X64 \
 *     --screenshot /tmp/condev.png --debugcon /tmp/condev.dbg \
 *     out/native-x64/drivers/console-device-smoke.efi
 *
 * Restore variant: compiling this SAME file with -DSELF_UNINSTALL_MS=N builds a
 * driver that takes over (wiping the frame pure black and rendering NOTHING),
 * then after N ms of REAL time uninstalls the device (re-tag + reconnect the
 * firmware GraphicsConsole) and immediately drives the restored gST->ConOut
 * directly with a block of text. If the restore reconnected the firmware console
 * into the fan-out, those lines paint onto the black frame -- a deterministic
 * proof the local display came back, with no dependence on keystroke timing.
 * (A broken restore wedges on the first OutputString: this is exactly the
 * use-after-free the smoke caught, where uninstall freed our SimpleTextOut while
 * ConSplitter still held it -- fixed by DisconnectController-before-free.) This
 * is the console-device-restore-smoke.efi target; see test-console-device-qemu.sh.
 *
 * Wide variant: -DGRID_COLS/-DGRID_ROWS (default 80x25) + -DAUTO_ALT=true builds a
 * NON-80x25 take-over console (console-device-wide-smoke, 142x44) to exercise a
 * full-screen geometry against the shell's ConsoleLogger (Scenario 3 drives repeated
 * `dh` to scroll and checks for no assert + content past x=640 = the wide geometry is
 * actually active). Matches AGT axcon's config (auto_alt_screen=true). The wide
 * target also defines -DPRECACHE_SMALL (see the block in smoke_entry) to make the
 * ConsoleLogger stale-RowsPerScreen bug fire deterministically rather than ~1-in-6.
 */

#include <axl.h>
#include <axl/axl-console-device.h>
#include <axl/axl-console-ops.h>
#include <axl/axl-driver.h>
#include <axl/axl-font.h>
#include <axl/axl-gfx.h>
#include <axl/axl-loop.h>
#ifdef SELF_UNINSTALL_MS
#include <axl/axl-time.h>
#endif
#ifdef PASSTHROUGH_LOCAL
#include <axl/axl-time.h>
#endif
#if defined(SELF_UNINSTALL_MS) || defined(PRECACHE_SMALL) || defined(PASSTHROUGH_LOCAL)
#include <uefi/axl-uefi.h>
#endif

/* Advertised (and grid) geometry + alt-screen mode. Overridable via -D so a
   wide-mode variant can exercise a non-80x25 take-over console matching axcon
   (which installs at auto_alt_screen=true). See console-device-wide-smoke. */
#ifndef GRID_COLS
#define GRID_COLS   80
#endif
#ifndef GRID_ROWS
#define GRID_ROWS   25
#endif
#ifndef AUTO_ALT
#define AUTO_ALT    false
#endif
#define BLIT_MS     40
#define DEFAULT_FG  7          /* EFI_LIGHTGRAY */
#define DEFAULT_BG  0          /* EFI_BLACK     */
#define CELL_MAX    4

typedef struct {
    char    utf8[CELL_MAX];
    uint8_t len;
    uint8_t fg;
    uint8_t bg;
} Cell;

/* The UEFI 16-colour console palette, in GOP order (same as cv2). */
static const AxlGfxPixel PALETTE[16] = {
    AXL_GFX_RGB(0x00, 0x00, 0x00), AXL_GFX_RGB(0x00, 0x00, 0xA8),
    AXL_GFX_RGB(0x00, 0xA8, 0x00), AXL_GFX_RGB(0x00, 0xA8, 0xA8),
    AXL_GFX_RGB(0xA8, 0x00, 0x00), AXL_GFX_RGB(0xA8, 0x00, 0xA8),
    AXL_GFX_RGB(0xA8, 0x54, 0x00), AXL_GFX_RGB(0xA8, 0xA8, 0xA8),
    AXL_GFX_RGB(0x54, 0x54, 0x54), AXL_GFX_RGB(0x54, 0x54, 0xFF),
    AXL_GFX_RGB(0x54, 0xFF, 0x54), AXL_GFX_RGB(0x54, 0xFF, 0xFF),
    AXL_GFX_RGB(0xFF, 0x54, 0x54), AXL_GFX_RGB(0xFF, 0x54, 0xFF),
    AXL_GFX_RGB(0xFF, 0xFF, 0x54), AXL_GFX_RGB(0xFF, 0xFF, 0xFF),
};

static Cell     grid[GRID_ROWS][GRID_COLS];
static int32_t  cur_row, cur_col;
static uint8_t  pen_fg = DEFAULT_FG;
static uint8_t  pen_bg = DEFAULT_BG;
static bool     cursor_visible = true;
static bool     dirty = true;
static bool     screen_cleared;

static AxlConsoleDevice *g_device;
static AxlLoop          *g_loop;

/* Instrumentation for the GOP status bars. */
static bool     s_installed;   /* axl_console_device_install returned AXL_OK  */
static bool     s_got_ops;     /* at least one output_text reached the grid   */

#if defined(SELF_UNINSTALL_MS) || defined(PASSTHROUGH_LOCAL)
static uint64_t s_takeover_ms; /* monotonic ms at take-over; restore/co-paint base */
#endif
#ifdef PASSTHROUGH_LOCAL
static bool     s_copaint_written;  /* the one-shot wide-line probe has been emitted */
#endif
#ifdef CYCLE_COUNT
static int      s_cycle;       /* completed take-over/restore cycles (regression) */
#endif

// ---------------------------------------------------------------------------
// Grid model (same shape as cv2)
// ---------------------------------------------------------------------------

static void
cell_blank(Cell *c)
{
    c->len = 0;
    c->fg  = pen_fg;
    c->bg  = pen_bg;
}

static void
grid_clear(void)
{
    for (int32_t r = 0; r < GRID_ROWS; r++) {
        for (int32_t c = 0; c < GRID_COLS; c++) {
            cell_blank(&grid[r][c]);
        }
    }
    cur_row = 0;
    cur_col = 0;
    dirty = true;
}

static void
grid_scroll(void)
{
    for (int32_t r = 0; r + 1 < GRID_ROWS; r++) {
        for (int32_t c = 0; c < GRID_COLS; c++) {
            grid[r][c] = grid[r + 1][c];
        }
    }
    for (int32_t c = 0; c < GRID_COLS; c++) {
        cell_blank(&grid[GRID_ROWS - 1][c]);
    }
}

static void
grid_newline(void)
{
    cur_row++;
    if (cur_row >= GRID_ROWS) {
        grid_scroll();
        cur_row = GRID_ROWS - 1;
    }
}

static void
grid_put_cp(uint32_t cp)
{
    if (cp == '\n') { grid_newline(); if (cur_col >= GRID_COLS) cur_col = GRID_COLS - 1; return; }
    if (cp == '\r') { cur_col = 0; return; }
    if (cp == '\b') { if (cur_col > 0) cur_col--; return; }
    if (cp == '\t') {
        cur_col = (cur_col + 8) & ~7;
        if (cur_col >= GRID_COLS) { cur_col = 0; grid_newline(); }
        return;
    }
    if (cp < 0x20) {
        return;
    }
    if (cur_col >= GRID_COLS) { cur_col = 0; grid_newline(); }

    Cell *cell = &grid[cur_row][cur_col];
    if (cp < 0x80) {
        cell->utf8[0] = (char)cp;
        cell->len = 1;
    } else if (cp < 0x800) {
        cell->utf8[0] = (char)(0xC0 | (cp >> 6));
        cell->utf8[1] = (char)(0x80 | (cp & 0x3F));
        cell->len = 2;
    } else {
        cell->utf8[0] = (char)(0xE0 | (cp >> 12));
        cell->utf8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        cell->utf8[2] = (char)(0x80 | (cp & 0x3F));
        cell->len = 3;
    }
    cell->fg = pen_fg;
    cell->bg = pen_bg;
    cur_col++;
    dirty = true;
}

// ---------------------------------------------------------------------------
// AxlConsoleOps -> grid. These are the whole point: the device delivers the
// shell's console output as these structured ops.
// ---------------------------------------------------------------------------

static void
ops_output_text(void *user, const char *utf8, size_t len)
{
    (void)user;
    s_got_ops = true;
    /* Minimal UTF-8 decode (BMP): the run is the device's sanitized text. */
    for (size_t i = 0; i < len; ) {
        unsigned char b = (unsigned char)utf8[i];
        uint32_t cp;
        if (b < 0x80) { cp = b; i += 1; }
        else if ((b & 0xE0) == 0xC0 && i + 1 < len) {
            cp = ((uint32_t)(b & 0x1F) << 6) | (uint32_t)(utf8[i + 1] & 0x3F); i += 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < len) {
            cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(utf8[i + 1] & 0x3F) << 6)
               | (uint32_t)(utf8[i + 2] & 0x3F); i += 3;
        } else { i += 1; continue; }
        grid_put_cp(cp);
    }
    dirty = true;
}

static void
ops_set_cursor(void *user, int32_t row, int32_t col)
{
    (void)user;
    cur_row = (row < 0) ? 0 : (row >= GRID_ROWS ? GRID_ROWS - 1 : row);
    cur_col = (col < 0) ? 0 : (col >= GRID_COLS ? GRID_COLS - 1 : col);
    dirty = true;
}

static void
ops_set_pen(void *user, const AxlConsolePen *pen)
{
    (void)user;
    /* The device only ever produces INDEXED (UEFI nibble) fg/bg. */
    if (pen->fg.kind == AXL_CONSOLE_COLOR_INDEXED) pen_fg = pen->fg.idx & 0x0F;
    if (pen->bg.kind == AXL_CONSOLE_COLOR_INDEXED) pen_bg = pen->bg.idx & 0x07;
}

static void
ops_clear_screen(void *user)
{
    (void)user;
    grid_clear();
}

static int
ops_set_term_prop(void *user, AxlConsoleProp prop, const AxlConsoleValue *val)
{
    (void)user;
    if (prop == AXL_CONSOLE_PROP_CURSOR_VISIBLE && val->kind == AXL_CONSOLE_VALUE_BOOL) {
        cursor_visible = val->u.boolean;
        dirty = true;
    }
    return 1;   /* accept everything, including what we ignore */
}

static const AxlConsoleOps GRID_OPS = {
    .output_text   = ops_output_text,
    .set_cursor    = ops_set_cursor,
    .set_pen       = ops_set_pen,
    .clear_screen  = ops_clear_screen,
    .set_term_prop = ops_set_term_prop,
};

// ---------------------------------------------------------------------------
// Blit + instrumentation (same discriminator as cv2). The restore variant
// renders nothing, so its blit path uses none of this (guarded out to keep the
// build warning-clean).
// ---------------------------------------------------------------------------

#ifndef SELF_UNINSTALL_MS
static void
status_bar(uint32_t slot, AxlGfxPixel color)
{
    AxlGfxInfo info;
    if (axl_gfx_get_info(&info) != AXL_OK) {
        return;
    }
    uint32_t bar_h = 12;
    uint32_t y     = info.height - (slot + 1) * (bar_h + 2);
    axl_gfx_fill_rect(0, y, 60, bar_h, color);
}

static void
draw_cursor(const AxlFont *font)
{
    if (!cursor_visible
        || cur_col < 0 || cur_col >= GRID_COLS || cur_row < 0 || cur_row >= GRID_ROWS) {
        return;
    }
    uint32_t cw = font->cell_width;
    uint32_t ch = font->cell_height;
    axl_gfx_fill_rect((uint32_t)cur_col * cw, (uint32_t)cur_row * ch + ch - 2,
                      cw, 2, PALETTE[pen_fg]);
}
#endif   /* !SELF_UNINSTALL_MS */

/* Install the take-over device with the compiled-in geometry/input config. Shared
   by smoke_entry and the CYCLE_COUNT re-install path so both take over identically. */
static int
take_over_device(void)
{
    AxlConsoleDeviceConfig cfg = {
#ifdef PASSTHROUGH_LOCAL
        /* Co-paint: we OBSERVE the console instead of owning it, so the firmware
           GraphicsConsole stays in the fan-out and the local display keeps working.
           Geometry MUST be 0 (physical) — install rejects an explicit size here,
           because two consoles painting one screen have to agree on the grid. */
        .cols = 0, .rows = 0, .passthrough_local = true,
#else
        .cols = GRID_COLS, .rows = GRID_ROWS,
#endif
        .auto_alt_screen = AUTO_ALT,
#ifdef TAKE_INPUT
        .take_input = true, .read_physical = true,
#else
        .take_input = false,
#endif
    };
    return axl_console_device_install(&GRID_OPS, NULL, &cfg, &g_device);
}

static bool
blit_cb(void *data)
{
    (void)data;

    /* One-time full-framebuffer wipe: erase the pre-load shell text the firmware
       GraphicsConsole painted before our surgery, so x>640 becomes an honest test
       surface. After it, anything at x>640 is a FRESH firmware paint; if eviction
       worked it stays black forever. */
    if (!screen_cleared) {
        AxlGfxInfo info;
        if (axl_gfx_get_info(&info) == AXL_OK) {
            axl_gfx_fill_rect(0, 0, info.width, info.height, PALETTE[0]);
            /* Only latch once the wipe actually ran: if get_info transiently
               fails, retry next blit rather than leave stale pre-boot content on
               the frame -- which would let the restore check pass on ghosts. */
            screen_cleared = true;
            dirty = true;
        }
    }

#if defined(CYCLE_COUNT)
    /* Cycle variant: take over -> restore -> re-take-over, CYCLE_COUNT times, in one
       boot. Each uninstall re-adds the firmware console; each re-install re-evicts
       the JUST-restored GraphicsConsole. Catches cumulative-state bugs a single
       teardown misses (a second uninstall, re-eviction, ConSplitter mode assert on
       a re-add). After the last cycle the firmware console is left restored and we
       paint the RESTORED banner so --restored proves the final state is live. */
    if (g_device != NULL && axl_time_get_ms() - s_takeover_ms >= SELF_UNINSTALL_MS) {
        axl_console_device_uninstall(g_device);
        g_device = NULL;
        s_cycle++;
        if (s_cycle >= CYCLE_COUNT) {
            if (gST != NULL && gST->ConOut != NULL) {
                for (int i = 0; i < 24; i++) {
                    gST->ConOut->OutputString(gST->ConOut,
                        (CHAR16 *)L"axl-console-device: firmware console RESTORED after uninstall\r\n");
                }
            }
            return AXL_SOURCE_REMOVE;
        }
        /* Prove the restore is live between cycles too, then re-take-over. */
        if (gST != NULL && gST->ConOut != NULL) {
            gST->ConOut->OutputString(gST->ConOut,
                (CHAR16 *)L"axl-console-device: cycle restore OK\r\n");
        }
        if (take_over_device() != AXL_OK) {
            return AXL_SOURCE_REMOVE;   /* re-install failed: stop (visible via missing banner) */
        }
        s_takeover_ms = axl_time_get_ms();
    }
    return AXL_SOURCE_CONTINUE;
#elif defined(SELF_UNINSTALL_MS)
    /* Restore-path variant renders NOTHING (the frame stays the pure black of the
       one-time wipe above); uninstall once SELF_UNINSTALL_MS of REAL time has
       elapsed since take-over. Crucially this reads axl_time_get_ms() (a monotonic
       wall-clock counter) rather than counting blit ticks or leaning on a loop
       timer: under DEBUG OVMF the blit loop pumps far slower than BLIT_MS, and any
       pump-derived clock overshoots the real delay by 3-4x -- which fired the
       uninstall AFTER the `ver` keystroke, so nothing repainted. The restored
       console painting `ver` is the whole signal. */
    if (g_device != NULL && axl_time_get_ms() - s_takeover_ms >= SELF_UNINSTALL_MS) {
        axl_console_device_uninstall(g_device);   /* re-tag + reconnect + disconnect self */
        g_device = NULL;
        /* Prove the restore deterministically: drive the restored gST->ConOut
           DIRECTLY (bypass the shell + streams). If uninstall reconnected the
           firmware console into the fan-out, these lines paint onto the frame we
           wiped black; a broken restore would wedge on the first OutputString
           (the exact UAF this smoke caught before the disconnect fix). */
        if (gST != NULL && gST->ConOut != NULL) {
            for (int i = 0; i < 24; i++) {
                gST->ConOut->OutputString(gST->ConOut,
                    (CHAR16 *)L"axl-console-device: firmware console RESTORED after uninstall\r\n");
            }
        }
        return AXL_SOURCE_REMOVE;   /* stop blitting; the restored console owns the frame */
    }
    return AXL_SOURCE_CONTINUE;
#else
#ifdef PASSTHROUGH_LOCAL
    /* Co-paint probe, emitted once a couple of seconds after take-over (past both
       connect storms). Drive gST->ConOut DIRECTLY with lines WIDER than our 80-col
       grid: ConSplitter fans them to us AND — if passthrough left it in the fan-out —
       to the firmware GraphicsConsole. Our grid clamps at x<640; GraphicsConsole
       paints the full width, so ink beyond x=660 can ONLY have come from it. That
       makes the analyzer's clean-region check invert cleanly: black there means the
       local console died (an eviction we did not ask for), ink means it is alive.
       Deliberately not keystroke-driven — `ver` output is far too narrow to reach
       x=660, so it could not tell co-painting from silence. */
    if (!s_copaint_written && s_installed
        && axl_time_get_ms() - s_takeover_ms >= 2000) {
        s_copaint_written = true;
        if (gST != NULL && gST->ConOut != NULL) {
            for (int i = 0; i < 12; i++) {
                gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)
                    L"axl-console-device PASSTHROUGH co-paint probe: this line is "
                    L"deliberately wider than the 80-column grid so the firmware "
                    L"console paints past it\r\n");
            }
        }
    }
#endif
    /* Status bars kept fresh even when the grid is idle. */
    status_bar(0, s_installed ? AXL_GFX_GREEN : AXL_GFX_RED);
    status_bar(1, s_got_ops   ? AXL_GFX_GREEN : AXL_GFX_RED);

    if (!dirty) {
        return AXL_SOURCE_CONTINUE;
    }
    dirty = false;

    const AxlFont *font = axl_gfx_default_font();
    uint32_t       cw = font->cell_width;
    uint32_t       ch = font->cell_height;
    char           buf[CELL_MAX + 1];

    for (int32_t r = 0; r < GRID_ROWS; r++) {
        for (int32_t c = 0; c < GRID_COLS; c++) {
            const Cell *cell = &grid[r][c];
            uint32_t x = (uint32_t)c * cw;
            uint32_t y = (uint32_t)r * ch;
            axl_gfx_fill_rect(x, y, cw, ch, PALETTE[cell->bg]);
            if (cell->len == 0) {
                continue;
            }
            for (uint8_t k = 0; k < cell->len; k++) {
                buf[k] = cell->utf8[k];
            }
            buf[cell->len] = '\0';
            axl_gfx_draw_text(font, x, y, buf, PALETTE[cell->fg], 1);
        }
    }
    draw_cursor(font);
    return AXL_SOURCE_CONTINUE;
#endif   /* !SELF_UNINSTALL_MS */
}

// ---------------------------------------------------------------------------
// Driver lifecycle
// ---------------------------------------------------------------------------

static int
smoke_entry(AxlHandle image, AxlSystemTable *st)
{
    (void)image;
    (void)st;

    if (!axl_gfx_available()) {
        return 1;
    }
    grid_clear();

#ifdef PRECACHE_SMALL
    /* Deterministic RED lever for the ConsoleLogger stale-RowsPerScreen bug: force
       the shell's ConsoleLogger to cache the SMALL (mode 0 = 80x25) geometry right
       before we take over at a taller grid. Take-over then changes the console
       geometry without (absent the fix) routing a SetMode through gST->ConOut, so
       ConsoleLogger's cached RowsPerScreen stays stale-small vs our grid and its
       history bound (RowsPerScreen*ScreenCount-1) overflows once the shell scrolls
       -> ShellPkg/ConsoleLogger.c:489 ASSERT / CpuDeadLoop under DEBUG OVMF. The
       SetMode-round-trip in axl_console_device_install re-caches it at our geometry
       and closes the window (GREEN). */
    if (gST != NULL && gST->ConOut != NULL && gST->ConOut->SetMode != NULL) {
        gST->ConOut->SetMode(gST->ConOut, 0);
    }
#endif

    /* THE call under test: become the sole console of the shell already at the
       prompt and deliver its output as GRID_OPS. With -DTAKE_INPUT we ALSO become
       the sole ConInEx (evicting the raw keyboard) and run the internal read loop,
       so the ONLY path from a --sendkey keystroke to the shell is our relay: if the
       grid then shows the typed command's output, input ownership + delivery work
       (and there is no double-delivery, since the physical keyboard is evicted). */
    if (take_over_device() != AXL_OK) {
        return 1;   /* status bar 0 stays red -> visible failure */
    }
    s_installed = true;
#if defined(SELF_UNINSTALL_MS) || defined(PASSTHROUGH_LOCAL)
    /* Restore trigger / co-paint probe both measure REAL time from here (a
       pump-derived clock overshoots badly under DEBUG OVMF — see the restore
       variant's note). */
    s_takeover_ms = axl_time_get_ms();
#endif

    g_loop = axl_loop_new();
    if (g_loop == NULL) {
        return 1;
    }
    if (axl_loop_add_timer(g_loop, BLIT_MS, blit_cb, NULL) == 0
        || axl_loop_attach_driver(g_loop, BLIT_MS) != AXL_OK) {
        axl_loop_free(g_loop);
        g_loop = NULL;
        return 1;
    }
    return 0;   /* resident */
}

static int
smoke_unload(AxlHandle image)
{
    (void)image;
    if (g_device != NULL) {
        axl_console_device_uninstall(g_device);   /* re-tags + reconnects the firmware console */
        g_device = NULL;
    }
    if (g_loop != NULL) {
        axl_loop_detach_driver(g_loop);
        axl_loop_free(g_loop);
        g_loop = NULL;
    }
    return 0;
}

AXL_DRIVER(smoke_entry, smoke_unload)
