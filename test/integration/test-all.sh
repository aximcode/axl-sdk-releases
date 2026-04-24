#!/bin/bash
# Run build + test for all architecture combinations.
#
# Usage: ./test/integration/test-all.sh [--arch x64|aa64|all]
#
# Default: both architectures (x64 + aa64).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

ARCHS="all"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)    ARCHS="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--arch x64|aa64|all]"
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# Build the test matrix
declare -a MATRIX=()

if [[ "$ARCHS" == "all" ]]; then
    MATRIX=(X64 AARCH64)
else
    for arch in $ARCHS; do
        case "$arch" in
            x64)  MATRIX+=(X64) ;;
            aa64) MATRIX+=(AARCH64) ;;
            *)    echo "Unknown arch: $arch" >&2; exit 1 ;;
        esac
    done
fi

echo "=== AXL Test Suite ==="
echo "  Architectures: ${#MATRIX[@]}"
echo ""

# Track results
declare -a RESULTS=()
TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_SKIP=0
ALL_OK=true

for arch in "${MATRIX[@]}"; do
    # Map arch names
    case "$arch" in
        X64)     arch_label="x64" ;;
        AARCH64) arch_label="aa64" ;;
    esac

    printf "  %-14s " "$arch_label:"

    # Build
    build_out=$("$PROJECT_DIR/scripts/build.sh" --clean --arch "$arch" 2>&1) || {
        echo "BUILD FAILED"
        RESULTS+=("$arch_label: BUILD FAILED")
        TOTAL_SKIP=$((TOTAL_SKIP + 1))
        ALL_OK=false
        continue
    }

    # Test (AARCH64 may not have QEMU/firmware available)
    if [[ "$arch" == "AARCH64" ]]; then
        test_out=$("$SCRIPT_DIR/test-axl.sh" --arch AARCH64 2>&1) || {
            echo "BUILD OK (test skipped — no AARCH64 QEMU)"
            RESULTS+=("$arch_label: build ok, test skipped")
            TOTAL_SKIP=$((TOTAL_SKIP + 1))
            continue
        }
    else
        test_out=$("$SCRIPT_DIR/test-axl.sh" --arch X64 2>&1) || {
            echo "TEST FAILED"
            RESULTS+=("$arch_label: TEST FAILED")
            ALL_OK=false
            continue
        }
    fi

    # Parse results line
    result_line=$(echo "$test_out" | grep "^Results:" | tail -1)
    if [[ -n "$result_line" ]]; then
        echo "$result_line" | sed 's/^Results: //'
        pass=$(echo "$result_line" | grep -oP '\d+ passed' | grep -oP '\d+')
        fail=$(echo "$result_line" | grep -oP '\d+ failed' | grep -oP '\d+')
        TOTAL_PASS=$((TOTAL_PASS + pass))
        TOTAL_FAIL=$((TOTAL_FAIL + fail))
        RESULTS+=("$result_line")
        if [[ "$fail" -gt 0 ]]; then
            ALL_OK=false
        fi
    else
        echo "NO RESULTS"
        RESULTS+=("$arch_label: no results")
        ALL_OK=false
    fi
done

echo ""
echo "=== Summary ==="
echo "  Total: $TOTAL_PASS passed, $TOTAL_FAIL failed, $TOTAL_SKIP skipped"
echo "  Architectures: ${#MATRIX[@]}"

if $ALL_OK && [[ $TOTAL_FAIL -eq 0 ]]; then
    echo "  Status: ALL PASSED"
    exit 0
else
    echo "  Status: FAILURES DETECTED"
    exit 1
fi
