#!/bin/bash
# test-meta: arch=both needs=qemu.qmp est=70 local-only=0 builds=agent,shell-launcher guest=none
# guest=none: the pool's own test: it boots, snapshots and restores pool
#   guests itself (test/pool/guest-cases.py).
# test-pool-guest-qemu.sh -- the guest pool's guest lifecycle (test/pool/
# axlpool/guest.py) and class key (klass.py), against real guests: boot and
# snapshot, restore, stop with the state kept, a cold start in a NEW QEMU
# from that state, kill (test/pool/guest-cases.py); and the key on the host
# (test/pool/klass-cases.py) and the classes file (test/pool/classes-cases.py)
# on the host, X64 only: they read dry runs and TOML, not guests.
#
# Usage: ./test/integration/test-pool-guest-qemu.sh [--arch X64|AARCH64]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1

TEST_ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *) shift ;;
    esac
done
declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
NATIVE="${_NATIVE[$TEST_ARCH]:-x64}"

if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NATIVE" agent shell-launcher > /dev/null 2>&1; then
    echo "  FAIL: the agent and shell-launcher did not build (ARCH=$NATIVE)"
    exit 1
fi
if ! python3 -c 'import qemu.qmp' 2>/dev/null; then
    echo "  FAIL: the python3 module qemu.qmp is not installed (pip install --user qemu.qmp)"
    exit 1
fi
PREFIX="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$NATIVE")"
WORK="$(mktemp -d -t axl-poolg.XXXXXXXX)"
# Anything the cases left running (they assert there is none) dies before
# its directory goes: found by PATH, not by pids the cases recorded.
cleanup() {
    local pid
    for pid in $(ps -eo pid=,args= | awk -v w="$WORK" '($2 ~ /qemu-system|virtiofsd/) && index($0, w) {print $1}'); do
        kill -9 "$pid" 2>/dev/null
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

PASS=0
FAIL=0
run_cases() {
    local out
    out=$(timeout 600 "$@" 2>&1)
    echo "$out" | grep -vE '^(guest|klass|classes)-cases:'
    PASS=$((PASS + $(grep -c '^  PASS:' <<< "$out")))
    FAIL=$((FAIL + $(grep -c '^  FAIL:' <<< "$out")))
    # A crash before the summary is a failure, never silence.
    grep -qE '^(guest|klass|classes)-cases: ' <<< "$out" || { echo "  FAIL: $2 printed no summary"; FAIL=$((FAIL + 1)); }
}

echo "=== guest pool: guest lifecycle ($TEST_ARCH) ==="
run_cases python3 "$PROJECT_DIR/test/pool/guest-cases.py" "$TEST_ARCH" "$PREFIX/tools/agent.efi" "$WORK"
if [[ "$TEST_ARCH" == X64 ]]; then
    echo "=== guest pool: class key ==="
    run_cases python3 "$PROJECT_DIR/test/pool/klass-cases.py" "$PREFIX/tools/agent.efi" \
        "$PREFIX/axl-shell-launcher.efi"
    echo "=== guest pool: the classes file ==="
    run_cases python3 "$PROJECT_DIR/test/pool/classes-cases.py"
fi
echo "pool-guest ($TEST_ARCH): $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
