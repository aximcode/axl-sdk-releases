/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-device.c
    Take-over console producer. See axl-console-device.h and
    AXL-Console-Device-Design.md. Graduates the proven AGT spike conprov-cv2.c:
    it publishes its own ConsoleOut device, connects it so EDK2's ConSplitter fans
    console output to it, then evicts the other console-out devices — all without
    touching gST->ConOut or writing NVRAM. The SIMPLE_TEXT_OUTPUT -> AxlConsoleOps
    translation is the shared axl-console-emit engine, so the device and the tap
    emit identical ops.
**/

#include <axl/axl-console-device.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>
#include <axl/axl-driver.h>
#include <axl/axl-time.h>
#include <uefi/axl-uefi.h>
#include "../backend/axl-backend.h"   /* axl_backend_console_text_set_mode */

#include "axl-console-emit.h"
#include "axl-console-input.h"

AXL_LOG_DOMAIN("condev");

// ---------------------------------------------------------------------------
// Macros and types
// ---------------------------------------------------------------------------

#define DEVICE_DEFAULT_COLS   80   /* when cfg geometry is 0 and no physical size */
#define DEVICE_DEFAULT_ROWS   25
#define DEVICE_MAX_EVICTED    16   /* console-out handles we can re-tag on uninstall */
#define DEVICE_MAX_MODES      32   /* physical text modes we mirror under passthrough */
#define DEVICE_DEFAULT_POLL_MS 15  /* read_physical timer period when cfg is 0 */

/* Our own console device path: HW Vendor node + End-Entire. Packed so the bytes
   are exactly what ConSplitter's device-path handling sees. */
typedef struct __attribute__((packed)) {
    VENDOR_DEVICE_PATH        vendor;
    EFI_DEVICE_PATH_PROTOCOL  end;
} DeviceDevicePath;

/* A process-lifetime singleton (one console take-over per system), so field
   packing is irrelevant — fields are grouped by role for readability, not by size.
   Suppress the opt-in padding perf check that would otherwise force a size-ordered
   reshuffle of the whole struct for zero benefit on a single instance. */
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct AxlConsoleDevice {
    /* Our published console-out device (a struct WE own; no foreign vtable is
       mutated, and gST->ConOut is never touched). */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  my_conout;
    SIMPLE_TEXT_OUTPUT_MODE          my_mode;
    DeviceDevicePath                 my_devpath;
    EFI_HANDLE                       my_handle;

    /* Shared SIMPLE_TEXT_OUTPUT -> AxlConsoleOps translation (emit.mode ==
       &my_mode). Geometry is fixed at install (the device advertises a single
       mode), so the engine gets the same value for resolved and configured. */
    AxlConsoleEmit                   emit;
    uint32_t                         cols;
    uint32_t                         rows;

    /* Mirrored physical text modes (passthrough only; see snapshot_physical_modes).
       Index-for-index with the console we co-paint, INCLUDING modes it reports as
       unsupported (`usable` false) -- so a mode number means the same thing whether
       or not we are installed. Empty (mode_count 0) in the take-over case, where we
       own the geometry and advertise the single mode we were configured with. */
    struct {
        uint32_t  cols;
        uint32_t  rows;
        bool      usable;
    }                                modes[DEVICE_MAX_MODES];
    uint32_t                         mode_count;
    bool                             passthrough;
    /* True from allocation until install returns. ConSplitter's AddDevice ends in
       ConsplitterSetConsoleOutMode, which unconditionally SetMode()s its preferred
       mode (ConSplitter.c:2977) -- so our dev_set_mode CAN run during the connect
       inside install, before the caller holds the AxlConsoleDevice *. Geometry
       still updates; only the consumer-visible resize is withheld, so a resize
       handler is never reached with a device pointer its caller cannot have yet. */
    bool                             installing;

    bool                             take_input;
    bool                             installed;   /* our protocols are on my_handle */

    /* Console-out handles we uninstalled the tag from, so uninstall can re-tag
       them and let ConSplitter restore GraphicsConsole. */
    EFI_HANDLE                       evicted[DEVICE_MAX_EVICTED];
    uint32_t                         evicted_count;

    /* --- Input relay (take_input) -------------------------------------------
       Our own ConIn/ConInEx published on my_handle (so gST->ConsoleInHandle's
       aggregate fans reads to us), fed by the shared input engine. The tap layers
       passthrough on its engine; the device is the SOLE input owner, so its ConInEx
       methods read the engine directly with no fallback. */
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL     my_conin;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  my_coninex;
    AxlConsoleInput                    in;
    EFI_EVENT                          wait_key;      /* our ConIn WaitForKey */
    EFI_EVENT                          wait_key_ex;   /* our ConInEx WaitForKeyEx */
    bool                               input_published;   /* ConIn protocols on my_handle */

    /* read_physical: a periodic timer reads the evicted firmware keyboards and
       re-injects their keys, so a resident take-over with no foreground input loop
       still gets live typing. The evicted ConIn handles are re-admitted on teardown
       (the input mirror of the ConOut restore), and their ConInEx pointers are
       cached so the timer polls them without a HandleProtocol per tick. */
    bool                               read_physical;
    EFI_EVENT                          read_timer;
    uint32_t                           input_poll_ms;
    EFI_HANDLE                         evicted_in[DEVICE_MAX_EVICTED];
    uint32_t                           evicted_in_count;

    /* --- Pointer take-over (take_pointer) -----------------------------------
       Every EFI_SIMPLE_POINTER_PROTOCOL is uninstalled from the handle database
       so a GUEST (e.g. UEFI edit) cannot locate one and runs mouse-free. The
       interfaces stay valid (their producing drivers are not stopped), so they are
       cached here for the CONSUMER to poll (axl_console_device_pointer_iface ->
       axl_input_attach_mouse_ifaces) and reinstalled on teardown. */
    bool                               take_pointer;
    EFI_HANDLE                         evicted_ptr[DEVICE_MAX_EVICTED];
    void                              *evicted_ptr_iface[DEVICE_MAX_EVICTED];
    uint32_t                           evicted_ptr_count;

    /* Yielding pointer PROXY: the single SimplePointer guests see after we evict the
       real ones. Its GetState blocks on a short timer before returning, so a guest that
       busy-polls GetState (the UEFI edit) HLTs between polls -> its loop idles the CPU
       instead of spinning a core (GetState runs at the caller's TPL_APPLICATION, where
       WaitForEvent is legal). On wake it forwards a real pointer's state, so the guest's
       cursor still tracks the mouse (with <= the timer period of latency). */
    EFI_SIMPLE_POINTER_PROTOCOL        proxy_sp;
    EFI_SIMPLE_POINTER_MODE            proxy_mode;
    EFI_HANDLE                         proxy_handle;
    EFI_EVENT                          proxy_timer;
    bool                               proxy_installed;

    /* Optional key-repeat gate (OFF unless the cfg sets a window). NOT a bounce
       cure — see the header / design §5. */
    uint32_t                           debounce_ms;   /* drop a repeat of the same key */
    uint32_t                           min_gap_ms;    /* drop any key within this gap */
    bool                             (*key_filter)(void *user, const void *key);
    void                              *key_filter_user;
    uint64_t                           last_key_ms;
    uint16_t                           last_scan;
    uint16_t                           last_unicode;
    bool                               have_last;
};

static AxlConsoleDevice *g_dev;   /* the one active instance */

/* The console-out marker tag ConSplitter binds on lives in the UEFI headers
   (gEfiConsoleOutDeviceGuid, hand-written in axl-uefi-extra.h — it is an
   MdeModulePkg impl GUID, not a spec GUID). Our own vendor GUID naming this
   console device is arbitrary and unique. */
static EFI_GUID DEVICE_VENDOR_GUID =
    { 0xA713D0A5, 0xC0DE, 0x4A11, { 0xB0, 0x0B, 0x10, 0xCA, 0x1C, 0x0F, 0xFE, 0xED } };

// ---------------------------------------------------------------------------
// Our SimpleTextOut methods — feed the shared engine, own my_mode. No foreign
// vtable, no passthrough (the device is the SOLE console writer after eviction).
// Geometry is fixed (dev->cols/rows), so resolved == configured for the engine.
// ---------------------------------------------------------------------------

