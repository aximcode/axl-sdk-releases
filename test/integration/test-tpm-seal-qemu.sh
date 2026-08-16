#!/bin/bash
# test-meta: arch=x64 needs=swtpm est=25 local-only=0
# test-tpm-seal-qemu.sh — axl_tpm_seal / axl_tpm_unseal against a
# swtpm-backed TPM 2.0.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# The unit suite boots with no TPM, so axl_tpm_present() is false and
# AxlTestPlatform's test_tpm_seal takes its argument-validation +
# SKIP-balanced "absent" branch. The real seal->unseal round-trip — a
# full TPM2 CreatePrimary/PCR_Read/Create/Load/StartAuthSession/PolicyPCR/
# Unseal chain over EFI_TCG2_PROTOCOL.SubmitCommand — only runs with an
# actual TPM. This wires one via swtpm and asserts the present-branch
# round-trip recovers the sealed secret.
#
# x64 only: tpm-tis is the well-supported QEMU model and the marshaling
# is arch-independent (fixed-width big-endian fields), so x64 validation
# suffices. (The absent path is covered on both arches by test-axl.sh.)
#
# Usage: ./test/integration/test-tpm-seal-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

EFI="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)/AxlTestPlatform.efi"
make -C "$PROJECT_DIR" ARCH=x64 tests 2>&1 | tail -1

SWTPM="${SWTPM:-$(command -v swtpm || true)}"
if [[ -z "$SWTPM" ]]; then
    echo "SKIP: swtpm not found (install swtpm, or set SWTPM=/path/to/swtpm)"
    exit 0
fi

WORK="$(mktemp -d)"
STATE="$WORK/state"
SOCK="$WORK/swtpm.sock"
SWLOG="$WORK/swtpm.log"
LOG="$WORK/qemu.log"
mkdir -p "$STATE"

cleanup() {
    [[ -n "${SWPID:-}" ]] && kill "$SWPID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

"$SWTPM" socket \
    --tpmstate "dir=$STATE" \
    --ctrl "type=unixio,path=$SOCK" \
    --tpm2 \
    --flags startup-clear \
    --log "file=$SWLOG,level=1" &
SWPID=$!
for _ in $(seq 1 100); do
    [[ -S "$SOCK" ]] && break
    if ! kill -0 "$SWPID" 2>/dev/null; then
        echo "ERROR: swtpm exited before creating its socket:"
        cat "$SWLOG" 2>/dev/null || true
        exit 1
    fi
    sleep 0.05
done
[[ -S "$SOCK" ]] || { echo "ERROR: swtpm did not create $SOCK in time"; exit 1; }

# Seal -> Unseal exercises a primary-key derivation (CreatePrimary), which
# is slow on a software-emulated TPM — hence the generous timeout.
timeout 200s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 170 \
    --qemu-arg -chardev --qemu-arg "socket,id=axl_tpmsock,path=$SOCK" \
    --qemu-arg -tpmdev --qemu-arg "emulator,id=axl_tpm,chardev=axl_tpmsock" \
    --qemu-arg -device --qemu-arg "tpm-tis,tpmdev=axl_tpm" \
    "$EFI" > "$LOG" 2>&1 || true

grep -iE "tpm seal|tpm unseal|TPM-SEAL|EXCEPTION" "$LOG" || true

fail=0

# The present branch must have run and passed the round-trip.
expect=(
    "tpm seal: succeeds when present"
    "tpm unseal: succeeds when present"
    "tpm seal/unseal round-trips the secret"
)
for line in "${expect[@]}"; do
    grep -qF "PASS: $line" "$LOG" \
        || { echo "  MISS: PASS: $line"; fail=1; }
done

grep -qF "TPM-SEAL:ok" "$LOG" \
    || { echo "  HIT: round-trip marker TPM-SEAL:ok not emitted"; fail=1; }

# No seal failures, and the absent branch must NOT have run (TPM detected).
grep -qE "^FAIL: tpm seal:|^FAIL: tpm unseal:" "$LOG" \
    && { echo "  HIT: a tpm seal/unseal check FAILED"; fail=1; }
grep -qF "tpm seal: SKIP balance (no TPM)" "$LOG" \
    && { echo "  HIT: test_tpm_seal took the absent branch (TPM not detected)"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$LOG" && { echo "  HIT: CPU exception"; fail=1; }

if (( fail )); then
    echo "FAIL: TPM seal/unseal round-trip"
    exit 1
fi
echo "All TPM seal/unseal round-trip checks passed."
exit 0
