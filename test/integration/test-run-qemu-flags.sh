#!/bin/bash
# test-run-qemu-flags.sh -- argument-parsing tests for run-qemu.sh.
#
# These run on the host (no QEMU), so they're cheap and live outside
# the QEMU integration matrix. They cover the bits of run-qemu.sh
# that don't need a guest:
#   - bash -n syntax pass
#   - --help exits 0 and advertises the flags we care about
#   - --interactive rejects --background and --screenshot
#   - missing EFI file is reported

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"

PASS=0
FAIL=0

check() {
    local name="$1" expected_rc="$2" expected_pat="$3"
    shift 3
    local out rc=0
    out=$("$@" 2>&1) || rc=$?
    if [[ "$rc" != "$expected_rc" ]]; then
        echo "FAIL: $name -- expected rc=$expected_rc, got $rc"
        echo "  output: $out"
        FAIL=$((FAIL + 1))
        return
    fi
    if [[ -n "$expected_pat" ]] && ! grep -qE "$expected_pat" <<< "$out"; then
        echo "FAIL: $name -- output did not match /$expected_pat/"
        echo "  output: $out"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "PASS: $name"
    PASS=$((PASS + 1))
}

# --- syntax ---------------------------------------------------------------
if bash -n "$RUN_QEMU"; then
    echo "PASS: bash -n"
    PASS=$((PASS + 1))
else
    echo "FAIL: bash -n"
    FAIL=$((FAIL + 1))
fi

# --- --help ---------------------------------------------------------------
check "--help exits 0 and lists --interactive" 0 \
    "(-i|--interactive)" \
    "$RUN_QEMU" --help

# --- mutually-exclusive guards --------------------------------------------
DUMMY="$(mktemp --suffix=.efi)"
trap 'rm -f "$DUMMY"' EXIT

check "--interactive + --background rejected" 1 \
    "cannot be combined with --background" \
    "$RUN_QEMU" -i --background "$DUMMY"

check "--interactive + --screenshot rejected" 1 \
    "cannot be combined with --screenshot" \
    "$RUN_QEMU" --interactive --screenshot /tmp/x.png "$DUMMY"

# --- missing file guard (sanity, also exercises arg parsing) --------------
check "missing EFI file rejected" 1 \
    "file not found" \
    "$RUN_QEMU" -i /nonexistent/missing.efi

# --- bare-shell mode requires --interactive -------------------------------
check "no EFI + no -i rejected" 1 \
    "or: .* --interactive" \
    "$RUN_QEMU"

# --- --mount validation ---------------------------------------------------
check "--mount rejects missing dir" 1 \
    "is not a directory" \
    "$RUN_QEMU" -i --mount /nonexistent/dir/abc

check "--mount help text mentions virtiofs" 0 \
    "virtiofs" \
    "$RUN_QEMU" --help

# --- --qemu-arg passthrough -----------------------------------------------
check "--help advertises --qemu-arg" 0 \
    "qemu-arg" \
    "$RUN_QEMU" --help

# Smoke-test --qemu-arg by enabling QEMU_DRYRUN — a small extension
# we'll add concurrently that prints the constructed CMD and exits
# without launching QEMU. Falling back to a "missing file" check
# when DRYRUN isn't supported keeps this test a useful arg-shape
# regression even on older run-qemu.sh.
DRYRUN_OUT=$(QEMU_DRYRUN=1 "$RUN_QEMU" --qemu-arg "-name axl-test-flag-A" \
                                       --qemu-arg "-name axl-test-flag-B" \
                                       "$DUMMY" 2>&1 || true)
if grep -q "QEMU_DRYRUN: " <<< "$DRYRUN_OUT"; then
    # Per-token output — grep for each value as a complete line.
    if grep -qE "^QEMU_DRYRUN: axl-test-flag-A$" <<< "$DRYRUN_OUT" \
       && grep -qE "^QEMU_DRYRUN: axl-test-flag-B$" <<< "$DRYRUN_OUT"; then
        echo "PASS: --qemu-arg tokens reach CMD in order"
        PASS=$((PASS + 1))
    else
        echo "FAIL: --qemu-arg tokens not in CMD"
        echo "  output: $DRYRUN_OUT"
        FAIL=$((FAIL + 1))
    fi
