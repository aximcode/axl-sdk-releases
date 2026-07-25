/* kbtune-drv.c — resident ConIn conditioning shim for the UEFI shell.
 *
 * Wraps gST->ConIn (Simple) and the ConsoleInHandle's SimpleTextInputEx with a
 * filter that (a) drops a too-fast same-key repeat (debounce) and (b) spaces out
 * delivery (min-gap), so keystrokes typed at the shell prompt over a laggy BMC/KVM
 * stop bouncing / repeating. Stays resident after the launcher (kbtune) exits, so
 * the setting persists for the shell; the launcher reattaches to re-tune.
 *
 * Reads the real console via ReadKeyStrokeEx (Ex — modifiers preserved,
 * authoritative queue) and presents conditioned survivors on BOTH the wrapped
 * Simple ConIn (what the shell's line reader calls) and the wrapped Ex interface
 * (what editors / HandleProtocol consumers call).
 *
 * NO timer: the conditioning is reactive. The shell blocks in
 * WaitForEvent(WaitForKey), which busy-polls our EVT_NOTIFY_WAIT notify; each poll
 * re-runs the filter, so a min-gap-held key is released once its gap elapses
 * without a dedicated timer event (the same timer-free technique
 * axl-console-mirror.c uses for its physical-keyboard poll). See
 * docs/AXL-KbTune-Design.md Phase 2 = "A".
 *
 * Built with AXL_DRIVER + axl_shared_driver_publish (custom {get,set} vtable, not
 * the SDK {run} vtable). Loaded by the kbtune launcher via
 * axl_shared_driver_locate_sibling (staged beside it; no embed).
 */

#include <axl.h>
#include <uefi/axl-uefi.h>
#include "kbtune-shared.h"

AXL_LOG_DOMAIN("kbtune-drv");

#define SURV_RING 64   /* conditioned keys ready for the consumer */

typedef struct {
    /* Saved originals (restored on unload). */
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL     *orig_conin;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *orig_coninex;

    /* Wrapper protocol structs — gST->ConIn / ConsoleInHandle point into these. */
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL      my_conin;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL   my_coninex;

    EFI_EVENT  wait_key;
    EFI_EVENT  wait_key_ex;
    bool       reinstalled_ex;

    /* Live config (updated via the published vtable's set()). */
    AxlKbTuneConfig cfg;

    /* Conditioning state. */
    AxlKeyDebounce  debounce;
    AxlKeyGate      gate;

    /* Survivor ring: conditioned keys READY for the consumer to read (already
       past debounce AND min-gap metering). */
    EFI_KEY_DATA  surv[SURV_RING];
    UINTN         surv_head;
    UINTN         surv_tail;

    /* Pending ring: keys that survived DEBOUNCE but are still waiting to clear
       the min-gap meter. Draining + debouncing the real queue eagerly (into
       here) — rather than holding it in the firmware queue — is what lets the
       debounce collapse a held key's typematic repeats: they are READ close
       together (so their read-time deltas stay inside the debounce window) even
       though delivery is later spaced by the gate. `gap` marks whether the entry
       is subject to min-gap (navigation/relayed keys bypass it). */
    EFI_KEY_DATA  pend_key[SURV_RING];
    bool          pend_gap[SURV_RING];
    UINTN         pend_head;
    UINTN         pend_tail;
} KbDrv;

static KbDrv      *g_drv;
static AxlHandle   g_handle;

// ---------------------------------------------------------------------------
// Survivor ring (touched from the WaitForKey notify at TPL_CALLBACK and from
// ReadKeyStroke at TPL_APPLICATION — raise to TPL_HIGH_LEVEL for atomicity, as
// axl-console-mirror.c does).
// ---------------------------------------------------------------------------

static bool
surv_pop(KbDrv *d, EFI_KEY_DATA *kd)
{
    EFI_TPL old = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    bool    ok  = (d->surv_head != d->surv_tail);
    if (ok) {
        *kd = d->surv[d->surv_tail];
        d->surv_tail = (d->surv_tail + 1) % SURV_RING;
    }
    gBS->RestoreTPL(old);
    return ok;
}

