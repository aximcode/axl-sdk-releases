/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * console-reshape-selftest.c — a passthrough take-over device must not hide the
 * physical console's text modes, and must reshape through them.
 *
 * A co-painting (`passthrough_local`) device is a READER: it does not evict the
 * firmware console, so it can honour every mode that console can. Before the
 * mirroring change it advertised exactly one mode, and ConSplitter intersects
 * modes across its members by (Columns, Rows) — so the aggregate collapsed to
 * that single geometry and `axl_console_text_set_mode` had nothing to switch to:
 *
 *   RED    mode_count 3 -> 1 while installed; no reshape possible
 *   GREEN  mode_count 3 -> 3; set_mode reshapes BOTH consoles, and the device
 *          reports the new geometry to its consumer
 *
 * Runs as an app, which works precisely BECAUSE passthrough does not evict: the
 * firmware console stays in the fan-out, so these very assertions still reach
 * the serial log while the device is installed.
 *
 * Only safe negatives are asserted (see the firmware-lifecycle hazard notes in
 * docs/AXL-Coding-Style.md): one clean install/uninstall pair, no double
 * uninstall, no reuse of a freed device.
 *
 * Emits "RESHAPE: <N> passed, <M> failed" as the final line.
 */

#include <axl.h>

static int g_pass = 0;
static int g_fail = 0;

static void
check(
    bool         cond,
    const char  *label
    )
{
    if (cond) {
        g_pass++;
        axl_printf("PASS: %s\n", label);
    } else {
        g_fail++;
        axl_printf("FAIL: %s\n", label);
    }
}

static void
check_u32(
    uint32_t     got,
    uint32_t     want,
    const char  *label
    )
{
    check(got == want, label);
    if (got != want) {
        axl_printf("       want=%u got=%u\n", want, got);
    }
}

/* ---- consumer side: record the resize notifications ---------------------- */

typedef struct {
    uint32_t  count;       ///< how many resize ops arrived
    uint32_t  cols;        ///< geometry reported by the most recent one
    uint32_t  rows;
    uint32_t  clears;      ///< how many clear_screen ops arrived
} ResizeLog;

static ResizeLog g_log;

static void
on_resize(
    void      *user,
    uint32_t   cols,
    uint32_t   rows
    )
{
    ResizeLog *log = (ResizeLog *)user;
    log->count++;
    log->cols = cols;
    log->rows = rows;
}

static void
on_clear_screen(
    void  *user
    )
{
    ((ResizeLog *)user)->clears++;
}

/* Deliberately sparse: this test is about geometry, not painting. The emitter
   NULL-checks every op, and erase/scrollrect are left unbound together (binding
   scrollrect alone would break the decomposition fallback). */
static const AxlConsoleOps g_ops = {
    .resize       = on_resize,
    .clear_screen = on_clear_screen,
};

/* The whole mode table as the console reports it, so a before/after comparison
   pins index-for-index mirroring rather than just the count. */
typedef struct {
    bool      usable;
    uint32_t  cols;
    uint32_t  rows;
} ModeSnap;

#define MAX_SNAP 32

static uint32_t
snap_modes(
    ModeSnap  *out
    )
{
    uint32_t count = axl_console_text_mode_count();
    if (count > MAX_SNAP) {
        count = MAX_SNAP;
    }
    for (uint32_t i = 0; i < count; i++) {
        AxlConsoleTextMode m;
        if (axl_console_text_query_mode(i, &m) == AXL_OK) {
            out[i].usable = true;
            out[i].cols   = m.columns;
            out[i].rows   = m.rows;
        } else {
            out[i].usable = false;
            out[i].cols   = 0;
            out[i].rows   = 0;
        }
    }
    return count;
}

/* Find a usable mode that is NOT @p avoid. Modes can have holes: OVMF reports
   mode 1 as unsupported (EDK2 zeroes TextOutQueryData[1] when 80x50 is absent),
   so a queryable-mode scan is the only safe walk.

   @return true when one was found. */
