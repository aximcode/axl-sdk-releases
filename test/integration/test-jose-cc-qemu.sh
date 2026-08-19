#!/bin/bash
# test-meta: arch=x64 needs= est=29 local-only=0
# test-jose-cc-qemu.sh — dogfood axl-jose through the SDK consumer path.
#
# Proves a real consumer can use <axl/axl-jose.h>: stages an AXL_TLS=1 SDK,
# compiles sdk/examples/jose-demo.c with axl-cc (the packaged toolchain +
# TLS libaxl.a are the artifact under test), runs it under QEMU, and
# asserts the end-to-end demo passes — JWT sign/verify with claim policy, a
# JWK export/parse round-trip, allow-list rejection, and a round-trip of
# every algorithm (ES256/ES384/RS256/PS256/HS256).
#
# Complements test-jose-qemu.sh (which exercises the library directly): this
# guards the SDK-packaging + axl-cc link path, the way test-axl-cc-service.sh
# guards `axl-cc --service`. x64 only — the SDK link path is arch-independent
# for this purpose and the crypto outcomes are covered on both arches by
# test-jose-qemu.sh.
#
# Usage: ./test/integration/test-jose-cc-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

# Stage a TLS SDK into a dedicated prefix so it never clobbers the default
# (non-TLS) out/ SDK other axl-cc tests rely on.
SDK_PREFIX="$PROJECT_DIR/out/sdk-tls"
echo "+ AXL_TLS=1 install.sh --arch x64 --prefix $SDK_PREFIX"
# Capture to a file and print the tail, rather than `| tail -1`: a pipeline's
# exit status is the LAST command's, so `install.sh | tail` reports tail's
# success and a failed install sails past `set -e`.
#
# That is not theoretical. Under -j8 this install lost a race with another
# test's concurrent install.sh (both build into the shared TLS build tree),
# failed, and was swallowed -- so the test carried on against a THREE-WEEK-OLD
# staged prefix and died ten lines later on `undefined reference to
# axl_crypto_rng`, which reads as a library defect rather than a staging
# failure. Diagnosing that cost more than the fix.
_inst_log="$(mktemp)"
if ! AXL_TLS=1 "$PROJECT_DIR/scripts/install.sh" --arch x64 \
        --prefix "$SDK_PREFIX" >"$_inst_log" 2>&1; then
    echo "FAIL: install.sh failed to stage the TLS SDK at $SDK_PREFIX" >&2
    echo "      (everything below would have run against a STALE prefix)" >&2
    tail -15 "$_inst_log" | sed 's/^/      /' >&2
    rm -f "$_inst_log"
    exit 1
fi
tail -1 "$_inst_log"
rm -f "$_inst_log"

AXL_CC="$SDK_PREFIX/bin/axl-cc"
[[ -x "$AXL_CC" ]] || { echo "FAIL: staged axl-cc missing at $AXL_CC"; exit 1; }

EFI="$(mktemp -d)/jose-demo.efi"
cleanup() { rm -rf "$(dirname "$EFI")"; }
trap cleanup EXIT

echo "+ axl-cc jose-demo.c -o jose-demo.efi"
"$AXL_CC" --arch x64 "$PROJECT_DIR/sdk/examples/jose-demo.c" -o "$EFI"
[[ -f "$EFI" ]] || { echo "FAIL: axl-cc did not produce $EFI"; exit 1; }

LOG="$(mktemp)"
trap 'rm -rf "$(dirname "$EFI")"; rm -f "$LOG"' EXIT

# Generous timeout: the demo runs a live RSA-3072 keygen (slow under TCG).
timeout 200s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 170 \
    "$EFI" 2>&1 | tee "$LOG" \
    | grep -iE "jose-demo|ok\]|FAIL|authenticated|Results|EXCEPTION|leak report" \
    || true

fail=0
grep -qF "jose-demo: all checks passed" "$LOG" \
    || { echo "  MISS: 'jose-demo: all checks passed'"; fail=1; }
grep -qE "\[FAIL\]|jose-demo: [0-9]+ check" "$LOG" \
    && { echo "  HIT: a demo check FAILED"; fail=1; }
grep -qiE "leak report" "$LOG" && { echo "  HIT: memory leak reported"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$LOG" && { echo "  HIT: CPU exception"; fail=1; }

if (( fail )); then
    echo "FAIL: jose-cc dogfood"
    exit 1
fi
echo "jose-cc dogfood: OK"
exit 0
