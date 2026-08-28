#!/bin/bash
# test-meta: arch=x64 needs= est=9 local-only=0
# test-lsacpi-qemu.sh — lsacpi against the firmware's own ACPI tables.
#
# The unit suite tests the AML walker and the typed readers against
# fixed buffers. This boots QEMU and runs tools/lsacpi.efi so the
# inventory runs over tables OVMF actually published: signatures,
# lengths, revisions, OEM strings and checksum verdicts.
#
# The sharpest assertion here is the FACS row. FACS has no standard
# ACPI header -- its first 8 bytes are signature and length, and offset
# 8 onward is HardwareSignature, NOT revision/checksum/OEM. A tool that
# renders every catalog entry through AxlAcpiHeader prints four columns
# of garbage and a meaningless checksum verdict for exactly one row, and
# nothing else in the output would look wrong.
#
# Usage: ./test/integration/test-lsacpi-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -1
EFI="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)/tools/lsacpi.efi"
[[ -f "$EFI" ]] || { echo "FAIL: lsacpi.efi not built"; exit 1; }

WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 "$EFI" 2>&1 \
    | tee "$LOG" >/dev/null

fail=0
note() { echo "  MISS: $1"; fail=1; }

# Header, exactly.
grep -qE '^SIG +LENGTH +REV +OEM ID +OEM TABLE ID +CHECKSUM$' "$LOG" \
    || note "inventory header line"

# A normal SDT row: 4-char signature, numeric length, numeric revision,
# and a checksum verdict of exactly OK or BAD. Anchored end to end so a
# row with a stray extra column fails.
grep -qE '^FACP +[0-9]+ +[0-9]+ +\S+ +\S+ +(OK|BAD)$' "$LOG" \
    || note "FACP row with a real revision and checksum verdict"
grep -qE '^DSDT +[0-9]+ +[0-9]+ +\S+ +\S+ +(OK|BAD)$' "$LOG" \
    || note "DSDT row (reached via the FADT, not the XSDT)"
grep -qE '^APIC +[0-9]+ +[0-9]+ +\S+ +\S+ +(OK|BAD)$' "$LOG" \
    || note "APIC row"

# THE FACS TRAP. Signature and length are real; every other column must
# be a dash, because FACS has no revision, no checksum and no OEM
# strings to report.
grep -qE '^FACS +[0-9]+ +- +- +- +-$' "$LOG" \
    || note "FACS row renders sig+length with '-' elsewhere"

# ... and it must NOT be rendered as a normal SDT. This is the negative
# half: without it, a tool that prints garbage columns for FACS could
# still satisfy everything above by printing BOTH kinds of row.
grep -qE '^FACS +[0-9]+ +[0-9]+ +\S+ +\S+ +(OK|BAD)$' "$LOG" \
    && { echo "  HIT: FACS rendered through AxlAcpiHeader (garbage columns)"; fail=1; }

# Every checksum the firmware published should be valid; a BAD here is
# either broken firmware or a broken checksum routine, and both are
# worth failing on.
grep -qE ' BAD$' "$LOG" && { echo "  HIT: a table failed its checksum"; fail=1; }

# --- typed decode -----------------------------------------------------
# MCFG through its typed reader: the ECAM window OVMF publishes for
# segment 0. Anchored so a "decoded but empty" result fails.
MLOG="$WORK/mcfg.log"
timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 \
    "$EFI" MCFG 2>&1 | tee "$MLOG" >/dev/null
grep -qE '^MCFG: [1-9][0-9]* ECAM window\(s\)$' "$MLOG" \
    || note "MCFG decodes to at least one ECAM window"
grep -qE '^  segment [0-9]+ +buses [0-9a-f]{2}\.\.[0-9a-f]{2} +base 0x[0-9a-f]+$' "$MLOG" \
    || note "MCFG window line with segment, bus range and base"

# --- automatic hexdump ------------------------------------------------
# WAET has no typed reader. It must dump its bytes with no flag asked
# for, and the dump must start at offset 0 with the signature.
WLOG="$WORK/waet.log"
timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 \
    "$EFI" WAET 2>&1 | tee "$WLOG" >/dev/null
grep -qE '^WAET: no typed reader, raw contents follow$' "$WLOG" \
    || note "WAET falls back to a hexdump automatically"
