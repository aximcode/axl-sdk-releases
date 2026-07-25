/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * fbcon-drv.c -- the resident take-over driver behind the `fbcon` tool.
 *
 * Embedded into and loaded by the `fbcon.efi` launcher app (tools/fbcon.c) via
 * AXL_EMBED + axl_driver_load_buffer, so the user runs `fbcon.efi` as a normal
 * command rather than `load`-ing a driver by hand. On entry it installs a presence
 * marker (fbcon-marker.h) on its own image handle so a later `fbcon.efi` run can find
 * and UnloadImage a prior instance before starting fresh.
 *
 * A resident driver: it takes over the Shell's console with
 * `axl-console-device` (output ops + input relay) and renders the op stream as a
 * cell grid on the GOP framebuffer via `AxlConsoleTerm`. Keystrokes reach the shell
 * through the device's input relay; the terminal peeks its own hotkeys (Shift+PgUp/
 * PgDn scrollback, Ctrl+Shift+C copy) via the device `key_filter`, and fbcon peeks
 * Ctrl+\ to drop the take-over and restore the firmware console. A pointer, if the
 * firmware exposes one, drives wheel-scroll + drag-select. fbcon also self-restores
 * when its hosted modern UEFI shell exits (so BDS / a re-launched shell gets a working
 * console), and `unload` restores the firmware console.
 *
 * This is the standalone tool on the reusable AxlConsoleTerm component -- no AGT
 * code. See docs/AXL-Console-Terminal-Design.md (Piece 3).
 */

#include <axl.h>
#include <axl/axl-console-device.h>
#include <axl/axl-console-term.h>
#include <axl/axl-driver.h>
#include <axl/axl-font.h>
#include <axl/axl-gfx.h>
#include <axl/axl-input.h>
#include <axl/axl-loop.h>
#include <axl/axl-shell.h>       /* axl_shell_kind: detect the hosted shell exiting */
#include <uefi/axl-uefi.h>       /* EFI_KEY_DATA / gST->ConOut for the Ctrl+\ hotkey + notice */

#include "fbcon-marker.h"        /* presence marker so the launcher can find + unload us */

AXL_LOG_DOMAIN("fbcon");

#define FBCON_RENDER_MS  40   /* render cadence (~25 Hz), matching the smoke's blit */

/* Parse fbcon's optional input-gate flags from the launcher-forwarded command line:
   "-d <ms>" sets the same-key debounce window, "-g <ms>" the all-key min-gap (see
   AxlConsoleDeviceConfig.debounce_ms / .min_gap_ms). Unknown tokens are ignored; a
   missing or non-numeric value leaves that knob at 0 (gate off). These are the cheap
   re-run knobs for the real-HW key-bounce A/B -- start ~50-80 ms debounce, min_gap 0. */
static void
fbcon_parse_gate_opts(const char *opts, uint32_t *debounce_ms, uint32_t *min_gap_ms)
{
    if (opts == NULL) {
        return;
    }
    const char *p = opts;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') {   /* skip separators */
            p++;
        }
        const char *tok = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {   /* consume the token */
            p++;
        }
        uint32_t *target = NULL;
        if (p - tok == 2 && tok[0] == '-' && tok[1] == 'd') {
            target = debounce_ms;
        } else if (p - tok == 2 && tok[0] == '-' && tok[1] == 'g') {
            target = min_gap_ms;
        }
        if (target != NULL) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            uint32_t v = 0;
            if (axl_str_to_u32(p, 10, &v, &p) == AXL_OK) {   /* p advances past digits */
                *target = v;
            }
        }
    }
}

static AxlConsoleDevice *g_device;
static AxlConsoleTerm   *g_term;
static AxlLoop          *g_loop;
static bool              g_mouse_attached;
static AxlSourceId       g_touch_source;    /* absolute pointer feeding the cursor sprite */
static uint32_t          g_scr_w, g_scr_h;  /* GOP pixel extent (touch coords map onto it) */
static bool              g_shell_seen;      /* the hosted shell was present at least once */
static volatile bool     g_leave_pending;   /* Ctrl+\ asked us to drop the take-over */

