#!/bin/bash
# test-meta: arch=x64 needs= est=21 local-only=0
# test-net-config-qemu.sh — AxlNet static-config / DNS / hostname end-to-end.
#
# The AxlTestUtil unit suite covers the pure descriptor group + AxlConfig
# round-trip + the hostname NV-store round-trip. This boots QEMU with a NIC
# and drives `netinfo config` (a thin dogfood over axl_net_init_static) to
# exercise the firmware paths the unit suite can't: IP4Config2 static address
# + DNS SetData, the wait_ip_settled poll (read-back must reflect the address
# we set, not a stale one), and the DHCP path.
#
# Usage: ./test/integration/test-net-config-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -1
EFI="$PROJECT_DIR/out/native-x64/tools/netinfo.efi"
[[ -f "$EFI" ]] || { echo "FAIL: netinfo.efi not built"; exit 1; }

WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

run_config() {  # $1 = log file, rest = netinfo config args
    local log="$1"; shift
    timeout 150s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 120 --net \
        "$EFI" config "$@" 2>&1 | tee "$log" \
        | grep -iE "Applying|address|hostname|failed|EXCEPTION|leak report" || true
}

fail=0

# --- Static path: set a fixed address + DNS + hostname, read it back. The
# read-back proving 10.0.2.50 (not the DHCP 10.0.2.15) confirms the static
# policy applied AND that wait_ip_settled waited for the new address. ---
SLOG="$WORK/static.log"
run_config "$SLOG" --mode static --ip 10.0.2.50 --mask 255.255.255.0 \
    --gw 10.0.2.2 --dns 10.0.2.3 --hostname axlbox
grep -qF "address: 10.0.2.50" "$SLOG" \
    || { echo "  MISS: static address 10.0.2.50 read-back"; fail=1; }
grep -qF "hostname: axlbox" "$SLOG" \
    || { echo "  MISS: hostname axlbox read-back"; fail=1; }
grep -qiF "net config failed" "$SLOG" \
    && { echo "  HIT: static config reported failure"; fail=1; }

# --- DHCP path: slirp leases 10.0.2.15; hostname still persists. ---
DLOG="$WORK/dhcp.log"
run_config "$DLOG" --mode dhcp --hostname dhcpbox
grep -qF "address: 10.0.2.15" "$DLOG" \
    || { echo "  MISS: DHCP address 10.0.2.15 read-back"; fail=1; }
grep -qF "hostname: dhcpbox" "$DLOG" \
    || { echo "  MISS: hostname dhcpbox read-back"; fail=1; }

for log in "$SLOG" "$DLOG"; do
    grep -qiE "leak report" "$log" && { echo "  HIT: memory leak"; fail=1; }
    grep -qiE "EXCEPTION|invalid opcode" "$log" && { echo "  HIT: CPU exception"; fail=1; }
done

if (( fail )); then
    echo "FAIL: net config checks"
    echo "--- static log ---"; cat "$SLOG"
    echo "--- dhcp log ---";   cat "$DLOG"
    exit 1
fi
echo "net config test: OK"
exit 0
