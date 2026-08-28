#!/bin/bash
# test-meta: arch=x64 needs= est=10 local-only=0
# test-pk-verify-qemu.sh — axl_pk_verify() ECDSA-P256 against a real mbedTLS.
#
# Auxiliary single-binary test (opt out of the test-axl.sh ratchet).
#
# It used to REBUILD AxlTestCrypto with AXL_TLS=1 into a segregated prefix,
# because the default unit suite built without mbedTLS and this binary took a
# fail-closed "verification not compiled in" branch. mbedTLS is unconditional
# now, so test-axl.sh already runs these assertions on both arches.
#
# WHAT THIS STILL ADDS over the ratchet: the ratchet is a COUNT, and a count
# cannot say WHICH assertion vanished. This names each verify outcome, each
# AEAD/cipher/ECDH KAT and each round-trip, so dropping one fails here by name.
#
# x64 only: these outcomes are pure computations over fixed vectors, so x64
# validation suffices and test-axl.sh covers both arches by count.
#
# Usage: ./test/integration/test-pk-verify-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

PREFIX_DIR="$("$PROJECT_DIR/scripts/build-prefix.sh" x64)"
EFI="$PROJECT_DIR/$PREFIX_DIR/AxlTestCrypto.efi"
make -C "$PROJECT_DIR" ARCH=x64 all tests 2>&1 | tail -1
[[ -f "$EFI" ]] || { echo "FAIL: build did not produce $EFI"; exit 1; }

LOG="$(mktemp)"
cleanup() { rm -f "$LOG"; }
trap cleanup EXIT

# Generous timeout: this build exercises a live RSA-3072 keygen, which is
# slow under TCG (no KVM) on CI runners.
timeout 200s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 170 \
    "$EFI" 2>&1 | tee "$LOG" \
    | grep -iE "pk_verify|pk_available|keygen|sign:|verify:|aead|cipher|ecdh|Results:|EXCEPTION" || true

fail=0

# The real mbedTLS-backed branches must have run and passed: detached
# verify, key generation, signing, and the RSA path.
expect=(
    "pk_available: true"
    "pk_verify: valid ECDSA-P256 signature -> AXL_OK"
    "pk_verify: tampered signature -> AXL_ERR"
    "keygen: ECDSA P-256 -> key"
    "verify: ECDSA raw round-trip -> AXL_OK"
    "round-trip: reloaded private key signs, original verifies"
    "interop: axl_pk_verify accepts key-handle pubkey + sig"
    "round-trip: RSA sign/verify"
    "keygen: RSA-3072 -> key"
    "aead aes256gcm: seal ciphertext matches KAT"
    "aead chachapoly: seal ciphertext matches KAT"
    "cipher aes256ctr: encrypt matches KAT"
    "cipher: chunked xcrypt continues one keystream"
    "ecdh p256: both sides derive the same secret"
    "ecdh x25519: both sides derive the same secret"
)
for line in "${expect[@]}"; do
    grep -qF "PASS: $line" "$LOG" \
        || { echo "  MISS: PASS: $line"; fail=1; }
done

# Any failed check, or a missing/non-zero Results footer, fails the run.
grep -qE "^[[:space:]]*FAIL:" "$LOG" && { echo "  HIT: a check FAILED"; fail=1; }
grep -qE "Results: [0-9]+ passed, 0 failed" "$LOG" \
    || { echo "  HIT: missing or non-zero Results footer (run truncated?)"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$LOG" && { echo "  HIT: CPU exception"; fail=1; }

if (( fail )); then
    echo "FAIL: pk crypto checks"
    exit 1
fi
echo "All pk crypto checks passed."
exit 0
