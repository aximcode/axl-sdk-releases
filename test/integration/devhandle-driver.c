/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * devhandle-driver.c -- a driver that binds nothing and says whether its
 * loaded image has a DeviceHandle: the stand-in test-sshd-rtk-qemu.sh embeds
 * on aarch64 (where QEMU ships no e1000 driver) to see the volume an
 * embedded driver is given.
 *
 * Output contract (exact lines):
 *   DEVHANDLE: set     LoadedImage->DeviceHandle is not NULL
 *   DEVHANDLE: none    it is NULL
 *
 * UEFI is used directly: the LoadedImage protocol is no AXL API
 * (AXL_ALLOW_UEFI, as every test in this tree builds with).
 */

#include <axl.h>
#include <uefi/axl-uefi.h>

static int devhandle_main(AxlHandle image, AxlSystemTable *st);
static int devhandle_unload(AxlHandle image);

AXL_DRIVER(devhandle_main, devhandle_unload)

static int
devhandle_main(
    AxlHandle       image,
    AxlSystemTable *st
    )
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;

    (void)st;
    bool set = !EFI_ERROR(gBS->HandleProtocol((EFI_HANDLE)image,
                                              &EFI_LOADED_IMAGE_PROTOCOL_GUID,
                                              (void **)&li))
               && li != NULL && li->DeviceHandle != NULL;
    axl_printf("DEVHANDLE: %s\n", set ? "set" : "none");
    return 0;
}

static int
devhandle_unload(
    AxlHandle image
    )
{
    (void)image;
    return 0;
}
