#!/bin/bash
# test-meta: arch=x64 needs=socat,virtiofsd est=15 local-only=0 guest=none
# guest=none: a real Ctrl-C over --serial-socket to a --background guest, on
#   a --mount volume: a machine no class gives.
# test-tar-ctrlc-qemu.sh -- a real Ctrl-C in the middle of `tar -x`.
#
# tar extracts each regular file to a ~TARhhhhhhhh.TMP temporary beside it
# and renames it into place (safe writes). A Ctrl-C mid-member must leave
# no temporary behind and must not touch the file the member would have
# replaced; tar then says "tar: interrupted" and exits 1, as tools/tar.c
# documents. The archive holds many 2 MiB members, extracted over virtiofs
# into a directory SEEDED with a small original of every member's name.
# Ctrl-C (0x03) goes over the serial socket once the second member is
# listed (-v), as test-axbench-ctrlc-qemu.sh does it.
#
# Asserted, on the host, after the guest is gone:
#   - "tar: interrupted" was printed and %lasterror% is 0x1;
#   - no ~TAR*.TMP is left anywhere in the destination;
#   - every destination file is EITHER its seeded original, byte for byte,
#     OR the whole member -- never short, never empty;
#   - at least one original survived (the Ctrl-C really landed mid-run;
#     without this a fast guest could finish first and pass vacuously).
#
# Usage: test/integration/test-tar-ctrlc-qemu.sh     Requires: socat.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
TAR="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)/tools/tar.efi"

export TEST_SKIP_RATCHET=1
PASS=0
FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "$2"; FAIL=$((FAIL + 1)); }

command -v socat >/dev/null 2>&1 || { echo "ERROR: socat required" >&2; exit 2; }

WORK="$(mktemp -d)"
SOCK="$WORK/serial.sock"
QIN="$WORK/stdin.fifo"
LOG="$WORK/serial.log"
RUN_LOG="$WORK/run.out"
BRIDGE_PID=""
QEMU_PID=""
cleanup() {
    [[ -n "$BRIDGE_PID" ]] && kill "$BRIDGE_PID" 2>/dev/null || true
    [[ -n "$QEMU_PID" ]] && kill -9 "$QEMU_PID" 2>/dev/null || true
    exec 9>&- 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 tools 2>&1 | tail -1
[[ -x "$TAR" ]] || { echo "FAIL: tar.efi not found at $TAR"; exit 1; }

# The archive: N members of 2 MiB, each a distinct byte pattern.
N=60
mkdir -p "$WORK/src/big" "$WORK/data/out/big"
for i in $(seq -w 1 "$N"); do
    head -c 2097152 /dev/zero | tr '\0' "$(printf "\\$(printf '%03o' $((65 + 10#$i % 26)))")" \
        > "$WORK/src/big/m$i.bin"
    printf 'orig-%s\n' "$i" > "$WORK/data/out/big/m$i.bin"
done
tar -cf "$WORK/data/big.tar" -C "$WORK/src" big
cp -r "$WORK/data/out" "$WORK/orig"

cat > "$WORK/tar.nsh" <<'EOF'
fs0:
echo CTRLC:START
tar.efi -v -x FS1:\big.tar -C FS1:\out
echo CTRLC:RC=%lasterror%
reset -s
EOF

qemu_timeout=120
if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then qemu_timeout=300; fi
"$RUN_QEMU" --background --timeout "$qemu_timeout" --serial-socket "$SOCK" \
    --mount "$WORK/data" --nsh "$WORK/tar.nsh" "$TAR" > "$RUN_LOG" 2>&1
QEMU_PID=$(grep -oP '^QEMU_PID=\K\d+' "$RUN_LOG" || true)
[[ -n "$QEMU_PID" ]] || { echo "ERROR: no QEMU pid"; cat "$RUN_LOG"; exit 1; }

for _ in $(seq 1 50); do [[ -S "$SOCK" ]] && break; sleep 0.1; done
[[ -S "$SOCK" ]] || { echo "ERROR: serial socket never appeared"; exit 1; }
mkfifo "$QIN"
exec 9<>"$QIN"
socat - "UNIX-CONNECT:$SOCK" < "$QIN" | tee -a "$LOG" >/dev/null &
BRIDGE_PID=$!

# Wait for the second member to be listed: extraction is under way.
for _ in $(seq 1 600); do
    grep -q 'big\\m02\.bin' "$LOG" 2>/dev/null && break
    sleep 0.1
done
grep -q 'big\\m02\.bin' "$LOG" 2>/dev/null \
    || { echo "ERROR: extraction never started"; tail -30 "$LOG"; exit 1; }
printf '\x03' > "$QIN"
echo "Ctrl-C (0x03) sent"

# The guest ends with `reset -s`; wait for QEMU to exit (bounded).
for _ in $(seq 1 600); do
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 0.1
done
kill -0 "$QEMU_PID" 2>/dev/null && { echo "ERROR: QEMU still running"; tail -30 "$LOG"; exit 1; }
QEMU_PID=""
CLEAN="$(tr -d '\r' < "$LOG")"

if grep -qx 'tar: interrupted' <<< "$CLEAN" && grep -qx 'CTRLC:RC=0x1' <<< "$CLEAN"; then
    pass "tar -x says 'tar: interrupted' and exits 0x1"
else
    fail "tar -x did not report the interrupt / exit 0x1" \
         "$(grep -E '^tar:|CTRLC:' <<< "$CLEAN" | tail -10)"
fi

# tar owns the interrupt: it installs a handler, so the runtime's own
# "Interrupted (Ctrl-C)" notice -- printed only when no handler is
# installed -- must not appear. Without the handler, or with the default
# restored before exit, it does.
if grep -q 'Interrupted (Ctrl-C)' <<< "$CLEAN"; then
    fail "the runtime's 'Interrupted (Ctrl-C)' notice appeared: tar did not own the interrupt" \
         "$(grep -n 'Interrupted' <<< "$CLEAN")"
else
    pass "only tar's own lines report the interrupt (no runtime notice)"
fi

# The end-of-extract backstop never has a temporary left to remove.
if grep -q 'tar: internal:' <<< "$CLEAN"; then
    fail "tar's end-of-extract backstop found a temporary" "$(grep 'tar: internal:' <<< "$CLEAN")"
else
    pass "no temporary outlived its member (backstop silent)"
fi

stray="$(find "$WORK/data/out" -iname '~tar*.tmp')"
if [[ -z "$stray" ]]; then
    pass "no ~TAR*.TMP temporary is left in the destination"
else
    fail "temporaries were left behind" "$stray"
fi

bad=""
kept=0
for i in $(seq -w 1 "$N"); do
    f="$WORK/data/out/big/m$i.bin"
    if cmp -s "$f" "$WORK/orig/big/m$i.bin"; then
        kept=$((kept + 1))
    elif ! cmp -s "$f" "$WORK/src/big/m$i.bin"; then
        bad+="m$i.bin ($(stat -c %s "$f" 2>/dev/null || echo missing) bytes) "
    fi
done
if [[ -z "$bad" ]]; then
    pass "every target is its original or the whole member -- never short"
else
    fail "targets left neither original nor whole" "$bad"
fi
if [[ "$kept" -gt 0 ]]; then
    pass "the Ctrl-C landed mid-run ($kept of $N originals untouched)"
else
    fail "every member was extracted: the Ctrl-C did not interrupt anything"
fi

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"
[[ "$FAIL" -eq 0 ]]
