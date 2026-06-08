#!/bin/bash
# test-axl-emulate.sh -- argument-parsing and fixture-translation
# tests for scripts/axl-emulate.
#
# Cheap host-side tests (no QEMU). axl-emulate honors
# AXL_EMULATE_DRYRUN=1 (mirrors run-qemu.sh's QEMU_DRYRUN convention)
# and prints the run-qemu.sh argv it would exec, one token per line
# prefixed with "AXL_EMULATE_DRYRUN: ".

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
AXL_EMULATE="$PROJECT_DIR/scripts/axl-emulate"

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

# Build fixture directories the tests will use. Each fixture is a
# minimal byte-faithful structure — ACPI sig detection reads the
# first 4 bytes of each .dat, and the SPD wiring needs at least
# one host page (4096 bytes) for memory-backend-file.
FIX_EMPTY="$(mktemp -d)"

FIX_FULL="$(mktemp -d)"
printf '_SM3_' > "$FIX_FULL/smbios.bin"
mkdir -p "$FIX_FULL/acpi" "$FIX_FULL/spd" "$FIX_FULL/tpm"
# ACPI tables: MCFG (denylisted, topology), BERT (kept, boot-error
# table), WAET (kept, Windows-emulation hint). acpidump uses
# lowercase filenames; the in-file signature is uppercase ASCII —
# test both encodings work via signature-not-filename matching.
printf 'MCFG' > "$FIX_FULL/acpi/mcfg.dat"
printf 'BERT' > "$FIX_FULL/acpi/bert.dat"
printf 'WAET' > "$FIX_FULL/acpi/waet.dat"
# SPD blobs at 0x50 / 0x51 — pad to one page so memory-backend-file
# accepts size=4096.
dd if=/dev/zero of="$FIX_FULL/spd/0x50.bin" bs=4096 count=1 status=none
dd if=/dev/zero of="$FIX_FULL/spd/0x51.bin" bs=4096 count=1 status=none
# tpm/ presence triggers --tpm-state passthrough. Empty subdir is
# fine for HF1-shape passthrough tests; HF5 will populate it.
# manifest.json is optional but commonly present.
cat > "$FIX_FULL/manifest.json" <<'EOF'
{
  "vendor": "QEMU",
  "model": "Standard PC (i440FX + PIIX, 1996)",
  "bios_rev": "rel-1.16.3",
  "capture_date": "2026-05-04",
  "capture_tool_version": "test"
}
EOF

# net.json (HF2.3 device manifest) — a NIC with a distinctive permanent
# MAC so the --mac replay knob has something concrete to wire.
cat > "$FIX_FULL/net.json" <<'EOF'
{
  "count": 1,
  "nics": [
    {
      "index": 0,
      "state": 1,
      "if_type": 1,
      "hw_address_size": 6,
      "mac": "de:ad:be:ef:00:02",
      "permanent_mac": "de:ad:be:ef:00:01",
      "media_present": true
    }
  ]
}
EOF

# cpu.json (HF2.2 manifest, x86_64) — distinctive family/model so the
# --cpu-from-fixture replay knob has concrete identity to wire.
cat > "$FIX_FULL/cpu.json" <<'EOF'
{
  "arch": "x86_64",
  "vendor": "GenuineIntel",
  "family": 6,
  "model": 42,
  "stepping": 7,
  "brand": "Test CPU @ 3.00GHz"
}
EOF

# An aarch64 cpu.json fixture for the MIDR-replay path.
FIX_ARM="$(mktemp -d)"
cat > "$FIX_ARM/cpu.json" <<'EOF'
{
  "arch": "aarch64",
  "midr_el1": "0x410fd0b0"
}
EOF

trap 'rm -rf "$FIX_EMPTY" "$FIX_FULL" "$FIX_ARM"' EXIT

# --- syntax + help --------------------------------------------------------
if python3 -c "compile(open('$AXL_EMULATE').read(), '$AXL_EMULATE', 'exec')" 2>/dev/null; then
    echo "PASS: python3 syntax"
    PASS=$((PASS + 1))
