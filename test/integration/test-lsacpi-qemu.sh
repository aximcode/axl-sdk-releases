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

# Header, EXACT BYTES. The default row carries every header field now --
# the same set Linux prints at boot -- so a consumer can read it without
# a flag and diff it against dmesg. Exact, not ' +': the last time this
# was a loose pattern, a stray space in the header went unnoticed and
# every column heading sat one place right of its own data.
grep -qxF 'SIG  ADDRESS        LENGTH REV OEM ID OEM TBL  OEM REV  CREATOR CRTR REV CHK' "$LOG" \
    || note "inventory header line, byte for byte"

# A normal SDT row: signature, physical address, hex length, revision,
# OEM strings, OEM revision, creator + creator revision, checksum verdict.
# Anchored end to end so a row with a stray extra column fails.
SDT_ROW='0x[0-9a-f]{12} [0-9a-f]{6} +[0-9]+ +\S+ +\S+ +[0-9a-f]{8} +\S+ +[0-9a-f]{8} +(OK|BAD)$'
grep -qE "^FACP +$SDT_ROW" "$LOG" \
    || note "FACP row with address, revisions, creator and checksum verdict"
grep -qE "^DSDT +$SDT_ROW" "$LOG" \
    || note "DSDT row (reached via the FADT, not the XSDT)"
grep -qE "^APIC +$SDT_ROW" "$LOG" \
    || note "APIC row"

# THE FACS TRAP. Signature, address and length are real; every other
# column must be a dash, because FACS has no revision, no checksum and no
# OEM strings to report. Its ADDRESS is real -- that is the pointer the
# tool already holds, not a header field FACS lacks.
grep -qE '^FACS +0x[0-9a-f]{12} +[0-9a-f]{6} +- +- +- +- +- +- +-$' "$LOG" \
    || note "FACS row renders sig+address+length with '-' elsewhere"

# ... and it must NOT be rendered as a normal SDT. This is the negative
# half: without it, a tool that prints garbage columns for FACS could
# still satisfy everything above by printing BOTH kinds of row.
grep -qE "^FACS +$SDT_ROW" "$LOG" \
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
assert dsdt[0]["creator_id"] == "BXPC", f'DSDT creator_id {dsdt[0]["creator_id"]!r}'
for k in ("oem_revision", "creator_revision"):
    assert isinstance(dsdt[0][k], int), f"DSDT {k} not an integer"
# EVERY row carries an address, FACS included: it is the pointer the tool
# already holds, not a header field FACS lacks -- and the text -v row
# prints one for FACS, so the JSON omitting it is a gap, not a policy.
for r in t:
    assert isinstance(r.get("address"), int), \
        f'{r["signature"]}: address missing or not an integer'
# Absent is null, uniformly. QEMU's BGRT zeroes its creator field; that
# must read the way FACS's absent fields read, not as an empty string a
# consumer has to special-case.
for r in t:
    assert r["creator_id"] != "", \
        f'{r["signature"]}: creator_id is "" -- absent should be null'
PYJSON

# A root port with a known Physical Slot Number, so the silicon side of
# the join has something real to anchor on. Shared by the JSON-modes
# boot below and the text slot section further down.
RP='pcie-root-port,id=rp0,bus=pcie.0,chassis=1,slot=23'

# --- JSON in every mode -----------------------------------------------
# --json used to be honoured by the inventory alone; the other three
# render modes parsed the flag and dropped it, so `lsacpi MCFG -j`
# printed text and exited 0. All four modes are batched into ONE boot
# because each of these is a full QEMU cycle.
cat > "$WORK/json-modes.nsh" <<'NSH'
echo ===ONEV===
lsacpi.efi FACP
lsacpi.efi MCFG
lsacpi.efi MCFG -v
lsacpi.efi -v
echo ===ONE===
lsacpi.efi MCFG -j
echo ===BYTES===
lsacpi.efi WAET -j
echo ===NS===
lsacpi.efi -n -j
echo ===SLOTS===
lsacpi.efi -s -j
echo ===END===
reset -s
NSH
JMLOG="$WORK/json-modes.log"
timeout 180s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 150 \
    --qemu-arg -device --qemu-arg "$RP" \
    --nsh "$WORK/json-modes.nsh" "$EFI" 2>&1 | tee "$JMLOG" >/dev/null

