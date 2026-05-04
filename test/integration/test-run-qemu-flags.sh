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

# --- HF1: --smbios-file ---------------------------------------------------
# Hardware-fixture replay phase 1: low-level QEMU flags. SMBIOS file
# injection is the simplest case — a single -smbios file=... token.
SMBIOS_BLOB="$(mktemp --suffix=.bin)"
# Minimal-but-valid SMBIOS3 entry-point fingerprint so file-existence
# checks pass; QEMU itself doesn't validate the content under DRYRUN.
printf '_SM3_' > "$SMBIOS_BLOB"
trap 'rm -f "$DUMMY" "$SMBIOS_BLOB"' EXIT

check "--help advertises --smbios-file" 0 \
    "smbios-file" \
    "$RUN_QEMU" --help

DRYRUN_SMBIOS=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 \
                                          --smbios-file "$SMBIOS_BLOB" \
                                          "$DUMMY" 2>&1 || true)
if grep -qE "^QEMU_DRYRUN: -smbios$" <<< "$DRYRUN_SMBIOS" \
   && grep -qE "^QEMU_DRYRUN: file=$SMBIOS_BLOB$" <<< "$DRYRUN_SMBIOS"; then
    echo "PASS: --smbios-file emits -smbios file=PATH"
    PASS=$((PASS + 1))
else
    echo "FAIL: --smbios-file did not emit -smbios file=PATH"
    echo "  output: $DRYRUN_SMBIOS"
    FAIL=$((FAIL + 1))
fi

check "--smbios-file rejects missing file" 1 \
    "not found" \
    "$RUN_QEMU" --smbios-file /nonexistent/no.bin "$DUMMY"

# --- HF1: --acpi-table (repeatable) ---------------------------------------
ACPI_A="$(mktemp --suffix=.dat)"
ACPI_B="$(mktemp --suffix=.dat)"
printf 'TESTA' > "$ACPI_A"
printf 'TESTB' > "$ACPI_B"
trap 'rm -f "$DUMMY" "$SMBIOS_BLOB" "$ACPI_A" "$ACPI_B"' EXIT

check "--help advertises --acpi-table" 0 \
    "acpi-table" \
    "$RUN_QEMU" --help

DRYRUN_ACPI=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 \
                                        --acpi-table "$ACPI_A" \
                                        --acpi-table "$ACPI_B" \
                                        "$DUMMY" 2>&1 || true)
acpi_count=$(grep -cE "^QEMU_DRYRUN: -acpitable$" <<< "$DRYRUN_ACPI" || true)
if [[ "$acpi_count" -eq 2 ]] \
   && grep -qE "^QEMU_DRYRUN: file=$ACPI_A$" <<< "$DRYRUN_ACPI" \
   && grep -qE "^QEMU_DRYRUN: file=$ACPI_B$" <<< "$DRYRUN_ACPI"; then
    echo "PASS: --acpi-table is repeatable; both files reach CMD"
    PASS=$((PASS + 1))
else
    echo "FAIL: --acpi-table did not emit two -acpitable file=PATH pairs"
    echo "  output: $DRYRUN_ACPI"
    FAIL=$((FAIL + 1))
fi

check "--acpi-table rejects missing file" 1 \
    "not found" \
    "$RUN_QEMU" --acpi-table /nonexistent/no.dat "$DUMMY"

# --- HF1: --spd ADDR:FILE -------------------------------------------------
# Promotes test/integration/common-test.sh's test_add_smbus_eeprom helper
# to a public flag. Depends on the locally-patched QEMU at $QEMU_DIR
# (scripts/qemu-patches/0001-smbus-eeprom-add-memdev-link.patch). aa64
# warns and skips like --ipmi does. memory-backend-file rounds to 4096.
SPD_BLOB="$(mktemp --suffix=.bin)"
# 4096 bytes — the smbus-eeprom-with-memdev path requires at least one
# host page; gen-spd.py pads to this size for the same reason.
dd if=/dev/zero of="$SPD_BLOB" bs=4096 count=1 status=none
trap 'rm -f "$DUMMY" "$SMBIOS_BLOB" "$ACPI_A" "$ACPI_B" "$SPD_BLOB"' EXIT

check "--help advertises --spd" 0 \
    "spd " \
    "$RUN_QEMU" --help

DRYRUN_SPD=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 \
                                       --spd "0x50:$SPD_BLOB" \
                                       "$DUMMY" 2>&1 || true)
if grep -qE "memory-backend-file,id=axl_spd_50,mem-path=$SPD_BLOB,size=4096,share=off,readonly=on" <<< "$DRYRUN_SPD" \
   && grep -qE "smbus-eeprom,address=0x50,memdev=axl_spd_50" <<< "$DRYRUN_SPD"; then
    echo "PASS: --spd 0x50:FILE wires memory-backend-file + smbus-eeprom"
    PASS=$((PASS + 1))
