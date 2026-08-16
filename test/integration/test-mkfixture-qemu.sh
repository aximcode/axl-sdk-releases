#!/bin/bash
# test-meta: arch=x64 needs= est=20 local-only=0
# test-mkfixture-qemu.sh -- end-to-end test for tools/mkfixture.efi.
#
# Captures a fixture from a running OVMF guest into a virtiofs-mounted
# host directory, validates the on-disk shape (smbios.bin starts with
# a parseable SMBIOS structure, acpi/ has at least the FACP/HPET tables
# every QEMU-OVMF guest publishes, manifest.json is well-formed JSON),
# then replays the captured fixture via axl-emulate against a second
# QEMU instance running with custom SMBIOS strings, and verifies the
# captured custom identity round-trips through to sysinfo.efi.
#
# Auxiliary; opt out of the test-axl.sh ratchet (--mount + sequential
# QEMU pipeline; not amenable to the unit-test pass-count baseline).
#
# x86-only — uses --mount virtiofs which depends on OVMF VirtioFsDxe;
# aa64 OVMF builds typically lack it. Mirrors test-spd-qemu.sh's policy.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
AXL_EMULATE="$PROJECT_DIR/scripts/axl-emulate"
MKFIXTURE="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)/tools/mkfixture.efi"
SYSINFO="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)/tools/sysinfo.efi"

# Auxiliary; don't clobber test-axl.sh's pass-count baseline.
export TEST_SKIP_RATCHET=1

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL + 1)); }

# Count files matching a glob, safe with zero matches under
# `set -euo pipefail` (the `ls glob | wc -l` idiom aborts the script when
# the glob has no match — ls exits non-zero, pipefail propagates it).
count_glob() {
    local n=0 f
    for f in $1; do [[ -e "$f" ]] && n=$((n + 1)); done
    echo "$n"
}

# Build mkfixture / sysinfo if missing (runs in <1s on a warm tree).
if [[ ! -x "$MKFIXTURE" ]] || [[ ! -x "$SYSINFO" ]]; then
    echo "Building tools..."
    make -C "$PROJECT_DIR" ARCH=x64 tools 2>&1 | tail -3
fi
[[ -x "$MKFIXTURE" ]] || { echo "FAIL: mkfixture.efi not found at $MKFIXTURE"; exit 1; }
[[ -x "$SYSINFO" ]]   || { echo "FAIL: sysinfo.efi not found at $SYSINFO"; exit 1; }

# ----------------------------------------------------------------------
# Capture phase: run mkfixture inside QEMU with a known-distinct SMBIOS
# (AximCode/TestRig/1.0/ABC123 — these strings DO NOT appear in OVMF's
# auto-built defaults, so seeing them in the replay proves the fixture
# was actually consumed, not just QEMU defaults coming through).
# ----------------------------------------------------------------------
FIX_DIR="$(mktemp -d)"
NVME_IMG="$(mktemp)"
trap 'rm -rf "$FIX_DIR" "$NVME_IMG"' EXIT
# 8 MiB backing store for the emulated NVMe controller (16384 512-byte
# LBAs) — gives nvme.json a concrete namespace size to assert.
truncate -s 8M "$NVME_IMG"

echo "=== Capture: mkfixture under custom-SMBIOS QEMU ==="
# --bridges adds a deterministic USB topology (qemu-xhci + usb-mouse +
# usb-hub + usb-tablet) and a PCIe root-port bridge tree; --net adds a
# virtio-net NIC (SNP); --gpu wires a real GOP framebuffer (the headless
# unit harness has none); the explicit -device nvme adds an NVMe
# controller. So the pci.json / usb.json / net.json / video.json /
# nvme.json device-manifest assertions below have concrete HID + hub
# interfaces, a NIC, a GOP mode list, an NVMe controller+namespace, and a
# non-trivial bus to capture.
timeout 60s "$RUN_QEMU" --mount "$FIX_DIR" --bridges --net --gpu \
    --qemu-arg "-drive" --qemu-arg "file=$NVME_IMG,if=none,id=nvm0,format=raw" \
    --qemu-arg "-device" --qemu-arg "nvme,drive=nvm0,serial=AXLNVME1" \
    --qemu-arg "-smbios" --qemu-arg "type=1,manufacturer=AximCode,product=TestRig,version=1.0,serial=ABC123" \
    "$MKFIXTURE" 'FS1:\fix' > /dev/null 2>&1 || true