# One JSON document per section. Each is emitted on a single line, so
# take the first line starting with '{' after the marker.
section_json() {
    awk -v start="===$1===" -v stop="===$2===" '
        $0 ~ start { on = 1; next }
        $0 ~ stop  { on = 0 }
        on && /^\{/ { print; exit }
    ' "$JMLOG"
}
# Asking for ONE table identifies it with the SAME row the inventory
# renders -- one owner for the columns, so the two cannot drift.
grep -qE "^FACP +$SDT_ROW" "$JMLOG" \
    || note "single-table view identifies the table with an inventory row"
# ... and then decodes it. MCFG has a typed reader; the row is not enough.
grep -qE '^MCFG: [1-9][0-9]* ECAM window\(s\)$' "$JMLOG" \
    || note "single-table view still runs the typed decode"

# -v is ONLY about raw bytes now: it adds a hexdump to a table that
# already decoded. Without it, MCFG shows the decode alone.
grep -qE '^ +0000: 4d43 4647' "$JMLOG" \
    || note "-v adds the raw hexdump to a table that has a typed decode"
awk '/^MCFG: [0-9]+ ECAM/ { seen++ } END { exit !(seen >= 2) }' "$JMLOG" \
    || note "MCFG decoded both with and without -v"

# -v on the INVENTORY has nothing to add and must say so rather than be
# silently dropped -- the whole defect class this tool spent a day on.
grep -qE 'lsacpi: -v applies to a single table' "$JMLOG" \
    || note "-v without a signature is refused, not ignored"

section_json ONE   BYTES > "$WORK/one.json"
section_json BYTES NS    > "$WORK/bytes.json"
section_json NS    SLOTS > "$WORK/ns.json"
section_json SLOTS END   > "$WORK/slots.json"

python3 - "$WORK/one.json" "$WORK/bytes.json" "$WORK/ns.json" "$WORK/slots.json" \
    <<'PYJSON' || note "--json is honoured by every render mode"
import json, sys
one_doc, byts_doc, ns, slots = (json.load(open(p)) for p in sys.argv[1:5])

# ALWAYS the inventory's envelope, even for one table: a consumer decodes
# one shape whichever view produced the document.
assert one_doc["count"] == len(one_doc["tables"]) == 1, one_doc
one = one_doc["tables"][0]
byts = byts_doc["tables"][0]
for k in ("signature", "length", "revision", "oem_id", "oem_table_id",
          "oem_revision", "creator_id", "creator_revision", "address",
          "checksum"):
    assert k in one, f"single-table JSON missing {k}"
assert one["signature"] == "MCFG", one["signature"]
# ... plus the typed decode, which is the reason to ask for one table.
w = one["mcfg"]["windows"]
assert w and isinstance(w[0]["segment"], int), "MCFG windows not decoded"
assert isinstance(w[0]["base"], int) and w[0]["base"] > 0, "no ECAM base"

# A table with no typed reader hands over its bytes rather than a shrug.
assert byts["signature"] == "WAET", byts["signature"]
assert byts["bytes"].lower().startswith("57414554"), \
    f'WAET bytes do not start with the signature: {byts["bytes"][:16]!r}'
assert len(byts["bytes"]) == 2 * byts["length"], "bytes length != table length"

# The namespace walk. The four AxlAmlValue kinds must SURVIVE into JSON:
# "declared as a Method but unreadable" is not the same fact as "absent",
# and a bare null would conflate them.
assert ns["tables"] >= 1 and ns["incomplete"] is False
assert ns["count"] == len(ns["devices"])
kinds = set()
for d in ns["devices"]:
    for k in ("_ADR", "_SUN", "_UID", "_SEG", "_BBN", "_PLD"):
        assert set(d[k]) == {"kind", "value"}, f'{d["path"]}.{k} shape {d[k]}'
        kinds.add(d[k]["kind"])
assert kinds <= {"static", "method", "non-integer", "absent"}, kinds
assert {"static", "absent"} <= kinds, f"expected both static and absent, got {kinds}"
# QEMU's PCI slot devices carry a real _ADR and no _SUN -- the same pair
# the text view pins, read back through the schema.
s08 = [d for d in ns["devices"] if d["path"].endswith("S08_")]
assert s08, "no \\_SB_.PCI0.S08_ device"
assert s08[0]["_ADR"] == {"kind": "static", "value": 0x10000}, s08[0]["_ADR"]
assert s08[0]["_SUN"]["kind"] == "absent", s08[0]["_SUN"]

# The slot correlation. Its whole point is the disagreements, so those
# are machine-readable codes rather than prose a script has to grep.
assert slots["count"] == len(slots["slots"])
# A real row, not just an envelope: without the root port this array is
# empty and every per-row check below would loop zero times and pass.
assert slots["count"] > 0, "no slot rows -- the per-row checks would be vacuous"
rp = [r for r in slots["slots"] if r["slot_number"] == 23]
assert rp, f'the slot=23 root port is missing: {[r["slot_number"] for r in slots["slots"]]}'
assert rp[0]["address"] and rp[0]["address"].startswith("0000:00:"), rp[0]["address"]
assert rp[0]["present"] in (True, False), "silicon row must have a real presence bit"
assert slots["disagreements"] == sum(len(r["disagreements"]) for r in slots["slots"]), \
    "summary disagreement count disagrees with the per-row arrays"
for r in slots["slots"]:
    assert set(r) >= {"address", "slot_number", "present", "join",
                      "designation", "silicon_only", "disagreements"}, set(r)
    assert r["present"] in (True, False, None), r["present"]
    assert isinstance(r["disagreements"], list)
PYJSON

# --- duplicate signatures ---------------------------------------------
# THE CASE THAT MOTIVATED THIS. A 2-socket Grace server publishes 19
# SSDTs, and 10 of them fall into groups that are IDENTICAL on every ACPI
# header field -- one 4-way tie and three 2-way ties on length, revision,
# OEM ID, OEM table ID, OEM revision, creator and creator revision. No
# header field can name one of them; only the physical address can.
#
# `lsacpi SSDT` used to decode the FIRST match and say nothing about the
# rest, which is a silent wrong answer on exactly that machine. Stock
# QEMU publishes ZERO SSDTs, so the tables are injected: two blobs whose
# headers are byte-identical, differing only in body content and address.
python3 - "$WORK" <<'PYSSDT'
import struct, sys, pathlib
out = pathlib.Path(sys.argv[1])
def ssdt(body: bytes) -> bytes:
    h = bytearray(b"SSDT")
    h += struct.pack("<I", 36 + len(body))
    h += bytes([2, 0])                  # revision, checksum placeholder
    h += b"AXLTST" + b"TWINSSDT"        # OEM ID (6) + OEM table ID (8)
    h += struct.pack("<I", 1)           # OEM revision
    h += b"AXL " + struct.pack("<I", 0x20260901)
    tbl = bytes(h) + body
    tbl = tbl[:9] + bytes([(-sum(tbl)) & 0xFF]) + tbl[10:]
    assert sum(tbl) & 0xFF == 0
    return tbl
for i, name in enumerate(("ssdt-a.aml", "ssdt-b.aml")):
    (out / name).write_bytes(ssdt(bytes([0xA0 + i]) * 8))
PYSSDT

TWLOG="$WORK/twin.log"
cat > "$WORK/twin.nsh" <<'NSH'
lsacpi.efi
echo ===INV===
lsacpi.efi SSDT
echo ===JSON===
lsacpi.efi SSDT -j
NSH
timeout 180s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 150 \
    --qemu-arg -acpitable --qemu-arg "file=$WORK/ssdt-a.aml" \
    --qemu-arg -acpitable --qemu-arg "file=$WORK/ssdt-b.aml" \
    --nsh "$WORK/twin.nsh" "$EFI" 2>&1 | tee "$TWLOG" >/dev/null

# Both are published and are indistinguishable in the inventory except
# by address -- if this fails, the fixture is wrong, not the tool.
INV_SSDT=$(awk '/===INV===/{exit} /^SSDT +0x[0-9a-f]+ +00002c /{n++} END{print n+0}' "$TWLOG")
[[ "$INV_SSDT" -eq 2 ]] \
    || note "the two injected SSDTs both appear in the inventory (saw $INV_SSDT)"
[[ $(grep -oE '^SSDT +0x[0-9a-f]{12}' "$TWLOG" | awk '{print $2}' | sort -u | wc -l) -eq 2 ]] \
    || note "the two SSDTs have DISTINCT addresses (the only unique key)"

# `lsacpi SSDT` must decode BOTH, not silently pick one.
[[ $(awk '/===JSON===/{exit} /^ +0000: 5353 4454/{n++} END{print n+0}' "$TWLOG") -eq 2 ]] \
    || note "lsacpi SSDT decodes EVERY match, not just the first"
grep -qE '^2 table\(s\) with signature SSDT$' "$TWLOG" \
    || note "the multi-match view says how many tables it found"

# --at picks exactly one, by the only key that can name it. Both addresses
# are exercised and their dumps must DIFFER: the twins are identical in every
# header field and differ only in body bytes, so a --at that quietly returned
# the first match every time would pass any single-address check.
SSDT_LO=$(grep -oE '^SSDT +0x[0-9a-f]{12}' "$TWLOG" | awk '{print $2}' | sort -u | head -1)
SSDT_HI=$(grep -oE '^SSDT +0x[0-9a-f]{12}' "$TWLOG" | awk '{print $2}' | sort -u | tail -1)
ATLOG="$WORK/at.log"
cat > "$WORK/at.nsh" <<NSH
lsacpi.efi --at $SSDT_LO
echo ===HI===
lsacpi.efi --at $SSDT_HI
echo ===BAD===
lsacpi.efi --at 0x1
NSH
timeout 180s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 150 \
    --qemu-arg -acpitable --qemu-arg "file=$WORK/ssdt-a.aml" \
    --qemu-arg -acpitable --qemu-arg "file=$WORK/ssdt-b.aml" \
    --nsh "$WORK/at.nsh" "$EFI" 2>&1 | tee "$ATLOG" >/dev/null

[[ $(awk '/===HI===/{exit} /^ +0000: 5353 4454/{n++} END{print n+0}' "$ATLOG") -eq 1 ]] \
    || note "--at decodes exactly ONE table"
grep -qE "^SSDT +$SSDT_LO " "$ATLOG" || note "--at selects the table asked for (low)"
grep -qE "^SSDT +$SSDT_HI " "$ATLOG" || note "--at selects the table asked for (high)"
# The body byte is the ONLY thing that differs between the twins.
LO_BODY=$(awk '/===HI===/{exit} /^ +0020: /{print $4}' "$ATLOG" | head -1)
HI_BODY=$(awk '/===HI===/{f=1} /===BAD===/{exit} f && /^ +0020: /{print $4}' "$ATLOG" | head -1)
[[ -n "$LO_BODY" && -n "$HI_BODY" && "$LO_BODY" != "$HI_BODY" ]] \
    || note "--at reaches BOTH twins, not the first match twice (lo=$LO_BODY hi=$HI_BODY)"
grep -qE 'no ACPI table at 0x1' "$ATLOG" \
    || note "--at on an address with no table is an error, not empty success"

# JSON for a multi-match signature is an ARRAY, like the inventory's.
sed -n '/===JSON===/,$p' "$TWLOG" | grep -m1 '^{' > "$WORK/twin.json"
python3 - "$WORK/twin.json" <<'PYTWIN' || note "multi-match --json is an array of full table objects"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["count"] == 2 and len(d["tables"]) == 2, d
addrs = {r["address"] for r in d["tables"]}
assert len(addrs) == 2, f"both rows share an address: {addrs}"
for r in d["tables"]:
    assert r["signature"] == "SSDT" and r["oem_table_id"] == "TWINSSDT"
    assert r["bytes"].lower().startswith("53534454"), r["bytes"][:16]
PYTWIN

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

    # The same two disagreements, read back as machine-readable codes.
    # Text and JSON share one owner (slot_disagreements()), so this is
    # what proves the codes track the prose rather than drifting from it.
    DJLOG="$WORK/disagree-json.log"
    timeout 180s "$PROJECT_DIR/scripts/run-qemu.sh" --timeout 120 \
        --qemu-arg -smbios --qemu-arg "file=$CSMBIOS" \
        --qemu-arg -device \
        --qemu-arg "pcie-root-port,id=rp1,bus=pcie.0,addr=1c.0,chassis=1,slot=99" \
        "$EFI" -s -j 2>&1 | tee "$DJLOG" >/dev/null
    grep -m1 '^{' "$DJLOG" > "$WORK/disagree.json"
    python3 - "$WORK/disagree.json" <<'PYDIS' || note "slot disagreements as JSON codes"
import json, sys
d = json.load(open(sys.argv[1]))
row = [r for r in d["slots"] if r["address"] == "0000:00:1c.0"]
assert row, f'no 0000:00:1c.0 row: {[r["address"] for r in d["slots"]]}'
codes = set(row[0]["disagreements"])
assert "slot_number_mismatch" in codes, codes
assert "in_use_but_no_presence_detect" in codes, codes
assert row[0]["slot_number"] == 99 and row[0]["join"] == "addr", row[0]
# The summary counts exactly what the rows list -- one owner, one number.
assert d["disagreements"] == sum(len(r["disagreements"]) for r in d["slots"])
PYDIS
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