static void
signal_if_survivors(KbDrv *d)
{
    if (d->surv_head != d->surv_tail) {
        if (d->wait_key != NULL)    { gBS->SignalEvent(d->wait_key); }
        if (d->wait_key_ex != NULL) { gBS->SignalEvent(d->wait_key_ex); }
    }
}

/* Pending ring (post-debounce, pre-gate). Same TPL discipline as the survivor
   ring. `gap` records whether the entry is metered by min-gap. */
static bool
pend_push(KbDrv *d, const EFI_KEY_DATA *kd, bool gap)
{
    EFI_TPL old  = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    UINTN   next = (d->pend_head + 1) % SURV_RING;
    bool    ok   = (next != d->pend_tail);
    if (ok) {
        d->pend_key[d->pend_head] = *kd;
        d->pend_gap[d->pend_head] = gap;
        d->pend_head = next;
    }
    gBS->RestoreTPL(old);
    return ok;
}

/* Move every pending entry into the survivor ring unconditionally (used on a
   config change so buffered, already-debounced keys are delivered, not lost).
   Inlines the survivor push so it takes only one TPL raise. */
static void
pend_flush_to_surv(KbDrv *d)
{
    EFI_TPL old = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    while (d->pend_tail != d->pend_head) {
        UINTN snext = (d->surv_head + 1) % SURV_RING;
        if (snext == d->surv_tail) {
            break;   /* survivor ring full — drop the rest (bounded) */
        }
        d->surv[d->surv_head] = d->pend_key[d->pend_tail];
        d->surv_head = snext;
        d->pend_tail = (d->pend_tail + 1) % SURV_RING;
    }
    gBS->RestoreTPL(old);
}

// ---------------------------------------------------------------------------
// The conditioning pump — pull one real key, filter it, manage the min-gap hold.
// Reactive: called from the WaitForKey notify AND from the wrapped reads, so it
// works for both WaitForEvent consumers (the shell) and poll-only consumers
// (kbtune's own read_key(0)).
// ---------------------------------------------------------------------------

static bool
read_real_ex(KbDrv *d, EFI_KEY_DATA *kd)
{
    if (d->orig_coninex != NULL) {
        EFI_STATUS st = d->orig_coninex->ReadKeyStrokeEx(d->orig_coninex, kd);
        return !EFI_ERROR(st);
    }
    if (d->orig_conin != NULL) {
        axl_memset(kd, 0, sizeof(*kd));
        EFI_STATUS st = d->orig_conin->ReadKeyStroke(d->orig_conin, &kd->Key);
        return !EFI_ERROR(st);
    }
    return false;
}

/* Printable = a visible character; scancode keys (unicode 0) and control codes
   (Backspace 0x08, DEL 0x7f) are navigation/editing keys exempted under
   printable_only. Matches axl_input_key_accept's own notion. */
static bool
key_is_printable(const EFI_KEY_DATA *kd)
{
    uint16_t u = kd->Key.UnicodeChar;
    return u >= 0x20 && u != 0x7f;
}

