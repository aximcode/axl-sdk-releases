/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * stdio-bridge-liveness-test.c — the handle-reuse false-alive regression.
 *
 * Scenario 1 (the RED regression): install a stdio-bridge instance that FOOLS
 * the pre-v2.7.1 liveness heuristic — launcher_image = this (alive) image, and
 * the 8-byte slot the old code reads as launcher_image_proto holds this image's
 * real EFI_LOADED_IMAGE_PROTOCOL* — so the proto-match reports it "alive". The
 * post-v2.7.1 gate reads that same slot as `token` and compares it against a
 * driver-resident AxlDispatchToken cell whose `current` we set to a distinct
 * value, so the gate reports it dead. Triggering axl_shared_driver_unload
 * (clears current=0 and reaps) drops it.
 *   RED  (pre-fix): proto-match keeps the bridge -> HandleProtocol succeeds.
 *   GREEN (post-fix): token gate drops it -> HandleProtocol fails.
 *
 * Scenario 2 (the token-inequality branch, current NONZERO): Scenario 1 reaps
 * via the cell->current==0 short-circuit, so it does not exercise the
 * `b->token == cell->current` comparison itself. Here a decoy bears a token no
 * monotonic counter can reach; driving axl_shared_driver_install_stdio_bridge()
 * stamps current = a fresh nonzero token and reaps, dropping the decoy ONLY
 * because token != the nonzero current — directly pinning the gate's compare.
 *
 * Public headers only: the internal AxlStdioBridge / AxlDispatchToken layouts
 * and GUIDs are mirrored locally (as stdio-bridge-reap-test.c mirrors the
 * bridge GUID). Keep these in lockstep with src/backend/axl-stdio-bridge.h.
 */

#include <axl.h>
#include <uefi/axl-uefi.h>

/* Mirrors AXL_STDIO_BRIDGE_GUID (c8f517d7-…) in the backend. */
static const EFI_GUID STDIO_BRIDGE_GUID = {
    0xc8f517d7, 0x36cc, 0x458d,
    {0x98, 0xd6, 0xb1, 0x16, 0x82, 0x5e, 0x30, 0xbf}
};

/* Mirrors AXL_DISPATCH_TOKEN_GUID (02dd6813-…) in the backend. */
static const EFI_GUID DISPATCH_TOKEN_GUID = {
    0x02dd6813, 0xd275, 0x4734,
    {0x98, 0xf8, 0xc7, 0xf6, 0x03, 0x31, 0x95, 0x8d}
};

/* Mirrors AxlStdioBridge (src/backend/axl-stdio-bridge.h). The 5th field is
   `token` post-fix and was `launcher_image_proto` (void*) pre-fix — same
   8-byte slot, so this one struct drives both code versions. */
typedef struct {
    void     *stdin_h;
    void     *stdout_h;
    void     *stderr_h;
    void     *launcher_image;
    uint64_t  token;            /* aka launcher_image_proto slot */
    uint64_t  pending_status;
    bool      has_pending;
} MirrorBridge;

/* Mirrors AxlDispatchToken. */
typedef struct {
    uint64_t  current;
} MirrorToken;

static int g_pass = 0;
static int g_fail = 0;

static void
check(bool ok, const char *msg)
{
    axl_printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
    if (ok) { g_pass++; } else { g_fail++; }
}