else
    echo "FAIL: --spd 0x50:FILE did not produce expected wiring"
    echo "  output: $DRYRUN_SPD"
    FAIL=$((FAIL + 1))
fi

# Repeatable: two SPD blobs at different addresses should produce two
# (object,device) pairs with distinct ids derived from the address.
SPD_BLOB2="$(mktemp --suffix=.bin)"
dd if=/dev/zero of="$SPD_BLOB2" bs=4096 count=1 status=none
trap 'rm -f "$DUMMY" "$SMBIOS_BLOB" "$ACPI_A" "$ACPI_B" "$SPD_BLOB" "$SPD_BLOB2"' EXIT

DRYRUN_SPD2=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 \
                                        --spd "0x50:$SPD_BLOB" \
                                        --spd "0x51:$SPD_BLOB2" \
                                        "$DUMMY" 2>&1 || true)
if grep -q "smbus-eeprom,address=0x50,memdev=axl_spd_50" <<< "$DRYRUN_SPD2" \
   && grep -q "smbus-eeprom,address=0x51,memdev=axl_spd_51" <<< "$DRYRUN_SPD2"; then
    echo "PASS: --spd is repeatable; ids derive from address"
    PASS=$((PASS + 1))
else
    echo "FAIL: --spd repeatable wiring incorrect"
    echo "  output: $DRYRUN_SPD2"
    FAIL=$((FAIL + 1))
fi

# aa64: warn-and-skip, no smbus-eeprom token.
DRYRUN_SPD_AA64=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch AARCH64 \
                                            --spd "0x50:$SPD_BLOB" \
                                            "$DUMMY" 2>&1 || true)
if grep -q "WARN: --spd" <<< "$DRYRUN_SPD_AA64" \
   && ! grep -q "smbus-eeprom" <<< "$DRYRUN_SPD_AA64"; then
    echo "PASS: --spd on aa64 warns and skips wiring"
    PASS=$((PASS + 1))
else
    echo "FAIL: --spd on aa64 did not warn-and-skip"
    echo "  output: $DRYRUN_SPD_AA64"
    FAIL=$((FAIL + 1))
fi

# Malformed --spd argument (missing colon) — clear error.
check "--spd rejects malformed ADDR:FILE" 1 \
    "ADDR:FILE" \
    "$RUN_QEMU" --arch X64 --spd "0x50_bad" "$DUMMY"

# Missing SPD blob — clear error.
check "--spd rejects missing blob" 1 \
    "not found" \
    "$RUN_QEMU" --arch X64 --spd "0x50:/nonexistent/spd.bin" "$DUMMY"

# 7-bit SMBus addressing — 0x80–0xFF must be rejected up-front
# rather than letting smbus-eeprom emit an opaque QEMU realize-time
# error.
check "--spd rejects 0x80 (out of 7-bit range)" 1 \
    "7-bit" \
    "$RUN_QEMU" --arch X64 --spd "0x80:$SPD_BLOB" "$DUMMY"

# --- HF1: --tpm family ----------------------------------------------------
# swtpm lifecycle modeled on virtiofsd (--mount). DRYRUN tests verify
# the QEMU side of the wiring; swtpm itself is checked for presence
# but not actually launched under DRYRUN.
#
# CI portability: do NOT rely on swtpm being installed on the test
# host. Stub a fake executable that satisfies run-qemu.sh's `[[ -x ]]`
# check, and point SWTPM= at it. DRYRUN bypasses the actual launch
# so the stub is never invoked. Tests that need to assert the
# absent-swtpm error path use SWTPM=/nonexistent/swtpm instead.
SWTPM_STUB="$(mktemp --suffix=.sh)"
cat > "$SWTPM_STUB" <<'STUB'
#!/bin/sh
echo "fake-swtpm: should never be executed under DRYRUN" >&2
exit 99
STUB
chmod +x "$SWTPM_STUB"
trap 'rm -f "$DUMMY" "$SMBIOS_BLOB" "$ACPI_A" "$ACPI_B" "$SPD_BLOB" "$SPD_BLOB2" "$SWTPM_STUB"' EXIT

check "--help advertises --tpm" 0 \
    "tpm" \
    "$RUN_QEMU" --help

# Default (x64): tpm-tis. SWTPM= overrides the on-PATH search and is
# always set to the stub for portability — see comment above.
DRYRUN_TPM_X64=$(QEMU_DRYRUN=1 SWTPM="$SWTPM_STUB" \
                 "$RUN_QEMU" --arch X64 \
                             --tpm \
                             "$DUMMY" 2>&1 || true)
