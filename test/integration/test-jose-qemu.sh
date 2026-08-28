#!/bin/bash
# test-meta: arch=x64 needs= est=11 local-only=0
# test-jose-qemu.sh — axl-jose (JWS/JWT/JWK) against a real mbedTLS.
#
# Auxiliary single-binary test (opt out of the test-axl.sh ratchet).
#
# It used to REBUILD AxlTestJose with AXL_TLS=1 into a segregated prefix,
# because the default unit suite built without mbedTLS and this binary took a
# fail-closed "not compiled in" branch. mbedTLS is unconditional now, so
# test-axl.sh already runs these assertions on both arches and the rebuild is
# gone.
#
# WHAT THIS STILL ADDS over the ratchet: the ratchet is a COUNT, and a count
# cannot tell you WHICH assertion vanished. This names each standards KAT, each
# round-trip and each rejection outcome, so silently dropping one is a failure
# here rather than an off-by-N nobody reads.
#
# x64 only: the outcomes are pure computations over fixed vectors (plus
# generated keys), so x64 validation suffices, and test-axl.sh covers both
# arches by count.
#
# Usage: ./test/integration/test-jose-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

PREFIX_DIR="$("$PROJECT_DIR/scripts/build-prefix.sh" x64)"
EFI="$PROJECT_DIR/$PREFIX_DIR/AxlTestJose.efi"
make -C "$PROJECT_DIR" ARCH=x64 all tests 2>&1 | tail -1
[[ -f "$EFI" ]] || { echo "FAIL: build did not produce $EFI"; exit 1; }

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
    "jose_available: true"
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
