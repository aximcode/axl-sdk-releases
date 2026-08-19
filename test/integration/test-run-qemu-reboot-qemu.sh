#!/bin/bash
# test-meta: arch=both needs= est=44 local-only=0
# test-run-qemu-reboot-qemu.sh — a guest-initiated reset must be able to
# re-POST the SAME VM, so a consumer can test what its firmware does on the
# NEXT boot (boot counters, rollback-on-next-boot, NVRAM that must survive).
#
# Asserts, over three boots of a startup.nsh that prints a marker and then
# `reset -c`:
#   1. DEFAULT is unchanged — no flag, no override: the reset ENDS the run
#      (QEMU exits, exactly one marker). Callers use "QEMU is gone" as the
#      observable signal that the guest reset itself; that must keep working.
#   2. `--reboot` re-POSTs: the marker appears again on a second boot.
#   3. `--qemu-arg -action --qemu-arg reboot=reset` re-POSTs too — run-qemu's
#      own `-no-reboot` must NOT beat an explicit caller token. This is the
#      general contract: --qemu-arg tokens are appended LAST.
#
# Usage: ./test/integration/test-run-qemu-reboot-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "ERROR: unknown arg '$1'"; exit 2 ;;
    esac
done
case "$ARCH" in
    X64)     NATIVE=x64 ;;
    AARCH64) NATIVE=aa64 ;;
    *) echo "ERROR: --arch X64|AARCH64"; exit 2 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
TOOLS="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$NATIVE")/tools"

make -C "$PROJECT_DIR" ARCH="$NATIVE" tools >/dev/null 2>&1 || true
EFI="$TOOLS/hexdump.efi"
[[ -f "$EFI" ]] || { echo "ERROR: $EFI not built"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

MARK="AXL_REPOST_MARK"
NSH="$TMP/reboot.nsh"
{ echo '@echo -off'; echo "echo $MARK"; echo 'reset -c'; } > "$NSH"

# TCG (aa64 everywhere, x64 without KVM) POSTs several times slower.
BOOT_SECS=25
if [[ "$ARCH" == "AARCH64" || ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    BOOT_SECS=90
fi

PASS=0
FAIL=0

# Boot in the background with the given extra run-qemu args, wait for either
# `want` markers or QEMU's exit, then report "<markers> <alive>". Kills QEMU
# and removes its TMPDIR before returning.
boot_marks() {
    local want="$1"; shift
    local log="$TMP/serial-$want-$RANDOM.log" out pid qtmp marks alive
    rm -f "$log"
    # Budget two boots plus slack; a re-POSTing guest never exits on its own,
    # so this is also the backstop that reaps it.
    out=$("$RUN_QEMU" --arch "$ARCH" --background \
                      --timeout $((BOOT_SECS * 2 + 30)) \
                      --serial-log "$log" --nsh "$NSH" "$@" "$EFI" 2>&1)
    pid=$(sed -n 's/^QEMU_PID=//p' <<< "$out")
    qtmp=$(sed -n 's/^TMPDIR=//p' <<< "$out")
    if [[ -z "$pid" ]]; then
        # Launch itself failed (unknown flag, missing firmware, ...). Report a
        # sentinel rather than a marker count — `expect` turns it into a FAIL
        # with run-qemu's own diagnostics attached.
        printf 'launch-failed %s\n' "${out//$'\n'/ | }"
        return
    fi
    local deadline=$((SECONDS + BOOT_SECS * 2 + 20))
    while (( SECONDS < deadline )); do
        marks=$(grep -ac "$MARK" "$log" 2>/dev/null || true)
        [[ "${marks:-0}" -ge "$want" ]] && break
        kill -0 "$pid" 2>/dev/null || break     # guest reset ended the run
        sleep 0.5
    done
    marks=$(grep -ac "$MARK" "$log" 2>/dev/null || true)
    alive=no
    kill -0 "$pid" 2>/dev/null && alive=yes
    kill "$pid" 2>/dev/null || true
    [[ -n "$qtmp" ]] && rm -rf "$qtmp"
    printf '%s %s\n' "${marks:-0}" "$alive"
}

expect() {
    local name="$1" want_marks="$2" want_alive="$3" got="$4"
    if [[ "$got" == "$want_marks $want_alive" ]]; then
        echo "PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name -- expected '$want_marks $want_alive', got '$got'"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== run-qemu reset handling ($ARCH) ==="

# 1. Default: the reset ends the run. Exactly one POST, QEMU gone.
expect "default: guest reset exits QEMU (one POST)" 1 no "$(boot_marks 2)"

# 2. --reboot: the same VM POSTs again.
expect "--reboot: guest reset re-POSTs the same VM" 2 yes \
       "$(boot_marks 2 --reboot)"

# 3. The caller's own -action must beat run-qemu's -no-reboot.
expect "--qemu-arg -action reboot=reset overrides the -no-reboot default" 2 yes \
       "$(boot_marks 2 --qemu-arg -action --qemu-arg reboot=reset)"

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"

[[ "$FAIL" -eq 0 ]]