/* Restore the firmware console and drop the take-over. Idempotent. Safe from the
   render-timer callback now that axl_console_device_uninstall no longer wedges when
   our non-80x25 device is re-added-around (the ConSplitter mode-assert fix). Does NOT
   touch the loop driver -- we may be running inside its tick; fbcon_unload detaches
   and frees the loop. */
static void
fbcon_teardown(void)
{
    if (g_touch_source != 0) {
        axl_input_detach_touch(g_loop);   /* absolute pointer feeding the cursor sprite */
        g_touch_source = 0;
    }
    if (g_mouse_attached) {
        axl_input_detach_mouse(g_loop);
        g_mouse_attached = false;
    }
    if (g_device != NULL) {
        axl_console_device_uninstall(g_device);   /* restores the firmware console + input */
        g_device = NULL;
        /* Wipe our full-screen frame so the restored firmware console doesn't paint
           over a ghost of the old grid (it only redraws the cells it touches). */
        if (gST != NULL && gST->ConOut != NULL && gST->ConOut->ClearScreen != NULL) {
            gST->ConOut->ClearScreen(gST->ConOut);
        }
    }
    if (g_term != NULL) {
        axl_console_term_free(g_term);
        g_term = NULL;
    }
}

/* key_filter: peek fbcon's own Ctrl+\ (drop the take-over) FIRST, then hand the rest
   to the terminal's hotkeys (scrollback, copy). Returns true to consume (the shell
   never sees the key). Ctrl+\ is the terminal SIGQUIT key -- thematically "leave". */
static bool
fbcon_hotkey(void *user, const void *key)
{
    if (key == NULL) {
        return false;
    }
    const EFI_KEY_DATA *kd    = key;
    uint32_t            shift = kd->KeyState.KeyShiftState;
    bool                ctrl  = (shift & (EFI_LEFT_CONTROL_PRESSED | EFI_RIGHT_CONTROL_PRESSED)) != 0;
    uint16_t            uni   = kd->Key.UnicodeChar;
    /* Ctrl+\ folds to the FS control char (0x1C = '\\' & 0x1F); some firmware instead
       reports '\\' with CONTROL held. Accept both. The teardown runs on the next
       render tick (this filter can fire at TPL_CALLBACK in the read loop). */
    if (uni == 0x1C || (ctrl && uni == (uint16_t)'\\')) {
        g_leave_pending = true;
        return true;
    }
    return axl_console_term_handle_hotkey(user, key);
}

/* Pointer events -> terminal (wheel scroll, drag select, Ctrl+wheel zoom). */
static bool
fbcon_on_pointer(const AxlInputEvent *ev, void *data)
{
    axl_console_term_handle_pointer(data, ev);
    return true;   /* keep the source attached */
}

/* Absolute pointer (EFI_ABSOLUTE_POINTER: a BMC/iDRAC remote-console virtual mouse, a
   digitizer, or QEMU's usb-tablet) -> the terminal's cursor sprite. Unlike the relative
   SimplePointer -- which the take-over proxy forwards to a guest (edit draws its own
   mouse block from it) -- the absolute pointer is not read by guests, so driving our
   arrow from it does not double up with edit's block. Coordinates arrive normalized to
   [0, AXL_INPUT_ABS_RANGE); map them onto the GOP extent (the terminal fills it, so
   screen pixels == target pixels). */
static bool
fbcon_on_touch(const AxlInputEvent *ev, void *data)
{
    switch (ev->type) {
    case AXL_INPUT_TOUCH_MOVE:
    case AXL_INPUT_TOUCH_DOWN:
    case AXL_INPUT_TOUCH_UP: {
        int32_t px = (int32_t)((int64_t)ev->x * (g_scr_w > 0 ? g_scr_w - 1 : 0)
                               / (AXL_INPUT_ABS_RANGE - 1));
        int32_t py = (int32_t)((int64_t)ev->y * (g_scr_h > 0 ? g_scr_h - 1 : 0)
                               / (AXL_INPUT_ABS_RANGE - 1));
        axl_console_term_set_pointer(data, px, py);
        break;
    }
    default:
        break;
    }
    return true;   /* keep the source attached */
}