else
    echo "FAIL: python3 syntax"
    FAIL=$((FAIL + 1))
fi

check "--help exits 0 and mentions fixture-dir" 0 \
    "fixture" \
    "$AXL_EMULATE" --help

check "--help advertises ACPI overrides" 0 \
    "keep-acpi|drop-acpi|strict-acpi" \
    "$AXL_EMULATE" --help

# --- argument validation --------------------------------------------------
check "missing fixture dir errors clearly" 2 \
    "" \
    "$AXL_EMULATE"

check "nonexistent fixture dir errors with path" 1 \
    "/nonexistent/no/dir" \
    "$AXL_EMULATE" /nonexistent/no/dir

# --- empty fixture (smoke: exec'd cmd has no fixture flags) --------------
DRY_EMPTY=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" "$FIX_EMPTY" 2>&1 || true)
DRY_EMPTY_ARGV=$(grep "^AXL_EMULATE_DRYRUN: " <<< "$DRY_EMPTY" || true)
if [[ -n "$DRY_EMPTY_ARGV" ]]; then
    if ! grep -qE "(-smbios|-acpitable|smbus-eeprom|--tpm-state)" <<< "$DRY_EMPTY_ARGV"; then
        echo "PASS: empty fixture produces no fixture-related flags"
        PASS=$((PASS + 1))
    else
        echo "FAIL: empty fixture leaked fixture flags"
        echo "  argv: $DRY_EMPTY_ARGV"
        FAIL=$((FAIL + 1))
    fi
else
    echo "FAIL: AXL_EMULATE_DRYRUN missing from empty-fixture output"
    echo "  output: $DRY_EMPTY"
    FAIL=$((FAIL + 1))
fi

# --- smbios.bin discovery ------------------------------------------------
DRY_FULL=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" "$FIX_FULL" 2>&1 || true)
DRY_FULL_ARGV_SCOPED=$(grep "^AXL_EMULATE_DRYRUN: " <<< "$DRY_FULL" || true)
# smbios.bin → -smbios file=PATH (emitted as --qemu-arg passthrough).
if grep -qE "^AXL_EMULATE_DRYRUN: -smbios$" <<< "$DRY_FULL_ARGV_SCOPED" \
   && grep -qE "^AXL_EMULATE_DRYRUN: file=$FIX_FULL/smbios.bin$" <<< "$DRY_FULL_ARGV_SCOPED"; then
    echo "PASS: fixture/smbios.bin → -smbios file=<path> (--qemu-arg)"
    PASS=$((PASS + 1))
else
    echo "FAIL: smbios.bin not wired to -smbios file="
    echo "  argv: $DRY_FULL_ARGV_SCOPED"
    FAIL=$((FAIL + 1))
fi

# --- ACPI denylist behavior (default-drop MCFG, keep BERT/WAET) ----------
# Grep only the argv lines (AXL_EMULATE_DRYRUN: prefix) — diagnostic
# messages on stderr will mention dropped filenames, so a naive grep
# would falsely match "mcfg.dat appears in output".
DRY_FULL_ARGV=$(grep "^AXL_EMULATE_DRYRUN: " <<< "$DRY_FULL" || true)
acpi_table_count=$(grep -cE "^AXL_EMULATE_DRYRUN: -acpitable$" <<< "$DRY_FULL" || true)
if [[ "$acpi_table_count" -eq 2 ]] \
   && grep -qE "bert\.dat" <<< "$DRY_FULL_ARGV" \
   && grep -qE "waet\.dat" <<< "$DRY_FULL_ARGV" \
   && ! grep -qE "mcfg\.dat" <<< "$DRY_FULL_ARGV"; then
    echo "PASS: default-drop denylist drops MCFG, keeps BERT/WAET"
    PASS=$((PASS + 1))
else
    echo "FAIL: ACPI denylist filtering incorrect"
    echo "  argv: $DRY_FULL_ARGV"
    FAIL=$((FAIL + 1))
