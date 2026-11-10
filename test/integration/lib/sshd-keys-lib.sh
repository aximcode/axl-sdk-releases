# shellcheck shell=bash
# sshd-keys-lib.sh -- the boot, the probe and the row machinery of the sshd
# key suites: a key typed over ssh reaches the UEFI Shell EXACTLY as the same
# key typed on a real USB keyboard -- same scan code, same character, same
# KeyShiftState, same KeyToggleState.
#
# One boot, one probe, two keyboards. The guest has a USB keyboard (usb-kbd on
# the xHCI controller), `sshd` is running, and key-dump.efi prints one exact
# `KEY sc= u= ss= ts=` line per key it reads from ConsoleInHandle's
# SIMPLE_TEXT_INPUT_EX -- the protocol `edit` reads. For each row the USB
# keyboard presses the combination first (QEMU monitor `sendkey`): that line
# is the ORACLE, measured from the firmware's own UsbKbDxe on this arch, so
# the table is never maintained by hand. Then tmux presses the same key in an
# ssh session -- tmux sends the bytes a terminal sends for it -- and the line
# that arrives must be byte-identical. Each side must produce exactly one line.
#
# TWO TESTS, ONE BOOT SEQUENCE. This was test-sshd-keys-qemu.sh, one boot
# running every scenario in ~85 s; one boot is the unit the runner's pool
# parallelises, so it was split into two parts that source this file:
#
#   test-sshd-keys-qemu.sh       D rows and their pins
#   test-sshd-keys-ext-qemu.sh   U rows, edit, only-usb
#
# Sourcing it runs `start` (below) on a fresh boot; the part then runs its
# scenarios and ends with keys_finish. start and keys_finish's three
# run-wide checks are re-proved by each part against its own boot; every
# other check lives in exactly one part.
#
# start      `sshd -k` prints its serving line; the ssh session in tmux (D)
#            runs key-dump.efi, which prints KEYDUMP-READY
export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"
source "$(dirname "$0")/lib/sshd-tmux-lib.sh"

sshd_require_tools ssh ssh-keygen socat tmux
# Session U needs `extended-keys always` to emit CSI-u. Measured on 3.4 and
# 3.5 (the same bytes for every U row); 3.5 added extended-keys-format, and
# its csi-u value is what 3.4 always sends, so the option is set only from
# 3.5 on (3.4 has no such option). Older releases are untested.
TMUX_V=$(timeout 5 tmux -V 2>&1 || echo "tmux (no version)")
TMUX_VER=$(sed -nE 's/^tmux (next-)?([0-9]+)\.([0-9]+).*/\2 \3/p' <<< "$TMUX_V")
read -r TMUX_MAJ TMUX_MIN <<< "${TMUX_VER:-0 0}"
if (( TMUX_MAJ < 3 || (TMUX_MAJ == 3 && TMUX_MIN < 4) )); then
    missing_tool "$TMUX_V is older than tmux 3.4 (untested for CSI-u extended keys)"
fi
U_CONF=('set -s extended-keys always')
if (( TMUX_MAJ > 3 || TMUX_MIN >= 5 )); then
    U_CONF+=('set -s extended-keys-format csi-u')
fi
# A tmux that runs: a throwaway server, a pane, one capture. A broken host
# tmux (one whose server dies on its first capture-pane, or hangs) would
# otherwise read as the guest failing every row.
if ! tmux_probe; then
    missing_tool "$TMUX_V at $(command -v tmux) cannot start a server and capture its pane"
fi
echo "  host: $TMUX_V at $(command -v tmux); session U: ${U_CONF[*]}"

test_parse_args "$@"
test_setup