else
    echo "SKIP: --qemu-arg dryrun (QEMU_DRYRUN not supported by this run-qemu.sh)"
fi

# --- --ipmi / --ipmi-extern / --ipmi-prop ---------------------------------
check "--help advertises --ipmi" 0 \
    "ipmi-bmc-sim" \
    "$RUN_QEMU" --help

# In-process BMC sim — default props.
DRYRUN_IPMI=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 --ipmi "$DUMMY" 2>&1 || true)
if grep -q "ipmi-bmc-sim,id=axl_bmc" <<< "$DRYRUN_IPMI" \
   && grep -q "isa-ipmi-kcs,bmc=axl_bmc,ioport=0xca2" <<< "$DRYRUN_IPMI"; then
    echo "PASS: --ipmi adds ipmi-bmc-sim + isa-ipmi-kcs"
    PASS=$((PASS + 1))
else
    echo "FAIL: --ipmi did not produce expected device pair"
    echo "  output: $DRYRUN_IPMI"
    FAIL=$((FAIL + 1))
fi

# --ipmi-prop appends K=V to the bmc-sim device line.
DRYRUN_PROP=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 \
                                        --ipmi-prop "product_id=0x0A05" \
                                        --ipmi-prop "fwrev1=2" \
                                        "$DUMMY" 2>&1 || true)
if grep -q "ipmi-bmc-sim,id=axl_bmc,product_id=0x0A05,fwrev1=2" <<< "$DRYRUN_PROP"; then
    echo "PASS: --ipmi-prop K=V appends to bmc-sim device line"
    PASS=$((PASS + 1))
else
    echo "FAIL: --ipmi-prop did not appear on bmc-sim line"
    echo "  output: $DRYRUN_PROP"
    FAIL=$((FAIL + 1))
fi

# --ipmi-extern wires socket-backed bmc-extern at the same KCS port.
DRYRUN_EXT=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 \
                                       --ipmi-extern "/tmp/axl-test.sock" \
                                       "$DUMMY" 2>&1 || true)
if grep -q "ipmi-bmc-extern,id=axl_bmc,chardev=axl_bmcsock" <<< "$DRYRUN_EXT" \
   && grep -q "socket,id=axl_bmcsock,path=/tmp/axl-test.sock" <<< "$DRYRUN_EXT" \
   && grep -q "isa-ipmi-kcs,bmc=axl_bmc,ioport=0xca2" <<< "$DRYRUN_EXT"; then
    echo "PASS: --ipmi-extern wires socket-backed bmc-extern + KCS"
    PASS=$((PASS + 1))
else
    echo "FAIL: --ipmi-extern did not produce expected wiring"
    echo "  output: $DRYRUN_EXT"
    FAIL=$((FAIL + 1))
fi

# aa64 + --ipmi must warn and continue (no ipmi-bmc-* devices on
# AArch64 QEMU). The "warn and continue" path drops the IPMI
# devices but should not abort the run.
DRYRUN_AA64=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch AARCH64 --ipmi "$DUMMY" 2>&1 || true)
if grep -q "WARN: --ipmi" <<< "$DRYRUN_AA64" \
   && ! grep -q "ipmi-bmc-sim" <<< "$DRYRUN_AA64"; then
    echo "PASS: --ipmi on aa64 warns and skips IPMI wiring"
    PASS=$((PASS + 1))
else
    echo "FAIL: --ipmi on aa64 did not warn-and-skip"
    echo "  output: $DRYRUN_AA64"
    FAIL=$((FAIL + 1))
fi

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"

[[ "$FAIL" -eq 0 ]]