int
main(int argc, char **argv)
{
    (void)argc; (void)argv;
    axl_printf("stdio-bridge-liveness-test: start\n");

    /* This image's real LoadedImage* — the value that makes the pre-fix
       proto-match accept our bridge as alive. */
    void     *li  = NULL;
    EFI_GUID  lig = gEfiLoadedImageProtocolGuid;
    if (EFI_ERROR(gBS->HandleProtocol((EFI_HANDLE)gImageHandle, &lig, &li))
        || li == NULL) {
        check(false, "resolve this image's LoadedImage protocol");
        axl_printf("stdio-bridge-liveness-test: %d passed, %d failed\n",
                   g_pass, g_fail);
        return 1;
    }

    /* Build a bridge that fools the proto-match: alive image + slot == li. */
    static MirrorBridge b;
    b.stdin_h        = NULL;
    b.stdout_h       = NULL;
    b.stderr_h       = NULL;
    b.launcher_image = (void *)gImageHandle;   /* genuinely alive */
    b.token          = (uint64_t)(uintptr_t)li; /* == launcher_image_proto slot */
    b.pending_status = 0;
    b.has_pending    = false;

    EFI_HANDLE bridge_handle = NULL;
    if (EFI_ERROR(gBS->InstallProtocolInterface(
            &bridge_handle, (EFI_GUID *)&STDIO_BRIDGE_GUID,
            EFI_NATIVE_INTERFACE, &b))) {
        check(false, "install the decoy bridge instance");
        axl_printf("stdio-bridge-liveness-test: %d passed, %d failed\n",
                   g_pass, g_fail);
        return 1;
    }

    /* Install (if absent) an AxlDispatchToken cell and set current to a fresh
       monotonic value — small, guaranteed != the pointer-valued token slot. */
    static MirrorToken cell;
    cell.current = 0;
    EFI_HANDLE cell_handle = NULL;
    gBS->InstallProtocolInterface(
        &cell_handle, (EFI_GUID *)&DISPATCH_TOKEN_GUID,
        EFI_NATIVE_INTERFACE, &cell);
    /* If a resident driver already installed the real cell, write through it so
       the backend reads what we set; otherwise our just-installed cell is the
       one LocateProtocol finds. Locate the live one and stamp it. */
    MirrorToken *live_cell = NULL;
    if (!EFI_ERROR(gBS->LocateProtocol(
            (EFI_GUID *)&DISPATCH_TOKEN_GUID, NULL, (void **)&live_cell))
        && live_cell != NULL) {
        uint64_t t = 0;
        gBS->GetNextMonotonicCount(&t);
        if (t == 0) { t = 1; }
        live_cell->current = t;                 /* != b.token (a pointer) */
    }

    /* Trigger the reap path. The name need not be resident: unload reaps
       before the not-found early return. */
    axl_shared_driver_unload("stdio-bridge-liveness-probe");

    /* Post-fix: token gate + current-clear reaped our decoy. Pre-fix:
       proto-match kept it alive. */
    void *iface = NULL;
    EFI_STATUS st = gBS->HandleProtocol(
        bridge_handle, (EFI_GUID *)&STDIO_BRIDGE_GUID, &iface);
    check(EFI_ERROR(st),
          "reap dropped the proto-match-fooling bridge (token gate)");

    /* --- Scenario 2: the token-inequality branch with a NONZERO current. ---
       Install a decoy whose token cannot equal any monotonic value (top bit
       set), then drive axl_shared_driver_install_stdio_bridge(): it stamps
       cell->current = a fresh (nonzero) token and reaps. The decoy is dropped
       ONLY because its token != that nonzero current — this exercises the
       gate's comparison, not the current==0 short-circuit Scenario 1 used. */
    static MirrorBridge b2;
    b2.stdin_h        = NULL;
    b2.stdout_h       = NULL;
    b2.stderr_h       = NULL;
    b2.launcher_image = (void *)gImageHandle;   /* alive; irrelevant to the token gate */
    b2.token          = 0x8000000000000001ULL;  /* a monotonic counter never reaches this */
    b2.pending_status = 0;
    b2.has_pending    = false;

    EFI_HANDLE b2_handle = NULL;
    if (EFI_ERROR(gBS->InstallProtocolInterface(
            &b2_handle, (EFI_GUID *)&STDIO_BRIDGE_GUID,
            EFI_NATIVE_INTERFACE, &b2))) {
        check(false, "install the nonzero-current decoy bridge");
    } else {
        /* Stamps cell->current = fresh nonzero token, then reaps. This image
           has shell stdio (launched from the shell), so install runs fully. */
        axl_shared_driver_install_stdio_bridge();
        /* Assert our exact decoy interface `&b2` is no longer installed
           anywhere. NOT via HandleProtocol(b2_handle): reaping frees b2_handle
           and install then allocates a handle for its OWN bridge, which UEFI
           may recycle to the same handle value — so the handle can resolve to
           a different, live bridge. Scanning for the interface pointer is
           recycling-proof. */
        bool decoy2_present = false;
        EFI_HANDLE *hs = NULL;
        UINTN       n  = 0;
        if (!EFI_ERROR(gBS->LocateHandleBuffer(
                ByProtocol, (EFI_GUID *)&STDIO_BRIDGE_GUID, NULL, &n, &hs))
            && hs != NULL) {
            for (UINTN i = 0; i < n; i++) {
                void *ifc = NULL;
                if (!EFI_ERROR(gBS->HandleProtocol(
                        hs[i], (EFI_GUID *)&STDIO_BRIDGE_GUID, &ifc))
                    && ifc == &b2) {
                    decoy2_present = true;
                }
            }
            gBS->FreePool(hs);
        }
        check(!decoy2_present,
              "reap dropped bridge with token != nonzero current (gate compare)");
    }

    axl_printf("stdio-bridge-liveness-test: %d passed, %d failed\n",
               g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
