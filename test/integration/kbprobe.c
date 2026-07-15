/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/* kbprobe.c — keyboard event timing probe for diagnosing key "bounce" / repeat
 * over KVM consoles (iDRAC virtual console, Avocent KVM, ...).
 *
 * (Graduated into the tracked tree from the gitignored local/kbprobe/ prototype
 * so the bounce A/B is CI-visible; this copy is canonical.)
 *
 * Doubles as the READER for test-kbtune-bounce-qemu.sh: run-qemu's --holdkey
 * reproduces the UsbKbDxe typematic bounce in QEMU, and kbprobe (with its
 * --throttle / --debounce knobs) is the F1/F3/F2 reader whose surfaced-event
 * count the test asserts on. See docs/AXL-KbTune-Design.md.
 *
 * Reads keystrokes exactly the way the UEFI shell command line does — a tight
 * blocking read loop over the console input — and logs EVERY keystroke with a
 * microsecond timestamp and the delta from the previous one. That turns a
 * single physical keypress into a readable signature:
 *
 *   1 event                         -> clean (no bounce)
 *   2-3 events within a few ms       -> BOUNCE: the KVM emitted down/up/down (or
 *                                       dropped the key-up), so the firmware saw
 *                                       multiple presses
 *   1 event, a ~500 ms gap, then a   -> USB TYPEMATIC auto-repeat: UsbKbDxe
 *   train ~20 ms apart                 treats the key as "still held" (a late or
 *                                       lost key-up over the KVM link) and
 *                                       synthesises repeats (500 ms delay,
 *                                       ~20 ms rate — MdeModulePkg UsbKbDxe).
 *
 * Why the shell bounces but `edit` / a full-screen editor does not: the shell's
 * StdIn reader (ShellPkg FileHandleWrappers.c) is a tight
 * `WaitForEvent(ConIn->WaitForKey) -> ReadKeyStroke` loop that drains every
 * queued keystroke immediately, so a KVM burst surfaces as several characters.
 * A full-screen editor reads at most one key per heavy pass (mouse poll +
 * screen refresh gated by a Stall). Both read from the SAME firmware key queue,
 * so the protocol (SimpleTextInput vs SimpleTextInputEx) is NOT the cause — the
 * consumption cadence is. `--throttle` reproduces the editor's slow pass;
 * `--debounce` proves the fix (drop a same-key repeat inside a short window).
 *
 * Build:  make ARCH=x64 kbprobe   (-> out/native-x64/kbprobe.efi)
 * Run over the KVM console, press keys deliberately, press Esc to finish.
 */

#include <axl.h>

/* UEFI scan code for Esc (EFI_INPUT_KEY.ScanCode); used as the quit key. */
#define SCAN_ESC 0x17

static uint64_t opt_debounce_ms = 0;   /* drop a same-key repeat within this window */
static uint64_t opt_throttle_ms = 0;   /* sleep after each key (mimic a heavy loop)  */

/* Render a UCS-2 char as a printable 'c' or a 0xNNNN hex escape. */
static const char *
charname(uint16_t uc, char *buf, size_t n)
{
    if (uc >= 0x20 && uc < 0x7f) {
        axl_snprintf(buf, n, "'%c'", (char)uc);
    } else {
        axl_snprintf(buf, n, "0x%04x", uc);
    }
    return buf;
}

static uint64_t
parse_ms(const char *s)
{
    uint64_t v = 0;
    return (axl_str_to_u64(s, 10, &v, NULL) == AXL_OK) ? v : 0;
}

int
main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (axl_strcmp(argv[i], "--debounce") == 0 && i + 1 < argc) {
            opt_debounce_ms = parse_ms(argv[++i]);
        } else if (axl_strcmp(argv[i], "--throttle") == 0 && i + 1 < argc) {
            opt_throttle_ms = parse_ms(argv[++i]);
        } else if (axl_strcmp(argv[i], "--help") == 0
                   || axl_strcmp(argv[i], "-h") == 0) {
            axl_printf(
                "kbprobe [--debounce MS] [--throttle MS]\r\n"
                "  Log each keystroke with a microsecond delta. Press Esc to finish.\r\n"
                "  --debounce MS  drop a same-key repeat within MS (validates the fix)\r\n"
                "  --throttle MS  sleep MS after each key (mimic a heavy per-key loop)\r\n");
            return 0;
        }
    }

    axl_printf("kbprobe: press keys (Esc to finish). debounce=%lums throttle=%lums\r\n",
               (unsigned long)opt_debounce_ms, (unsigned long)opt_throttle_ms);
    axl_printf("        d = microseconds since the previous keystroke.\r\n\r\n");

    uint64_t t0       = axl_time_get_us();
    bool     have_prev = false;
    uint64_t prev_us  = 0;
    AxlKey   prev     = { 0 };
    unsigned shown    = 0;
    unsigned dropped  = 0;

    for (;;) {
        AxlKey k;
        if (axl_console_read_key(UINT64_MAX, &k) != AXL_OK) {
            if (axl_interrupted()) {
                break;
            }
            continue;
        }
        uint64_t now = axl_time_get_us();

        if (k.scan_code == SCAN_ESC) {
            break;
        }

        uint64_t rel  = now - t0;
        uint64_t d    = have_prev ? (now - prev_us) : 0;
        bool     same = have_prev
                        && k.scan_code == prev.scan_code
                        && k.unicode_char == prev.unicode_char;

        char cb[12];
        charname(k.unicode_char, cb, sizeof cb);

        /* Heuristic signature tag for a same-key repeat. */
        const char *tag = "";
        if (same && d > 0) {
            if (d < 5000) {
                tag = "  <== BOUNCE? (<5ms burst)";
            } else if (d >= 14000 && d <= 26000) {
                tag = "  <== typematic? (~20ms)";
            } else if (d < 60000) {
                tag = "  <== fast repeat";
            }
        }

        if (opt_debounce_ms != 0 && same && d < opt_debounce_ms * 1000) {
            dropped++;
            axl_printf("  [drop]  t=+%lu.%03lums  d=%6lu us  scan=0x%04x char=%s (debounced)\r\n",
                       (unsigned long)(rel / 1000), (unsigned long)(rel % 1000),
                       (unsigned long)d, k.scan_code, cb);
        } else {
            shown++;
            axl_printf("#%-4u  t=+%lu.%03lums  d=%6lu us  scan=0x%04x char=%s%s\r\n",
                       shown, (unsigned long)(rel / 1000), (unsigned long)(rel % 1000),
                       (unsigned long)d, k.scan_code, cb, tag);
        }

        prev      = k;
        prev_us   = now;
        have_prev = true;

        if (opt_throttle_ms != 0) {
            axl_msleep(opt_throttle_ms);
        }
    }

    axl_printf("\r\nkbprobe: %u events shown, %u debounced-drops.\r\n", shown, dropped);
    return 0;
}