fi

# --- ACPI signature detection is content-based, not filename-based -------
# Create a fixture where the filename lies about the table.
FIX_SIG="$(mktemp -d)"
mkdir -p "$FIX_SIG/acpi"
printf 'MCFG' > "$FIX_SIG/acpi/innocuous.dat"   # filename clean, sig denylisted
printf 'WAET' > "$FIX_SIG/acpi/MCFG.dat"        # filename denylisted, sig safe
trap 'rm -rf "$FIX_EMPTY" "$FIX_FULL" "$FIX_SIG"' EXIT

DRY_SIG=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" "$FIX_SIG" 2>&1 || true)
DRY_SIG_ARGV=$(grep "^AXL_EMULATE_DRYRUN: " <<< "$DRY_SIG" || true)
# innocuous.dat has MCFG content → dropped (signature is denylisted).
# MCFG.dat has WAET content → kept (filename is irrelevant; signature is safe).
if grep -qE "MCFG\.dat" <<< "$DRY_SIG_ARGV" \
   && ! grep -qE "innocuous\.dat" <<< "$DRY_SIG_ARGV"; then
    echo "PASS: ACPI denylist matches by 4-byte signature, not filename"
    PASS=$((PASS + 1))
else
    echo "FAIL: ACPI denylist matched by filename"
    echo "  argv: $DRY_SIG_ARGV"
    FAIL=$((FAIL + 1))
fi

# --- --keep-acpi override ------------------------------------------------
DRY_KEEP=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" --keep-acpi MCFG "$FIX_FULL" 2>&1 || true)
DRY_KEEP_ARGV=$(grep "^AXL_EMULATE_DRYRUN: " <<< "$DRY_KEEP" || true)
if grep -qE "mcfg\.dat" <<< "$DRY_KEEP_ARGV"; then
    echo "PASS: --keep-acpi MCFG overrides default-drop"
    PASS=$((PASS + 1))
else
    echo "FAIL: --keep-acpi MCFG did not survive denylist"
    echo "  argv: $DRY_KEEP_ARGV"
    FAIL=$((FAIL + 1))
fi

# --- --drop-acpi adds to denylist ----------------------------------------
# FIX_FULL has BERT (default-keep) and WAET (default-keep) and MCFG
# (default-drop). --drop-acpi BERT extends the denylist; expect WAET
# to stay, MCFG and BERT to drop.
DRY_DROP=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" --drop-acpi BERT "$FIX_FULL" 2>&1 || true)
DRY_DROP_ARGV=$(grep "^AXL_EMULATE_DRYRUN: " <<< "$DRY_DROP" || true)
if grep -qE "waet\.dat" <<< "$DRY_DROP_ARGV" \
   && ! grep -qE "bert\.dat" <<< "$DRY_DROP_ARGV" \
   && ! grep -qE "mcfg\.dat" <<< "$DRY_DROP_ARGV"; then
    echo "PASS: --drop-acpi BERT extends denylist"
    PASS=$((PASS + 1))
else
    echo "FAIL: --drop-acpi did not extend denylist correctly"
    echo "  argv: $DRY_DROP_ARGV"
    FAIL=$((FAIL + 1))
fi

# --- core-singleton tables in default denylist ---------------------------
# FACP/FACS/DSDT are ACPI singletons; replaying alongside QEMU's
# auto-generated copies hangs the boot (verified by direct bisection
# against the Proxmox VM fixture). APIC/SSDT/HPET multi-occurrence
# is tolerated by the spec / OVMF / OSes, and inject cleanly — they
# stay default-keep.
FIX_CORE="$(mktemp -d)"
mkdir -p "$FIX_CORE/acpi"
# Drops:
for sig in FACP FACS DSDT; do
    printf '%s' "$sig" > "$FIX_CORE/acpi/${sig,,}.dat"
done
# Keeps:
for sig in APIC SSDT HPET WAET; do
    printf '%s' "$sig" > "$FIX_CORE/acpi/${sig,,}.dat"