static void
condition_pump(KbDrv *d)
{
    uint64_t now = axl_time_get_us();

    /* PHASE 1 — drain the ENTIRE real queue now, debouncing as we read. Reading
       eagerly (instead of leaving repeats in the firmware queue behind a held
       key) is what makes debounce work alongside min-gap: a held key's typematic
       repeats are READ back-to-back here, so their read-time deltas stay inside
       the debounce window and collapse to one — even though delivery is later
       spaced by the gate. Survivors go to the pending ring (post-debounce). */
    EFI_KEY_DATA kd;
    while (read_real_ex(d, &kd)) {
        /* Disabled -> transparent relay; partials and (under printable_only)
           navigation/editing keys bypass BOTH conditioners (gap = false). */
        if (!d->cfg.enabled
            || (kd.Key.ScanCode == 0 && kd.Key.UnicodeChar == 0)
            || (d->cfg.printable_only && !key_is_printable(&kd))) {
            pend_push(d, &kd, false);
            continue;
        }
        AxlInputEvent ev = {
            .type         = AXL_INPUT_KEY_DOWN,
            .keycode      = kd.Key.ScanCode,
            .unicode      = kd.Key.UnicodeChar,
            .timestamp_us = now,
        };
        if (!axl_input_key_accept(&d->debounce, &ev)) {
            continue;   /* dropped as a bounce */
        }
        pend_push(d, &kd, true);   /* survived debounce; still gated by min-gap */
    }

    /* PHASE 2 — release pending head(s) into the survivor ring, in FIFO order.
       A non-gap entry releases immediately; a gap entry releases only once the
       min-gap since the last delivered key has elapsed, and holds (stops the
       drain) otherwise so order is preserved. At most one gap key is released
       per pump (gate_mark advances the window past `now`). */
    for (;;) {
        EFI_TPL old = gBS->RaiseTPL(TPL_HIGH_LEVEL);
        if (d->pend_tail == d->pend_head) {
            gBS->RestoreTPL(old);
            break;                     /* nothing pending */
        }
        bool gap = d->pend_gap[d->pend_tail];
        if (gap && now < axl_input_key_gate_ready_at(&d->gate, d->cfg.min_gap_ms)) {
            gBS->RestoreTPL(old);
            break;                     /* head not yet due — hold, preserve order */
        }
        EFI_KEY_DATA out = d->pend_key[d->pend_tail];
        d->pend_tail = (d->pend_tail + 1) % SURV_RING;
        UINTN snext = (d->surv_head + 1) % SURV_RING;
        if (snext != d->surv_tail) {
            d->surv[d->surv_head] = out;
            d->surv_head = snext;
        }
        if (gap) {
            axl_input_key_gate_mark(&d->gate, now);
        }
        gBS->RestoreTPL(old);
    }
}

// ---------------------------------------------------------------------------
// WaitForKey[Ex] notify — reactive: pump, then leave the event signaled only
// while a survivor is actually ready, so a dropped/held key never leaves the
// shell spinning on an empty read.
// ---------------------------------------------------------------------------

static void EFIAPI
wait_key_cb(EFI_EVENT Event, void *Context)
{
    (void)Event;
    (void)Context;
    KbDrv *d = g_drv;
    if (d == NULL) {
        return;
    }
    condition_pump(d);
    signal_if_survivors(d);
}

// ---------------------------------------------------------------------------
// Wrapped ConIn / ConInEx — deliver conditioned survivors only (no fall-through
// to the raw console; that would leak unconditioned keys). Pump first so a
// poll-only reader still drives the filter.
// ---------------------------------------------------------------------------

