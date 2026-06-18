#!/bin/bash
# test-meta: arch=x64 needs= est=10 local-only=0
# test-pk-verify-qemu.sh — axl_pk_verify() ECDSA-P256 against a real mbedTLS.
#
# Auxiliary single-binary test (opt out of the test-axl.sh ratchet). The
# default unit suite builds without AXL_TLS, so AxlTestCrypto takes its
# fail-closed "verification not compiled in" branch. The real verify
# outcomes — a valid signature returning AXL_OK and every tamper/wrong-key
# case returning AXL_ERR — only run with mbedTLS linked. This builds
# AxlTestCrypto with AXL_TLS=1 and asserts the cryptographic branch passes.
#
# x64 only: the ECDSA-P256 verify outcome is arch-independent (it is a pure
# computation over a fixed test vector), so x64 validation suffices. The
# fail-closed branch is covered on both arches by test-axl.sh; the AXL_TLS
# path on aa64 can be spot-checked with:
#   AXL_TLS=1 ARCH=aa64 make tests && \
#   TEST_APPS_ONLY=AxlTestCrypto TEST_SKIP_RATCHET=1 AXL_TLS=1 ARCH=aa64 \
#     ./test/integration/test-axl.sh
#
# Usage: ./test/integration/test-pk-verify-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

# Build the AXL_TLS=1 variant into a segregated prefix. Toggling AXL_TLS
# changes per-TU CFLAGS (-DAXL_HAVE_TLS) but not the .c timestamps, so
# sharing the default out/native-x64 cache would leave AXL_HAVE_TLS .o's
# behind for a later non-TLS test-axl.sh run (the cache hazard install.sh
# segregates against). A dedicated prefix keeps the ratcheted suite clean.
TLS_PREFIX="out/native-x64-tls"
EFI="$PROJECT_DIR/$TLS_PREFIX/AxlTestCrypto.efi"
rm -f "$EFI"
make -C "$PROJECT_DIR" ARCH=x64 AXL_TLS=1 PREFIX="$TLS_PREFIX" all tests 2>&1 | tail -1
[[ -f "$EFI" ]] || { echo "FAIL: AXL_TLS build did not produce $EFI"; exit 1; }

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
    "pk_available: true in AXL_TLS build"
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

# The fail-closed branch must NOT have run (would mean mbedTLS was absent).
grep -qF "verification not built" "$LOG" \
    && { echo "  HIT: AxlTestCrypto took the fail-closed branch (no mbedTLS?)"; fail=1; }
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
