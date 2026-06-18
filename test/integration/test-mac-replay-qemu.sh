#!/bin/bash
# test-meta: arch=x64 needs= est=20 local-only=0
# test-mac-replay-qemu.sh — HF4: end-to-end NIC MAC replay.
#
# Proves the full chain: a fixture net.json carrying a distinctive
# permanent MAC, through `axl-emulate --mac` → run-qemu.sh `--net --mac`
# → QEMU's NIC mac= property → the guest's EFI_SIMPLE_NETWORK, where
# netinfo.efi reports exactly that address. The host arg-parsing for
# both --mac knobs is pinned separately by the DRYRUN tests
# (test-run-qemu-flags.sh, test-axl-emulate.sh); this is the live
# confirmation that the value actually reaches the guest.
#
# Plain user-mode networking — no virtiofs, no patched QEMU, no shim —
# so it is CI-friendly. Auxiliary; opts out of the test-axl.sh ratchet.
# x86-only (CI integration job is x64).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
AXL_EMULATE="$PROJECT_DIR/scripts/axl-emulate"
NETINFO="$PROJECT_DIR/out/native-x64/tools/netinfo.efi"

export TEST_SKIP_RATCHET=1
PASS=0
FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL + 1)); }

if [[ ! -x "$NETINFO" ]]; then
    echo "Building tools..."
    make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -3
fi
[[ -x "$NETINFO" ]] || { echo "FAIL: netinfo.efi not found at $NETINFO"; exit 1; }

# A MAC that is NOT QEMU's default (52:54:00:12:34:56), so seeing it in
# the guest proves the fixture value flowed through — not a default.
TEST_MAC="de:ad:be:ef:00:01"

FIX="$(mktemp -d)"
trap 'rm -rf "$FIX"' EXIT
cat > "$FIX/net.json" <<EOF
{
  "count": 1,
  "nics": [
    {
      "index": 0,
      "state": 1,
      "if_type": 1,
      "hw_address_size": 6,
      "mac": "de:ad:be:ef:00:02",
      "permanent_mac": "$TEST_MAC",
      "media_present": true
    }
  ]
}
EOF

# ----------------------------------------------------------------------
# Full chain: axl-emulate --mac <fixture> netinfo.efi -- list
# ----------------------------------------------------------------------
echo "=== axl-emulate --mac → guest SNP MAC ==="
OUT=$(timeout 90s "$AXL_EMULATE" --mac "$FIX" "$NETINFO" -- list 2>&1 || true)

if grep -q "$TEST_MAC" <<< "$OUT"; then
    pass "guest SNP reports the fixture's captured MAC ($TEST_MAC)"
else
    fail "captured MAC did not reach the guest" \
         "$(grep -iE 'IF#|[0-9a-f]{2}:[0-9a-f]{2}:' <<< "$OUT" | head)"
fi

# Sanity: the QEMU default MAC must NOT appear — proves the knob
# actually overrode it rather than the test passing on a coincidence.
if ! grep -q "52:54:00:12:34:56" <<< "$OUT"; then
    pass "QEMU default MAC (52:54:00:12:34:56) was overridden"
else
    fail "guest still shows the QEMU default MAC — --mac had no effect"
fi

# ----------------------------------------------------------------------
# run-qemu.sh --mac directly (one less layer) as an independent check.
# ----------------------------------------------------------------------
echo "=== run-qemu.sh --net --mac → guest SNP MAC ==="
OUT2=$(timeout 90s "$RUN_QEMU" --net --mac "$TEST_MAC" "$NETINFO" list 2>&1 || true)
if grep -q "$TEST_MAC" <<< "$OUT2"; then
    pass "run-qemu.sh --mac sets the guest NIC address directly"
else
    fail "run-qemu.sh --mac did not reach the guest" \
         "$(grep -iE 'IF#|[0-9a-f]{2}:[0-9a-f]{2}:' <<< "$OUT2" | head)"
fi

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"
[[ "$FAIL" -eq 0 ]]