done
trap 'rm -rf "$FIX_EMPTY" "$FIX_FULL" "$FIX_SIG" "$FIX_STRICT" "$DUMMY_EFI" "$FIX_CORE"' EXIT

DRY_CORE=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" "$FIX_CORE" 2>&1 || true)
DRY_CORE_ARGV=$(grep "^AXL_EMULATE_DRYRUN: " <<< "$DRY_CORE" || true)
all_dropped=true
for sig in facp facs dsdt; do
    if grep -qE "${sig}\.dat" <<< "$DRY_CORE_ARGV"; then
        all_dropped=false
        break
    fi
done
all_kept=true
for sig in apic ssdt hpet waet; do
    if ! grep -qE "${sig}\.dat" <<< "$DRY_CORE_ARGV"; then
        all_kept=false
        break
    fi
done
if $all_dropped && $all_kept; then
    echo "PASS: core-singleton tables (FACP/FACS/DSDT) drop; APIC/SSDT/HPET/WAET keep"
    PASS=$((PASS + 1))
else
    echo "FAIL: core-singleton denylist incorrect"
    echo "  argv: $DRY_CORE_ARGV"
    FAIL=$((FAIL + 1))
fi

# --- --strict-acpi reduces denylist to MCFG only -------------------------
# A fixture with SRAT (default-drop) should keep SRAT under --strict-acpi.
FIX_STRICT="$(mktemp -d)"
mkdir -p "$FIX_STRICT/acpi"
printf 'MCFG' > "$FIX_STRICT/acpi/mcfg.dat"
printf 'SRAT' > "$FIX_STRICT/acpi/srat.dat"
printf 'FACP' > "$FIX_STRICT/acpi/facp.dat"
trap 'rm -rf "$FIX_EMPTY" "$FIX_FULL" "$FIX_SIG" "$FIX_STRICT"' EXIT

DRY_STRICT=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" --strict-acpi "$FIX_STRICT" 2>&1 || true)
DRY_STRICT_ARGV=$(grep "^AXL_EMULATE_DRYRUN: " <<< "$DRY_STRICT" || true)
if grep -qE "srat\.dat" <<< "$DRY_STRICT_ARGV" \
   && grep -qE "facp\.dat" <<< "$DRY_STRICT_ARGV" \
   && ! grep -qE "mcfg\.dat" <<< "$DRY_STRICT_ARGV"; then
    echo "PASS: --strict-acpi keeps SRAT, only drops MCFG"
    PASS=$((PASS + 1))
else
    echo "FAIL: --strict-acpi behavior incorrect"
    echo "  argv: $DRY_STRICT_ARGV"
    FAIL=$((FAIL + 1))
fi

# --- --keep-acpi takes precedence over --drop-acpi -----------------------
# Same signature on both lists → keep wins. Documents the contract
# so a future refactor can't silently flip it.
DRY_PRECEDENCE=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" \
                 --keep-acpi MCFG --drop-acpi MCFG "$FIX_FULL" 2>&1 || true)
DRY_PRECEDENCE_ARGV=$(grep "^AXL_EMULATE_DRYRUN: " <<< "$DRY_PRECEDENCE" || true)
if grep -qE "mcfg\.dat" <<< "$DRY_PRECEDENCE_ARGV"; then
    echo "PASS: --keep-acpi wins over --drop-acpi when both set"
    PASS=$((PASS + 1))
else
    echo "FAIL: precedence wrong (--drop-acpi appears to win)"
    echo "  argv: $DRY_PRECEDENCE_ARGV"
    FAIL=$((FAIL + 1))
fi