# ----------------------------------------------------------------------
# Validate captured layout
# ----------------------------------------------------------------------
[[ -d "$FIX_DIR/fix" ]] && pass "fixture root directory created" \
    || fail "fixture root not created" "expected $FIX_DIR/fix"

[[ -f "$FIX_DIR/fix/smbios.bin" ]] && pass "smbios.bin written" \
    || fail "smbios.bin missing"

[[ -d "$FIX_DIR/fix/acpi" ]] && pass "acpi/ directory created" \
    || fail "acpi/ directory missing"

# smbios.bin first byte should be a valid SMBIOS Type (0-127). For
# OVMF-Q35 with custom Type 1 injection, Type 0 (BIOS Info) typically
# leads. We assert it's in the valid range, NOT the dmidecode '_'
# (0x5F) prefix — that would mean we wrote the EP, which QEMU rejects.
if [[ -f "$FIX_DIR/fix/smbios.bin" ]]; then
    first_byte=$(head -c 1 "$FIX_DIR/fix/smbios.bin" | od -An -tu1 -N1 | tr -d ' ')
    if [[ "$first_byte" -le 127 ]]; then
        pass "smbios.bin first byte ($first_byte) is a valid SMBIOS Type"
    else
        fail "smbios.bin first byte is $first_byte (likely dmidecode-EP format)"
    fi
fi

# Spec-required tables must be present. FACP (FADT) is the one ACPI
# table every spec-compliant ACPI platform must publish — it points
# at FACS/DSDT and defines the PM interfaces. Asserting on FACP
# specifically (vs a loose count) catches a mkfixture regression
# where (e.g.) the ACPI walker stops after the first table.
if [[ -f "$FIX_DIR/fix/acpi/facp.dat" ]]; then
    pass "acpi/facp.dat present (spec-required FADT captured)"
else
    fail "acpi/facp.dat missing — ACPI walk likely truncated" \
         "(present tables: $(ls "$FIX_DIR/fix/acpi/" 2>/dev/null))"
fi
acpi_count=$(count_glob "$FIX_DIR/fix/acpi/*.dat")
pass "acpi/ has $acpi_count tables (informational)"

# manifest.json must be well-formed JSON containing the captured strings.
if [[ -f "$FIX_DIR/fix/manifest.json" ]]; then
    if python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$FIX_DIR/fix/manifest.json" 2>/dev/null; then
        pass "manifest.json is well-formed JSON"
    else
        fail "manifest.json is not valid JSON"
    fi
    if grep -q '"vendor": "AximCode"' "$FIX_DIR/fix/manifest.json"; then
        pass "manifest.json captures custom vendor (AximCode)"
    else
        fail "manifest.json missing custom vendor" \
             "$(cat "$FIX_DIR/fix/manifest.json")"
    fi
    if grep -q '"model": "TestRig"' "$FIX_DIR/fix/manifest.json"; then
        pass "manifest.json captures custom model (TestRig)"
    else
        fail "manifest.json missing custom model"
    fi
fi

# HF2.2: cpu.json must be present and well-formed JSON. On x86 the
# arch is "x86_64" and the vendor field comes from CPUID leaf 0
# (GenuineIntel/AuthenticAMD/etc.); on aa64 the arch is "aarch64"
# and the doc has midr_el1 instead. Test the x86 case here since the
# integration test is x86-only (line 19 in this file).
if [[ -f "$FIX_DIR/fix/cpu.json" ]]; then
    if python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$FIX_DIR/fix/cpu.json" 2>/dev/null; then
        pass "cpu.json is well-formed JSON"
    else
        fail "cpu.json is not valid JSON"
    fi
    if grep -q '"arch": "x86_64"' "$FIX_DIR/fix/cpu.json"; then
        pass "cpu.json reports arch=x86_64"
    else
        fail "cpu.json missing or wrong arch" \
             "$(cat "$FIX_DIR/fix/cpu.json")"
    fi
    if grep -qE '"vendor": "(GenuineIntel|AuthenticAMD|TCGTCGTCGTCG)"' "$FIX_DIR/fix/cpu.json"; then
        pass "cpu.json captures plausible CPUID vendor string"
    else
        fail "cpu.json vendor doesn't match known x86 patterns" \
             "$(grep vendor "$FIX_DIR/fix/cpu.json")"
    fi
    if grep -qE '"family": [0-9]+' "$FIX_DIR/fix/cpu.json"; then
        pass "cpu.json reports family"
    else
        fail "cpu.json missing family"
    fi
