/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * fake-nic-driver-info.c -- axl_net_get_driver_info() names a disk-loaded
 * NIC driver by its WHOLE file path, however long.
 *
 * A NIC owned by a driver loaded from a file is named by the path its loaded
 * image records -- from a release stick's versioned directory, ~80 bytes.
 * AxlNetDriverInfo.driver once held 64 and cut it. No QEMU NIC is bound by a
 * driver this suite stages (the bundled iPXE that was is gone), so this image
 * plays the driver itself: it publishes a fake NIC -- a vendor device path,
 * an SNP whose Mode carries a MAC, and an NII 3.1 it opens BY_DRIVER with its
 * OWN image handle as the agent -- then asks the real library who owns that
 * MAC and prints the answer. Run from a long path, the answer must be that
 * path, whole. Nothing connects the fake NIC (no `connect -r`), so no driver
 * ever calls the SNP's empty function table; it is torn down before exit.
 *
 * Output contract (exact lines test-netload-qemu.sh compares):
 *   FAKENIC: driver=<di.driver> layer=<di.layer>
 *   FAKENIC: FAIL <stage>
 *
 * UEFI is used directly: the fixture IS a NIC driver, which no AXL API
 * publishes (AXL_ALLOW_UEFI, as every test in this tree builds with).
 */

#include <axl.h>
#include <uefi/axl-uefi.h>

#pragma pack(1)
typedef struct {
    VENDOR_DEVICE_PATH       vendor;
    EFI_DEVICE_PATH_PROTOCOL end;
} FakeNicPath;
#pragma pack()

static const uint8_t FAKE_MAC[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x77 };

static FakeNicPath                 g_path;
static EFI_SIMPLE_NETWORK_MODE     g_mode;
static EFI_SIMPLE_NETWORK_PROTOCOL g_snp;   /* functions NULL: never connected */
static uint64_t                    g_nii;   /* any non-NULL interface */

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_path.vendor.Header.Type    = HARDWARE_DEVICE_PATH;
    g_path.vendor.Header.SubType = HW_VENDOR_DP;
    g_path.vendor.Header.Length[0] = (UINT8)sizeof(VENDOR_DEVICE_PATH);
    g_path.vendor.Header.Length[1] = 0;
    g_path.vendor.Guid = (EFI_GUID){ 0x6b0f2a51, 0x5c1d, 0x4e0a,
        { 0x9a, 0x77, 0x46, 0x41, 0x4b, 0x45, 0x4e, 0x49 } };
    g_path.end.Type      = END_DEVICE_PATH_TYPE;
    g_path.end.SubType   = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    g_path.end.Length[0] = (UINT8)sizeof(EFI_DEVICE_PATH_PROTOCOL);
    g_path.end.Length[1] = 0;

    g_mode.HwAddressSize = 6;
    axl_memcpy(&g_mode.CurrentAddress, FAKE_MAC, 6);
    axl_memcpy(&g_mode.PermanentAddress, FAKE_MAC, 6);
    g_mode.MediaPresent = true;
    g_snp.Mode = &g_mode;

    EFI_HANDLE nic = NULL;
    EFI_GUID nii31 = gEfiNetworkInterfaceIdentifierProtocolGuid_31;
    /* One protocol at a time: no varargs across the EFI calling convention. */
    EFI_STATUS st = gBS->InstallProtocolInterface(&nic,
        &EFI_DEVICE_PATH_PROTOCOL_GUID, EFI_NATIVE_INTERFACE, &g_path);
    if (!EFI_ERROR(st)) {
        st = gBS->InstallProtocolInterface(&nic,
            &EFI_SIMPLE_NETWORK_PROTOCOL_GUID, EFI_NATIVE_INTERFACE, &g_snp);
    }
    if (!EFI_ERROR(st)) {
        st = gBS->InstallProtocolInterface(&nic, &nii31,
                                           EFI_NATIVE_INTERFACE, &g_nii);
    }
    if (EFI_ERROR(st)) {
        axl_printf("FAKENIC: FAIL install\n");
        return 1;
    }
    void *iface = NULL;
    st = gBS->OpenProtocol(nic, &nii31, &iface, gImageHandle, nic,
                           EFI_OPEN_PROTOCOL_BY_DRIVER);
    int rc = 0;
    if (EFI_ERROR(st)) {
        axl_printf("FAKENIC: FAIL open-by-driver\n");
        rc = 1;
    } else {
        AxlNetDriverInfo di;
        if (axl_net_get_driver_info(FAKE_MAC, &di) != AXL_OK) {
            axl_printf("FAKENIC: FAIL driver-info\n");
            rc = 1;
        } else {
            axl_printf("FAKENIC: driver=%s layer=%s\n", di.driver, di.layer);
        }
        gBS->CloseProtocol(nic, &nii31, gImageHandle, nic);
    }
    gBS->UninstallProtocolInterface(nic, &nii31, &g_nii);
    gBS->UninstallProtocolInterface(nic, &EFI_SIMPLE_NETWORK_PROTOCOL_GUID, &g_snp);
    gBS->UninstallProtocolInterface(nic, &EFI_DEVICE_PATH_PROTOCOL_GUID, &g_path);
    return rc;
}