# --- spd/*.bin → -object memory-backend-file + -device smbus-eeprom ------
# Emitted as --qemu-arg passthrough: one smbus-eeprom device per blob,
# its memory-backend-file mem-path pointing at the captured blob.
spd_dev_count=$(grep -cE "^AXL_EMULATE_DRYRUN: smbus-eeprom,address=0x5[01]," <<< "$DRY_FULL" || true)
if [[ "$spd_dev_count" -eq 2 ]] \
   && grep -qE "^AXL_EMULATE_DRYRUN: smbus-eeprom,address=0x50,memdev=axl_spd_50$" <<< "$DRY_FULL" \
   && grep -qE "^AXL_EMULATE_DRYRUN: smbus-eeprom,address=0x51,memdev=axl_spd_51$" <<< "$DRY_FULL" \
   && grep -qE "mem-path=$FIX_FULL/spd/0x50\.bin," <<< "$DRY_FULL" \
   && grep -qE "mem-path=$FIX_FULL/spd/0x51\.bin," <<< "$DRY_FULL"; then
    echo "PASS: spd/0xNN.bin → smbus-eeprom + memory-backend-file (--qemu-arg)"
    PASS=$((PASS + 1))
else
    echo "FAIL: SPD wiring incorrect"
    echo "  output: $DRY_FULL"
    FAIL=$((FAIL + 1))
fi

# --- tpm/ subdirectory wires the emulator-backend TPM (swtpm) ------------
# axl-emulate spawns swtpm itself and wires QEMU's emulator TPM to its
# control socket via --qemu-arg (x86 default model tpm-tis).
if grep -qE "^AXL_EMULATE_DRYRUN: -tpmdev$" <<< "$DRY_FULL" \
   && grep -qE "^AXL_EMULATE_DRYRUN: emulator,id=axl_tpm,chardev=axl_tpmsock$" <<< "$DRY_FULL" \
   && grep -qE "^AXL_EMULATE_DRYRUN: tpm-tis,tpmdev=axl_tpm$" <<< "$DRY_FULL" \
   && grep -qE "^AXL_EMULATE_DRYRUN: socket,id=axl_tpmsock,path=" <<< "$DRY_FULL"; then
    echo "PASS: tpm/ subdir → swtpm chardev + tpmdev emulator + tpm-tis"
    PASS=$((PASS + 1))
else
    echo "FAIL: tpm/ subdir did not wire the emulator TPM"
    echo "  output: $DRY_FULL"
    FAIL=$((FAIL + 1))
fi

# aarch64 uses tpm-tis-device (sysbus), not tpm-tis (ISA).
DRY_TPM_AA64=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" --arch AARCH64 "$FIX_FULL" 2>&1 || true)
if grep -qE "^AXL_EMULATE_DRYRUN: tpm-tis-device,tpmdev=axl_tpm$" <<< "$DRY_TPM_AA64"; then
    echo "PASS: tpm/ on aarch64 → tpm-tis-device"
    PASS=$((PASS + 1))
else
    echo "FAIL: tpm/ on aarch64 did not use tpm-tis-device"
    echo "  output: $DRY_TPM_AA64"
    FAIL=$((FAIL + 1))
fi

# --- positional efi-file forwards to run-qemu.sh -------------------------
DUMMY_EFI="$(mktemp --suffix=.efi)"
trap 'rm -rf "$FIX_EMPTY" "$FIX_FULL" "$FIX_SIG" "$FIX_STRICT" "$DUMMY_EFI" "$FIX_ARM"' EXIT

DRY_EFI=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" "$FIX_FULL" "$DUMMY_EFI" 2>&1 || true)
if grep -qE "^AXL_EMULATE_DRYRUN: $DUMMY_EFI$" <<< "$DRY_EFI"; then
    echo "PASS: positional efi-file forwarded to run-qemu.sh"
    PASS=$((PASS + 1))
else
    echo "FAIL: positional efi-file not forwarded"
    echo "  output: $DRY_EFI"
    FAIL=$((FAIL + 1))
fi

# --- -- separator: pass-through args reach run-qemu.sh -------------------
DRY_PASS=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" "$FIX_EMPTY" "$DUMMY_EFI" \
                              -- --background --serial-log /tmp/axl-emulate-test.log \
                              2>&1 || true)