# The dump itself must start at offset 0 with the signature bytes
# ("WAET" = 57 41 45 54, printed in 2-byte groups).
grep -qE '^ +0000: 5741 4554' "$WLOG" \
    || note "hexdump starts at offset 0 with the WAET signature bytes"

# A table that does not exist must be an error, not an empty success.
XLOG="$WORK/none.log"
timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 \
    "$EFI" ZZZZ 2>&1 | tee "$XLOG" >/dev/null
grep -qE "no table with signature 'ZZZZ'" "$XLOG" \
    || note "an absent signature is reported, not silently empty"

# --- JSON -------------------------------------------------------------
# Exact bytes are not the same as valid output, so parse it with a real
# parser rather than grepping shapes.
JLOG="$WORK/json.log"
timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 \
    "$EFI" -j 2>&1 | tee "$JLOG" >/dev/null
# AxlJsonWriter emits the document on ONE line, so take that line
# rather than a /^{/,/^}/ range -- the range never sees a closing brace
# at column 0 and would swallow the trailing log lines.
grep -m1 '^{' "$JLOG" > "$WORK/out.json"
python3 - "$WORK/out.json" <<'PYJSON' || note "-j emits parseable JSON with FACS nulls and a matching count"
import json, sys
d = json.load(open(sys.argv[1]))
t = d["tables"]
assert len(t) == d["count"], f'count {d["count"]} != {len(t)} rows'
facs = [r for r in t if r["signature"] == "FACS"]
assert facs, "no FACS row"
# FACS has no revision/OEM/checksum to report -- null, not 0 or "".
for k in ("revision", "oem_id", "oem_table_id", "checksum"):
    assert facs[0][k] is None, f"FACS {k} should be null, got {facs[0][k]!r}"
# ... while a normal SDT reports all of them.
dsdt = [r for r in t if r["signature"] == "DSDT"]
assert dsdt and dsdt[0]["checksum"] == "OK", "DSDT row missing or not OK"
assert isinstance(dsdt[0]["revision"], int), "DSDT revision not an integer"
PYJSON

# --- namespace view ---------------------------------------------------
# The AML walker, surfaced. QEMU's DSDT declares PCI slot devices with
# real _ADR values and no _SUN, so this pins both a value and an absence.
NLOG="$WORK/ns.log"
timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 \
    "$EFI" -n 2>&1 | tee "$NLOG" >/dev/null
grep -qE '^PATH +_ADR +_SUN +_UID +_SEG +_BBN +_PLD$' "$NLOG" \
    || note "namespace header line"
grep -qE '^\\_SB_\.PCI0\.S08_ +10000 +- +- +- +- +-$' "$NLOG" \
    || note "namespace row with a decoded _ADR and dashes for what is absent"
grep -qE '^[0-9]+ device\(s\) across [0-9]+ table\(s\)$' "$NLOG" \
    || note "namespace summary line, and the walk was NOT incomplete"

# --- slot correlation -------------------------------------------------
# A root port with a known Physical Slot Number, so the silicon side of
# the join has something real to anchor on.
RP='pcie-root-port,id=rp0,bus=pcie.0,chassis=1,slot=23'
SLOG="$WORK/slots.log"
timeout 120s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 90 \
    --qemu-arg -device --qemu-arg "$RP" \
    "$EFI" -s 2>&1 | tee "$SLOG" >/dev/null
grep -qE '^ADDRESS +SLOT# +PRESENT +JOIN +DESIGNATION +NOTES$' "$SLOG" \
    || note "slots header line"
grep -qE '^0000:00:[0-9a-f]{2}\.[0-9a-f] +23 +' "$SLOG" \
    || note "the slot=23 root port appears with its Physical Slot Number"
grep -qE '^[0-9]+ row\(s\), [0-9]+ disagreement\(s\)$' "$SLOG" \
    || note "slots summary line"

