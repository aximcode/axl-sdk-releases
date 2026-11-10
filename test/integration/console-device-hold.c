/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * console-device-hold.c -- resident service driver fixture that HOLDS the
 * console take-over and is steered by marker lines the Shell prints.
 *
 * It is an AXL_SERVICE_DRIVER loaded with the Shell's `load`, so its loop is
 * the library's own service loop, attached in driver mode with the library's
 * defaults -- nothing here touches the loop's Ctrl-C handling. Its setup
 * installs the take-over device (passthrough_local, so the serial console
 * keeps painting; take_input, so keys can be injected) and a 10 ms tick that
 * watches the output_text ops for:
 *
 *   "cdh-arm-break" -> 1500 ms later, inject Ctrl-C (VALID|LEFT_CONTROL 'c')
 *                      through the device, as a remote console would
 *   "cdh-arm-busy"  -> 1500 ms later, inject Ctrl-C the same way and then,
 *                      still inside the tick, do the ordinary library work a
 *                      resident driver does: a short-lived sync wait
 *                      (axl_wait_ms, its own loop that is never attached) and
 *                      a file read (which polls the break between reads).
 *                      Neither may take the Shell's break, latch an interrupt
 *                      or try to exit this image
 *   "cdh-ping"      -> print HOLD-PONG (proves the loop still dispatches)
 *   "cdh-release"   -> uninstall the device (the driver stays resident)
 *   "cdh-fail-release" -> the same, with the next DisconnectController
 *                      refused through the library's test point, so the
 *                      uninstall returns AXL_ERR and the device is kept for a
 *                      later "cdh-release" to resume
 *   "cdh-fail-conin" -> the same, with the uninstall of the device's ConIn
 *                      protocols refused instead (the "conin-uninstall" test
 *                      point), after the firmware consoles are restored. The
 *                      device then no longer sees the Shell's output, so no
 *                      marker can reach it: the fixture retries by itself
 *                      15 s later, well after the test has typed at it
 *   "cdh-errline"   -> counted, each time it is drawn (a line another image
 *                      writes to StdErr: the device is in that fan-out too)
 *   "cdh-report"    -> print HOLD-SEEN errline=<that count>
 *
 * Built twice: console-device-hold.efi takes over with passthrough_local, and
 * console-device-hold-takeover.efi (-DHOLD_TAKEOVER) as a real take-over,
 * evicting the firmware consoles (test-console-device-errout-qemu.sh).
 *
 * The markers are printed through a Shell variable (`set -v T cdh`, then
 * `echo %T%-release`), so the Shell's own echo of the command line can never
 * match one. Reports go to gST->StdErr, never ConOut, so they do not re-enter
 * the ops this fixture is watching.
 *
 * Drives test-console-device-hold-qemu.sh.
 */

#include <axl.h>
#include <axl/axl-console-device.h>
#include <axl/axl-console-ops.h>
#include <axl/axl-loop.h>
#include <axl/axl-service.h>
#include <axl/axl-signal.h>
#include <axl/axl-fs.h>
#include <axl/axl-wait.h>
#include <axl/axl-str.h>
#include <axl/axl-time.h>
#include <uefi/axl-uefi.h>

#include "../../src/util/axl-console-device-internal.h"

#include <stdarg.h>

#define HOLD_TICK_MS   10
#define HOLD_TAIL_MAX  128
#define HOLD_PATH_CAP  512

enum {
    ACT_BREAK,
    ACT_BUSY,
    ACT_PING,
    ACT_RELEASE,
    ACT_FAIL_RELEASE,
    ACT_FAIL_CONIN,
    ACT_REPORT,
    ACT_N
};

static AxlConsoleDevice *g_dev;
static char              g_tail[HOLD_TAIL_MAX + 1];
static size_t            g_tail_len;
static uint64_t          g_due[ACT_N];   /* 0 = not armed; else a ms deadline */
static unsigned          g_errline;      /* times "cdh-errline" was drawn */
static unsigned          g_fail_disc;    /* disconnects still to refuse */
static unsigned          g_fail_conin;   /* ConIn uninstalls still to refuse */
static bool              g_self_retry;   /* retry a failed release unprompted */