static EFI_STATUS EFIAPI
dev_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ext)
{
    (void)This;
    (void)ext;
    axl_console_emit_home_cursor(&g_dev->emit);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
dev_output_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{
    (void)This;
    AxlConsoleDevice *d = g_dev;
    /* The device advertises a single geometry, so resolved == configured: d->cols/
       d->rows are the cursor-clamp bounds. */
    axl_console_emit_text(&d->emit, String, d->cols, d->rows);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
dev_test_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{
    (void)This;
    (void)String;
    /* Claim every char renderable — the Shell probes this before writing
       box-drawing; the consumer draws whatever glyph its font has. */
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
dev_query_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber,
               UINTN *Columns, UINTN *Rows)
{
    (void)This;
    /* A registration-notify dispatched from inside InstallMultipleProtocolInterfaces
       can reach us before g_dev is assigned, and the failed-connect path clears it
       before uninstalling -- both leave our protocol reachable with no device. */
    if (g_dev == NULL || Columns == NULL || Rows == NULL) {
        return EFI_UNSUPPORTED;
    }

    /* Passthrough: answer for the physical console we co-paint, mode for mode.
       ConSplitter intersects the members' modes BY (Columns, Rows), so mirroring
       is what keeps the aggregate's full list alive -- advertising one mode
       collapses it to that geometry and strands every other physical mode. */
    if (g_dev->passthrough && g_dev->mode_count > 0) {
        if (ModeNumber >= (UINTN)g_dev->mode_count
            || !g_dev->modes[ModeNumber].usable)
        {
            return EFI_UNSUPPORTED;   /* mirrors the physical console's own hole */
        }
        *Columns = g_dev->modes[ModeNumber].cols;
        *Rows    = g_dev->modes[ModeNumber].rows;
        return EFI_SUCCESS;
    }

    /* Take-over: one mode (mode 0 = our advertised geometry). We evicted the
       firmware console, so the grid is ours to define. */
    if (ModeNumber == 0) {
        *Columns = g_dev->cols;
        *Rows    = g_dev->rows;
        return EFI_SUCCESS;
    }
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
dev_set_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber)
{
    (void)This;
    if (g_dev == NULL) {
        return EFI_UNSUPPORTED;
    }

    /* Passthrough: ConSplitter fans SetMode to every member, so this runs in the
       same breath as the physical console's own switch. Re-advertise to the new
       geometry HERE -- if we do not, the two co-painting consoles end up on
       different grids while each believes it agreed, and the consumer keeps
       painting at the old size with nothing telling it otherwise. */
    if (g_dev->passthrough && g_dev->mode_count > 0) {
        if (ModeNumber >= (UINTN)g_dev->mode_count
            || !g_dev->modes[ModeNumber].usable)
        {
            return EFI_UNSUPPORTED;
        }
        g_dev->cols = g_dev->modes[ModeNumber].cols;
        g_dev->rows = g_dev->modes[ModeNumber].rows;
        axl_console_emit_set_mode(&g_dev->emit, (uint32_t)ModeNumber);
        /* SetMode clears the display -- the UEFI contract, and what the firmware
           console we co-paint with does (GraphicsConsoleConOutSetMode). ConSplitter
           does NOT do it for us: its TextOutSetMode only rewrites Mode + cursor
           (ConSplitterGraphics.c:298). Without this the consumer's screen model
           keeps the pre-reshape content and reinterprets it at the new geometry.
           A shell's ConsoleLogger issues its own clear on top; clears are
           idempotent, so the duplicate is harmless. */
        axl_console_emit_clear_screen(&g_dev->emit);
        /* After the geometry is live, so a consumer may read either this or
           axl_console_device_get_size and see the same answer. Withheld during
           install -- see AxlConsoleDevice::installing. */
        if (!g_dev->installing) {
            axl_console_emit_resize(&g_dev->emit, g_dev->cols, g_dev->rows);
        }
        return EFI_SUCCESS;
    }

    if (ModeNumber != 0) {
        return EFI_UNSUPPORTED;
    }
    axl_console_emit_set_mode(&g_dev->emit, (uint32_t)ModeNumber);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
dev_set_attribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute)
{
    (void)This;
    axl_console_emit_set_attribute(&g_dev->emit, (uint32_t)Attribute);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
dev_clear_screen(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
{
    (void)This;
    axl_console_emit_clear_screen(&g_dev->emit);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
dev_set_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row)
{
    (void)This;
    axl_console_emit_set_cursor(&g_dev->emit, (uint32_t)Column, (uint32_t)Row);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
dev_enable_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible)
{
    (void)This;
    axl_console_emit_enable_cursor(&g_dev->emit, Visible ? true : false);
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Our SimpleTextInput / SimpleTextInputEx methods (input relay, take_input).
// The device is the SOLE input owner after eviction, so these read the shared
// AxlConsoleInput engine directly -- no passthrough (unlike the tap's swap form,
// which falls back to the firmware ConIn when it is not capturing).
// ---------------------------------------------------------------------------

static EFI_STATUS EFIAPI
dev_in_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN ext)
{
    (void)This;
    (void)ext;
    axl_console_input_drain(&g_dev->in);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
dev_in_read_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
    (void)This;
    /* Simple protocol: pop + Ctrl+letter fold (the engine applies it). */
    return axl_console_input_read_key(&g_dev->in, Key) ? EFI_SUCCESS : EFI_NOT_READY;
}

static EFI_STATUS EFIAPI
dev_inex_reset(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, BOOLEAN ext)
{
    (void)This;
    (void)ext;
    axl_console_input_drain(&g_dev->in);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
dev_inex_read_key(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *KeyData)
{
    (void)This;
    /* Ex protocol: the guest reads back the full EFI_KEY_DATA (KeyState included). */
    return axl_console_input_read_key_ex(&g_dev->in, KeyData) ? EFI_SUCCESS : EFI_NOT_READY;
}

static EFI_STATUS EFIAPI
dev_inex_set_state(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, void *toggle_state)
{
    (void)This;
    (void)toggle_state;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
dev_inex_register_notify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *KeyData,
                         EFI_KEY_NOTIFY_FUNCTION fn, void **NotifyHandle)
{
    (void)This;
    return axl_console_input_register_notify(&g_dev->in, KeyData, fn, NotifyHandle);
}

static EFI_STATUS EFIAPI
dev_inex_unregister_notify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, void *handle)
{
    (void)This;
    return axl_console_input_unregister_notify(&g_dev->in, handle);
}

/* WaitForKey / WaitForKeyEx NOTIFY_WAIT callback. The device is the sole input
   owner, so there is nothing to poll here: the engine's ring push (from inject_*
   or the read loop) already signals both events. A NOTIFY_WAIT event must have a
   notify function, so this is the required no-op. */
static void EFIAPI
dev_wait_key_cb(EFI_EVENT Event, void *Context)
{
    (void)Event;
    (void)Context;
}

/* Optional key-repeat gate on the read_physical path. OFF (admits everything)
   unless the cfg set a window -- NOT a bounce cure, just tunable plumbing for the
   real-HW A/B (header / design §5). Timestamps track the last ADMITTED key, so a
   burst is throttled to at most one key per min_gap_ms. */
static bool
input_gate_admits(AxlConsoleDevice *d, const EFI_KEY_DATA *kd)
{
    if (d->debounce_ms == 0 && d->min_gap_ms == 0) {
        return true;
    }
    uint64_t now = axl_time_get_ms();
    if (d->have_last) {
        uint64_t dt = now - d->last_key_ms;
        if (d->min_gap_ms != 0 && dt < d->min_gap_ms) {
            return false;
        }
        bool same = kd->Key.ScanCode == d->last_scan
                 && kd->Key.UnicodeChar == d->last_unicode;
        if (d->debounce_ms != 0 && same && dt < d->debounce_ms) {
            return false;
        }
    }
    d->last_scan    = kd->Key.ScanCode;
    d->last_unicode = kd->Key.UnicodeChar;
    d->last_key_ms  = now;
    d->have_last    = true;
    return true;
}

/* read_physical timer: drain each evicted firmware keyboard's ConInEx and
   re-inject its keys into our ring. Runs at TPL_CALLBACK. The evicted keyboards'
   protocols stay installed (only their ConsoleInDevice tag was removed), so their
   own driver keeps queuing keys we read here. */
/* Deliver one physical key to the shell: let the consumer's key_filter peek it first
   (a true return consumes it), then the optional repeat gate, then inject to our ring.
   Shared by the read loop and the headless test seam. */
static void
dev_deliver_physical_key(AxlConsoleDevice *d, const EFI_KEY_DATA *kd)
{
    if (d->key_filter != NULL && d->key_filter(d->key_filter_user, kd)) {
        return;   /* consumed by the consumer (e.g. a terminal hotkey) */
    }
    if (input_gate_admits(d, kd)) {
        axl_console_input_push_notify(&d->in, *kd);
    }
}

static void EFIAPI
dev_read_timer_cb(EFI_EVENT Event, void *Context)
{
    (void)Event;
    (void)Context;
    AxlConsoleDevice *d = g_dev;
    if (d == NULL) {
        return;
    }
    for (uint32_t i = 0; i < d->evicted_in_count; i++) {
        /* Re-resolve the keyboard's SimpleTextInputEx from its STABLE handle every
           tick. A firmware keyboard can reallocate or remove its interface at any
           time (USB re-enumeration under a flaky KVM -- observed on real iDRAC),
           which leaves a cached pointer dangling; calling it #GPs. HandleProtocol
           returns the live interface, or fails if it's gone -> skip it. (If a
           re-enumeration frees the whole handle and makes a NEW one, this stored
           handle stops resolving and that keyboard is silently dropped until
           teardown -- strictly better than the crash; a full per-tick re-sweep by
           SimpleTextInputEx would recover it, left as a follow-up.) */
        void *iface = NULL;
        if (d->evicted_in[i] == NULL
            || EFI_ERROR(gBS->HandleProtocol(d->evicted_in[i],
                             &gEfiSimpleTextInputExProtocolGuid, &iface))
            || iface == NULL) {
            continue;
        }
        EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *ex = iface;
        if (ex->ReadKeyStrokeEx == NULL) {
            continue;
        }
        EFI_KEY_DATA kd;
        while (!EFI_ERROR(ex->ReadKeyStrokeEx(ex, &kd))) {
            dev_deliver_physical_key(d, &kd);
        }
    }
}

// ---------------------------------------------------------------------------
// Console-device publication + fan-out surgery (NO NVRAM, NO gST->ConOut swap)
// ---------------------------------------------------------------------------

/* Resolve the geometry our single mode advertises: the configured size, else the
   physical console's current mode (queried BEFORE we evict it), else the 80x25
   default. */
static void
resolve_geometry(AxlConsoleDevice *d, const AxlConsoleDeviceConfig *cfg)
{
    uint32_t c = cfg->cols;
    uint32_t r = cfg->rows;
    if ((c == 0 || r == 0) && gST->ConOut != NULL && gST->ConOut->Mode != NULL
        && gST->ConOut->QueryMode != NULL) {
        UINTN oc = 0;
        UINTN orow = 0;
        if (!EFI_ERROR(gST->ConOut->QueryMode(gST->ConOut,
                           (UINTN)gST->ConOut->Mode->Mode, &oc, &orow))) {
            if (c == 0) { c = (uint32_t)oc; }
            if (r == 0) { r = (uint32_t)orow; }
        }
    }
    d->cols = (c != 0) ? c : DEVICE_DEFAULT_COLS;
    d->rows = (r != 0) ? r : DEVICE_DEFAULT_ROWS;
}

/* Mirror the physical console's text-mode list into d->modes.

   Called under passthrough only, and BEFORE we publish -- at this point gST->ConOut
   reports the console WITHOUT us (the firmware aggregate, or a shell's ConsoleLogger
   wrapping it, which forwards QueryMode straight down), so its list is the one we
   have to match. Once we are a member, that list is the intersection WITH us and
   reading it back would be circular.

   Modes are copied index-for-index, holes included: EDK2 leaves an unsupported mode
   in place with 0x0 geometry (OVMF's mode 1 when 80x50 is absent), and a mode number
   has to mean the same thing whether or not we are installed. Unusable entries
   answer EFI_UNSUPPORTED from dev_query_mode, exactly as the physical console does.

   Index stability is exact for modes 0 and 1 and best-effort above: ConSplitter
   only special-cases the mode-1 hole (ConSplitter.c:2445) and starts its removal
   walk at index 2 (ConSplitter.c:2365), so a hole at index >= 2 -- or any mode past
   the DEVICE_MAX_MODES clamp -- loses its map entry and is dropped from the
   aggregate, renumbering the modes above it. Stock GraphicsConsole only ever puts a
   hole at index 1, so this is a caveat for exotic consoles rather than a live case.

   Leaves mode_count 0 when the console cannot be enumerated, which falls the device
   back to advertising its single resolved geometry -- the pre-mirroring behaviour. */
static void
snapshot_physical_modes(AxlConsoleDevice *d)
{
    d->mode_count = 0;

    if (gST->ConOut == NULL || gST->ConOut->Mode == NULL
        || gST->ConOut->QueryMode == NULL)
    {
        return;
    }

    INT32 max = gST->ConOut->Mode->MaxMode;
    if (max <= 0) {
        return;
    }
    uint32_t count = (uint32_t)max;
    if (count > DEVICE_MAX_MODES) {
        axl_warning("console device: %u text modes, mirroring the first %u "
                    "(the rest drop out of the aggregate while we are installed)",
                    count, (uint32_t)DEVICE_MAX_MODES);
        count = DEVICE_MAX_MODES;
    }

    uint32_t usable = 0;
    for (uint32_t i = 0; i < count; i++) {
        UINTN c = 0;
        UINTN r = 0;
        if (!EFI_ERROR(gST->ConOut->QueryMode(gST->ConOut, (UINTN)i, &c, &r))
            && c > 0 && r > 0)
        {
            d->modes[i].cols   = (uint32_t)c;
            d->modes[i].rows   = (uint32_t)r;
            d->modes[i].usable = true;
            usable++;
        } else {
            d->modes[i].cols   = 0;
            d->modes[i].rows   = 0;
            d->modes[i].usable = false;
        }
    }

    /* All holes would advertise a mode list nothing can select; fall back. */
    d->mode_count = (usable > 0) ? count : 0;
}

/* Build our SimpleTextOut + Mode + device path and install them on a fresh handle
   TOGETHER WITH gEfiConsoleOutDeviceGuid — we tag ourselves directly, so
   ConSplitter binds us with no ConOut-variable read. */
static bool
publish_console_device(AxlConsoleDevice *d)
{
    /* Passthrough mirrors the physical list so ConSplitter's by-geometry
       intersection keeps every mode; the take-over case owns the grid and
       advertises the one geometry it resolved. */
    d->my_mode.MaxMode       = (d->passthrough && d->mode_count > 0)
                                   ? (INT32)d->mode_count : 1;
    d->my_mode.Mode          = 0;
    if (d->passthrough && d->mode_count > 0
        && gST->ConOut != NULL && gST->ConOut->Mode != NULL)
    {
        /* Start on the mode the console is already in, so joining the fan-out
           does not look like a mode change to anything above us. */
        INT32 cur = gST->ConOut->Mode->Mode;
        if (cur >= 0 && (uint32_t)cur < d->mode_count && d->modes[cur].usable) {
            d->my_mode.Mode = cur;
        }
    }
    d->my_mode.Attribute     = 0x07;     /* light gray on black */
    d->my_mode.CursorColumn  = 0;
    d->my_mode.CursorRow     = 0;
    d->my_mode.CursorVisible = TRUE;

    d->my_conout.Reset             = dev_reset;
    d->my_conout.OutputString      = dev_output_string;
    d->my_conout.TestString        = dev_test_string;
    d->my_conout.QueryMode         = dev_query_mode;
    d->my_conout.SetMode           = dev_set_mode;
    d->my_conout.SetAttribute      = dev_set_attribute;
    d->my_conout.ClearScreen       = dev_clear_screen;
    d->my_conout.SetCursorPosition = dev_set_cursor;
    d->my_conout.EnableCursor      = dev_enable_cursor;
    d->my_conout.Mode              = &d->my_mode;

    d->my_devpath.vendor.Header.Type      = HARDWARE_DEVICE_PATH;
    d->my_devpath.vendor.Header.SubType   = HW_VENDOR_DP;
    d->my_devpath.vendor.Header.Length[0] = (uint8_t)sizeof(VENDOR_DEVICE_PATH);
    d->my_devpath.vendor.Header.Length[1] = 0;
    d->my_devpath.vendor.Guid             = DEVICE_VENDOR_GUID;
    d->my_devpath.end.Type      = END_DEVICE_PATH_TYPE;
    d->my_devpath.end.SubType   = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    d->my_devpath.end.Length[0] = (uint8_t)sizeof(EFI_DEVICE_PATH_PROTOCOL);
    d->my_devpath.end.Length[1] = 0;

    d->my_handle = NULL;
    EFI_STATUS st = gBS->InstallMultipleProtocolInterfaces(
        &d->my_handle,
        &gEfiDevicePathProtocolGuid,          &d->my_devpath,
        &gEfiSimpleTextOutputProtocolGuid,       &d->my_conout,
        &gEfiConsoleOutDeviceGuid,             NULL,
        NULL);
    if (EFI_ERROR(st)) {
        return false;
    }
    d->installed = true;
    return true;
}

/* Evict every OTHER gEfiConsoleOutDeviceGuid-tagged handle from the fan-out by
   uninstalling the tag — the core DisconnectControllers ConSplitter from it, so
   ConSplitter Stop deletes it from the fan-out. The evicted handles are recorded
   so uninstall can re-tag them. GraphicsConsole's SimpleTextOut and the GOP
   producer are untouched. */
static void
evict_other_conout_devices(AxlConsoleDevice *d)
{
    UINTN       n       = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS  st = gBS->LocateHandleBuffer(ByProtocol, &gEfiConsoleOutDeviceGuid,
                                             NULL, &n, &handles);
    if (EFI_ERROR(st) || handles == NULL) {
        return;
    }
    for (UINTN i = 0; i < n; i++) {
        if (handles[i] == d->my_handle) {
            continue;
        }
        st = gBS->UninstallProtocolInterface(handles[i],
                                             &gEfiConsoleOutDeviceGuid, NULL);
        if (!EFI_ERROR(st)) {
            if (d->evicted_count < DEVICE_MAX_EVICTED) {
                d->evicted[d->evicted_count++] = handles[i];
            } else {
                /* Evicted but not recorded: uninstall cannot re-tag it, so this
                   firmware console stays gone. n is 2-4 on real platforms, so the
                   cap is generous, but a surplus must not vanish silently. */
                axl_warning("console device: >%d console-out handles evicted; "
                            "the surplus will not be restored on uninstall",
                            DEVICE_MAX_EVICTED);
            }
        }
    }
    gBS->FreePool(handles);
}

/* Re-tag the handles we evicted and reconnect them, so ConSplitter re-adds the
   firmware console (GraphicsConsole) to the fan-out — the inverse of eviction. */
static void
restore_evicted_conout_devices(AxlConsoleDevice *d)
{
    for (uint32_t i = 0; i < d->evicted_count; i++) {
        EFI_HANDLE h = d->evicted[i];
        EFI_STATUS st = gBS->InstallMultipleProtocolInterfaces(
            &h, &gEfiConsoleOutDeviceGuid, NULL, NULL);
        if (!EFI_ERROR(st)) {
            axl_driver_connect_handle(h);   /* == ConnectController(h,NULL,NULL,TRUE) */
        }
    }
    d->evicted_count = 0;
}

// ---------------------------------------------------------------------------
// Input relay publication + fan-out surgery (the ConIn mirror of the ConOut
// path above). Only run when cfg.take_input; symmetric self-tag + evict + restore.
// ---------------------------------------------------------------------------

/* Wire the ConIn/ConInEx vtables + WaitForKey events, then install them (with the
   gEfiConsoleInDeviceGuid tag) on our EXISTING handle, so the same ConnectController
   binds ConSplitter's console-in aggregate to us. Returns false (protocols/events
   cleaned up) on failure. */
/* Wire the ConIn/ConInEx method vtables (no events, no install) — shared by
   publish_console_in and the headless test seam. */
static void
dev_wire_conin(AxlConsoleDevice *d)
{
    d->my_conin.Reset         = dev_in_reset;
    d->my_conin.ReadKeyStroke = dev_in_read_key;

    d->my_coninex.Reset               = (void *)dev_inex_reset;
    d->my_coninex.ReadKeyStrokeEx     = dev_inex_read_key;
    d->my_coninex.SetState            = (void *)dev_inex_set_state;
    d->my_coninex.RegisterKeyNotify   = dev_inex_register_notify;
    d->my_coninex.UnregisterKeyNotify = dev_inex_unregister_notify;
}

static bool
publish_console_in(AxlConsoleDevice *d)
{
    dev_wire_conin(d);

    if (EFI_ERROR(gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_CALLBACK,
                                   dev_wait_key_cb, NULL, &d->wait_key))) {
        return false;
    }
    d->my_conin.WaitForKey = d->wait_key;

    if (EFI_ERROR(gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_CALLBACK,
                                   dev_wait_key_cb, NULL, &d->wait_key_ex))) {
        gBS->CloseEvent(d->wait_key);
        d->wait_key = NULL;
        return false;
    }
    d->my_coninex.WaitForKeyEx = d->wait_key_ex;

    /* The engine signals both events on every ring push (inject or read loop). */
    axl_console_input_set_wait_events(&d->in, d->wait_key, d->wait_key_ex);

    EFI_STATUS st = gBS->InstallMultipleProtocolInterfaces(
        &d->my_handle,
        &gEfiSimpleTextInputProtocolGuid,   &d->my_conin,
        &gEfiSimpleTextInputExProtocolGuid, &d->my_coninex,
        &gEfiConsoleInDeviceGuid,           NULL,
        NULL);
    if (EFI_ERROR(st)) {
        gBS->CloseEvent(d->wait_key);
        gBS->CloseEvent(d->wait_key_ex);
        d->wait_key = NULL;
        d->wait_key_ex = NULL;
        return false;
    }
    d->input_published = true;
    return true;
}

/* Evict every OTHER gEfiConsoleInDeviceGuid-tagged handle (the raw firmware
   keyboard) so ConSplitter fans console input ONLY to us -- the mirror of the
   ConOut eviction, and what prevents double-delivery. Records each evicted
   keyboard's stable HANDLE (its ConInEx protocol stays installed; only the tag is
   removed) so the read_physical timer can re-resolve its ConInEx each tick and poll
   it, and so it can be re-admitted on uninstall. */
static void
evict_other_conin_devices(AxlConsoleDevice *d)
{
    UINTN       n       = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS  st = gBS->LocateHandleBuffer(ByProtocol, &gEfiConsoleInDeviceGuid,
                                             NULL, &n, &handles);
    if (EFI_ERROR(st) || handles == NULL) {
        return;
    }
    for (UINTN i = 0; i < n; i++) {
        if (handles[i] == d->my_handle) {
            continue;
        }
        /* Store only the stable handle; dev_read_timer_cb re-resolves the live
           SimpleTextInputEx from it each tick (a cached interface can dangle). */
        if (!EFI_ERROR(gBS->UninstallProtocolInterface(handles[i],
                           &gEfiConsoleInDeviceGuid, NULL))) {
            if (d->evicted_in_count < DEVICE_MAX_EVICTED) {
                d->evicted_in[d->evicted_in_count] = handles[i];
                d->evicted_in_count++;
            } else {
                axl_warning("console device: >%d console-in handles evicted; the "
                            "surplus will not be restored on uninstall",
                            DEVICE_MAX_EVICTED);
            }
        }
    }
    gBS->FreePool(handles);
}

/* Re-tag + reconnect the evicted keyboards — the inverse of eviction, so
   ConSplitter re-admits the firmware keyboard when the relay is torn down. */
static void
restore_evicted_conin_devices(AxlConsoleDevice *d)
{
    for (uint32_t i = 0; i < d->evicted_in_count; i++) {
        EFI_HANDLE h = d->evicted_in[i];
        EFI_STATUS st = gBS->InstallMultipleProtocolInterfaces(
            &h, &gEfiConsoleInDeviceGuid, NULL, NULL);
        if (!EFI_ERROR(st)) {
            axl_driver_connect_handle(h);
        }
    }
    d->evicted_in_count = 0;
}

// ---------------------------------------------------------------------------
// Pointer take-over (take_pointer). Unlike the ConIn/ConOut eviction — which
// removes only the gEfiConsole*DeviceGuid TAG and leaves the protocol installed
// for the read loop — a guest locates the pointer by gEfiSimplePointerProtocolGuid
// ITSELF, so hiding it means uninstalling that protocol. The interface stays valid
// (its producer is not stopped), so it is cached for the consumer and reinstalled
// on teardown.
// ---------------------------------------------------------------------------

/* Evict ONE SimplePointer handle: cache its interface, then remove the protocol
   from the database so a guest can no longer locate it. Returns true when evicted
   (recorded for restore), false if the handle has no pointer / the uninstall fails
   / the cache is full. Factored out so a unit test drives it on a synthetic handle
   without the live LocateHandleBuffer sweep. */
static bool
evict_one_pointer_handle(AxlConsoleDevice *d, EFI_HANDLE h)
{
    /* Capacity-check BEFORE uninstalling: an evicted pointer we cannot record could
       never be restored, so leave a surplus findable rather than orphan it. */
    if (d->evicted_ptr_count >= DEVICE_MAX_EVICTED) {
        axl_warning("console device: >%d pointers present; the surplus stays visible "
                    "to guests", DEVICE_MAX_EVICTED);
        return false;
    }
    void *iface = NULL;
    if (EFI_ERROR(gBS->HandleProtocol(h, &gEfiSimplePointerProtocolGuid, &iface))
        || iface == NULL) {
        return false;   /* no pointer on this handle */
    }
    EFI_STATUS st = gBS->UninstallProtocolInterface(h, &gEfiSimplePointerProtocolGuid,
                                                    iface);
    if (EFI_ERROR(st)) {
        /* A driver holding the pointer BY_DRIVER (e.g. ConSplitter aggregation) makes
           this EFI_ACCESS_DENIED; the guest then keeps a locatable pointer. Warn so a
           silently-ineffective take-over on such firmware is diagnosable. */
        axl_warning("console device: SimplePointer uninstall refused (0x%llx); the "
                    "guest keeps this pointer", (unsigned long long)st);
        return false;
    }
    d->evicted_ptr[d->evicted_ptr_count]       = h;
    d->evicted_ptr_iface[d->evicted_ptr_count] = iface;
    d->evicted_ptr_count++;
    return true;
}

/* Uninstall every EFI_SIMPLE_POINTER_PROTOCOL so guests run mouse-free; cache the
   interfaces for the consumer. Skips our own handle (we never publish a pointer).
   Physical handles are cached FIRST and gST->ConsoleInHandle (the ConSplitter
   pointer aggregator) LAST, so a consumer polling the cache in order reads the
   physical device — which carries the scroll wheel (RelativeMovementZ) — before the
   aggregator, whose GetState consumes the child's state but drops the wheel. This is
   the same ordering axl_input_attach_mouse applies to its bound handles. */
static void
evict_pointer_devices(AxlConsoleDevice *d)
{
    UINTN       n       = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS  st = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimplePointerProtocolGuid,
                                             NULL, &n, &handles);
    if (EFI_ERROR(st) || handles == NULL) {
        return;   /* no pointer present -> nothing to hide */
    }
    EFI_HANDLE con_in = (gST != NULL) ? gST->ConsoleInHandle : NULL;
    for (UINTN i = 0; i < n; i++) {   /* physical devices first */
        if (handles[i] != d->my_handle && handles[i] != con_in) {
            evict_one_pointer_handle(d, handles[i]);
        }
    }
    for (UINTN i = 0; i < n; i++) {   /* the ConsoleInHandle aggregator last */
        if (handles[i] == con_in && con_in != d->my_handle) {
            evict_one_pointer_handle(d, handles[i]);
        }
    }
    gBS->FreePool(handles);
}

/* Reinstall every cached pointer interface, so a guest that starts after teardown
   can locate its mouse again — the inverse of the eviction. */
static void
restore_pointer_devices(AxlConsoleDevice *d)
{
    for (uint32_t i = 0; i < d->evicted_ptr_count; i++) {
        EFI_HANDLE h  = d->evicted_ptr[i];
        EFI_STATUS st = gBS->InstallProtocolInterface(&h, &gEfiSimplePointerProtocolGuid,
                                                      EFI_NATIVE_INTERFACE,
                                                      d->evicted_ptr_iface[i]);
        if (EFI_ERROR(st)) {
            /* The pointer stays hidden from guests -- surface it rather than leave a
               silently mouse-less system after the take-over ends. */
            axl_warning("console device: could not restore SimplePointer on teardown "
                        "(0x%llx); it stays hidden from guests", (unsigned long long)st);
        }
    }
    d->evicted_ptr_count = 0;
}

static EFI_STATUS EFIAPI
dev_proxy_reset(EFI_SIMPLE_POINTER_PROTOCOL *This, BOOLEAN ext)
{
    AxlConsoleDevice *d = g_dev;
    (void)This;
    for (uint32_t i = 0; i < d->evicted_ptr_count; i++) {
        EFI_SIMPLE_POINTER_PROTOCOL *sp = d->evicted_ptr_iface[i];
        if (sp != NULL && sp->Reset != NULL) {
            (void)sp->Reset(sp, ext);
        }
    }
    return EFI_SUCCESS;
}

/* Are we running at TPL_APPLICATION? WaitForEvent is only legal there; a caller at a
   raised TPL (unusual for GetState, but be safe) gets a non-blocking read instead of
   an assert. Reads the current TPL without changing it. */
static bool
at_tpl_application(void)
{
    EFI_TPL cur = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    gBS->RestoreTPL(cur);
    return cur == TPL_APPLICATION;
}

/* Forward the first PHYSICAL cached pointer reporting real activity (movement or a
   button). EFI_NOT_READY otherwise. Two guards matter:
     - Skip the gST->ConsoleInHandle aggregate. Uninstalling ConSplitter's own
       SimplePointer can leave its interface unsafe to call; only the physical
       producers' interfaces (UsbMouseDxe et al.) stay valid across the eviction.
     - Treat a zeroed / no-op SUCCESS as "no input". A guest (edit) would otherwise
       read every idle poll as mouse activity and starve its keyboard read. */
static EFI_STATUS
proxy_forward_real(AxlConsoleDevice *d, EFI_SIMPLE_POINTER_STATE *State)
{
    EFI_HANDLE con_in = (gST != NULL) ? gST->ConsoleInHandle : NULL;
    for (uint32_t i = 0; i < d->evicted_ptr_count; i++) {
        if (d->evicted_ptr[i] == con_in) {
            continue;   /* the aggregator is unreliable post-eviction; physical only */
        }
        EFI_SIMPLE_POINTER_PROTOCOL *sp = d->evicted_ptr_iface[i];
        EFI_SIMPLE_POINTER_STATE     s  = {0};
        if (sp != NULL && sp->GetState != NULL && sp->GetState(sp, &s) == EFI_SUCCESS
            && (s.RelativeMovementX != 0 || s.RelativeMovementY != 0
                || s.RelativeMovementZ != 0 || s.LeftButton || s.RightButton)) {
            *State = s;
            return EFI_SUCCESS;   /* real movement / button -> the guest cursor tracks */
        }
    }
    return EFI_NOT_READY;
}

/* The proxy GetState: yield the CPU until a real pointer has input or a short timer
   elapses, then forward the first real pointer that has data. A guest polling this in
   a tight loop (edit) idles instead of spinning; a real mouse move still reaches it. */
static EFI_STATUS EFIAPI
dev_proxy_getstate(EFI_SIMPLE_POINTER_PROTOCOL *This, EFI_SIMPLE_POINTER_STATE *State)
{
    AxlConsoleDevice *d = g_dev;
    (void)This;

    if (d->proxy_timer != NULL && at_tpl_application()) {
        /* Wait on the timer ALONE -> the guest's GetState poll HLTs for the period and
           its loop idles the CPU. We deliberately do NOT add a pointer's WaitForInput
           to the wait set: some pointers (a VNC/relative usb-mouse; the ConSplitter
           aggregate) keep that event effectively always-signaled, which would make
           WaitForEvent return instantly every call and defeat the yield (the observed
           ~90% CPU). Polling on a fixed timer forwards movement with <= the period of
           latency, which is imperceptible and, crucially, always idles. */
        gBS->SetTimer(d->proxy_timer, TimerRelative, 40000ULL /*100ns units = 4ms*/);
        UINTN idx = 0;
        (void)gBS->WaitForEvent(1, &d->proxy_timer, &idx);
    }

    return proxy_forward_real(d, State);
}

/* Install the yielding proxy on a fresh handle so a guest locates it in place of the
   real pointers we evicted. No-op (and harmless) when nothing was evicted. */
static void
install_pointer_proxy(AxlConsoleDevice *d)
{
    if (d->evicted_ptr_count == 0) {
        return;   /* no real pointer was present -> nothing for a proxy to stand in for */
    }
    if (EFI_ERROR(gBS->CreateEvent(EVT_TIMER, TPL_APPLICATION, NULL, NULL,
                                   &d->proxy_timer))) {
        d->proxy_timer = NULL;   /* proxy still works, just non-yielding (no idle) */
    }
    d->proxy_sp.Reset        = dev_proxy_reset;
    d->proxy_sp.GetState     = dev_proxy_getstate;
    d->proxy_sp.WaitForInput = NULL;   /* guests poll GetState; we do the waiting inside it */
    d->proxy_sp.Mode         = &d->proxy_mode;
    d->proxy_mode.ResolutionX = 1;
    d->proxy_mode.ResolutionY = 1;
    d->proxy_mode.ResolutionZ = 1;
    d->proxy_mode.LeftButton  = TRUE;
    d->proxy_mode.RightButton = TRUE;
    d->proxy_handle = NULL;
    if (EFI_ERROR(gBS->InstallProtocolInterface(&d->proxy_handle,
                      &gEfiSimplePointerProtocolGuid, EFI_NATIVE_INTERFACE,
                      &d->proxy_sp))) {
        axl_warning("console device: could not install the pointer proxy; guests run "
                    "mouse-free (still no stuck cursor)");
        if (d->proxy_timer != NULL) {
            gBS->CloseEvent(d->proxy_timer);
            d->proxy_timer = NULL;
        }
        return;
    }
    d->proxy_installed = true;
}

/* Remove the proxy (before restoring the real pointers). */
static void
uninstall_pointer_proxy(AxlConsoleDevice *d)
{
    if (d->proxy_installed) {
        EFI_STATUS st = gBS->UninstallProtocolInterface(d->proxy_handle,
                            &gEfiSimplePointerProtocolGuid, &d->proxy_sp);
        if (EFI_ERROR(st)) {
            axl_warning("console device: could not uninstall the pointer proxy "
                        "(0x%llx); it lingers in the handle database",
                        (unsigned long long)st);
        }
        d->proxy_installed = false;
    }
    if (d->proxy_timer != NULL) {
        gBS->CloseEvent(d->proxy_timer);
        d->proxy_timer = NULL;
    }
}

/* Start the periodic raw-keyboard read loop (read_physical). Returns false (event
   cleaned up) on failure — the caller then downgrades to inject-only. */
static bool
start_input_read_loop(AxlConsoleDevice *d)
{
    uint32_t period = d->input_poll_ms != 0 ? d->input_poll_ms : DEVICE_DEFAULT_POLL_MS;
    if (EFI_ERROR(gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                   dev_read_timer_cb, NULL, &d->read_timer))) {
        return false;
    }
    /* SetTimer's TriggerTime is in 100ns units. */
    if (EFI_ERROR(gBS->SetTimer(d->read_timer, TimerPeriodic,
                                (UINT64)period * 10000ULL))) {
        gBS->CloseEvent(d->read_timer);
        d->read_timer = NULL;
        return false;
    }
    return true;
}