# --- the correlation against REAL firmware ----------------------------
# QEMU publishes no SMBIOS Type 9 at all, so the join and the
# disagreement marking cannot be exercised by QEMU alone. Injecting a
# captured server's SMBIOS gives 34 real System Slot records with real
# designations and slot IDs. The capture is local-only by policy, so
# this SKIPS on CI and on a fresh clone.
SMBIOS="$PROJECT_DIR/test/fixtures/real-hw/server/smbios.bin"
if [[ -f "$SMBIOS" ]]; then
    RLOG="$WORK/real.log"
    timeout 180s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 120 \
        --qemu-arg -smbios --qemu-arg "file=$SMBIOS" \
        --qemu-arg -device --qemu-arg "$RP" \
        "$EFI" -s 2>&1 | tee "$RLOG" >/dev/null

    # The SLOT-NUMBER join path, which is the one 74% of that machine's
    # Type 9 records depend on: "PCIe Slot 23" (slot_id 23) has no bus
    # address, and reaches the slot=23 root port by number alone.
    grep -qE '^0000:00:[0-9a-f]{2}\.[0-9a-f] +23 +\S+ +slot +PCIe Slot 23' "$RLOG" \
        || note "Type 9 'PCIe Slot 23' joins the root port BY SLOT NUMBER"

    # Disagreement marking: those Type 9 records name addresses that
    # exist on the captured machine and not in this guest.
    grep -qE '\[SMBIOS names an address with no device\]' "$RLOG" \
        || note "an SMBIOS address with nothing behind it is marked"

    # SENTINELS ARE NOT ADDRESSES. 25 of that machine's 34 Type 9
    # records publish 0xFFFF/0xFF/0xFF for segment/bus/dev-func, meaning
    # "never filled in". Rendering those as ffff:ff:1f.7 would invent a
    # device and then report it as missing -- a fabricated disagreement,
    # which is worse than a missed one. Added after a sabotage of the
    # sentinel guard went UNDETECTED.
    grep -qE '^ffff:|^[0-9a-f]{4}:ff:' "$RLOG" \
        && { echo "  HIT: an SMBIOS sentinel was rendered as a PCI address"; fail=1; }

    # More than one source contributed, so this really is a join.
    grep -qE '^[0-9]+ row\(s\), [1-9][0-9]* disagreement\(s\)$' "$RLOG" \
        || note "the real-SMBIOS run reports at least one disagreement"
else
    echo "  SKIP: real-hardware SMBIOS capture not present (local-only by policy)"
fi

# --- the address-join disagreements -----------------------------------
# The client capture publishes Type 9 "PCIe SLOT X1" AT 0000:00:1c.0
# with slot ID 1 and Current Usage "In Use". Putting a root port at
# that exact address with a different Physical Slot Number makes both
# sources land on ONE row via the ADDRESS join, which is what the
# slot-number and presence disagreements need in order to fire at all.
# On the physical machine this is 1 vs 2 with the slot empty; here it
# is 1 vs 99, the same shape.
CSMBIOS="$PROJECT_DIR/test/fixtures/real-hw/client/smbios.bin"
if [[ -f "$CSMBIOS" ]]; then
    DLOG="$WORK/disagree.log"
    timeout 180s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 120 \
        --qemu-arg -smbios --qemu-arg "file=$CSMBIOS" \
        --qemu-arg -device \
        --qemu-arg "pcie-root-port,id=rp1,bus=pcie.0,addr=1c.0,chassis=1,slot=99" \
        "$EFI" -s 2>&1 | tee "$DLOG" >/dev/null

    grep -qE '^0000:00:1c\.0 +99 +.*addr +PCIe SLOT X1 .*\[slot# SMBIOS 1 vs silicon 99\]' "$DLOG" \
        || note "slot# disagreement fires when SMBIOS and silicon share an address"
    grep -qE '\[SMBIOS says In Use, Presence Detect says empty\]' "$DLOG" \
        || note "In-Use vs Presence Detect disagreement fires"
else
    echo "  SKIP: client SMBIOS capture not present (local-only by policy)"
fi

# Hygiene.
grep -qiE "leak report" "$LOG" && { echo "  HIT: memory leak reported"; fail=1; }
grep -qiE "EXCEPTION|invalid opcode" "$LOG" && { echo "  HIT: CPU exception"; fail=1; }

if (( fail )); then
    echo "FAIL: lsacpi inventory checks"
    echo "--- serial log ---"; cat "$LOG"
    exit 1
fi
echo "lsacpi inventory test: OK"
exit 0