if grep -qE "^AXL_EMULATE_DRYRUN: --background$" <<< "$DRY_PASS" \
   && grep -qE "^AXL_EMULATE_DRYRUN: --serial-log$" <<< "$DRY_PASS" \
   && grep -qE "^AXL_EMULATE_DRYRUN: /tmp/axl-emulate-test\.log$" <<< "$DRY_PASS"; then
    echo "PASS: -- separator passes args through to run-qemu.sh"
    PASS=$((PASS + 1))
else
    echo "FAIL: -- pass-through args not forwarded"
    echo "  output: $DRY_PASS"
    FAIL=$((FAIL + 1))
fi

# --- --arch forwards to run-qemu.sh --------------------------------------
DRY_ARCH=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" --arch AARCH64 "$FIX_EMPTY" 2>&1 || true)
if grep -qE "^AXL_EMULATE_DRYRUN: --arch$" <<< "$DRY_ARCH" \
   && grep -qE "^AXL_EMULATE_DRYRUN: AARCH64$" <<< "$DRY_ARCH"; then
    echo "PASS: --arch forwarded to run-qemu.sh"
    PASS=$((PASS + 1))
else
    echo "FAIL: --arch not forwarded"
    echo "  output: $DRY_ARCH"
    FAIL=$((FAIL + 1))
fi

# --- manifest.json summary printed to stderr at startup ------------------
# axl-emulate prints the manifest summary to stderr so it doesn't
# pollute the AXL_EMULATE_DRYRUN stdout stream tests grep on. Test
# captures stderr explicitly.
MFEST_OUT=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" "$FIX_FULL" 2>&1 >/dev/null || true)
if grep -q "Standard PC (i440FX" <<< "$MFEST_OUT"; then
    echo "PASS: manifest.json summary printed at startup"
    PASS=$((PASS + 1))
else
    echo "FAIL: manifest.json summary not surfaced"
    echo "  stderr: $MFEST_OUT"
    FAIL=$((FAIL + 1))
fi

# --- run-qemu.sh is the wrapped tool (last token before pass-through) ----
# The first token of the constructed CMD must be the run-qemu.sh path.
first_tok=$(grep -m1 "^AXL_EMULATE_DRYRUN: " <<< "$DRY_EMPTY" | sed 's/^AXL_EMULATE_DRYRUN: //')
if [[ "$first_tok" == *"run-qemu.sh"* ]]; then
    echo "PASS: axl-emulate exec target is run-qemu.sh"
    PASS=$((PASS + 1))
else
    echo "FAIL: first DRYRUN token is not run-qemu.sh: '$first_tok'"
    FAIL=$((FAIL + 1))
fi

# --- HF4: --mac (replay captured NIC MAC from net.json) ------------------
# Opt-in: without --mac, net.json is informational and no NIC is wired.
if ! grep -qE "^AXL_EMULATE_DRYRUN: --mac$" <<< "$DRY_FULL"; then
    echo "PASS: net.json alone does not wire a NIC (--mac is opt-in)"
    PASS=$((PASS + 1))
else
    echo "FAIL: net.json leaked a --mac without the flag"
    FAIL=$((FAIL + 1))
fi

# With --mac, the first NIC's permanent_mac is wired via --net + --mac.
DRY_MAC=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" --mac "$FIX_FULL" 2>&1 || true)
DRY_MAC_ARGV=$(grep "^AXL_EMULATE_DRYRUN: " <<< "$DRY_MAC" || true)
if grep -qE "^AXL_EMULATE_DRYRUN: --net$" <<< "$DRY_MAC_ARGV" \
   && grep -qE "^AXL_EMULATE_DRYRUN: --mac$" <<< "$DRY_MAC_ARGV" \
   && grep -qE "^AXL_EMULATE_DRYRUN: de:ad:be:ef:00:01$" <<< "$DRY_MAC_ARGV"; then
    echo "PASS: --mac wires net.json permanent_mac via --net --mac"
    PASS=$((PASS + 1))
