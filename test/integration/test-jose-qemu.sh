#!/bin/bash
# test-meta: arch=x64 needs= est=11 local-only=0
# test-jose-qemu.sh — axl-jose (JWS/JWT/JWK) against a real mbedTLS.
#
# Auxiliary single-binary test (opt out of the test-axl.sh ratchet). The
# default unit suite builds without AXL_TLS, so AxlTestJose takes its
# fail-closed "not compiled in" branch. The real signing/verification
# outcomes — the RFC 7515 ES256/HS256 KATs, the sign/verify round-trips,
# the rejection matrix, JWT claim validation, and JWK parse/export — only
# run with mbedTLS linked. This builds AxlTestJose with AXL_TLS=1 and
# asserts the cryptographic branch passes.
#
# x64 only: the outcomes are pure computations over fixed vectors (plus
# generated keys), so x64 validation suffices. The fail-closed branch is
# covered on both arches by test-axl.sh; the AXL_TLS path on aa64 can be
# spot-checked with:
#   AXL_TLS=1 ARCH=aa64 make tests && \
#   TEST_APPS_ONLY=AxlTestJose TEST_SKIP_RATCHET=1 AXL_TLS=1 ARCH=aa64 \
#     ./test/integration/test-axl.sh
#
# Usage: ./test/integration/test-jose-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

# Build the AXL_TLS=1 variant into a segregated prefix (see the rationale
# in test-pk-verify-qemu.sh: toggling AXL_TLS changes per-TU CFLAGS but not
# .c timestamps, so a dedicated prefix keeps the ratcheted suite clean).
TLS_PREFIX="out/native-x64-tls"
EFI="$PROJECT_DIR/$TLS_PREFIX/AxlTestJose.efi"
rm -f "$EFI"
make -C "$PROJECT_DIR" ARCH=x64 AXL_TLS=1 PREFIX="$TLS_PREFIX" all tests 2>&1 | tail -1
[[ -f "$EFI" ]] || { echo "FAIL: AXL_TLS build did not produce $EFI"; exit 1; }

LOG="$(mktemp)"
cleanup() { rm -f "$LOG"; }
trap cleanup EXIT

# Generous timeout: the round-trip tests run a live RSA-3072 keygen, slow
# under TCG (no KVM) on CI runners.
timeout 200s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 170 \
    "$EFI" 2>&1 | tee "$LOG" \
    | grep -iE "jws|jwt|jwk|jose|Results:|EXCEPTION|leak report|bytes at 0x" || true

fail=0

# The real mbedTLS-backed branches must have run and passed: the standards
# KATs, the round-trips, the rejection matrix, JWT claims, and JWK I/O.
expect=(
    "jose_available: true in AXL_TLS build"
    "jws_verify: RFC 7515 A.3 ES256 KAT -> AXL_OK"
    "jws_verify: A.3 tampered signature -> AXL_ERR"
    "jws_verify: RFC 7515 A.1 HS256 KAT -> AXL_OK"
    "jws_verify: ES384 KAT -> AXL_OK"
    "jws_verify: PS256 KAT -> AXL_OK"
    "jws round-trip: ES384 sign->verify recovers payload"
    "jws round-trip: PS256 sign->verify recovers payload"
    "jws_verify: mixed symmetric+asymmetric allow-list -> AXL_ERR"
    "jws_verify: alg:none (empty sig) -> AXL_ERR"
    "jws round-trip: ES256 sign->verify recovers payload"
    "jws round-trip: RS256 sign->verify recovers payload"
    "jwt_verify: valid token + matching policy -> AXL_OK"
    "jwt_verify: expired exp -> AXL_ERR"
    "jwt_verify: aud array membership match -> AXL_OK"
    "jwk round-trip: export->parse->verify EC token"
    "jwk round-trip: export->parse->verify RSA token"
    "jwks_find: key-1 present"
)
for line in "${expect[@]}"; do
    grep -qF "PASS: $line" "$LOG" \
        || { echo "  MISS: PASS: $line"; fail=1; }
done

# The fail-closed branch must NOT have run (would mean mbedTLS was absent).
grep -qF "fails closed without AXL_TLS" "$LOG" \
    && { echo "  HIT: AxlTestJose took the fail-closed branch (no mbedTLS?)"; fail=1; }
# Any failed check, a leak, or a missing/non-zero Results footer fails.
grep -qE "^[[:space:]]*FAIL:" "$LOG" && { echo "  HIT: a check FAILED"; fail=1; }
grep -qiE "leak report" "$LOG" && { echo "  HIT: memory leak reported"; fail=1; }
grep -qE "Results: [0-9]+ passed, 0 failed" "$LOG" \
    || { echo "  HIT: missing or non-zero Results footer (run truncated?)"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$LOG" && { echo "  HIT: CPU exception"; fail=1; }

if (( fail )); then
    echo "FAIL: jose checks"
    exit 1
fi
echo "All jose checks passed."
exit 0
