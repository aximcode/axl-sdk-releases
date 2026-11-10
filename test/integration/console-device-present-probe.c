/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * console-device-present-probe.c -- a SECOND image asking whether a console
 * take-over is active anywhere. It never installs one itself, so its answer can
 * only come from the firmware's handle database, not from this image's own
 * singleton. Prints exactly one line: "PROBE present=0" or "PROBE present=1".
 *
 * `probe.efi decoy` first publishes a DECOY: a SimpleTextOut handle whose device
 * path is a bare End node. LocateDevicePath is a longest-prefix match, and an
 * empty path is a prefix of every query, so a probe that took any hit at face
 * value would answer true here. Prints "PROBE decoy present=N", then removes
 * the decoy.
 *
 * Three more modes, for test-console-device-errout-qemu.sh, which needs a
 * screen in ErrOut and a way to see which consoles are in the StdErr fan-out
 * while a take-over evicts them:
 *
 *   probe.efi errout       copy ConOut to ErrOut (NV), so after a reset the
 *                          display is a StdErr device too, as on firmware
 *                          that lists it. Prints "PROBE errout=<bytes>".
 *   probe.efi tags         count the handles tagged as StdErr devices and how
 *                          many of them are screens (carry a GOP).
 *                          Prints "PROBE stderr=<n> screen=<m>" -- to StdErr,
 *                          which the serial terminal stays in under a take-over.
 *   probe.efi say-err W    write "cdh-W" to StdErr (the marker is built here,
 *                          so the Shell's echo of the command never matches).
 *   probe.efi conout       count the handles tagged as ConOut devices and how
 *                          many are screens. Prints "PROBE conout=<n>
 *                          screen=<m>" to StdErr.
 *   probe.efi retag-err    put the StdErr tag back on every screen that lost
 *                          both its tags to a take-over, as a reconnect could
 *                          while the take-over holds the console -- the restore
 *                          must still give the screen its ConOut tag. Prints
 *                          "PROBE retagged=<n>" to StdErr.
 *
 * Drives test-console-device-hold-qemu.sh with console-device-hold.efi.
 */

#include <axl.h>
#include <axl/axl-console-device.h>
#include <uefi/axl-uefi.h>

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL g_decoy_conout;   /* never called */
static EFI_DEVICE_PATH_PROTOCOL        g_decoy_path = {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (uint8_t)sizeof(EFI_DEVICE_PATH_PROTOCOL), 0 }
};

static int
probe_with_decoy(void)
{
    EFI_HANDLE h = NULL;
    if (EFI_ERROR(gBS->InstallMultipleProtocolInterfaces(
            &h,
            &gEfiDevicePathProtocolGuid,       &g_decoy_path,
            &gEfiSimpleTextOutputProtocolGuid, &g_decoy_conout,
            NULL))) {
        axl_printf("PROBE decoy install failed\n");
        return 1;
    }
    bool present = axl_console_device_present();
    gBS->UninstallMultipleProtocolInterfaces(
        h,
        &gEfiDevicePathProtocolGuid,       &g_decoy_path,
        &gEfiSimpleTextOutputProtocolGuid, &g_decoy_conout,
        NULL);
    axl_printf("PROBE decoy present=%d\n", present ? 1 : 0);
    return 0;
}

/* ErrOut := ConOut, in the global namespace, as BDS writes it. */
static int
probe_errout(void)
{
    void  *buf = NULL;
    size_t len = 0;
    if (axl_nvstore_get_alloc("global", "ConOut", &buf, &len) != AXL_OK) {
        axl_printf("PROBE errout failed: no ConOut\n");
        return 1;
    }
    int rc = axl_nvstore_set("global", "ErrOut", buf, len,
                             AXL_NV_PERSISTENT | AXL_NV_BOOT | AXL_NV_RUNTIME);
    axl_free(buf);
    if (rc != AXL_OK) {
        axl_printf("PROBE errout failed: cannot write ErrOut\n");
        return 1;
    }
    axl_printf("PROBE errout=%u\n", (unsigned)len);
    return 0;
}

static int
probe_tags(void)
{
    UINTN       n       = 0;
    EFI_HANDLE *handles = NULL;
    unsigned    screens = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gEfiStandardErrorDeviceGuid,
                                          NULL, &n, &handles))) {
        n = 0;
    }
    for (UINTN i = 0; i < n; i++) {
        void *gop = NULL;
        if (!EFI_ERROR(gBS->HandleProtocol(handles[i], &gEfiGraphicsOutputProtocolGuid,
                                           &gop))) {
            screens++;
        }
    }
    if (handles != NULL) {
        gBS->FreePool(handles);  /* axl-pool-direct: LocateHandleBuffer result */
    }
    axl_printerr("PROBE stderr=%u screen=%u\n", (unsigned)n, screens);
    return 0;
}

static bool
has_protocol(EFI_HANDLE h, EFI_GUID *guid)
{
    void *iface = NULL;
    return !EFI_ERROR(gBS->HandleProtocol(h, guid, &iface));
}

static int
probe_conout(void)
{
    UINTN       n       = 0;
    EFI_HANDLE *handles = NULL;
    unsigned    screens = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gEfiConsoleOutDeviceGuid,
                                          NULL, &n, &handles))) {
        n = 0;
    }
    for (UINTN i = 0; i < n; i++) {
        if (has_protocol(handles[i], &gEfiGraphicsOutputProtocolGuid)) {
            screens++;
        }
    }
    if (handles != NULL) {
        gBS->FreePool(handles);  /* axl-pool-direct: LocateHandleBuffer result */
    }
    axl_printerr("PROBE conout=%u screen=%u\n", (unsigned)n, screens);
    return 0;
}

static int
probe_retag_err(void)
{
    UINTN       n       = 0;
    EFI_HANDLE *handles = NULL;
    unsigned    done    = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gEfiGraphicsOutputProtocolGuid,
                                          NULL, &n, &handles))) {
        n = 0;
    }
    for (UINTN i = 0; i < n; i++) {
        EFI_HANDLE h = handles[i];
        if (has_protocol(h, &gEfiSimpleTextOutputProtocolGuid)
            && !has_protocol(h, &gEfiConsoleOutDeviceGuid)
            && !has_protocol(h, &gEfiStandardErrorDeviceGuid)
            && !EFI_ERROR(gBS->InstallProtocolInterface(&h, &gEfiStandardErrorDeviceGuid,
                                                        EFI_NATIVE_INTERFACE, NULL))) {
            done++;
        }
    }
    if (handles != NULL) {
        gBS->FreePool(handles);  /* axl-pool-direct: LocateHandleBuffer result */
    }
    axl_printerr("PROBE retagged=%u\n", done);
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc > 1 && axl_strcmp(argv[1], "decoy") == 0) {
        return probe_with_decoy();
    }
    if (argc > 1 && axl_strcmp(argv[1], "errout") == 0) {
        return probe_errout();
    }
    if (argc > 1 && axl_strcmp(argv[1], "tags") == 0) {
        return probe_tags();
    }
    if (argc > 1 && axl_strcmp(argv[1], "conout") == 0) {
        return probe_conout();
    }
    if (argc > 1 && axl_strcmp(argv[1], "retag-err") == 0) {
        return probe_retag_err();
    }
    if (argc > 2 && axl_strcmp(argv[1], "say-err") == 0) {
        axl_printerr("cdh-%s\n", argv[2]);
        return 0;
    }
    axl_printf("PROBE present=%d\n", axl_console_device_present() ? 1 : 0);
    return 0;
}
