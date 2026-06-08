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
# Each --qemu-arg is ONE literal token, appended verbatim (no word-
# split): two flags → two tokens in order, AND a token may contain a
# space (the property programmatic callers like axl-emulate rely on for
# device specs whose file paths contain spaces).
DRYRUN_OUT=$(QEMU_DRYRUN=1 "$RUN_QEMU" --qemu-arg "axl-flag-A" \
                                       --qemu-arg "axl-flag-B" \
                                       --qemu-arg "tok with space" \
                                       "$DUMMY" 2>&1 || true)
if grep -q "QEMU_DRYRUN: " <<< "$DRYRUN_OUT"; then
    if grep -qE "^QEMU_DRYRUN: axl-flag-A$" <<< "$DRYRUN_OUT" \
       && grep -qE "^QEMU_DRYRUN: axl-flag-B$" <<< "$DRYRUN_OUT"; then
        echo "PASS: --qemu-arg tokens reach CMD in order"
        PASS=$((PASS + 1))
    else
        echo "FAIL: --qemu-arg tokens not in CMD"
        echo "  output: $DRYRUN_OUT"
        FAIL=$((FAIL + 1))
    fi
    # The space-containing token must survive as ONE line, intact.
    if grep -qE "^QEMU_DRYRUN: tok with space$" <<< "$DRYRUN_OUT"; then
        echo "PASS: --qemu-arg preserves a space-containing token verbatim"
        PASS=$((PASS + 1))
    else
        echo "FAIL: --qemu-arg split a space-containing token"
        echo "  output: $DRYRUN_OUT"
        FAIL=$((FAIL + 1))
    fi
else
    echo "SKIP: --qemu-arg dryrun (QEMU_DRYRUN not supported by this run-qemu.sh)"
fi

# --- HF4: --mac ADDR ------------------------------------------------------
# Replay a captured NIC's hardware address. Sets the QEMU NIC device's
# mac= property and implies --net (like --nic-model). aa64 supports
# virtio-net too, so no arch gating here.
check "--help advertises --mac" 0 \
    "mac " \
    "$RUN_QEMU" --help

DRYRUN_MAC=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 \
                                       --mac "de:ad:be:ef:00:01" \
                                       "$DUMMY" 2>&1 || true)
if grep -qE "^QEMU_DRYRUN: virtio-net-pci,netdev=net0,mac=de:ad:be:ef:00:01$" \
        <<< "$DRYRUN_MAC"; then
    echo "PASS: --mac sets the NIC device mac= and implies --net"
    PASS=$((PASS + 1))
else
    echo "FAIL: --mac did not wire mac= onto the NIC device"
    echo "  output: $DRYRUN_MAC"
    FAIL=$((FAIL + 1))
fi

# --mac composes with --nic-model (mac= on the chosen model).
DRYRUN_MAC_E1000=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 \
                                             --nic-model e1000 \
                                             --mac "52:54:00:ab:cd:ef" \
                                             "$DUMMY" 2>&1 || true)
if grep -qE "^QEMU_DRYRUN: e1000,netdev=net0,mac=52:54:00:ab:cd:ef$" \
        <<< "$DRYRUN_MAC_E1000"; then
    echo "PASS: --mac composes with --nic-model"
    PASS=$((PASS + 1))
else
    echo "FAIL: --mac did not compose with --nic-model"
    echo "  output: $DRYRUN_MAC_E1000"
    FAIL=$((FAIL + 1))
fi

# Malformed MAC rejected up-front (clear error, not an opaque QEMU one).
check "--mac rejects malformed address" 1 \
    "MAC" \
    "$RUN_QEMU" --arch X64 --mac "not-a-mac" "$DUMMY"

check "--mac rejects too-few octets" 1 \
    "MAC" \
    "$RUN_QEMU" --arch X64 --mac "de:ad:be:ef:00" "$DUMMY"

# --- HF4: --cpu SPEC ------------------------------------------------------
# Replay a captured CPU identity by overriding QEMU's -cpu model. The
# override replaces the default model (host under KVM, cortex-a57 on
# aa64) — a single -cpu token, not an extra one.
check "--help advertises --cpu" 0 \
    "guest CPU model" \
    "$RUN_QEMU" --help

DRYRUN_CPU=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 \
                                       --cpu "qemu64,vendor=GenuineIntel,family=6,model=42,stepping=7" \
                                       "$DUMMY" 2>&1 || true)
if grep -qE "^QEMU_DRYRUN: -cpu$" <<< "$DRYRUN_CPU" \
   && grep -qE "^QEMU_DRYRUN: qemu64,vendor=GenuineIntel,family=6,model=42,stepping=7$" <<< "$DRYRUN_CPU"; then
    echo "PASS: --cpu overrides the x86 -cpu model"
    PASS=$((PASS + 1))
else
    echo "FAIL: --cpu did not set the -cpu model"
    echo "  output: $DRYRUN_CPU"
    FAIL=$((FAIL + 1))
fi

# The override must REPLACE the default — exactly one -cpu in the CMD,
# not the default plus the override.
cpu_count=$(grep -cE "^QEMU_DRYRUN: -cpu$" <<< "$DRYRUN_CPU" || true)
if [[ "$cpu_count" -eq 1 ]]; then
    echo "PASS: --cpu yields exactly one -cpu (replaces the default)"
    PASS=$((PASS + 1))
else
    echo "FAIL: expected one -cpu, got $cpu_count"
    echo "  output: $DRYRUN_CPU"
    FAIL=$((FAIL + 1))
fi

# aa64: override the cortex-a57 default (e.g. MIDR replay via -cpu max).
DRYRUN_CPU_AA64=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch AARCH64 \
                                            --cpu "max,midr=0x410fd0b0" \
                                            "$DUMMY" 2>&1 || true)
if grep -qE "^QEMU_DRYRUN: max,midr=0x410fd0b0$" <<< "$DRYRUN_CPU_AA64" \
   && ! grep -qE "^QEMU_DRYRUN: cortex-a57$" <<< "$DRYRUN_CPU_AA64"; then
    echo "PASS: --cpu overrides the aa64 default model"
    PASS=$((PASS + 1))
else
    echo "FAIL: --cpu did not override the aa64 model"
    echo "  output: $DRYRUN_CPU_AA64"
    FAIL=$((FAIL + 1))
fi

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"

[[ "$FAIL" -eq 0 ]]
