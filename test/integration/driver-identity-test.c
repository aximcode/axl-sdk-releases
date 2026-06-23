/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * driver-identity-test.c — proves a buffer-loaded driver image gets a
 * non-NULL, renderable device path, so the aarch64 UEFI shell's `dh -p` /
 * `dh -v` does not fault on it.
 *
 * Background: axl_driver_load_buffer() loads via gBS->LoadImage with
 * DevicePath=NULL, which leaves LoadedImage->FilePath and the handle's
 * gEfiLoadedImageDevicePathProtocol interface NULL. The x64 shell prints
 * "<null string>"; the aarch64 shell dereferences the device-path pointer
 * while rendering the handle and raises a Synchronous Exception. AXL now
 * synthesizes a MemoryMapped(...)/FilePath device path after the load.
 *
 * Test:
 *   1. read fs0:\driver.efi into a buffer,
 *   2. axl_driver_load_buffer() it (do NOT start — the device path is set
 *      at load time, independent of StartImage),
 *   3. install a private MARKER protocol on the loaded *image* handle so
 *      `dh -p <marker-guid>` targets exactly this handle from the shell,
 *   4. read back EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL + LoadedImage and
 *      assert both are non-NULL and the path renders to text,
 *   5. leave the image resident so the startup.nsh `dh` commands can
 *      inspect it.
 *
 * The shell side (test-driver-identity-qemu.sh) then runs
 * `dh -p <marker>` / `dh -v -p <marker>` and asserts no Synchronous
 * Exception and that the rendered path appears.
 */

#include <axl.h>
#include <uefi/axl-uefi.h>

/* Private marker protocol — installed on the loaded image handle purely so
 * the shell can target that one handle by GUID. Printed below for the
 * startup.nsh `dh -p` argument. */
static const AxlGuid MARKER_GUID =
    AXL_GUID(0xa11ce5ed, 0x1de4, 0x7e57,
             0xb0, 0x0b, 0x5f, 0xda, 0x84, 0xd4, 0x00, 0x01);

static int g_pass = 0;
static int g_fail = 0;

static void
check(bool ok, const char *msg)
{
    axl_printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
    }
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* 1. Read the test driver image into a buffer. */
    void   *buf = NULL;
    size_t  len = 0;
    if (axl_file_get_contents("fs0:\\driver.efi", &buf, &len) != AXL_OK
        || buf == NULL || len == 0) {
        check(false, "read fs0:\\driver.efi");
        axl_printf("Results: %d passed, %d failed\n", g_pass, g_fail);
        return 1;
    }

    /* 2. Buffer-load it (not started). */
    AxlDriverHandle drv = NULL;
    int rc = axl_driver_load_buffer((const unsigned char *)buf, len, &drv);
    axl_free(buf);  /* LoadImage copies the source buffer */
    if (rc != AXL_OK || drv == NULL) {
        check(false, "axl_driver_load_buffer");
        axl_printf("Results: %d passed, %d failed\n", g_pass, g_fail);
        return 1;
    }

    EFI_HANDLE eh = (EFI_HANDLE)drv;

    /* 2b. Teardown exercise: load a SECOND copy, confirm its synthesized
     *     device path, then unload it. axl_driver_unload must uninstall +
     *     free the synthesized path (image_dp_release) cleanly — no leak, no
     *     crash, no stale protocol left on the (being-destroyed) handle. */
    {
        void   *b2 = NULL;
        size_t  l2 = 0;
        if (axl_file_get_contents("fs0:\\driver.efi", &b2, &l2) == AXL_OK
            && b2 != NULL) {
            AxlDriverHandle d2 = NULL;
            int r2 = axl_driver_load_buffer((const unsigned char *)b2, l2, &d2);
            axl_free(b2);
            if (r2 == AXL_OK && d2 != NULL) {
                EFI_DEVICE_PATH_PROTOCOL *dp2 = NULL;
                check(gBS->HandleProtocol((EFI_HANDLE)d2,
                          &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID,
                          (void **)&dp2) == EFI_SUCCESS && dp2 != NULL,
                      "cycle: device path non-NULL before unload");
                check(axl_driver_unload(d2) == AXL_OK,
                      "cycle: unload after identity synthesis succeeds");
            } else {
                check(false, "cycle: second load_buffer");
            }
        } else {
            check(false, "cycle: second read of driver.efi");
        }
    }

    /* 3. Tag the image handle so the shell can target it by GUID. */
    void       *marker_iface = (void *)&MARKER_GUID;  /* any non-NULL sentinel */
    EFI_STATUS  ms = gBS->InstallProtocolInterface(
        &eh, (EFI_GUID *)&MARKER_GUID, EFI_NATIVE_INTERFACE, marker_iface);
    check(!EFI_ERROR(ms), "install marker protocol on image handle");

    /* 4a. The loaded-image-device-path protocol interface must be non-NULL. */
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
    EFI_STATUS dps = gBS->HandleProtocol(
        eh, &EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID, (void **)&dp);
    check(!EFI_ERROR(dps) && dp != NULL,
          "LoadedImageDevicePath interface is non-NULL");

    /* 4b. It must render to text (this is what the aa64 shell faults on). */
    char *text = (dp != NULL) ? axl_device_path_to_text(dp) : NULL;
    check(text != NULL, "device path renders to text");
    if (text != NULL) {
        axl_printf("DEVPATH: %s\n", text);
        axl_free(text);
    }

    /* 4c. LoadedImage->FilePath must be non-NULL. */
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_STATUS lis = gBS->HandleProtocol(
        eh, &EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&li);
    check(!EFI_ERROR(lis) && li != NULL && li->FilePath != NULL,
          "LoadedImage->FilePath is non-NULL");

    /* 5. Print the marker GUID for the startup.nsh `dh -p` argument and
     *    leave the image resident (do NOT unload). */
    axl_printf("MARKER: %08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
               MARKER_GUID.data1, MARKER_GUID.data2, MARKER_GUID.data3,
               MARKER_GUID.data4[0], MARKER_GUID.data4[1], MARKER_GUID.data4[2],
               MARKER_GUID.data4[3], MARKER_GUID.data4[4], MARKER_GUID.data4[5],
               MARKER_GUID.data4[6], MARKER_GUID.data4[7]);

    axl_printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
