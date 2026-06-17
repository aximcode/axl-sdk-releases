#!/bin/bash
# test-netdiag-qemu.sh — AxlNet diagnostics against a live (DHCP'd) network.
#
# Boots AxlTestNet's `net-diag` mode, which brings up networking via DHCP
# (SLIRP hands out a deterministic lease: 10.0.2.15, gateway/DNS
# 10.0.2.2/10.0.2.3) and exercises the diagnostics primitives:
#   - axl_net_get_dhcp_lease: the active DHCP-leased config (address / mask /
#     gateway / DNS) read back from the resident IP4Config2 layer.
#
# The lease lifetimes (lease/T1/T2 seconds), server identity, and domain are
# deliberately NOT covered: IP4Config2 discards them after applying the lease,
# and surfacing them needs a resident DHCP-owning driver — out of scope for a
# transient tool. axl_net_resolve_ptr's positive round-trip needs a reverse
# DNS zone (a future tap+dnsmasq harness); only its safe negatives run in the
# unit suite.
#
# Opt out of the unit-suite ratchet — this is a topology-gated integration
# scenario, not a unit binary.
#
# Usage: ./test/integration/test-netdiag-qemu.sh [--arch X64|AARCH64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

ARCH="X64"
if [[ "${1:-}" == "--arch" && -n "${2:-}" ]]; then
    ARCH="$2"
fi
declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
NATIVE="${_NATIVE[$ARCH]:-x64}"

make -C "$PROJECT_DIR" ARCH="$NATIVE" tests 2>&1 | tail -1
EFI="$PROJECT_DIR/out/native-$NATIVE/AxlTestNet.efi"
[[ -f "$EFI" ]] || { echo "FAIL: AxlTestNet.efi not built ($EFI)"; exit 1; }

WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

RUNQEMU_ARGS=(--timeout 100 --net)
[[ "$ARCH" == "AARCH64" ]] && RUNQEMU_ARGS=(--arch AARCH64 --timeout 110 --net)

echo "=== AxlNet diagnostics ($ARCH) ==="
timeout 160s "$PROJECT_DIR/scripts/run-qemu.sh" "${RUNQEMU_ARGS[@]}" \
    "$EFI" net-diag 2>&1 | tee "$LOG" \
    | grep -iE "net-diag|PASS:|FAIL:|lease:|EXCEPTION|leak report" || true

fail=0

# The net-diag run must report all checks passed.
if ! grep -qE "net-diag Results: [0-9]+ passed, 0 failed" "$LOG"; then
    echo "  MISS: net-diag did not report '0 failed'"; fail=1
fi
# At least the DHCP-lease checks must have run (guard against an empty pass).
grep -qE "PASS: dhcp-lease: address == 10.0.2.15" "$LOG" \
    || { echo "  MISS: DHCP lease address check"; fail=1; }
grep -qiE "FAIL:" "$LOG" && { echo "  HIT: a net-diag check FAILed"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$LOG" && { echo "  HIT: CPU exception"; fail=1; }
grep -qiE "leak report" "$LOG" && { echo "  HIT: memory leak reported"; fail=1; }

if (( fail )); then
    echo "FAIL: net-diag checks ($ARCH)"
    echo "--- serial log ---"; cat "$LOG"
    exit 1
fi

echo "PASS: net-diag ($ARCH)"