/* Strong over the library's weak "never": refuse the armed number of each. */
bool
_axl_console_device_test_point(const char *point)
{
    unsigned *left = axl_strcmp(point, "disconnect") == 0      ? &g_fail_disc
                   : axl_strcmp(point, "conin-uninstall") == 0 ? &g_fail_conin
                   : NULL;
    if (left != NULL && *left > 0) {
        (*left)--;
        return true;
    }
    return false;
}

/* One line to the serial log through StdErr (see the file header). */
static void
hold_say(const char *fmt, ...)
{
    char    buf[160];
    CHAR16  wide[164];
    va_list ap;
    size_t  i;

    va_start(ap, fmt);
    axl_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (i = 0; buf[i] != '\0' && i < sizeof(buf) - 1; i++) {
        wide[i] = (CHAR16)(unsigned char)buf[i];
    }
    wide[i++] = L'\r';
    wide[i++] = L'\n';
    wide[i]   = 0;
    if (gST != NULL && gST->StdErr != NULL) {
        gST->StdErr->OutputString(gST->StdErr, wide);
    }
}

static void
scan_tail(void)
{
    static const struct {
        const char *marker;
        int         act;
        uint64_t    delay_ms;
    } MARKERS[] = {
        { "cdh-arm-break", ACT_BREAK,   1500 },
        { "cdh-arm-busy",  ACT_BUSY,    1500 },
        { "cdh-ping",      ACT_PING,    0    },
        { "cdh-release",   ACT_RELEASE, 0    },
        { "cdh-fail-release", ACT_FAIL_RELEASE, 0 },
        { "cdh-fail-conin", ACT_FAIL_CONIN, 0 },
        { "cdh-report",    ACT_REPORT,  0    },
    };

    if (axl_strstr(g_tail, "cdh-errline") != NULL) {
        g_errline++;
        g_tail_len = 0;
        g_tail[0]  = '\0';
        return;
    }

    for (size_t i = 0; i < sizeof(MARKERS) / sizeof(MARKERS[0]); i++) {
        if (axl_strstr(g_tail, MARKERS[i].marker) != NULL) {
            g_due[MARKERS[i].act] = axl_time_get_ms() + MARKERS[i].delay_ms + 1;
            g_tail_len = 0;
            g_tail[0]  = '\0';
            return;
        }
    }
}

static void
ops_output_text(void *user, const char *utf8, size_t len)
{
    (void)user;
    for (size_t i = 0; i < len; i++) {
        if (g_tail_len == HOLD_TAIL_MAX) {
            axl_memmove(g_tail, g_tail + HOLD_TAIL_MAX / 2, HOLD_TAIL_MAX / 2);
            g_tail_len = HOLD_TAIL_MAX / 2;
        }
        g_tail[g_tail_len++] = utf8[i];
        g_tail[g_tail_len]   = '\0';
        scan_tail();
    }
}

static int
inject_ctrl_c(void)
{
    return axl_console_device_inject_key_ex(
        g_dev, 0, 'c',
        AXL_CONSOLE_SHIFT_STATE_VALID | AXL_CONSOLE_LEFT_CONTROL_PRESSED, 0);
}

/* Library work under a pending Shell break, inside a driver image. The wait
   spans 300 ms so a break the device signals asynchronously is pending before
   it ends; the read comes second so a latched interrupt (from the wait) would
   reach the read's break poll, which is where an exit would be attempted. */
static void
busy_under_break(void)
{
    hold_say("HOLD-BUSY-INJECTED rc=%d", inject_ctrl_c());

    int wait_rc = (int)axl_wait_ms(NULL, 300);
    hold_say("HOLD-BUSY wait=%d interrupted=%d", wait_rc, axl_interrupted() ? 1 : 0);

    void  *buf = NULL;
    size_t len = 0;
    char   path[HOLD_PATH_CAP];
    /* busy.nsh beside this image. This read runs at TPL_CALLBACK and may
       preempt the Shell inside a request on the same volume, so that volume's
       driver must serialize its requests as FAT does; OVMF's VirtioFsDxe does
       not, which is why the test refuses a leased guest's virtio-fs share. */
    int    read_rc = axl_app_image_dir_path("busy.nsh", path, sizeof(path)) == AXL_OK
                         ? axl_file_get_contents(path, &buf, &len) : -1;
    axl_free(buf);
    hold_say("HOLD-BUSY read=%d interrupted=%d", read_rc, axl_interrupted() ? 1 : 0);
}

