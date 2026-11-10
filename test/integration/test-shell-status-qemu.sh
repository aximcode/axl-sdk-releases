#!/bin/bash
# test-meta: arch=both needs= est=40 local-only=0 builds=shell-status-selftest,exit-status-selftest,shell-launcher guest=none
# guest=none: the Shell is the subject: its status reporting in a Shell
#   booted with and without -nonest (AXL_SHELL_OPTIONS, fixed at boot), and
#   before any current directory exists on its boot disk.
# test-shell-status-qemu.sh -- axl_shell_execute_status reports whether a line
# ran, and its status, exactly as docs/AXL-Test-Guest-Pool-Design.md §2.4
# measured: in a Shell booted -nonest every case has a definite answer, and in
# a default (nesting) Shell every answer is UNKNOWN.
#
# shell-status-selftest.efi prints one exact line per case; each is asserted
# whole (exact strings: a substring would let the outcome or status drift).
# Fixtures, all staged on fs0: -- s3.nsh (`exit /b 3`), s0.nsh (`echo`),
# sbreak.nsh (signals the Shell's break event -- a scripted Ctrl-C -- then
# would echo; it reads RAN 0, a limit the contract states), a directory,
# exit-status-selftest.efi (an app returning an exact status) and wrong.efi
# (the same image with its PE machine field set to the other architecture,
# so LoadImage refuses it).
#
# Usage: ./test/integration/test-shell-status-qemu.sh [--arch X64|AARCH64]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1
# -nonest comes from the launcher; never let auto mode boot without it.
export AXL_SHELL_LAUNCHER=1

TEST_ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *) shift ;;
    esac
done
declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
NATIVE="${_NATIVE[$TEST_ARCH]:-x64}"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NATIVE" shell-status-selftest \
    exit-status-selftest shell-launcher 2>&1 | tail -1
PREFIX="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$NATIVE")"

WORK="$(mktemp -d -t axl-shstatus.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT
cp "$PREFIX/exit-status-selftest.efi" "$WORK/exit-status-selftest.efi"
# wrong.efi: the PE Machine field (2 bytes at e_lfanew + 4) set to the OTHER
# architecture's, so the firmware refuses to load it.
cp "$PREFIX/exit-status-selftest.efi" "$WORK/wrong.efi"
lfanew=$(od -An -t u4 -j 60 -N 4 "$WORK/wrong.efi" | tr -d ' ')
if [[ "$TEST_ARCH" == X64 ]]; then other='\x64\xaa'; else other='\x64\x86'; fi
printf "$other" | dd of="$WORK/wrong.efi" bs=1 seek=$((lfanew + 4)) conv=notrunc status=none

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# boot <mode> <log>: run the selftest in a fresh guest. The nsh stages the
# script fixtures itself (UCS-2 files the Shell writes), runs the selftest
# once before any current directory exists (nocwd), then from fs0:.
boot() {
    local mode="$1" log="$2" nsh="$WORK/$1.nsh"
    {
        echo 'fs0:\shell-status-selftest.efi nocwd'
        echo 'fs0:'
        echo 'mkdir adir'
        echo 'echo "exit /b 3" > s3.nsh'
        echo 'echo "echo in-script" > s0.nsh'
        echo 'echo "fs0:\shell-status-selftest.efi break" > sbreak.nsh'
        echo 'echo "echo AFTER-BREAK" >> sbreak.nsh'
        echo "fs0:\\shell-status-selftest.efi $mode fs0:"
    } > "$nsh"
    local opts=""
    [[ "$mode" == nonest ]] && opts="-nonest"
    AXL_SHELL_OPTIONS="$opts" timeout 200s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$TEST_ARCH" --timeout 150 --nsh "$nsh" \
        --extra "$WORK/exit-status-selftest.efi" --extra "$WORK/wrong.efi" \
        "$PREFIX/shell-status-selftest.efi" > "$log" 2>&1
    tr -d '\r' < "$log" | grep -a '^CASE|' > "$log.cases"
}

# expect <log> <exact line>...
expect() {
    local log="$1"; shift
    local line
    for line in "$@"; do
        if grep -qxF "$line" "$log.cases"; then
            pass "$line"
        else
            fail "$line"
            local name; name=$(cut -d'|' -f2 <<< "$line")
            grep -a "^CASE|$name|" "$log.cases" | sed 's/^/      got: /'
        fi
    done
}