else
    echo "FAIL: --mac did not wire the captured NIC MAC"
    echo "  argv: $DRY_MAC_ARGV"
    FAIL=$((FAIL + 1))
fi

# --mac against a fixture with no net.json warns and skips (no --net).
DRY_MAC_NONE=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" --mac "$FIX_EMPTY" 2>&1 || true)
if ! grep -qE "^AXL_EMULATE_DRYRUN: --mac$" <<< "$DRY_MAC_NONE" \
   && grep -qE "no usable MAC" <<< "$DRY_MAC_NONE"; then
    echo "PASS: --mac with no net.json warns and skips"
    PASS=$((PASS + 1))
else
    echo "FAIL: --mac with no net.json did not warn-and-skip"
    echo "  output: $DRY_MAC_NONE"
    FAIL=$((FAIL + 1))
fi

# --- HF4: --cpu-from-fixture (replay captured CPU identity) ---------------
# Opt-in: cpu.json alone does not set --cpu.
if ! grep -qE "^AXL_EMULATE_DRYRUN: --cpu$" <<< "$DRY_FULL"; then
    echo "PASS: cpu.json alone does not set --cpu (opt-in)"
    PASS=$((PASS + 1))
else
    echo "FAIL: cpu.json leaked a --cpu without the flag"
    FAIL=$((FAIL + 1))
fi

# x86_64 cpu.json → qemu64 with vendor/family/model/stepping overrides.
DRY_CPU=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" --cpu-from-fixture "$FIX_FULL" 2>&1 || true)
DRY_CPU_ARGV=$(grep "^AXL_EMULATE_DRYRUN: " <<< "$DRY_CPU" || true)
if grep -qE "^AXL_EMULATE_DRYRUN: --cpu$" <<< "$DRY_CPU_ARGV" \
   && grep -qE "^AXL_EMULATE_DRYRUN: qemu64,vendor=GenuineIntel,family=6,model=42,stepping=7$" <<< "$DRY_CPU_ARGV"; then
    echo "PASS: --cpu-from-fixture wires x86 cpu.json → -cpu spec"
    PASS=$((PASS + 1))
else
    echo "FAIL: --cpu-from-fixture x86 spec wrong"
    echo "  argv: $DRY_CPU_ARGV"
    FAIL=$((FAIL + 1))
fi

# aarch64 cpu.json → max with the captured MIDR.
DRY_CPU_ARM=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" --cpu-from-fixture --arch AARCH64 "$FIX_ARM" 2>&1 || true)
if grep -qE "^AXL_EMULATE_DRYRUN: --cpu$" <<< "$DRY_CPU_ARM" \
   && grep -qE "^AXL_EMULATE_DRYRUN: max,midr=0x410fd0b0$" <<< "$DRY_CPU_ARM"; then
    echo "PASS: --cpu-from-fixture wires aarch64 MIDR → -cpu max,midr"
    PASS=$((PASS + 1))
else
    echo "FAIL: --cpu-from-fixture aarch64 spec wrong"
    echo "  output: $DRY_CPU_ARM"
    FAIL=$((FAIL + 1))
fi

# No cpu.json → warn and keep the default model (no --cpu).
DRY_CPU_NONE=$(AXL_EMULATE_DRYRUN=1 "$AXL_EMULATE" --cpu-from-fixture "$FIX_EMPTY" 2>&1 || true)
if ! grep -qE "^AXL_EMULATE_DRYRUN: --cpu$" <<< "$DRY_CPU_NONE" \
   && grep -qE "no usable identity" <<< "$DRY_CPU_NONE"; then
    echo "PASS: --cpu-from-fixture with no cpu.json warns and skips"
    PASS=$((PASS + 1))
else
    echo "FAIL: --cpu-from-fixture with no cpu.json did not warn-and-skip"
    echo "  output: $DRY_CPU_NONE"
    FAIL=$((FAIL + 1))
fi

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"

[[ "$FAIL" -eq 0 ]]