# tmux key name -> QEMU sendkey combination.
ROWS_D=(
    'a=a' 'A=shift-a' 'C-a=ctrl-a' 'C-e=ctrl-e' 'C-k=ctrl-k'
    'M-x=alt-x' 'C-M-x=ctrl-alt-x'
    'Up=up' 'S-Up=shift-up' 'C-Up=ctrl-up' 'M-Up=alt-up' 'Down=down'
    'C-Left=ctrl-left' 'C-Right=ctrl-right' 'S-Left=shift-left'
    'Home=home' 'C-Home=ctrl-home' 'S-Home=shift-home'
    'End=end' 'C-End=ctrl-end' 'S-End=shift-end' 'C-S-End=ctrl-shift-end'
    'C-M-End=ctrl-alt-end'
    'PPage=pgup' 'C-PPage=ctrl-pgup' 'S-PPage=shift-pgup'
    'NPage=pgdn' 'C-NPage=ctrl-pgdn'
    'IC=insert' 'C-IC=ctrl-insert' 'DC=delete' 'C-DC=ctrl-delete' 'S-DC=shift-delete'
    'F1=f1' 'S-F1=shift-f1' 'C-F1=ctrl-f1' 'M-F1=alt-f1' 'C-S-F1=ctrl-shift-f1'
    'F2=f2' 'F3=f3' 'F5=f5' 'F12=f12' 'S-F12=shift-f12' 'C-F12=ctrl-f12'
    'Tab=tab' 'BTab=shift-tab' 'BSpace=backspace' 'Enter=ret'
    'Escape=esc' 'M-Escape=alt-esc' 'Space=spc' 'C-Space=ctrl-spc'
    'C-\=ctrl-backslash' 'C-]=ctrl-bracket_right'
)
# A third field is the bytes to send instead of the key name. tmux (3.4 and
# 3.5) cannot send C-i, C-m, C-h or C-[ as anything but 09 0d 08 1b, even with
# extended keys: it folds them into Tab, Enter, Backspace and Escape before
# encoding. So those four rows send ESC [ <letter> ; 5 u, which a terminal
# sends UNASKED when so configured -- WezTerm with enable_csi_u_key_encoding,
# xterm with modifyOtherKeys:2 and formatOtherKeys:1 (sshd never asks a
# client for an extended-key mode). They test the decoder's CSI-u handling,
# not tmux.
ROWS_U=(
    'C-Tab=ctrl-tab' 'C-Enter=ctrl-ret' 'S-Enter=shift-ret' 'C-1=ctrl-1'
    'C-BSpace=ctrl-backspace' 'S-Space=shift-spc'
    'C-i=ctrl-i=1b5b3130353b3575' 'C-m=ctrl-m=1b5b3130393b3575'
    'C-h=ctrl-h=1b5b3130343b3575' 'C-[=ctrl-bracket_left=1b5b39313b3575'
)
# Rows whose line is also pinned by hand (session D).
declare -A PIN=(
    [a]='KEY sc=0000 u=0061 ss=80000000 ts=c0'
    [C-a]='KEY sc=0000 u=0061 ss=80000008 ts=c0'
    [C-End]='KEY sc=0006 u=0000 ss=80000008 ts=c0'
    [M-x]='KEY sc=0000 u=0078 ss=80000020 ts=c0'
    [BTab]='KEY sc=0000 u=0009 ss=80000002 ts=c0'
    [S-F12]='KEY sc=0016 u=0000 ss=80000002 ts=c0'
)
KEY_RE='^KEY sc=[0-9a-f]{4} u=[0-9a-f]{4} ss=[0-9a-f]{8} ts=[0-9a-f]{2}$'
# Seconds a side has to produce its line, and the quiet after it that proves
# no second line follows.
KEY_WAIT_S=3
KEY_SETTLE_S=0.3

sshd_build
TEST_BUILD_DIR="$(test_build_dir)"
declare -A _KD_ARCH=([X64]=x64 [AARCH64]=aa64)
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_KD_ARCH[$TEST_ARCH]}" key-dump \
    2>&1 | tail -1
[[ -f "$TEST_BUILD_DIR/key-dump.efi" ]] || { echo "FAIL: key-dump.efi was not built"; exit 1; }
test_add_efi "$TEST_BUILD_DIR/key-dump.efi" key-dump.efi
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/ak"
printf 'line one\r\n' > "$TEST_STAGING/kp.txt"

echo "=== sshd keys: ssh vs a USB keyboard ($TEST_ARCH) ==="
# The keyboard sits on the harness's hub (port 2): qemu-xhci has four USB 2
# root ports, the harness holds 1-2 and gives 3-4 to its USB sticks, and a
# full-speed device cannot take a USB 3 port (5-8).
#
# The oracle must BE the USB keyboard. On x64, q35 also has a PS/2 i8042, and
# which keyboard a monitor sendkey reaches depends on QEMU's input-handler
# order -- so the i8042 is switched off, leaving usb-kbd the only keyboard
# that can deliver. The only-usb control at the end proves it on both arches:
# with usb-kbd unplugged, a sendkey must produce no key at all.
SSHD_QEMU_EXTRA=(
    -device "usb-kbd,bus=axl_usb0.0,port=2.2,id=axl_ukbd"
    -monitor "unix:$TEST_TMPDIR/mon.sock,server,nowait"
)
[[ "$TEST_ARCH" == X64 ]] && SSHD_QEMU_EXTRA+=(-machine i8042=off)
sshd_boot ""
hmp_open || { echo "FAIL: the QEMU monitor socket never appeared"; exit 1; }

# --- start ------------------------------------------------------------------
send "sshd -k ak -x $HOST_AS_SEEN\\r"
check "start: the launcher prints its serving line" \
    wait_guest "sshd: serving the UEFI Shell on $GUEST_IP port $GUEST_PORT" 1 60
