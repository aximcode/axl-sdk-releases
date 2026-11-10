/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-device-internal.h
    Internal to axl-console-device.c and its tests -- not public API.
**/

#ifndef AXL_CONSOLE_DEVICE_INTERNAL_H
#define AXL_CONSOLE_DEVICE_INTERNAL_H

#include <stdbool.h>

/*
 * The console device's test seam: true makes the library act at @p point as
 * if the firmware had refused it, so a test can reach axl_console_device_-
 * uninstall's AXL_ERR paths, which OVMF never produces, and show what they
 * leave behind. The points:
 *
 *   "disconnect"       DisconnectController of our handle is refused;
 *   "conin-uninstall"  the uninstall of our ConIn protocols is refused.
 *
 * Each is a CLEAN refusal: the refused call is not made at all. Neither is the
 * shape EDK2 usually produces. Its DisconnectController (DriverSupport.c)
 * returns EFI_SUCCESS when any binding stopped and EFI_NOT_FOUND when none
 * did -- a total refusal, which axl_driver_disconnect_handle maps to AXL_OK --
 * so a refusing binding is caught only at the next step, when it still holds
 * our protocols and their uninstall is refused ("conin-uninstall" models
 * that step). An error from the disconnect itself means a bad parameter or a
 * resource failure, possibly after some bindings stopped; the seam does not
 * model that partial shape.
 *
 * The library's definition is WEAK and always false, so production carries
 * no trace of a test control; a test fixture links a strong one
 * (test/integration/console-device-hold.c). Same pattern as
 * _axl_service_stage_test_point.
 *
 * Keep the `*_test_point` name: test-efi-pack-release-qemu.sh finds the
 * seams in a shipped image by that suffix alone, and fails one that is not
 * the weak stub's size.
 */
bool
_axl_console_device_test_point(
    const char *point);

#endif /* AXL_CONSOLE_DEVICE_INTERNAL_H */