else
    fail "cpu.json missing"
fi

# HF2.2: esrt.json. On QEMU/OVMF the ESRT config table is typically
# absent — accept either "skipped — no ESRT" path (manifest skipped
# by mkfixture) OR the file exists with valid JSON if the firmware
# does publish one. The point: mkfixture must not crash on absence.
if [[ -f "$FIX_DIR/fix/esrt.json" ]]; then
    if python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$FIX_DIR/fix/esrt.json" 2>/dev/null; then
        pass "esrt.json is well-formed JSON (firmware published ESRT)"
    else
        fail "esrt.json is not valid JSON"
    fi
else
    pass "esrt.json absent (firmware did not publish ESRT — expected on OVMF)"
fi

# HF2.3: pci.json — manifest of every responding PCI function (VID/DID,
# class, subsystem, BARs). On QEMU q35 the bus always carries at least
# the Intel 82G33/G35 host bridge at 0000:00:00.0 (vendor 0x8086), so
# we can assert a concrete device is present, not just a loose count.
if [[ -f "$FIX_DIR/fix/pci.json" ]]; then
    if python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$FIX_DIR/fix/pci.json" 2>/dev/null; then
        pass "pci.json is well-formed JSON"
    else
        fail "pci.json is not valid JSON" "$(cat "$FIX_DIR/fix/pci.json")"
    fi
    # Structural + content check: count matches the devices[] length,
    # every device carries the required keys, and the q35 host bridge
    # at 0000:00:00.0 is present with the Intel vendor ID.
    if python3 - "$FIX_DIR/fix/pci.json" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
devs = d["devices"]
assert d["count"] == len(devs), f"count {d['count']} != devices {len(devs)}"
assert len(devs) >= 1, "no PCI devices captured"
required = {"address", "vendor_id", "device_id", "class_code", "header_type", "bars"}
for dev in devs:
    missing = required - dev.keys()
    assert not missing, f"device {dev.get('address')} missing keys {missing}"
hb = [x for x in devs if x["address"] == "0000:00:00.0"]
assert hb, "q35 host bridge 0000:00:00.0 not captured"
assert hb[0]["vendor_id"] == "0x8086", f"host bridge vendor {hb[0]['vendor_id']} != 0x8086"
PYEOF
    then
        pass "pci.json captures q35 host bridge (0000:00:00.0, vendor 0x8086) with full keys"
    else
        fail "pci.json structure/content invalid" "$(cat "$FIX_DIR/fix/pci.json")"
    fi
else
    fail "pci.json missing"
fi

# HF2.3: usb.json — manifest of every EFI_USB_IO interface (topology
# depth, VID/PID, class triplet + decoded name, string descriptors) plus
# per-device raw config-descriptor blobs in usb/. With --bridges the
# guest has a usb-mouse + usb-tablet (HID, class 0x03) behind a usb-hub
# (class 0x09) on a qemu-xhci controller, so we can assert concrete
# HID + hub interfaces, not just a count.
if [[ -f "$FIX_DIR/fix/usb.json" ]]; then
    if python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$FIX_DIR/fix/usb.json" 2>/dev/null; then
        pass "usb.json is well-formed JSON"
    else
        fail "usb.json is not valid JSON" "$(cat "$FIX_DIR/fix/usb.json")"
    fi
    if python3 - "$FIX_DIR/fix/usb.json" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
ifaces = d["interfaces"]
assert d["count"] == len(ifaces), f"count {d['count']} != interfaces {len(ifaces)}"
assert len(ifaces) >= 1, "no USB interfaces captured"
required = {"bus", "address", "interface", "depth", "vendor_id",
            "product_id", "class", "subclass", "protocol"}
for i in ifaces:
    missing = required - i.keys()
    assert not missing, f"interface {i.get('bus')}-{i.get('address')} missing {missing}"