echo "SERVING $(date +%s.%N)" > "$TEST_TMPDIR/serving-at.log"
tmux_ssh_open D
tmux_wait_screen D 'FS0:\>' 40 || true
tmux_text D 'key-dump.efi'
tmux_keys D Enter
check "start: key-dump.efi, typed over ssh, prints KEYDUMP-READY" \
    wait_guest "KEYDUMP-READY" 1 30

# The KEY lines in the serial log, and how many have been consumed.
key_lines() { test_clean_log; grep -aE -- "$KEY_RE" "$TEST_CLEAN_LOG" || true; }
KEY_SEEN=0
# NEXT_KEYS: the KEY lines that arrived since the last call, joined by " | ",
# or `(nothing)`. Waits up to KEY_WAIT_S for the first, then KEY_SETTLE_S
# more. Not a $(...) function: KEY_SEEN must survive the call.
next_keys() {
    local i n new
    for ((i = 0; i < KEY_WAIT_S * 10; i++)); do
        n=$(key_lines | wc -l)
        (( n > KEY_SEEN )) && break
        sleep 0.1
    done
    sleep "$KEY_SETTLE_S"
    new=$(key_lines | tail -n +$((KEY_SEEN + 1)))
    KEY_SEEN=$((KEY_SEEN + $(grep -c . <<< "$new" || true)))
    if [[ -z "$new" ]]; then
        NEXT_KEYS='(nothing)'
    else
        NEXT_KEYS=$(paste -sd'|' <<< "$new" | sed 's/|/ | /g')
    fi
}

# One row, `TMUXKEY=COMBO[=HEX]`: the USB keyboard presses COMBO, then
# session $2 presses TMUXKEY (or sends HEX).
key_row() {
    local row="$1" sess="$2" tkey combo hex usb ssh late
    IFS='=' read -r tkey combo hex <<< "$row"
    # A KEY line nobody has accounted for arrived after the previous row's
    # settle: it is that row's, so it fails there, not in this row's lines.
    late=$(key_lines | tail -n +$((KEY_SEEN + 1)))
    if [[ -n "$late" ]]; then
        fail "late KEY line(s) after the row before $tkey: [$(paste -sd'|' <<< "$late" | sed 's/|/ | /g')]"
        KEY_SEEN=$((KEY_SEEN + $(grep -c . <<< "$late" || true)))
    fi
    hmp_sendkey "$combo"
    next_keys
    usb=$NEXT_KEYS
    if [[ -n "$hex" ]]; then tmux_hex "$sess" "$hex"; else tmux_keys "$sess" "$tkey"; fi
    next_keys
    ssh=$NEXT_KEYS
    echo "  $sess $tkey / $combo: usb [$usb] ssh [$ssh]" >> "$TEST_TMPDIR/rows.txt"
    if [[ "$usb" =~ $KEY_RE && "$ssh" == "$usb" ]]; then
        pass "row $tkey ($combo): ssh line == USB line [$usb]"
    else
        fail "row $tkey ($combo): USB [$usb] ssh [$ssh]"
    fi
    if [[ -n "${PIN[$tkey]:-}" ]]; then
        check "pin $tkey: USB and ssh are exactly '${PIN[$tkey]}' (usb [$usb] ssh [$ssh])" \
            test "$usb" = "${PIN[$tkey]}" -a "$ssh" = "${PIN[$tkey]}"
    fi
}

# keys_finish SUMMARY EXPECTED -- power off, the three run-wide checks, the
# summary line, and the exit status: every check passed and exactly EXPECTED
# of them ran.
keys_finish() {
    local summary="$1" expected="$2" n
    sshd_poweroff
    n=$(heap_corruptions)
    check "no heap corruption reported in the run ($n)" test "$n" = 0
    n=$(cpu_exceptions)
    check "no CPU exception reported in the run ($n)" test "$n" = 0
    test_clean_log
    if test_check_leaks; then
        pass "leak gate: every verdict in the run is clean"
    else
        fail "leak gate: a leak or a missing verdict"
    fi
    echo ""
    printf "%s: %d passed, %d failed (%s)\n" "$summary" "$PASS" "$FAIL" "$TEST_ARCH"
    if [[ $FAIL -gt 0 || $PASS -ne $expected ]]; then
        echo "--- every row ---"; cat "$TEST_TMPDIR/rows.txt" 2>/dev/null
        echo "--- the last edit screen ---"; cat "$TEST_TMPDIR/edit-screen.log" 2>/dev/null
        echo "--- Serial log ---"; tail -60 "$TEST_CLEAN_LOG"
    fi
    [[ $FAIL -eq 0 && $PASS -eq $expected ]] && exit 0 || exit 1
}