static bool
find_other_mode(
    uint32_t   avoid,
    uint32_t  *out_index,
    uint32_t  *out_cols,
    uint32_t  *out_rows
    )
{
    uint32_t count = axl_console_text_mode_count();
    for (uint32_t i = 0; i < count; i++) {
        AxlConsoleTextMode m;
        if (i == avoid || axl_console_text_query_mode(i, &m) != AXL_OK) {
            continue;
        }
        *out_index = i;
        *out_cols  = m.columns;
        *out_rows  = m.rows;
        return true;
    }
    return false;
}

int
main(
    int    argc,
    char  *argv[]
    )
{
    (void)argc;
    (void)argv;

    /* ---- 1. baseline, before the device exists ---------------------- */
    uint32_t pre_count   = axl_console_text_mode_count();
    uint32_t pre_current = 0;
    bool     have_current =
        (axl_console_text_current_mode(&pre_current) == AXL_OK);

    axl_printf("RESHAPE: pre_count=%u pre_current=%u\n",
               pre_count, have_current ? pre_current : 0u);

    if (pre_count < 2 || !have_current) {
        /* Nothing to switch to — the firmware enumerates a single mode. Not a
           failure of the code under test, but this run proves nothing, so say
           so loudly rather than reporting a hollow pass. */
        axl_printf("RESHAPE: SKIP - need >= 2 enumerated modes to reshape\n");
        axl_printf("RESHAPE: %d passed, %d failed\n", g_pass, g_fail);
        return 0;
    }

    AxlConsoleTextMode pre_mode;
    check(axl_console_text_query_mode(pre_current, &pre_mode) == AXL_OK,
          "baseline: current mode is queryable");

    ModeSnap pre_table[MAX_SNAP];
    uint32_t pre_snapped = snap_modes(pre_table);

    /* ---- 2. install the co-painting device -------------------------- */
    AxlConsoleDevice       *dev = NULL;
    AxlConsoleDeviceConfig  cfg = {
        .cols = 0, .rows = 0,        /* passthrough REQUIRES physical geometry */
        .passthrough_local = true,
    };
    if (axl_console_device_install(&g_ops, &g_log, &cfg, &dev) != AXL_OK) {
        axl_printf("FAIL: install\n");
        axl_printf("RESHAPE: %d passed, %d failed\n", g_pass, g_fail + 1);
        return 1;
    }
    /* ---- 3. the mode list survives the install ---------------------- */
    /* THE regression this test exists for: a single-mode device collapses
       ConSplitter's intersection and the physical modes become unreachable. */
    check_u32(axl_console_text_mode_count(), pre_count,
              "installed device does not hide the physical mode list");

    /* The count alone is a weak guard -- it would still pass if the geometries
       shifted or a hole turned queryable. Compare the whole table, index for
       index: that IS the mirroring contract. */
    ModeSnap post_table[MAX_SNAP];
    uint32_t post_snapped = snap_modes(post_table);
    bool     table_same   = (post_snapped == pre_snapped);
    for (uint32_t i = 0; table_same && i < pre_snapped; i++) {
        if (pre_table[i].usable != post_table[i].usable
            || pre_table[i].cols != post_table[i].cols
            || pre_table[i].rows != post_table[i].rows)
        {
            table_same = false;
            axl_printf("       mode %u: pre usable=%d %ux%u  post usable=%d %ux%u\n",
                       i, (int)pre_table[i].usable, pre_table[i].cols,
                       pre_table[i].rows, (int)post_table[i].usable,
                       post_table[i].cols, post_table[i].rows);
        }
    }
    check(table_same,
          "every mode keeps its index, geometry and queryability while installed");

    /* The op contract says install does not deliver a resize -- the firmware may
       re-mode the console while ConSplitter binds us, and a handler must not be
       reached before its caller holds the device. */
    /* NOTE: whether the firmware re-modes us during ConSplitter's AddDevice is
       platform-dependent (ConsplitterSetConsoleOutMode SetMode()s PcdConOutColumn/
       Row's preferred mode; ArmVirtQemuKernel sets both to 0 = "highest"). On a
       firmware already in its preferred mode this passes without exercising the
       suppression at all -- it guards the contract, it does not prove it here. */
    check_u32(g_log.count, 0, "install delivers no resize op");
    uint32_t cur = 0;
    check(axl_console_text_current_mode(&cur) == AXL_OK && cur == pre_current,
          "install does not change the current mode");

    uint32_t cols = 0;
    uint32_t rows = 0;
    axl_console_device_get_size(dev, &cols, &rows);
    check(cols == pre_mode.columns && rows == pre_mode.rows,
          "advertised geometry is the physical console's");

    /* ---- 4. reshape to another enumerated mode ---------------------- */
    uint32_t tgt_index = 0;
    uint32_t tgt_cols  = 0;
    uint32_t tgt_rows  = 0;
    bool     have_tgt  = find_other_mode(pre_current,
                                         &tgt_index, &tgt_cols, &tgt_rows);
    check(have_tgt, "a second enumerated mode is reachable while installed");

    if (have_tgt) {
        axl_printf("RESHAPE: switching mode %u (%ux%u) -> %u (%ux%u)\n",
                   pre_current, pre_mode.columns, pre_mode.rows,
                   tgt_index, tgt_cols, tgt_rows);

        g_log.count  = 0;
        g_log.clears = 0;
        check(axl_console_text_set_mode(tgt_index) == AXL_OK,
              "set_mode through the installed device succeeds");
        check(axl_console_text_current_mode(&cur) == AXL_OK && cur == tgt_index,
              "current mode is the requested one after set_mode");

        /* The device must re-advertise, or the consumer's encoder keeps
           painting at the old grid. */
        axl_console_device_get_size(dev, &cols, &rows);
        check_u32(cols, tgt_cols, "device re-advertises the new width");
        check_u32(rows, tgt_rows, "device re-advertises the new height");

        /* ...and must SAY so: a consumer that has to poll would never learn. */
        check(g_log.count >= 1, "consumer got a resize notification");
        check_u32(g_log.cols, tgt_cols, "resize op reports the new width");
        check_u32(g_log.rows, tgt_rows, "resize op reports the new height");

        /* SetMode clears the display (UEFI contract; the firmware console we
           co-paint with does it). Without this the consumer's screen model keeps
           the pre-reshape content and reinterprets it at the new geometry. */
        check(g_log.clears >= 1, "reshape clears the consumer's screen");

        /* ---- 5. and back, so the console is left as we found it ----- */
        check(axl_console_text_set_mode(pre_current) == AXL_OK,
              "set_mode back to the original succeeds");
        axl_console_device_get_size(dev, &cols, &rows);
        check(cols == pre_mode.columns && rows == pre_mode.rows,
              "device re-advertises the original geometry");
    }

    /* ---- 5b. set_size is refused while co-painting ------------------ */
    /* The geometry is a two-console agreement; moving only our half is the
       desync passthrough exists to avoid. */
    axl_console_device_get_size(dev, &cols, &rows);
    uint32_t before_cols = cols;
    uint32_t before_rows = rows;
    axl_console_device_set_size(dev, 40, 10);
    axl_console_device_get_size(dev, &cols, &rows);
    check(cols == before_cols && rows == before_rows,
          "set_size is ignored under passthrough");

    /* ---- 6. uninstall restores the bare-console view ---------------- */
    axl_console_device_uninstall(dev);
    check_u32(axl_console_text_mode_count(), pre_count,
              "uninstall leaves the physical mode list intact");
    check(axl_console_text_current_mode(&cur) == AXL_OK && cur == pre_current,
          "uninstall leaves the console in the original mode");

    axl_printf("RESHAPE: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
