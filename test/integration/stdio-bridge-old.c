/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * stdio-bridge-old.c — a launcher built before the bridge carried a target.
 *
 * Such a launcher installs its stdio bridge under the ORIGINAL bridge GUID
 * only, in the original 56-byte layout, and stamps its token into the
 * dispatch cell like any launcher. A newer driver that reads that bridge as
 * if it had the newer layout reads `target` 8 bytes past the end of a static
 * in the launcher's image. Here those 8 bytes hold the driver's own
 * published interface -- what such a read finds when it is unlucky -- so a
 * driver that reads them takes this launcher's StdIn and prints what the
 * test redirected into it. A driver that ignores the original GUID prints
 * GOT:<EOF>: no bridging, no read past the end.
 *
 * Relies on stdio-bridge-fix.efi having made the driver resident. Public
 * headers plus the UEFI ones the fixture opts into; the layout and GUIDs are
 * mirrored locally, as stdio-bridge-liveness-test.c does.
 */

#include <axl.h>
#include <uefi/axl-uefi.h>   /* gBS, gImageHandle, Shell parameters */
#include "stdio-bridge-fix.h"

/* Mirrors the original AXL_STDIO_BRIDGE_GUID (c8f517d7-...). */
static const EFI_GUID OLD_BRIDGE_GUID = {
    0xc8f517d7, 0x36cc, 0x458d,
    {0x98, 0xd6, 0xb1, 0x16, 0x82, 0x5e, 0x30, 0xbf}
};

/* Mirrors AXL_DISPATCH_TOKEN_GUID (02dd6813-...). */
static const EFI_GUID DISPATCH_TOKEN_GUID = {
    0x02dd6813, 0xd275, 0x4734,
    {0x98, 0xf8, 0xc7, 0xf6, 0x03, 0x31, 0x95, 0x8d}
};

/* The original bridge layout, 56 bytes. */
typedef struct {
    void     *stdin_h;
    void     *stdout_h;
    void     *stderr_h;
    void     *launcher_image;
    uint64_t  token;
    uint64_t  pending_status;
    bool      has_pending;
} OldBridge;

/* The bridge, and the static that happens to follow it in this image. */
typedef struct {
    OldBridge    bridge;
    const void  *after;
} OldBridgeImage;

int
main(int argc, char **argv)
{
    AxlGuid guid;
    void   *vt = NULL;
    if (axl_shared_driver_guid(STDIO_BRIDGE_FIX_NAME, &guid) != AXL_OK
        || axl_protocol_find_guid(&guid, &vt) != AXL_OK || vt == NULL) {
        axl_printf("stdio-bridge-old: driver not resident\n");
        return 1;
    }

    EFI_SHELL_PARAMETERS_PROTOCOL *sp  = NULL;
    EFI_GUID                       spg = gEfiShellParametersProtocolGuid;
    if (EFI_ERROR(gBS->HandleProtocol((EFI_HANDLE)gImageHandle, &spg,
                                      (void **)&sp)) || sp == NULL) {
        axl_printf("stdio-bridge-old: no Shell parameters\n");
        return 1;
    }

    uint64_t *current = NULL;
    if (EFI_ERROR(gBS->LocateProtocol((EFI_GUID *)&DISPATCH_TOKEN_GUID, NULL,
                                      (void **)&current)) || current == NULL) {
        axl_printf("stdio-bridge-old: no dispatch cell\n");
        return 1;
    }
    uint64_t t = 0;
    while (t == 0) {
        gBS->GetNextMonotonicCount(&t);
    }

    static OldBridgeImage img;
    img.bridge.stdin_h        = sp->StdIn;
    img.bridge.stdout_h       = sp->StdOut;
    img.bridge.stderr_h       = sp->StdErr;
    img.bridge.launcher_image = (void *)gImageHandle;
    img.bridge.token          = t;
    img.bridge.pending_status = 0;
    img.bridge.has_pending    = false;
    img.after                 = vt;

    EFI_HANDLE h = NULL;
    if (EFI_ERROR(gBS->InstallProtocolInterface(
            &h, (EFI_GUID *)&OLD_BRIDGE_GUID, EFI_NATIVE_INTERFACE,
            &img.bridge))) {
        axl_printf("stdio-bridge-old: bridge install failed\n");
        return 1;
    }
    *current = t;

    /* argv[0] is this launcher; the driver's argv[0] is the verb. */
    int rc = ((FixVtable *)vt)->run(argc - 1, argv + 1);

    *current = 0;
    gBS->UninstallProtocolInterface(h, (EFI_GUID *)&OLD_BRIDGE_GUID, &img.bridge);
    return rc;
}