/* Cancel + close the read loop timer. Called FIRST on teardown so its TPL_CALLBACK
   callback can't preempt the rest and poll a keyboard we're re-admitting / freeing. */
static void
stop_input_read_loop(AxlConsoleDevice *d)
{
    if (d->read_timer != NULL) {
        gBS->SetTimer(d->read_timer, TimerCancel, 0);
        gBS->CloseEvent(d->read_timer);
        d->read_timer = NULL;
    }
}

/* Uninstall our ConIn protocols + close the wait events. Assumes the read loop is
   stopped and (on the success path) our handle has already been
   DisconnectController'd, so ConSplitter no longer references our ConIn structs. */
static void
teardown_console_in(AxlConsoleDevice *d)
{
    if (d->input_published) {
        gBS->UninstallMultipleProtocolInterfaces(
            d->my_handle,
            &gEfiSimpleTextInputProtocolGuid,   &d->my_conin,
            &gEfiSimpleTextInputExProtocolGuid, &d->my_coninex,
            &gEfiConsoleInDeviceGuid,           NULL,
            NULL);
        d->input_published = false;
    }
    if (d->wait_key != NULL) {
        gBS->CloseEvent(d->wait_key);
        d->wait_key = NULL;
    }
    if (d->wait_key_ex != NULL) {
        gBS->CloseEvent(d->wait_key_ex);
        d->wait_key_ex = NULL;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int
axl_console_device_install(const AxlConsoleOps *ops, void *user,
                           const AxlConsoleDeviceConfig *cfg, AxlConsoleDevice **out)
{
    if (out != NULL) {
        *out = NULL;
    }
    if (out == NULL || ops == NULL || cfg == NULL) {
        return AXL_ERR;
    }
    if (g_dev != NULL) {
        axl_warning("console device already installed");
        return AXL_ERR;
    }
    /* Co-painting requires physical geometry. Two consoles drawing one screen must
       agree on the grid: we advertise a single mode, ConSplitter intersects it with
       GraphicsConsole's, and a size the firmware console does not share leaves the
       aggregate describing a grid one of us is not drawing — which is also exactly
       the stale-ConsoleLogger deadloop (RowsPerScreen) the eviction path needs its
       SetMode re-sync to avoid. Refuse rather than half-work: a caller asking for
       both wants something incoherent, and failing loudly beats a garbled local
       display or a firmware hang. cols/rows 0 resolves to the physical mode. */
    if (cfg->passthrough_local && (cfg->cols != 0 || cfg->rows != 0)) {
        axl_warning("console device: passthrough_local requires physical geometry "
                    "(cols/rows must be 0); refusing %ux%u",
                    (unsigned)cfg->cols, (unsigned)cfg->rows);
        return AXL_ERR;
    }

    AxlConsoleDevice *d = axl_calloc(1, sizeof(*d));
    if (d == NULL) {
        return AXL_ERR;
    }
    d->take_input    = cfg->take_input;
    d->take_pointer  = cfg->take_pointer;
    d->passthrough   = cfg->passthrough_local;
    d->installing    = true;
    d->read_physical = cfg->take_input && cfg->read_physical;
    d->input_poll_ms = cfg->input_poll_ms;
    d->debounce_ms   = cfg->debounce_ms;
    d->min_gap_ms    = cfg->min_gap_ms;
    d->key_filter      = cfg->key_filter;
    d->key_filter_user = cfg->key_filter_user;
    resolve_geometry(d, cfg);

    /* Co-painting: mirror the physical mode list so ConSplitter's by-geometry
       intersection keeps every mode selectable. MUST run before we publish --
       once we are a member of the aggregate, its list is the intersection with
       ours and reading it back would be circular. */
    if (d->passthrough) {
        snapshot_physical_modes(d);
    }

    /* Publish + connect + evict, then bind the engine to the owned Mode. */
    if (!publish_console_device(d)) {
        axl_free(d);
        return AXL_ERR;
    }
    axl_console_emit_init(&d->emit, ops, user, &d->my_mode, cfg->auto_alt_screen);

    g_dev = d;   /* live before ConnectController (or any protocol-notify) can call us */

    /* Input relay: publish our ConIn/ConInEx on the SAME handle BEFORE
       ConnectController, so the one recursive connect binds both ConSplitter
       aggregates (console-out and console-in) to us. */
    if (cfg->take_input) {
        axl_console_input_init(&d->in);
        if (!publish_console_in(d)) {
            axl_warning("console device: could not publish the input relay");
            g_dev = NULL;
            gBS->UninstallMultipleProtocolInterfaces(
                d->my_handle,
                &gEfiDevicePathProtocolGuid,       &d->my_devpath,
                &gEfiSimpleTextOutputProtocolGuid, &d->my_conout,
                &gEfiConsoleOutDeviceGuid,         NULL,
                NULL);
            axl_free(d);
            return AXL_ERR;
        }
    }

    /* axl_driver_connect_handle == ConnectController(handle, NULL, NULL, TRUE),
       the recursive connect the cv2 spike proved binds ConSplitter to us. */
    if (axl_driver_connect_handle(d->my_handle) != AXL_OK) {
        /* ConSplitter didn't bind us: undo the publish and fail. Clear g_dev
           BEFORE uninstalling (safe here: a failed connect bound nothing, so no
           disconnect callback can fire into a dev_* method). The success-path
           uninstall clears g_dev AFTER, where a live disconnect could still call
           us -- the asymmetry is deliberate. */
        g_dev = NULL;
        if (cfg->take_input) {
            teardown_console_in(d);   /* uninstall ConIn + close wait events */
        }
        gBS->UninstallMultipleProtocolInterfaces(
            d->my_handle,
            &gEfiDevicePathProtocolGuid,    &d->my_devpath,
            &gEfiSimpleTextOutputProtocolGuid, &d->my_conout,
            &gEfiConsoleOutDeviceGuid,       NULL,
            NULL);
        axl_free(d);
        return AXL_ERR;
    }

    /* Evict the firmware console-out devices so we are the SOLE console — unless the
       consumer only OBSERVES the console and needs the local display to keep working
       (passthrough_local), in which case ConSplitter simply fans to both of us. This
       is the ONLY step that silences GraphicsConsole; steps 1-2 above (publish +
       self-tag, ConnectController) are what deliver the op stream, and they ran
       either way. */
    if (!cfg->passthrough_local) {
        evict_other_conout_devices(d);
    }

    /* Become the SOLE console-in device (evict the raw keyboard), then start the
       optional physical read loop. A read-loop failure downgrades to inject-only
       rather than failing the whole take-over. */
    if (cfg->take_input) {
        evict_other_conin_devices(d);
        if (d->read_physical && !start_input_read_loop(d)) {
            axl_warning("console device: read loop unavailable; input is inject-only");
            d->read_physical = false;
        }
    }

    /* Take over the pointer: evict the real SimplePointer(s) so guests can't drain the
       same consume-once device, then interpose a yielding proxy in their place so a
       guest's GetState poll (edit) idles the CPU while still tracking the mouse. The
       consumer keeps direct access to the cached reals. Independent of take_input. */
    if (cfg->take_pointer) {
        evict_pointer_devices(d);
        install_pointer_proxy(d);
    }

    /* Re-sync any console logger stacked above us to OUR geometry. A UEFI shell
       wraps gST->ConOut in a ConsoleLogger that caches its scrollback row count
       (RowsPerScreen) from QueryMode at shell start and refreshes it ONLY when a
       SetMode is routed through gST->ConOut. Our take-over just changed the console
       geometry via ConnectController + eviction WITHOUT such a SetMode, so the
       logger keeps a stale row count; when that stale count is smaller than our
       grid, its history bound (RowsPerScreen*ScreenCount-1) is exceeded once the
       shell scrolls and it asserts / CpuDeadLoops (ShellPkg ConsoleLogger.c:489). A
       single SetMode round-trip forces ConsoleLoggerSetMode -> ResetBuffers to
       re-read QueryMode at our geometry, closing the window. We advertise one mode
       (0), which ConSplitter exposes post-eviction, so SetMode(0) maps to us. When
       nothing wraps gST->ConOut this is a harmless re-affirm of the current mode. */
    /* Skipped under passthrough_local, deliberately. The staleness this defends
       against is caused by the EVICTION changing the console geometry behind the
       logger's back — and we did not evict. Our advertised mode is the physical one
       (the guard above enforces that), so the aggregate geometry is unchanged and
       there is nothing to re-sync. Forcing SetMode(0) anyway would be the riskier
       choice: mode 0 of the co-painted aggregate is not ours to define, so a console
       currently in a non-80x25 mode could be visibly resized by our arrival — the
       opposite of "leave the local display alone", which is the whole point here. */
    if (!cfg->passthrough_local
        && gST->ConOut != NULL && gST->ConOut->Mode != NULL
        && gST->ConOut->SetMode != NULL) {
        /* Route through the backend's SetMode wrapper (gST->ConOut->SetMode(., 0))
           rather than the raw protocol call. */
        if (axl_backend_console_text_set_mode(0) != AXL_OK) {
            /* Can't happen in the take-over path (post-eviction MaxMode==1, mode 0
               valid, our dev_set_mode(0) returns success); but if it ever did, the
               stale-cache deadloop window would silently reopen -- make that
               observable rather than a mystery hang for the next debugger. */
            axl_warning("console device: ConOut SetMode re-sync failed");
        }
    }

    /* Report the cell rule once, now that our output path is the console. */
    axl_console_emit_report_cell_rule(&d->emit);

    /* Everything the firmware might have re-mode'd us to during the connect has
       landed in d->cols/d->rows by now, and the caller is about to hold the
       device -- so resize ops from here on are safe to deliver. */
    d->installing = false;

    *out = d;
    axl_info("console device installed (%ux%u, %u evicted)",
             d->cols, d->rows, d->evicted_count);
    return AXL_OK;
}

void
axl_console_device_uninstall(AxlConsoleDevice *d)
{
    if (d == NULL || g_dev != d) {
        return;
    }
    /* Stop the read loop FIRST so its TPL_CALLBACK timer can't preempt teardown and
       poll a keyboard we are about to re-admit / free. NULL-safe when read_physical
       was off. */
    stop_input_read_loop(d);

    /* Disconnect ConSplitter from OUR handle BEFORE re-adding the firmware console.
       Two reasons, order matters:

       1. Mode reconstruction. Re-adding GraphicsConsole runs ConSplitter's
          AddDevice, which reconstructs the aggregate's text modes across every
          member and SetMode()s the result. If OUR device is still a member, it
          advertises a single non-80x25 geometry (e.g. 160x50); the reconstruction
          picks a PreferMode neither device can honor, falls back to the 80x25
          BaseMode, and — because an EVICTING device advertises one non-80x25 mode
          — that SetMode fails too, tripping ASSERT(!EFI_ERROR) at
          (ConsplitterSetConsoleOutMode, ConSplitter.c:2983 -> CpuDeadLoop under
          DEBUG OVMF; a silent wedge under
          RELEASE). Dropping ourselves first means GraphicsConsole is re-added into
          an aggregate that contains only firmware consoles, so a common mode exists.
          (fbcon at 160x50 tripped this on self-restore; the 80x25 restore smokes did
          not, since 80x25 IS the fallback. Guards: test-console-device-qemu.sh
          Scenarios 5b wide-restore + 5c cycle, both non-80x25.)

       2. UAF guard (the original reason). ConSplitter opened our SimpleTextOut /
          SimpleTextInputEx BY_DRIVER and holds raw pointers to them in its fan-out
          lists; UninstallMultipleProtocolInterfaces alone does not reliably make it
          drop them, so the disconnect's ConSplitter Stop must run before we free d.

       On failure ConSplitter may still reference our structs, so leak the instance
       (leak-over-use-after-free), keep g_dev set so a re-install is refused, and
       leave d->installed so a retry can resume. */
    bool self_dropped = false;
    if (d->installed) {
        if (axl_driver_disconnect_handle(d->my_handle) != AXL_OK) {
            axl_warning("console device uninstall: could not disconnect our "
                        "console handle; leaking the instance to avoid a UAF");
            return;
        }
        self_dropped = true;
    }

    /* Re-tag + reconnect the firmware console(s) we evicted, so the local display
       and keyboard come back. Our device is already out of the aggregates, so the
       ConSplitter mode reconstruction above sees only the firmware consoles. */
    restore_evicted_conout_devices(d);
    restore_evicted_conin_devices(d);
    uninstall_pointer_proxy(d);   /* remove our proxy before the reals come back */
    restore_pointer_devices(d);   /* reinstall the SimplePointer(s) we hid from guests */
    if (self_dropped) {
        /* Both aggregates have dropped us: uninstall our ConIn protocols + close the
           wait events, then the ConOut protocols. */
        teardown_console_in(d);
        if (EFI_ERROR(gBS->UninstallMultipleProtocolInterfaces(
                d->my_handle,
                &gEfiDevicePathProtocolGuid,       &d->my_devpath,
                &gEfiSimpleTextOutputProtocolGuid, &d->my_conout,
                &gEfiConsoleOutDeviceGuid,         NULL,
                NULL))) {
            axl_warning("console device uninstall: could not uninstall our "
                        "console protocols; leaking the instance to avoid a UAF");
            return;
        }
        d->installed = false;
    }
    g_dev = NULL;
    axl_free(d);
    axl_info("console device uninstalled");
}

// --- Input relay injection (functional only when take_input=true) ------------
// A key delivered here enters the shared engine's ring exactly like a read-loop
// key, so the guest reads it through our ConIn/ConInEx. Rejected when the device
// is output-only (take_input=false): a caller must not think it owns input it
// doesn't. Mirrors axl_console_tap_inject_*.

int
axl_console_device_inject_key(AxlConsoleDevice *d, uint16_t scan, uint16_t unicode)
{
    return axl_console_device_inject_key_ex(d, scan, unicode, 0, 0);
}

int
axl_console_device_inject_key_ex(AxlConsoleDevice *d, uint16_t scan, uint16_t unicode,
                                 uint32_t shift_state, uint8_t toggle_state)
{
    if (d == NULL || !d->take_input) {
        return AXL_ERR;
    }
    return axl_console_input_inject_key_ex(&d->in, scan, unicode, shift_state,
                                           toggle_state);
}

int
axl_console_device_inject_text(AxlConsoleDevice *d, const char *bytes, size_t len)
{
    if (d == NULL || !d->take_input || bytes == NULL) {
        return AXL_ERR;
    }
    return axl_console_input_inject_text(&d->in, bytes, len);
}

// --- Runtime geometry + session ----------------------------------------------

void
axl_console_device_set_size(AxlConsoleDevice *d, uint32_t cols, uint32_t rows)
{
    if (d == NULL) {
        return;
    }
    /* Under passthrough the grid is not ours to set: the firmware console is
       painting the same screen, and moving only our half of the agreement is the
       desync this mode exists to avoid. Install already refuses an explicit
       geometry for that reason; honouring one here would be the same bug arriving
       later. Reshape through the text-mode API instead -- our mirrored mode list
       makes axl_console_text_set_mode move BOTH consoles together. */
    if (d->passthrough) {
        axl_warning("console device: set_size ignored under passthrough_local "
                    "(use axl_console_text_set_mode to reshape both consoles)");
        return;
    }
    d->cols = (cols != 0) ? cols : DEVICE_DEFAULT_COLS;
    d->rows = (rows != 0) ? rows : DEVICE_DEFAULT_ROWS;
    /* The take-over path's only geometry change, so it is the one a consumer
       binding `resize` needs here -- a mode switch cannot deliver it, the device
       advertising exactly one mode. */
    if (!d->installing) {
        axl_console_emit_resize(&d->emit, d->cols, d->rows);
    }
}

void
axl_console_device_get_size(const AxlConsoleDevice *d, uint32_t *cols, uint32_t *rows)
{
    if (cols != NULL) {
        *cols = (d != NULL) ? d->cols : 0;
    }
    if (rows != NULL) {
        *rows = (d != NULL) ? d->rows : 0;
    }
}

void
axl_console_device_reset(AxlConsoleDevice *d)
{
    if (d == NULL) {
        return;
    }
    axl_console_emit_reset(&d->emit);
}

// --- Pointer take-over accessors ---------------------------------------------

size_t
axl_console_device_pointer_count(const AxlConsoleDevice *d)
{
    return (d == NULL) ? 0 : d->evicted_ptr_count;
}

void *
axl_console_device_pointer_iface(const AxlConsoleDevice *d, size_t index)
{
    if (d == NULL || index >= d->evicted_ptr_count) {
        return NULL;
    }
    return d->evicted_ptr_iface[index];
}

// --- Alt-screen --------------------------------------------------------------

void
axl_console_device_enter_alt_screen(AxlConsoleDevice *d)
{
    if (d != NULL) {
        axl_console_emit_enter_alt_screen(&d->emit);
    }
}

void
axl_console_device_leave_alt_screen(AxlConsoleDevice *d)
{
    if (d != NULL) {
        axl_console_emit_leave_alt_screen(&d->emit);
    }
}

bool
axl_console_device_in_alt_screen(const AxlConsoleDevice *d)
{
    return d != NULL && axl_console_emit_in_alt_screen(&d->emit);
}

// ---------------------------------------------------------------------------
// Test seams for the input relay (no public header). A full install wedges the
// combined unit boot (it wraps the live console), so these drive the REAL ConIn/
// ConInEx read + notify path against a bare, un-installed instance published as
// g_dev -- no protocols installed, no ConnectController, no WaitForKey event.
// Mirrors the tap's _axl_console_tap_test_* seams. Pair _begin with _end.
// ---------------------------------------------------------------------------

AxlConsoleDevice *
_axl_console_device_new_for_test(void)
{
    return axl_calloc(1, sizeof(AxlConsoleDevice));
}

void
_axl_console_device_test_begin(AxlConsoleDevice *d, bool take_input)
{
    if (d == NULL) {
        return;
    }
    d->take_input = take_input;
    axl_console_input_init(&d->in);
    dev_wire_conin(d);   /* same ConIn/ConInEx vtable publish_console_in installs */
    g_dev = d;
}

EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *
_axl_console_device_test_coninex(AxlConsoleDevice *d)
{
    return (d == NULL) ? NULL : &d->my_coninex;
}

EFI_SIMPLE_TEXT_INPUT_PROTOCOL *
_axl_console_device_test_conin(AxlConsoleDevice *d)
{
    return (d == NULL) ? NULL : &d->my_conin;
}

void
_axl_console_device_test_end(AxlConsoleDevice *d)
{
    g_dev = NULL;
    axl_free(d);
}

bool
_axl_console_device_test_evict_one_pointer(AxlConsoleDevice *d, void *handle)
{
    return (d == NULL) ? false : evict_one_pointer_handle(d, (EFI_HANDLE)handle);
}

void
_axl_console_device_test_restore_pointer(AxlConsoleDevice *d)
{
    if (d != NULL) {
        restore_pointer_devices(d);
    }
}

void
_axl_console_device_test_install_proxy(AxlConsoleDevice *d)
{
    if (d != NULL) {
        g_dev = d;   /* the proxy vtable reads g_dev */
        install_pointer_proxy(d);
    }
}

void
_axl_console_device_test_uninstall_proxy(AxlConsoleDevice *d)
{
    if (d != NULL) {
        uninstall_pointer_proxy(d);
    }
}

/* Drive the proxy's forward path (no yield) so a unit test can assert it relays a
   cached real pointer's state without blocking on WaitForEvent. */
int
_axl_console_device_test_proxy_forward(AxlConsoleDevice *d, void *state_out)
{
    if (d == NULL) {
        return -1;
    }
    return (int)proxy_forward_real(d, (EFI_SIMPLE_POINTER_STATE *)state_out);
}

/* Run the device's real read-loop key delivery (key_filter -> gate -> push) on a
   synthetic key, isolated to just this key, and report whether it reached the shell
   ring (true = forwarded, false = consumed by the filter). */
bool
_axl_console_device_test_feed_physical(AxlConsoleDevice *d,
        bool (*filter)(void *, const void *), void *fuser,
        uint16_t scan, uint16_t uni, uint32_t shift)
{
    d->key_filter      = filter;
    d->key_filter_user = fuser;
    axl_console_input_drain(&d->in);
    EFI_KEY_DATA kd = {0};
    kd.Key.ScanCode           = scan;
    kd.Key.UnicodeChar        = uni;
    kd.KeyState.KeyShiftState = shift;
    dev_deliver_physical_key(d, &kd);
    EFI_KEY_DATA out;
    return axl_console_input_pop(&d->in, &out);
}

/* Seed the read loop's evicted-keyboard list with a synthetic handle, mirroring
   real ConIn eviction, so a unit test can drive dev_read_timer_cb against it. */
bool
_axl_console_device_test_add_evicted_conin(AxlConsoleDevice *d, void *handle)
{
    if (d == NULL || d->evicted_in_count >= DEVICE_MAX_EVICTED) {
        return false;
    }
    d->evicted_in[d->evicted_in_count] = (EFI_HANDLE)handle;
    d->evicted_in_count++;
    return true;
}

/* Run one read-loop timer tick (the physical-keyboard drain) so a test can assert
   it re-resolves each evicted keyboard's interface instead of calling a stale one. */
void
_axl_console_device_test_read_tick(AxlConsoleDevice *d)
{
    if (d == NULL) {
        return;
    }
    g_dev = d;
    dev_read_timer_cb(NULL, NULL);
}