static const AxlConsoleOps HOLD_OPS = {
    .output_text = ops_output_text,
};

static bool
hold_tick(void *data)
{
    (void)data;
    uint64_t now = axl_time_get_ms();

    for (int a = 0; a < ACT_N; a++) {
        if (g_due[a] == 0 || now < g_due[a]) {
            continue;
        }
        g_due[a] = 0;
        switch (a) {
        case ACT_BREAK:
            hold_say("HOLD-INJECTED-CTRLC rc=%d", inject_ctrl_c());
            break;
        case ACT_BUSY:
            busy_under_break();
            break;
        case ACT_PING:
            hold_say("HOLD-PONG");
            break;
        case ACT_REPORT:
            hold_say("HOLD-SEEN errline=%u", g_errline);
            break;
        case ACT_FAIL_RELEASE:
            g_fail_disc = 1;
            g_due[ACT_RELEASE] = now + 1;
            break;
        case ACT_FAIL_CONIN:
            g_fail_conin = 1;
            g_self_retry = true;
            g_due[ACT_RELEASE] = now + 1;
            break;
        case ACT_RELEASE: {
            /* This tick can preempt the Shell part-way through a console call;
               the device then refuses (AXL_BUSY) and is still installed, so
               try again on the next tick rather than claim a release. */
            int rc = axl_console_device_uninstall(g_dev);
            if (rc == AXL_BUSY) {
                g_due[a] = now + 1;
                break;
            }
            if (rc == AXL_OK) {
                g_dev = NULL;
                hold_say("HOLD-RELEASED");
            } else {
                /* Kept: the device still calls into this image, and a later
                   uninstall resumes from where this one stopped. */
                hold_say("HOLD-RELEASE-FAILED rc=%d", rc);
                if (g_self_retry) {
                    g_self_retry = false;
                    g_due[a] = now + 15000;
                }
            }
            break;
        }
        default:
            break;
        }
    }
    return AXL_SOURCE_CONTINUE;
}

static int
hold_setup(AxlLoop *loop, void *user)
{
    (void)user;

    AxlConsoleDeviceConfig cfg = {
#ifdef HOLD_TAKEOVER
        .passthrough_local = false,
#else
        .passthrough_local = true,
#endif
        .take_input        = true,
        .read_physical     = true,
    };
    if (axl_console_device_install(&HOLD_OPS, NULL, &cfg, &g_dev) != AXL_OK) {
        hold_say("HOLD-INSTALL-FAILED");
        return AXL_ERR;
    }
    if (axl_loop_add_timer(loop, HOLD_TICK_MS, hold_tick, NULL) == 0) {
        hold_say("HOLD-LOOP-FAILED");
        /* An error return unloads this image: only safe once the device is
           gone, since it calls into HOLD_OPS here. Otherwise stay resident. */
        int rc = axl_console_device_uninstall(g_dev);
        if (rc != AXL_OK) {
            hold_say("HOLD-RELEASE-FAILED rc=%d", rc);
            return AXL_OK;
        }
        g_dev = NULL;
        return AXL_ERR;
    }
    hold_say("HOLD-READY");
    return AXL_OK;
}

static int
hold_teardown(void *user)
{
    (void)user;
    int rc = axl_console_device_uninstall(g_dev);
    if (rc != AXL_OK) {
        hold_say("HOLD-RELEASE-FAILED rc=%d", rc);
        return rc;   /* the device still calls into this image */
    }
    g_dev = NULL;
    return AXL_OK;
}

static const AxlService console_device_hold = {
    .name           = "console-device-hold",
    .setup          = hold_setup,
    .teardown       = hold_teardown,
    .driver_tick_ms = HOLD_TICK_MS,
};

AXL_SERVICE_DRIVER(console_device_hold);