/* Render timer: repaint the terminal's dirty rows to the GOP. Also drops the
   take-over when the user asks (Ctrl+\) or when the hosted shell exits -- fbcon takes
   over the ONE shell it was loaded into, so once that shell's EFI_SHELL_PROTOCOL
   disappears (the user typed `exit`) we restore the firmware console and stop, letting
   BDS and any newly-launched shell (e.g. picking "EFI Internal Shell") use the normal
   console. */
static bool
fbcon_render(void *data)
{
    (void)data;
    if (g_leave_pending) {
        axl_info("Ctrl+\\ -- restoring the firmware console");
        fbcon_teardown();
        /* We stay in the same shell, so tell the user how to come back. This resident
           instance lingers (a driver can't self-unload from inside its own image), but
           the `fbcon.efi` launcher reaps it on the next run -- so `fbcon.efi` just works. */
        if (gST != NULL && gST->ConOut != NULL) {
            gST->ConOut->OutputString(gST->ConOut,
                (CHAR16 *)L"[fbcon] firmware console restored -- run 'fbcon.efi' to take over again\r\n");
        }
        return AXL_SOURCE_REMOVE;
    }
    if (axl_shell_kind() == AXL_SHELL_KIND_UEFI) {
        g_shell_seen = true;
    } else if (g_shell_seen) {
        axl_info("hosted shell exited -- restoring the firmware console");
        fbcon_teardown();
        return AXL_SOURCE_REMOVE;
    }
    axl_console_term_render(g_term);
    return AXL_SOURCE_CONTINUE;
}

