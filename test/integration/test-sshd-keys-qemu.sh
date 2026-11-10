#!/bin/bash
# test-meta: arch=both needs=ssh,ssh-keygen,socat,tmux est=86 local-only=0 builds=sshd,key-dump guest=none
# guest=none: it types on the guest's USB keyboard (QMP send-key) and into
#   its Shell (test_send_stdin); a leased guest takes neither.
# test-sshd-keys-qemu.sh -- one part of the sshd key suite: the keys a plain
# xterm can say, typed over ssh, reach the UEFI Shell EXACTLY as the same key
# typed on a real USB keyboard.
#
# The boot, the probe, `start` and the row machinery are
# lib/sshd-keys-lib.sh, whose header says how the suite is split and why;
# test-sshd-keys-ext-qemu.sh is the other part.
#
# Scenarios, in run order:
#   start        (the lib's) sshd serving, key-dump.efi ready in session D
#   D rows       a plain xterm-256color tmux session: letters, Ctrl/Alt
#                letters, cursor and editing keys with Shift/Ctrl/Alt,
#                F-keys with modifiers, Tab/BTab, Backspace, Enter, Esc,
#                Space, Ctrl-Space, C-\ and C-] (ROWS_D in the lib)
#   pins         six rows also pinned to exact hand-written lines, so both
#                sides regressing together cannot pass the differential
#   end          Ctrl+Alt+q ends the probe, its KEY line the only one since
#                the last row
#
# EXCLUDED from session D, and why:
#   C-h C-i C-m C-[   a terminal sends 08 / 09 / 0d / 1b, which are also
#                     Backspace / Tab / Enter / Esc; those bytes stay those
#                     keys (byte-ambiguous). Covered in session U, where
#                     CSI-u says which key it was.
#   C-S-a             a terminal sends 0x01, the same as C-a: Shift is lost
#                     before the bytes leave the client.
#   C-c               the Shell's out-of-band break, not a key.
#   C-s C-q           the ordered-queue pause/resume, and the USB keyboard's
#                     ctrl-s fires the Shell's HaltOutput notify, which eats
#                     the next key (survey Q2).
#   M-[ M-O           ESC [ and ESC O introduce CSI and SS3 sequences.
#
# Usage: ./test/integration/test-sshd-keys-qemu.sh [--arch X64|AARCH64]


source "$(dirname "$0")/lib/sshd-keys-lib.sh"

# --- D rows -------------------------------------------------------------------
for r in "${ROWS_D[@]}"; do key_row "$r" D; done

# --- end ----------------------------------------------------------------------
# The probe ends before the power-off (a running key-dump.efi eats the
# `reset -s`), and a late line from the LAST row fails here: a later row's
# check is what would otherwise catch it, and there is none.
hmp_sendkey ctrl-alt-q
check "end: Ctrl+Alt+q ends key-dump.efi" wait_guest "KEYDUMP-DONE" 1 10
next_keys
check "end: the only KEY line since the last row is Ctrl+Alt+q's [$NEXT_KEYS]" \
    test "$NEXT_KEYS" = 'KEY sc=0000 u=0071 ss=80000028 ts=c0'
tmux_close D

# start 2, a row per key, the pins, end 2, the three run-wide checks.
keys_finish "sshd keys" $(( 2 + ${#ROWS_D[@]} + ${#PIN[@]} + 2 + 3 ))