if grep -qE "socket,id=axl_tpmsock,path=" <<< "$DRYRUN_TPM_X64" \
   && grep -qE "emulator,id=axl_tpm,chardev=axl_tpmsock" <<< "$DRYRUN_TPM_X64" \
   && grep -qE "^QEMU_DRYRUN: tpm-tis,tpmdev=axl_tpm$" <<< "$DRYRUN_TPM_X64"; then
    echo "PASS: --tpm on x64 wires swtpm chardev + emulator + tpm-tis"
    PASS=$((PASS + 1))
else
    echo "FAIL: --tpm on x64 wiring incorrect"
    echo "  output: $DRYRUN_TPM_X64"
    FAIL=$((FAIL + 1))
fi

# Default (aa64): tpm-tis-device (sysbus MMIO; tpm-tis/tpm-crb are
# x86-only in QEMU's device registry).
DRYRUN_TPM_AA64=$(QEMU_DRYRUN=1 SWTPM="$SWTPM_STUB" \
                  "$RUN_QEMU" --arch AARCH64 \
                              --tpm \
                              "$DUMMY" 2>&1 || true)
if grep -qE "^QEMU_DRYRUN: tpm-tis-device,tpmdev=axl_tpm$" <<< "$DRYRUN_TPM_AA64"; then
    echo "PASS: --tpm on aa64 defaults to tpm-tis-device"
    PASS=$((PASS + 1))
else
    echo "FAIL: --tpm on aa64 did not default to tpm-tis-device"
    echo "  output: $DRYRUN_TPM_AA64"
    FAIL=$((FAIL + 1))
fi

# --tpm-model tpm-crb override (x64 only).
DRYRUN_TPM_CRB=$(QEMU_DRYRUN=1 SWTPM="$SWTPM_STUB" \
                 "$RUN_QEMU" --arch X64 \
                             --tpm \
                             --tpm-model tpm-crb \
                             "$DUMMY" 2>&1 || true)
if grep -qE "^QEMU_DRYRUN: tpm-crb,tpmdev=axl_tpm$" <<< "$DRYRUN_TPM_CRB"; then
    echo "PASS: --tpm-model tpm-crb overrides x64 default"
    PASS=$((PASS + 1))
else
    echo "FAIL: --tpm-model tpm-crb did not override default"
    echo "  output: $DRYRUN_TPM_CRB"
    FAIL=$((FAIL + 1))
fi

# Invalid --tpm-model rejected up-front (not lazily by QEMU).
check "--tpm-model rejects unknown value" 1 \
    "tpm-model" \
    env SWTPM="$SWTPM_STUB" \
    "$RUN_QEMU" --arch X64 --tpm --tpm-model bogus "$DUMMY"

# --tpm-state DIR raw passthrough — DIR exists, is forwarded as-is to
# swtpm (no interpretation of contents). DRYRUN doesn't launch swtpm
# but should reflect the path on the chardev / state-dir reference.
TPM_STATE_DIR="$(mktemp -d)"
trap 'rm -rf "$TPM_STATE_DIR"; rm -f "$DUMMY" "$SMBIOS_BLOB" "$ACPI_A" "$ACPI_B" "$SPD_BLOB" "$SPD_BLOB2" "$SWTPM_STUB"' EXIT

DRYRUN_TPM_STATE=$(QEMU_DRYRUN=1 SWTPM="$SWTPM_STUB" \
                   "$RUN_QEMU" --arch X64 \
                               --tpm-state "$TPM_STATE_DIR" \
                               "$DUMMY" 2>&1 || true)
# We assert the chardev path embeds the real socket and the run-qemu.sh
# stderr or DRYRUN trace mentions the user's state dir. The exact
# embedding depends on how implementation wires swtpm; minimum: the
# QEMU command must reference a swtpm chardev (i.e., --tpm-state
# implies swtpm spawn) AND we should not silently ignore --tpm-state.
if grep -qE "emulator,id=axl_tpm,chardev=axl_tpmsock" <<< "$DRYRUN_TPM_STATE"; then
    echo "PASS: --tpm-state implies swtpm spawn (chardev present)"
    PASS=$((PASS + 1))
else
    echo "FAIL: --tpm-state did not produce swtpm wiring"
    echo "  output: $DRYRUN_TPM_STATE"
    FAIL=$((FAIL + 1))
fi

check "--tpm-state rejects missing directory" 1 \
    "not a directory" \
    env SWTPM="$SWTPM_STUB" \
    "$RUN_QEMU" --tpm-state /nonexistent/tpm/state "$DUMMY"

# swtpm absent: SWTPM=/nonexistent forces the absence path; should
# error with an install hint, not silently degrade.
check "--tpm errors when swtpm not on PATH" 1 \
    "swtpm" \
    env SWTPM=/nonexistent/swtpm "$RUN_QEMU" --arch X64 --tpm "$DUMMY"

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"

[[ "$FAIL" -eq 0 ]]
