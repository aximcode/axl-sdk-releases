/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * key-dump.c -- print every key the console delivers, whole: one line per
 * ReadKeyStrokeEx on gST->ConsoleInHandle's SIMPLE_TEXT_INPUT_EX, which is
 * the protocol `edit` and `hexedit` read (ConSplitter's Ex, so the shift and
 * toggle state survive exactly as a keyboard driver reported them).
 *
 *   key-dump.efi [SECONDS]
 *
 * Prints `KEYDUMP-READY`, then for each key
 *   KEY sc=<scan %04x> u=<unicode %04x> ss=<KeyShiftState %08x> ts=<KeyToggleState %02x>
 * polling every 5 ms, and ends with `KEYDUMP-DONE` after Ctrl+Alt+q (whose own
 * KEY line is printed first) or after SECONDS (default 300). A bad SECONDS
 * prints `KEYDUMP-USAGE` and returns 2.
 *
 * Drives test-sshd-keys-qemu.sh: the same key combinations typed through a
 * USB keyboard and through an ssh session must produce identical lines.
 */

#include <axl.h>
#include <uefi/axl-uefi.h>

#define KEYDUMP_DEFAULT_S  300u
#define KEYDUMP_POLL_MS    5u
/* Ctrl+Alt+q as a USB keyboard reports it: the letter, left Ctrl + left Alt. */
#define KEYDUMP_QUIT_UNI   0x0071u
#define KEYDUMP_QUIT_SHIFT 0x80000028u

int
main(
    int    argc,
    char **argv
)
{
    uint32_t seconds = KEYDUMP_DEFAULT_S;
    if (argc > 2
        || (argc == 2
            && (axl_str_to_u32(argv[1], 10, &seconds, NULL) != AXL_OK
                || seconds == 0))) {
        axl_printf("KEYDUMP-USAGE key-dump.efi [SECONDS]\n");
        return 2;
    }

    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *ex = NULL;
    if (EFI_ERROR(gBS->HandleProtocol(gST->ConsoleInHandle,
                                      &gEfiSimpleTextInputExProtocolGuid,
                                      (void **)&ex))
        || ex == NULL) {
        axl_printf("KEYDUMP-NOEX\n");
        return 1;
    }

    axl_printf("KEYDUMP-READY\n");
    uint64_t end = axl_time_get_ms() + (uint64_t)seconds * 1000u;
    while (axl_time_get_ms() < end) {
        EFI_KEY_DATA kd;
        if (ex->ReadKeyStrokeEx(ex, &kd) == EFI_SUCCESS) {
            axl_printf("KEY sc=%04x u=%04x ss=%08x ts=%02x\n",
                       (unsigned)kd.Key.ScanCode,
                       (unsigned)kd.Key.UnicodeChar,
                       (unsigned)kd.KeyState.KeyShiftState,
                       (unsigned)kd.KeyState.KeyToggleState);
            if (kd.Key.ScanCode == 0
                && kd.Key.UnicodeChar == KEYDUMP_QUIT_UNI
                && kd.KeyState.KeyShiftState == KEYDUMP_QUIT_SHIFT) {
                break;
            }
            continue;
        }
        axl_msleep(KEYDUMP_POLL_MS);
    }
    axl_printf("KEYDUMP-DONE\n");
    return 0;
}