static int
fbcon_entry(AxlHandle image, AxlSystemTable *st)
{
    (void)st;

    if (!axl_gfx_available()) {
        axl_error("no GOP -- fbcon needs a graphics display");
        return 1;
    }

    AxlGfxInfo info;
    if (axl_gfx_get_info(&info) != AXL_OK) {
        return 1;
    }
    const AxlFont *font = axl_gfx_default_font();
    uint32_t       cols = info.width / font->cell_width;
    uint32_t       rows = info.height / font->cell_height;
    if (cols == 0 || rows == 0) {
        return 1;
    }
    g_scr_w = info.width;   /* touch coords map onto this extent */
    g_scr_h = info.height;

    /* GOP target, full bounds. mouse_cursor draws a software arrow (AxlCursor,
       save-under on the GOP) that we drive from the absolute pointer below. */
    AxlConsoleTermConfig tcfg = { .cols = cols, .rows = rows, .mouse_cursor = true };
    g_term = axl_console_term_new(&tcfg);
    if (g_term == NULL) {
        return 1;
    }

    /* Optional input-gate knobs, forwarded by the launcher as LoadOptions
       ("-d <ms>" debounce, "-g <ms>" min-gap). Off (0) unless the user asks. */
    uint32_t debounce_ms = 0, min_gap_ms = 0;
    char    *opts = axl_driver_get_load_options();
    if (opts != NULL) {
        fbcon_parse_gate_opts(opts, &debounce_ms, &min_gap_ms);
        axl_free(opts);
    }
    if (debounce_ms != 0 || min_gap_ms != 0) {
        axl_info("input gate: debounce=%u ms, min_gap=%u ms", debounce_ms, min_gap_ms);
    }

    void                  *ops_user = NULL;
    const AxlConsoleOps   *ops      = axl_console_term_ops(g_term, &ops_user);
    AxlConsoleDeviceConfig dcfg = {
        .cols            = cols,
        .rows            = rows,
        .take_input      = true,
        .read_physical   = true,
        .take_pointer    = true,   /* guests (e.g. `edit`) run mouse-free; we poll the
                                      evicted pointers ourselves for scroll + select */
        .debounce_ms     = debounce_ms,
        .min_gap_ms      = min_gap_ms,
        .key_filter      = fbcon_hotkey,
        .key_filter_user = g_term,
    };
    if (axl_console_device_install(ops, ops_user, &dcfg, &g_device) != AXL_OK) {
        axl_console_term_free(g_term);
        g_term = NULL;
        return 1;
    }

    g_loop = axl_loop_new();
    if (g_loop == NULL) {
        axl_console_device_uninstall(g_device);
        g_device = NULL;
        axl_console_term_free(g_term);
        g_term = NULL;
        return 1;
    }
    /* fbcon is a background console renderer, not the foreground app: Ctrl+C belongs
       to the hosted shell (and to whatever full-screen app it runs), NOT to fbcon's
       render loop. Opt out of the loop's Ctrl+C=quit interception so a guest Ctrl+C
       does not tear down our render/pointer pump (which would freeze the display and
       the mouse cursor). fbcon's own "leave" gesture is Ctrl+\, peeked in fbcon_hotkey. */
    axl_loop_set_intercept_break(g_loop, false);
    if (axl_loop_add_timer(g_loop, FBCON_RENDER_MS, fbcon_render, NULL) == 0
        || axl_loop_attach_driver(g_loop, FBCON_RENDER_MS) != AXL_OK) {
        axl_loop_free(g_loop);
        g_loop = NULL;
        axl_console_device_uninstall(g_device);
        g_device = NULL;
        axl_console_term_free(g_term);
        g_term = NULL;
        return 1;
    }

    /* Pointer handling. When take_pointer evicted the real SimplePointer(s), the
       device interposed a yielding proxy that serves guests (edit) directly -- it
       both idles a guest's GetState poll AND forwards real movement to its cursor. We
       therefore do NOT attach our own mouse in that case: a second reader would
       contend with the proxy for the same consume-once device. When no pointer was
       present (nothing evicted, no proxy), fall back to the locate-based attach so a
       later-appearing pointer can still drive our own scroll/select. */
    if (axl_console_device_pointer_count(g_device) > 0) {
        g_mouse_attached = false;   /* the proxy owns the pointer on behalf of guests */
    } else {
        g_mouse_attached = axl_input_attach_mouse(g_loop, fbcon_on_pointer, g_term) != 0;
    }

    /* Independently drive the cursor sprite from the ABSOLUTE pointer (a BMC/iDRAC
       remote-console virtual mouse arrives here, multiplexed through ConIn). Separate
       device from the relative SimplePointer above, so no contention with the proxy and
       no double cursor with edit's block. Returns 0 when no absolute pointer exists
       (e.g. QEMU pre-boot OVMF backs none) -- then there is simply no arrow, no harm. */
    g_touch_source = axl_input_attach_touch(g_loop, fbcon_on_touch, g_term);

    /* Tell the user how to leave (goes through the taken-over console -> our grid). */
    if (gST != NULL && gST->ConOut != NULL) {
        gST->ConOut->OutputString(gST->ConOut,
            (CHAR16 *)L"[fbcon] press Ctrl+\\ to leave and restore the firmware console\r\n");
    }

    /* Install the presence marker on our image handle so a later `fbcon.efi` run finds
       and unloads us. Best-effort: if it fails we still run, only losing the launcher's
       ability to reap this instance. */
    {
        EFI_HANDLE h = (EFI_HANDLE)image;
        if (EFI_ERROR(gBS->InstallProtocolInterface(&h, &FBCON_PRESENCE_GUID,
                                                    EFI_NATIVE_INTERFACE, NULL))) {
            axl_warning("fbcon: could not install presence marker (relaunch won't reap us)");
        }
    }

    return 0;   /* resident */
}

static int
fbcon_unload(AxlHandle image)
{
    /* Drop the presence marker first so a concurrent launcher can't try to reap a
       handle we are already unloading. */
    EFI_HANDLE h = (EFI_HANDLE)image;
    gBS->UninstallProtocolInterface(h, &FBCON_PRESENCE_GUID, NULL);
    if (g_loop != NULL) {
        axl_loop_detach_driver(g_loop);   /* detach BEFORE uninstall/free (no notify in flight) */
    }
    fbcon_teardown();   /* restore console + drop the take-over (no-op if self-restored) */
    if (g_loop != NULL) {
        axl_loop_free(g_loop);
        g_loop = NULL;
    }
    return 0;
}

AXL_DRIVER(fbcon_entry, fbcon_unload)
