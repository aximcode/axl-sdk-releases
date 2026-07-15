/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* Presence marker the resident fbcon take-over driver installs on its own image
   handle. The `fbcon.efi` launcher app locates handles carrying it to UnloadImage
   any prior instance before loading a fresh one (a driver cannot unload itself, but
   the separate launcher image can). GUID compared by value, so a static copy per
   translation unit is fine. */

#ifndef FBCON_MARKER_H
#define FBCON_MARKER_H

#include <uefi/axl-uefi.h>

static EFI_GUID FBCON_PRESENCE_GUID =
    { 0xFBC04E00, 0x1C1C, 0x4A5C, { 0xB0, 0x0B, 0xF8, 0xC0, 0x4E, 0x5E, 0x51, 0x7E } };

#endif /* FBCON_MARKER_H */