classes = {i["class"] for i in ifaces}
assert "0x03" in classes, f"no HID (0x03) interface captured; classes={classes}"
assert "0x09" in classes, f"no hub (0x09) interface captured; classes={classes}"
PYEOF
    then
        pass "usb.json captures HID (0x03) + hub (0x09) interfaces with full keys"
    else
        fail "usb.json structure/content invalid" "$(cat "$FIX_DIR/fix/usb.json")"
    fi
    # Per-device config-descriptor blobs land in usb/<bus>-<addr>.bin.
    blob_count=$(count_glob "$FIX_DIR/fix/usb/*.bin")
    if [[ "$blob_count" -ge 1 ]]; then
        pass "usb/ has $blob_count raw config-descriptor blob(s)"
    else
        fail "usb/ has no descriptor blobs (control-transfer capture failed?)"
    fi
else
    fail "usb.json missing"
fi

# HF2.3: net.json — per-NIC MAC + link state via EFI_SIMPLE_NETWORK.
# With --net the guest has a virtio-net NIC whose default QEMU MAC is in
# the 52:54:00 locally-administered prefix, so we can assert a concrete
# MAC, not just a count.
if [[ -f "$FIX_DIR/fix/net.json" ]]; then
    if python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$FIX_DIR/fix/net.json" 2>/dev/null; then
        pass "net.json is well-formed JSON"
    else
        fail "net.json is not valid JSON" "$(cat "$FIX_DIR/fix/net.json")"
    fi
    if python3 - "$FIX_DIR/fix/net.json" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
nics = d["nics"]
assert d["count"] == len(nics), f"count {d['count']} != nics {len(nics)}"
assert len(nics) >= 1, "no NICs captured"
required = {"index", "state", "if_type", "hw_address_size", "mac",
            "permanent_mac", "media_present"}
for n in nics:
    missing = required - n.keys()
    assert not missing, f"nic {n.get('index')} missing keys {missing}"
macs = [n["mac"] for n in nics]
assert any(m.startswith("52:54:00") for m in macs), \
    f"no virtio-net QEMU MAC (52:54:00:*) captured; macs={macs}"
PYEOF
    then
        pass "net.json captures virtio-net NIC (52:54:00 MAC) with full keys"
    else
        fail "net.json structure/content invalid" "$(cat "$FIX_DIR/fix/net.json")"
    fi
else
    fail "net.json missing"
fi

# HF2.3: video.json — GOP mode list, current mode, framebuffer, pixel
# format (+ per-display edid/*.bin when EFI_EDID_DISCOVERED is present).
# --gpu wires a real GOP, so available must be true with a non-empty
# mode list. EDID is best-effort: QEMU's std VGA may not publish
# EFI_EDID_DISCOVERED, so edid/ blobs are checked leniently.
if [[ -f "$FIX_DIR/fix/video.json" ]]; then
    if python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$FIX_DIR/fix/video.json" 2>/dev/null; then
        pass "video.json is well-formed JSON"
    else
        fail "video.json is not valid JSON" "$(cat "$FIX_DIR/fix/video.json")"
    fi
    if python3 - "$FIX_DIR/fix/video.json" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["available"] is True, "GOP not available under --gpu"
modes = d["modes"]
assert d["mode_count"] == len(modes), f"mode_count {d['mode_count']} != modes {len(modes)}"
assert len(modes) >= 1, "no GOP modes captured"
for m in modes:
    assert {"index", "width", "height", "stride"} <= m.keys(), f"mode missing keys: {m}"
assert isinstance(d["current_mode"], int), "current_mode not an int"
assert d["width"] >= 1 and d["height"] >= 1, f"bad geometry {d['width']}x{d['height']}"
assert "pixel_format" in d, "pixel_format missing"
PYEOF
    then
        pass "video.json captures GOP mode list + current geometry + pixel format"
    else
        fail "video.json structure/content invalid" "$(cat "$FIX_DIR/fix/video.json")"
    fi
    # EDID is best-effort (std VGA often omits EFI_EDID_DISCOVERED).
    edid_count=$(count_glob "$FIX_DIR/fix/edid/*.bin")
    if [[ "$edid_count" -ge 1 ]]; then
        pass "edid/ has $edid_count EDID blob(s) (display published EDID)"
    else
        pass "edid/ empty (no EFI_EDID_DISCOVERED — expected on QEMU std VGA)"
    fi