echo "=== axl_shell_execute_status ($TEST_ARCH) ==="

boot nonest "$WORK/nonest.log"
if ! grep -q SHELL-STATUS-DONE "$WORK/nonest.log"; then
    fail "nonest: the selftest finished"
    tail -15 "$WORK/nonest.log" | sed 's/^/      /'
fi
expect "$WORK/nonest.log" \
    'CASE|nocwd-unknown|UNKNOWN|0xe|nonest=1|reset=0' \
    'CASE|echo|RAN|0x0|nonest=1|reset=1' \
    'CASE|unknown|NOT_RUN|0xe|nonest=1|reset=1' \
    'CASE|missing-efi|NOT_RUN|0xe|nonest=1|reset=1' \
    'CASE|wrong-arch|NOT_RUN|0x3|nonest=1|reset=1' \
    'CASE|directory|NOT_RUN|0xe|nonest=1|reset=1' \
    'CASE|app-34|RAN|0x34|nonest=1|reset=1' \
    'CASE|app-aborted|RAN|0x15|nonest=1|reset=1' \
    'CASE|goto|RAN|0x800000000000000e|nonest=1|reset=1' \
    'CASE|nsh-exit3|RAN|0x3|nonest=1|reset=1' \
    'CASE|nsh-echo|RAN|0x0|nonest=1|reset=1' \
    'CASE|nsh-break|RAN|0x0|nonest=1|reset=1' \
    'CASE|drive|RAN|0x0|nonest=1|reset=1' \
    'CASE|comment|RAN|0x0|nonest=1|reset=1' \
    'CASE|pipe|RAN|0x5|nonest=1|reset=1' \
    'CASE|double-redirect|NOT_RUN|0x0|nonest=1|reset=1' \
    'CASE|unmapped-drive|NOT_RUN|0x0|nonest=1|reset=1' \
    'CASE|exit|DENIED' \
    'CASE|exit-quoted|DENIED' \
    'CASE|exit-part-quoted|DENIED' \
    'CASE|exit-escaped|DENIED' \
    'CASE|exit-glued|NOT_RUN|0xe|nonest=1|reset=1' \
    'CASE|at-echo|NOT_RUN|0xe|nonest=1|reset=1' \
    'CASE|at-space-echo|NOT_RUN|0xe|nonest=1|reset=1' \
    'CASE|alias-exit-define|RAN|0x0|nonest=1|reset=1' \
    'CASE|alias-exit|DENIED' \
    'CASE|empty|ERR' \
    'CASE|exit-in-one-word|NOT_RUN|0xe|nonest=1|reset=1'

# The known limit, pinned: the break DID stop the script (its second line
# never ran), yet the line reads RAN -- the Shell consumes the break before
# Execute returns, so nothing is left to see.
# Control: the same check SEES a script's echo line (s0.nsh's), so the
# absence below is the break's doing, not a log that never shows one.
if tr -d '\r' < "$WORK/nonest.log" | grep -qx 'in-script'; then
    pass "nonest: control -- a script's echo line reaches the log"
else
    fail "nonest: control -- a script's echo line reaches the log"
fi
if tr -d '\r' < "$WORK/nonest.log" | grep -qx 'AFTER-BREAK'; then
    fail "nonest: the break stopped sbreak.nsh before its second line"
else
    pass "nonest: the break stopped sbreak.nsh before its second line"
fi

boot nested "$WORK/nested.log"
if ! grep -q SHELL-STATUS-DONE "$WORK/nested.log"; then
    fail "nested: the selftest finished"
    tail -15 "$WORK/nested.log" | sed 's/^/      /'
fi
expect "$WORK/nested.log" \
    'CASE|echo|UNKNOWN|0x0|nonest=0|reset=0' \
    'CASE|unknown|UNKNOWN|0x0|nonest=0|reset=0' \
    'CASE|wrong-arch|UNKNOWN|0x3|nonest=0|reset=0' \
    'CASE|app-34|UNKNOWN|0x34|nonest=0|reset=0' \
    'CASE|exit|DENIED' \
    'CASE|exit-quoted|DENIED' \
    'CASE|exit-part-quoted|DENIED' \
    'CASE|exit-escaped|DENIED' \
    'CASE|empty|ERR'

echo ""
echo "--- every case, as reported ---"
for m in nonest nested; do sed "s/^/  $m: /" "$WORK/$m.log.cases"; done
echo ""
echo "shell-status ($TEST_ARCH): $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
