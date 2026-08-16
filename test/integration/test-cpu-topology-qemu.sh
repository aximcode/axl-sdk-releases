#!/bin/bash
# test-meta: arch=x64 needs= est=41 local-only=0
# test-cpu-topology-qemu.sh — axl_cpu_topology() against explicit QEMU
# -smp layouts.
#
# Auxiliary single-binary test (opt-out of the test-axl.sh ratchet).
# The unit suite boots a single vCPU, where EFI_MP_SERVICES_PROTOCOL is
# absent or reports one processor — so the count-and-fill path, the
# per-CPU location/status decode, the "exactly one BSP" invariant, and
# truncation can only be reproduced with an explicit multi-processor
# topology. Pinning explicit `-smp` layouts makes the processor count
# deterministic on any host:
#
#   -smp 1                          → 1 processor (matches the uni floor).
#   -smp 4,sockets=1,cores=4        → 4 processors, one BSP.
#   -smp 4,sockets=2,cores=2        → 4 processors across 2 packages.
#
# The selftest prints a "TOPO: total=N enabled=M" summary plus one
# "CPU[i]:" line per processor and a PASS/FAIL tally of the contract
# invariants; this script asserts the layout-specific exact counts.
#
# Usage: ./test/integration/test-cpu-topology-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

EFI="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)/cpu-topology-selftest.efi"
make -C "$PROJECT_DIR" ARCH=x64 cpu-topology-selftest 2>&1 | tail -2

overall_fail=0

# run_layout <smp-spec> <expect-total> <expect-packages>
#   expect-packages: number of distinct package indices expected across
#   the written CPU[] entries (0 = don't check — location reporting can
#   vary by firmware).
run_layout() {
    local smp="$1" expect_total="$2" expect_packages="$3"
    local log; log="$(mktemp)"

    echo "=== -smp $smp (expect total=$expect_total, packages=$expect_packages) ==="
    timeout 90s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 40 \
        --qemu-arg -smp --qemu-arg "$smp" "$EFI" 2>&1 | tee "$log" \
        | grep -iE "TOPO:|CPU\[|CPU-TOPOLOGY-SELFTEST|FAIL:|EXCEPTION" || true

    local fail=0
    grep -qE "^TOPO: total=$expect_total enabled=$expect_total\b" "$log" \
        || { echo "  MISS: total=$expect_total enabled=$expect_total"; fail=1; }
    grep -qE "^CPU-TOPOLOGY-SELFTEST: [0-9]+ passed, 0 failed" "$log" \
        || { echo "  MISS: clean selftest verdict"; fail=1; }
    grep -qiE "EXCEPTION|invalid opcode" "$log" && { echo "  HIT: CPU exception"; fail=1; }

    # Exactly one BSP line among the written entries.
    local bsp_lines
    bsp_lines="$(grep -cE "^CPU\[[0-9]+\]:.* bsp=1" "$log" || true)"
    [[ "$bsp_lines" == "1" ]] || { echo "  MISS: expected exactly one bsp=1 (got $bsp_lines)"; fail=1; }

    # Number of CPU[] lines written must equal the total (buffer is 64).
    local cpu_lines
    cpu_lines="$(grep -cE "^CPU\[[0-9]+\]:" "$log" || true)"
    [[ "$cpu_lines" == "$expect_total" ]] \
        || { echo "  MISS: expected $expect_total CPU[] lines (got $cpu_lines)"; fail=1; }

    if [[ "$expect_packages" != "0" ]]; then
        local pkgs
        pkgs="$(grep -oE "pkg=[0-9]+" "$log" | sort -u | wc -l)"
        [[ "$pkgs" == "$expect_packages" ]] \
            || { echo "  MISS: expected $expect_packages distinct packages (got $pkgs)"; fail=1; }
    fi

    if (( fail )); then
        echo "FAIL (-smp $smp)"; overall_fail=$((overall_fail + 1))
    else
        echo "OK (-smp $smp)"
    fi
    rm -f "$log"
    echo
}

# Single processor: matches the uniprocessor floor the unit suite pins.
run_layout 1 1 0
# Four cores, one socket: count + single-BSP + dense fill.
run_layout "4,sockets=1,cores=4,threads=1" 4 0
# Two cores x two SMT threads, one socket: exercises the thread index.
run_layout "4,sockets=1,cores=2,threads=2" 4 1
# Four logical CPUs across two sockets: package index spans both.
run_layout "4,sockets=2,cores=2,threads=1" 4 2

if (( overall_fail > 0 )); then
    echo "$overall_fail -smp layout(s) failed"
    exit 1
fi
echo "All CPU-topology layout checks passed."
exit 0