else
    fail "video.json missing"
fi

# HF2.3: nvme/<n>.json — per-controller Identify Controller + per-
# namespace Identify via EFI_NVM_EXPRESS_PASS_THRU. The emulated nvme
# device reports model "QEMU NVMe Ctrl", the serial we set (AXLNVME1),
# and one namespace over the 8 MiB / 512 B backing store (16384 blocks).
nvme_count=$(count_glob "$FIX_DIR/fix/nvme/*.json")
if [[ "$nvme_count" -ge 1 ]]; then
    pass "nvme/ has $nvme_count controller manifest(s)"
    NVME_JSON=$(ls "$FIX_DIR/fix/nvme/"*.json 2>/dev/null | head -1)
    if python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$NVME_JSON" 2>/dev/null; then
        pass "nvme controller json is well-formed"
    else
        fail "nvme controller json is not valid JSON" "$(cat "$NVME_JSON")"
    fi
    if python3 - "$NVME_JSON" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
required = {"vendor_id", "serial", "model", "firmware", "namespace_count",
            "namespaces"}
missing = required - d.keys()
assert not missing, f"controller missing keys {missing}"
assert d["serial"] == "AXLNVME1", f"serial {d['serial']!r} != AXLNVME1"
assert "QEMU" in d["model"], f"model {d['model']!r} lacks QEMU"
ns = d["namespaces"]
assert d["namespace_count"] == len(ns), \
    f"namespace_count {d['namespace_count']} != {len(ns)}"
assert len(ns) >= 1, "no namespaces captured"
for n in ns:
    assert {"nsid", "size_blocks", "lba_size"} <= n.keys(), f"ns missing keys: {n}"
assert ns[0]["lba_size"] == 512, f"lba_size {ns[0]['lba_size']} != 512"
assert ns[0]["size_blocks"] == 16384, f"size_blocks {ns[0]['size_blocks']} != 16384"
PYEOF
    then
        pass "nvme json captures QEMU controller (serial AXLNVME1) + 8 MiB namespace"
    else
        fail "nvme json structure/content invalid" "$(cat "$NVME_JSON")"
    fi
else
    fail "nvme/ has no controller manifests"
fi

# ----------------------------------------------------------------------
# Replay phase: feed the captured fixture into a fresh Q35 OVMF guest
# (NO custom SMBIOS this time) and verify the captured custom identity
# comes through. If only OVMF defaults appeared, mkfixture wrote
# bytes QEMU couldn't consume.
# ----------------------------------------------------------------------
echo "=== Replay: axl-emulate captured fixture into vanilla OVMF ==="
REPLAY_OUT=$(timeout 60s "$AXL_EMULATE" "$FIX_DIR/fix/" "$SYSINFO" 2>&1 || true)

# The replay layer's manifest summary line on stderr must mention the
# captured strings.
if grep -qE "axl-emulate: replaying AximCode" <<< "$REPLAY_OUT"; then
    pass "axl-emulate prints captured-fixture identity at startup"
else
    fail "axl-emulate did not surface captured identity" \
         "(expected 'replaying AximCode' line)"
fi

# Inside the guest, sysinfo.efi must report the captured custom strings,
# not OVMF's auto-built Type 1 defaults.
if grep -q "Manufacturer: AximCode" <<< "$REPLAY_OUT"; then
    pass "guest sysinfo reports captured Manufacturer (AximCode)"
else
    fail "guest reported wrong/missing Manufacturer" \
         "(captured AximCode lost in replay)"
fi
if grep -q "Product:      TestRig" <<< "$REPLAY_OUT"; then
    pass "guest sysinfo reports captured Product (TestRig)"
else
    fail "guest reported wrong/missing Product"
fi
if grep -q "Version:      1.0" <<< "$REPLAY_OUT"; then
    pass "guest sysinfo reports captured Version (1.0)"
else
    fail "guest reported wrong/missing Version"
fi
if grep -q "Serial:       ABC123" <<< "$REPLAY_OUT"; then
    pass "guest sysinfo reports captured Serial (ABC123)"
else
    fail "guest reported wrong/missing Serial"
fi

# ----------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------
echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"
[[ "$FAIL" -eq 0 ]]
