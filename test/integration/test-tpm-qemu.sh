#!/bin/bash
# test-tpm-qemu.sh — axl_tpm_* against a swtpm-backed TPM 2.0.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# The unit suite boots with no TPM, so axl_tpm_present() is false and
# AxlTestPlatform's test_tpm takes its deterministic "absent" branch.
# The populated path — a real EFI_TCG2_PROTOCOL.GetCapability call that
# fills the hand-written EFI_TCG2_BOOT_SERVICE_CAPABILITY struct — can
# only run with an actual TPM. This wires one via swtpm (the same
# emulator axl-emulate spawns) and asserts the present-branch checks
# pass, validating the struct layout end to end.
#
# x64 only: tpm-tis is the well-supported QEMU model; the capability
# struct uses fixed-width fields with natural alignment, so its parse
# is arch-independent and x64 validation suffices. (The absent path is
# covered on both arches by test-axl.sh.)
#
# Usage: ./test/integration/test-tpm-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

EFI="$PROJECT_DIR/out/native-x64/AxlTestPlatform.efi"
make -C "$PROJECT_DIR" ARCH=x64 tests 2>&1 | tail -1

SWTPM="${SWTPM:-$(command -v swtpm || true)}"
if [[ -z "$SWTPM" ]]; then
    echo "SKIP: swtpm not found (install swtpm, or set SWTPM=/path/to/swtpm)"
    exit 0
fi

WORK="$(mktemp -d)"
STATE="$WORK/state"        # persistent TPM state (shared across both boots)
SOCK="$WORK/swtpm.sock"
SWLOG="$WORK/swtpm.log"
LOG="$WORK/qemu.log"
mkdir -p "$STATE"

cleanup() {
    [[ -n "${SWPID:-}" ]] && kill "$SWPID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

# Start a fresh socket-backed swtpm on the shared state dir. A second
# swtpm on the same dir reloads the persisted Endorsement Primary Seed,
# so the EK it derives is identical — which is exactly the cross-boot
# stability the test asserts.
start_swtpm() {
    "$SWTPM" socket \
        --tpmstate "dir=$STATE" \
        --ctrl "type=unixio,path=$SOCK" \
        --tpm2 \
        --flags startup-clear \
        --log "file=$SWLOG,level=1" &
    SWPID=$!
    for _ in $(seq 1 100); do
        [[ -S "$SOCK" ]] && return 0
        if ! kill -0 "$SWPID" 2>/dev/null; then
            echo "ERROR: swtpm exited before creating its socket:"
            cat "$SWLOG" 2>/dev/null || true
            exit 1
        fi
        sleep 0.05
    done
    echo "ERROR: swtpm did not create $SOCK in time"; exit 1
}

stop_swtpm() {
    [[ -n "${SWPID:-}" ]] && kill "$SWPID" 2>/dev/null || true
    wait "$SWPID" 2>/dev/null || true
    SWPID=""
    rm -f "$SOCK"
}

# Run AxlTestPlatform with the TPM wired in; its test_tpm takes the
# present branch (GetCapability + EK read). Run twice against the SAME
# swtpm state to prove the EK is stable across boots. The EK read derives
# a primary in the endorsement hierarchy, which on a TCG-emulated CPU is
# slower than the capability query — hence the generous timeout.
LOG2="$WORK/qemu2.log"

run_once() {
    local out="$1"
    timeout 200s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 170 \
        --qemu-arg -chardev --qemu-arg "socket,id=axl_tpmsock,path=$SOCK" \
        --qemu-arg -tpmdev --qemu-arg "emulator,id=axl_tpm,chardev=axl_tpmsock" \
        --qemu-arg -device --qemu-arg "tpm-tis,tpmdev=axl_tpm" \
        "$EFI" > "$out" 2>&1 || true
}

# Boot 1, then a full TPM power-cycle (new swtpm, same state dir), boot 2.
start_swtpm; run_once "$LOG";  stop_swtpm
start_swtpm; run_once "$LOG2"; stop_swtpm
grep -iE "tpm:|EK-PUB|EXCEPTION" "$LOG" || true

fail=0

# The present branch must have run and passed (proves the TCG2 protocol
# was located, GetCapability filled the struct, and the EK was read).
expect=(
    "tpm: get_capability succeeds when present"
    "tpm: at least one active PCR bank"
    "tpm: supported hash bitmap covers the active banks"
    "tpm: read_ek_pub succeeds when EK present"
    "tpm: read_ek_pub reports a known EK algorithm"
    "tpm: EK length matches its algorithm"
    "tpm: read_ek_pub size query matches"
    "tpm: read_ek_pub too-small buffer -> AXL_ERR + required size"
    "tpm: read_ek_pub is deterministic within a boot"
)
for line in "${expect[@]}"; do
    grep -qF "PASS: $line" "$LOG" \
        || { echo "  MISS: PASS: $line"; fail=1; }
done

# Acceptance: two boots against the same swtpm return identical EK bytes.
ek1=$( { grep -oE "EK-PUB:[0-9]+:[0-9a-f]+" "$LOG"  || true; } | head -1)
ek2=$( { grep -oE "EK-PUB:[0-9]+:[0-9a-f]+" "$LOG2" || true; } | head -1)
if [[ -z "$ek1" ]]; then
    echo "  HIT: no EK-PUB emitted (EK read failed?)"; fail=1
elif [[ "$ek1" != "$ek2" ]]; then
    echo "  HIT: EK differs across boots ('$ek1' vs '$ek2')"; fail=1
else
    echo "  PASS: EK identical across two boots ($ek1)"
fi

# No tpm failures, and the absent branch must NOT have run (TPM detected).
grep -qE "^FAIL: tpm:" "$LOG" && { echo "  HIT: a tpm check FAILED"; fail=1; }
grep -qF "SKIP balance (no TPM" "$LOG" \
    && { echo "  HIT: test_tpm took the absent branch (TPM not detected)"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$LOG" && { echo "  HIT: CPU exception"; fail=1; }

if (( fail )); then
    echo "FAIL: TPM populated-path checks"
    exit 1
fi
echo "All TPM populated-path + EK checks passed."
exit 0
