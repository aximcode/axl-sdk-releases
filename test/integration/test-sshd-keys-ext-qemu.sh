#!/bin/bash
# test-meta: arch=both needs=ssh,ssh-keygen,socat,tmux est=59 local-only=0 builds=sshd,key-dump guest=none
# guest=none: it types on the guest's USB keyboard (QMP send-key) and into
#   its Shell (test_send_stdin); a leased guest takes neither.
# test-sshd-keys-ext-qemu.sh -- one part of the sshd key suite: the keys only
# a CSI-u terminal can say, typed over ssh, reach the UEFI Shell EXACTLY as
# the same key typed on a real USB keyboard; `edit` reads them alike; and the
# oracle really was the USB keyboard.
#
# The boot, the probe, `start` and the row machinery are
# lib/sshd-keys-lib.sh, whose header says how the suite is split and why;
# test-sshd-keys-qemu.sh is the other part.
#
# Scenarios, in run order:
#   start        (the lib's) sshd serving, key-dump.efi ready in session D
#   U rows       a second tmux session, which must join repainted with the
#                console so far, with `extended-keys always` in csi-u
#                format, for what a plain xterm cannot say: Ctrl-Tab,
#                Ctrl/Shift-Enter, Ctrl-1, Ctrl-Backspace, Shift-Space and
#                the four byte-ambiguous Ctrl letters, which tmux cannot
#                encode and are sent as a CSI-u terminal's bytes (ROWS_U in
#                the lib)
#   edit         Ctrl+Alt+q ends the probe, its KEY line the only one since
#                the last row; `edit kp.txt` (session D): USB Ctrl-End shows "Unknown
#                Command" and leaves the cursor at 1,1 (row,column); End
#                moves to 1,9 and clears the status; Home returns to 1,1; Ctrl-End
#                typed over ssh then shows a status row byte-identical to the
#                USB keyboard's; F3 exits
#   only-usb     the oracle really was the USB keyboard: with usb-kbd
#                unplugged (device_del) a sendkey produces no key, while a
#                key typed over ssh still reaches the probe. On x64 the
#                q35 i8042 is switched off so PS/2 cannot take the keys.
#
# Usage: ./test/integration/test-sshd-keys-ext-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/lib/sshd-keys-lib.sh"

# --- U rows -------------------------------------------------------------------
tmux_ssh_open U "${U_CONF[@]}"
# U joins a console already in use, and sshd repaints the new session with
# the screen so far, whose newest row is the probe's KEYDUMP-READY. That
# repaint is U attached.
check "U: the second session joins repainted with the console so far" \
    tmux_wait_screen U 'KEYDUMP-READY' 20
for r in "${ROWS_U[@]}"; do key_row "$r" U; done
tmux_close U

# --- edit ---------------------------------------------------------------------
hmp_sendkey ctrl-alt-q
check "edit: Ctrl+Alt+q ends key-dump.efi" wait_guest "KEYDUMP-DONE" 1 10
# Exactly the Ctrl+Alt+q line itself: a late line from the last U row would
# otherwise be absorbed here and blamed on no row.
next_keys
check "edit: the only KEY line since the last row is Ctrl+Alt+q's [$NEXT_KEYS]" \
    test "$NEXT_KEYS" = 'KEY sc=0000 u=0071 ss=80000028 ts=c0'
tmux_text D 'edit kp.txt'
tmux_keys D Enter
# The editor's status bar: ` <row>,<col>`, the status message, `|INS|`.
POS_RE='^ *[0-9]+,[0-9]+ '
tmux_wait_screen D '|INS|' 30 || true

# The status row; the whole screen it came from is kept in edit-screen.log
# for the failure report.
status_row() {
    tmux_screen D > "$TEST_TMPDIR/edit-screen.log" 2>&1 || true
    grep -aE -- "$POS_RE" "$TEST_TMPDIR/edit-screen.log" | head -1 || true
}
# Wait up to 3 s for the status row to match the extended regex $1.
wait_row() {
    local i
    for ((i = 0; i < 15; i++)); do
        grep -qE -- "$1" <<< "$(status_row)" && return 0
        sleep 0.2
    done
    return 1
}

hmp_sendkey ctrl-end
wait_row 'Unknown Command' || true
row_a=$(status_row)
check "edit: USB Ctrl-End leaves the cursor at row 1, column 1 [$row_a]" \
    grep -qE '^ *1,1 ' <<< "$row_a"
check "edit: USB Ctrl-End shows 'Unknown Command' [$row_a]" \
    grep -qF 'Unknown Command' <<< "$row_a"
tmux_keys D End
wait_row '^ *1,9 ' || true
row_c=$(status_row)
check "edit: End moves to column 9 [$row_c]" grep -qE '^ *1,9 ' <<< "$row_c"
check "edit: End clears the status, so the next row is fresh [$row_c]" \
    test -n "$row_c" -a "$(grep -cF 'Unknown Command' <<< "$row_c")" = 0
tmux_keys D Home
wait_row '^ *1,1 ' || true
row_h=$(status_row)
check "edit: Home returns to column 1 [$row_h]" grep -qE '^ *1,1 ' <<< "$row_h"
tmux_keys D C-End
wait_row 'Unknown Command' || true
row_b=$(status_row)
check "edit: ssh Ctrl-End shows the USB keyboard's status row byte for byte (usb [$row_a] ssh [$row_b])" \
    test -n "$row_a" -a "$row_b" = "$row_a"
tmux_keys D F3
check "edit: F3 exits to the Shell" tmux_wait_screen D 'FS0:\>' 15

# --- only-usb -----------------------------------------------------------------
# Every sendkey above went to usb-kbd: unplugged, nothing else can type. The
# probe runs again (ending on its own after ONLY_USB_S), the keyboard is
# removed, and a sendkey must produce NO line -- while a key typed over ssh
# still does, so the silence is not a probe that stopped reading.
ONLY_USB_S=15
tmux_text D "key-dump.efi $ONLY_USB_S"
tmux_keys D Enter
check "only-usb: key-dump.efi runs again" wait_guest "KEYDUMP-READY" 2 30
late=$(key_lines | tail -n +$((KEY_SEEN + 1)))
check "only-usb: no KEY line unaccounted for before the unplug [${late:-none}]" test -z "$late"
KEY_SEEN=$(key_lines | wc -l)
printf 'device_del axl_ukbd\n' >&"$MON_FD"
sleep 3
hmp_sendkey a
next_keys
# The monitor's own word on the unplug, for the failure report.
mon_err=$(grep -a 'Error' "$TEST_TMPDIR/mon.log" | tail -1 || true)
check "only-usb: with usb-kbd unplugged a sendkey reaches no keyboard [$NEXT_KEYS] (monitor: ${mon_err:-no error})" \
    test "$NEXT_KEYS" = '(nothing)'
tmux_keys D a
next_keys
check "only-usb: the probe still reads keys typed over ssh [$NEXT_KEYS]" \
    test "$NEXT_KEYS" = "${PIN[a]}"
check "only-usb: key-dump.efi ends after ${ONLY_USB_S}s" \
    wait_guest "KEYDUMP-DONE" 2 $((ONLY_USB_S + 10))
tmux_close D

# start 2, U's join, a row per key, edit 9, only-usb 5, the three run-wide
# checks.
keys_finish "sshd keys ext" $(( 2 + 1 + ${#ROWS_U[@]} + 9 + 5 + 3 ))