static EFI_STATUS EFIAPI
wrap_in_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN ext)
{
    (void)This;
    (void)ext;
    KbDrv *d = g_drv;
    if (d != NULL && d->orig_conin != NULL) {
        d->orig_conin->Reset(d->orig_conin, ext);
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_in_read_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
    (void)This;
    KbDrv *d = g_drv;
    if (d == NULL) {
        return EFI_NOT_READY;
    }
    condition_pump(d);
    EFI_KEY_DATA kd;
    if (surv_pop(d, &kd)) {
        *Key = kd.Key;
        signal_if_survivors(d);
        return EFI_SUCCESS;
    }
    return EFI_NOT_READY;
}

static EFI_STATUS EFIAPI
wrap_inex_reset(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, BOOLEAN ext)
{
    (void)This;
    (void)ext;
    KbDrv *d = g_drv;
    if (d != NULL && d->orig_coninex != NULL && d->orig_coninex->Reset != NULL) {
        ((EFI_INPUT_RESET)d->orig_coninex->Reset)(
            (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *)d->orig_coninex, ext);
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_inex_read_key(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *KeyData)
{
    (void)This;
    KbDrv *d = g_drv;
    if (d == NULL) {
        return EFI_NOT_READY;
    }
    condition_pump(d);
    EFI_KEY_DATA kd;
    if (surv_pop(d, &kd)) {
        *KeyData = kd;
        signal_if_survivors(d);
        return EFI_SUCCESS;
    }
    return EFI_NOT_READY;
}

/* SetState is void* in the AXL Ex struct — pass through to the real one. */
static EFI_STATUS EFIAPI
wrap_inex_set_state(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, void *toggle_state)
{
    (void)This;
    KbDrv *d = g_drv;
    if (d != NULL && d->orig_coninex != NULL && d->orig_coninex->SetState != NULL) {
        typedef EFI_STATUS (EFIAPI *SetStateFn)(
            EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *, void *);
        return ((SetStateFn)d->orig_coninex->SetState)(d->orig_coninex, toggle_state);
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_inex_register_notify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                          EFI_KEY_DATA *KeyData,
                          EFI_KEY_NOTIFY_FUNCTION fn,
                          void **NotifyHandle)
{
    (void)This;
    KbDrv *d = g_drv;
    if (d != NULL && d->orig_coninex != NULL
        && d->orig_coninex->RegisterKeyNotify != NULL) {
        return d->orig_coninex->RegisterKeyNotify(d->orig_coninex, KeyData, fn,
                                                  NotifyHandle);
    }
    if (NotifyHandle != NULL) {
        *NotifyHandle = (void *)(uintptr_t)0x1;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
wrap_inex_unregister_notify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, void *handle)
{
    (void)This;
    KbDrv *d = g_drv;
    if (d != NULL && d->orig_coninex != NULL
        && d->orig_coninex->UnregisterKeyNotify != NULL) {
        return d->orig_coninex->UnregisterKeyNotify(d->orig_coninex, handle);
    }
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Config application + the published {get,set} vtable
// ---------------------------------------------------------------------------

/* Push the config's debounce window to the global tuning and reset the
   conditioning state so a new setting starts clean. printable_only is handled
   in condition_pump (exemption before the filter), so the global filter runs
   with printable_only=false. Any keys already past debounce and buffered for
   min-gap are flushed to the survivor ring FIRST, so a config change (e.g. the
   F10 commit) never swallows an in-flight keystroke. */
static void
apply_cfg(KbDrv *d)
{
    pend_flush_to_surv(d);
    axl_input_set_key_debounce(d->cfg.enabled ? d->cfg.debounce_ms : 0, false);
    d->debounce = (AxlKeyDebounce){0};
    d->gate     = (AxlKeyGate){0};
}

static int
drv_get(AxlKbTuneConfig *out)
{
    if (out == NULL || g_drv == NULL) {
        return AXL_ERR;
    }
    *out = g_drv->cfg;
    return AXL_OK;
}

static int
drv_set(const AxlKbTuneConfig *in)
{
    if (in == NULL || g_drv == NULL || in->version != KBTUNE_CONFIG_VERSION) {
        return AXL_ERR;
    }
    EFI_TPL old = gBS->RaiseTPL(TPL_HIGH_LEVEL);
    g_drv->cfg = *in;
    apply_cfg(g_drv);
    gBS->RestoreTPL(old);
    return AXL_OK;
}

static KbTuneVtable g_vtable = {
    .version = KBTUNE_VTABLE_VERSION,
    .get     = drv_get,
    .set     = drv_set,
};

// ---------------------------------------------------------------------------
// Install / uninstall the console wrap (mirrors axl-console-mirror.c ordering)
// ---------------------------------------------------------------------------

static int
install_wrap(KbDrv *d)
{
    d->orig_conin = gST->ConIn;

    /* Simple ConIn wrapper. */
    d->my_conin.Reset         = wrap_in_reset;
    d->my_conin.ReadKeyStroke = wrap_in_read_key;

    EFI_STATUS st = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_CALLBACK,
                                     wait_key_cb, NULL, &d->wait_key);
    if (EFI_ERROR(st)) {
        return AXL_ERR;
    }
    d->my_conin.WaitForKey = d->wait_key;

    /* Ex wrapper. */
    d->my_coninex.Reset               = (void *)wrap_inex_reset;
    d->my_coninex.ReadKeyStrokeEx     = wrap_inex_read_key;
    d->my_coninex.SetState            = (void *)wrap_inex_set_state;
    d->my_coninex.RegisterKeyNotify   = wrap_inex_register_notify;
    d->my_coninex.UnregisterKeyNotify = wrap_inex_unregister_notify;

    st = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_CALLBACK,
                          wait_key_cb, NULL, &d->wait_key_ex);
    if (EFI_ERROR(st)) {
        gBS->CloseEvent(d->wait_key);
        d->wait_key = NULL;
        return AXL_ERR;
    }
    d->my_coninex.WaitForKeyEx = d->wait_key_ex;

    /* Publish the singleton BEFORE swapping gST so the wrappers (which may fire
       from a ReinstallProtocolInterface notify) see a live instance. */
    g_drv = d;

    /* Replace SimpleTextInputEx on ConsoleInHandle so editors / HandleProtocol
       consumers see the conditioned stream too. Best-effort. */
    EFI_GUID ex_guid = gEfiSimpleTextInputExProtocolGuid;
    st = gBS->HandleProtocol(gST->ConsoleInHandle, &ex_guid,
                             (void **)&d->orig_coninex);
    if (!EFI_ERROR(st) && d->orig_coninex != NULL) {
        st = gBS->ReinstallProtocolInterface(gST->ConsoleInHandle, &ex_guid,
                                             d->orig_coninex, &d->my_coninex);
        if (!EFI_ERROR(st)) {
            d->reinstalled_ex = true;
        } else {
            axl_warning("kbtune-drv: ConInEx reinstall failed");
        }
    } else {
        d->orig_coninex = NULL;
    }

    /* Swap gST->ConIn AFTER the reinstall (ConSplitter may rewrite gST->ConIn
       during the reinstall). */
    gST->ConIn = &d->my_conin;

    axl_info("kbtune-drv: console conditioning wrap installed");
    return AXL_OK;
}

static void
uninstall_wrap(KbDrv *d)
{
    if (g_drv != d) {
        return;
    }
    if (d->reinstalled_ex && d->orig_coninex != NULL) {
        EFI_GUID ex_guid = gEfiSimpleTextInputExProtocolGuid;
        gBS->ReinstallProtocolInterface(gST->ConsoleInHandle, &ex_guid,
                                        &d->my_coninex, d->orig_coninex);
    }
    if (d->orig_conin != NULL) {
        gST->ConIn = d->orig_conin;
    }
    if (d->wait_key != NULL) {
        gBS->CloseEvent(d->wait_key);
    }
    if (d->wait_key_ex != NULL) {
        gBS->CloseEvent(d->wait_key_ex);
    }
    g_drv = NULL;
    axl_info("kbtune-drv: console conditioning wrap removed");
}

// ---------------------------------------------------------------------------
// Driver entry / unload
// ---------------------------------------------------------------------------

static KbDrv g_instance;

int
kbtune_drv_entry(AxlHandle image, AxlSystemTable *systab)
{
    (void)image;
    (void)systab;

    /* Warm the monotonic clock at TPL_APPLICATION so its first-call calibration
       (an x86 gBS->Stall + protocol publish) never happens inside the raised-TPL
       notify. */
    (void)axl_time_get_us();

    axl_memset(&g_instance, 0, sizeof(g_instance));
    /* Default config: installed but disabled (transparent relay) until the
       launcher commits a window via set(). */
    g_instance.cfg.version        = KBTUNE_CONFIG_VERSION;
    g_instance.cfg.enabled        = false;
    g_instance.cfg.printable_only = true;
    apply_cfg(&g_instance);

    if (install_wrap(&g_instance) != AXL_OK) {
        axl_printerr("kbtune-drv: failed to install console wrap\n");
        return 1;
    }
    if (axl_shared_driver_publish(KBTUNE_SHARED_NAME, &g_vtable, &g_handle)
        != AXL_OK) {
        uninstall_wrap(&g_instance);
        axl_printerr("kbtune-drv: failed to publish vtable\n");
        return 1;
    }
    return 0;
}

int
kbtune_drv_unload(AxlHandle image)
{
    (void)image;
    if (g_handle != NULL) {
        axl_shared_driver_unpublish(KBTUNE_SHARED_NAME, &g_vtable, g_handle);
        g_handle = NULL;
    }
    uninstall_wrap(&g_instance);
    return 0;
}

AXL_DRIVER(kbtune_drv_entry, kbtune_drv_unload)
