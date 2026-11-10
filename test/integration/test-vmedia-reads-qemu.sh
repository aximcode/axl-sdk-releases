#!/bin/bash
# test-meta: arch=both needs= est=30 local-only=0 builds=vmedia,sshd guest=none
# guest=none: its driver publishes AXLVM: as a copy of fs0:\vm\, and on a
#   leased guest fs0: is not the test's disk.
# test-vmedia-reads-qemu.sh -- AXL reads files in chunks of at most 64 KB, so
# media that corrupts LARGER single reads cannot spoil what AXL reads.
#
# Measured on real hardware booted through a BMC's browser virtual media:
# packed sshd.efi, loaded by the firmware's LoadImage in one large read,
# failed its CRC 7 times in 10; sha256sum of the same file (64 KB reads) was
# correct 5 times in 5; a local USB copy and a RAM disk never failed.
#
# vmedia-fs-driver.efi reproduces that deterministically: it publishes AXLVM:,
# a copy of fs0:\vm\, and inverts the byte in the MIDDLE of a file whenever a
# single Read of more than 64 KB covers it, printing one `VMFS: corrupted ...`
# line each time.
#
#   contents  axl_file_get_contents of a 229,000-byte file: the digest the
#             guest prints is the host's, and the media saw no large read
#   driver    axl_driver_load + start of a RAW 318 KB driver: its pad table
#             is intact, it reports its own file path (so it was loaded
#             with the file's device path, not a synthesized MemoryMapped
#             identity), and the media saw no large read
#   packed    packed sshd.efi launched from the Shell: the firmware's own
#             large read IS corrupted (the control: the fixture fires), the
#             stub says so in its one re-read line, and its chunked re-read
#             runs the tool, whose --version line is exact
#
# Files named ioerr-* also fail every read past offset 0, so a chunked reader
# gets one good chunk and then an error:
#
#   readerr   axl_file_get_contents reports the error and returns no buffer
#             -- never the first 64 KB as if they were the file
#   fread     one axl_fread of the whole file delivers the 65,536 bytes read
#             before the error AND flags the error (the bytes are not lost)
#   refuse    axl_driver_load of a raw driver whose read fails part-way is
#             refused, with one exact warning, and the firmware is NOT asked
#             to read the file in one large call instead
#
# Host-side preconditions prove the flip lands where it is observable: inside
# the padded driver's table (all 0x5A around the middle) and inside sshd's
# .axlpack payload. A flip anywhere else would make a pass meaningless.
#
# One boot. Usage: ./test/integration/test-vmedia-reads-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

if ! "$PROJECT_DIR/scripts/axl-build.sh" \
        ARCH="$_native_arch" \
        vmedia sshd > "$TEST_TMPDIR/make.log" 2>&1; then
    tail -20 "$TEST_TMPDIR/make.log"
    echo "vmedia-reads test: FAIL (build)"
    exit 1
fi

NATIVE_DIR="$(test_build_dir "$_native_arch")"
FS_DRV="$NATIVE_DIR/vmedia-fs-driver.efi"
APP="$NATIVE_DIR/vmedia-check.efi"
BIG_DRV="$NATIVE_DIR/image-path-driver-big.efi"
SSHD="$NATIVE_DIR/tools/sshd.efi"
VER=$(cat "$PROJECT_DIR/VERSION")
RELIABLE=$((64 * 1024))

FAIL=0
PASS=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

u32() { od -An -tu4 -j"$2" -N4 "$1" | tr -d ' '; }
u16() { od -An -tu2 -j"$2" -N2 "$1" | tr -d ' '; }

# "<PointerToRawData> <SizeOfRawData>" of the .axlpack section, or NONE.
axlpack_range() {
    local img="$1" pe nsec opt tbl i ent name
    pe=$(u32 "$img" 60)
    nsec=$(u16 "$img" $((pe + 6)))
    opt=$(u16 "$img" $((pe + 20)))
    tbl=$((pe + 24 + opt))
    for ((i = 0; i < nsec; i++)); do
        ent=$((tbl + 40 * i))
        name=$(dd if="$img" bs=1 skip="$ent" count=8 status=none | tr -d '\0')
        if [[ "$name" == ".axlpack" ]]; then
            echo "$(u32 "$img" $((ent + 20))) $(u32 "$img" $((ent + 16)))"
            return
        fi
    done
    echo NONE
}

# ---------------------------------------------------------------------------
# Host side: the staged files, and where each one's middle byte falls.
# ---------------------------------------------------------------------------
BLOB="$TEST_TMPDIR/blob.bin"
seq 1 40000 > "$BLOB"
BLOB_SIZE=$(stat -c%s "$BLOB")
BLOB_SHA=$(sha256sum "$BLOB" | cut -d' ' -f1)
BIG_SIZE=$(stat -c%s "$BIG_DRV")
SSHD_SIZE=$(stat -c%s "$SSHD")

for f in "$BLOB" "$BIG_DRV" "$SSHD"; do
    sz=$(stat -c%s "$f")
    if (( sz > RELIABLE )); then
        pass "$(basename "$f") is $sz B, over the 64 KB a single read survives"
    else
        fail "$(basename "$f") is $sz B, over the 64 KB a single read survives"
    fi
done

# The padded driver's middle is inside its 0x5A table: 4 KB either side of
# the middle byte is all 0x5A ('Z'), so the flip is one its entry point sees.
mid=$((BIG_SIZE / 2))
left=$(dd if="$BIG_DRV" bs=1 skip=$((mid - 4096)) count=8192 status=none | tr -d 'Z' | wc -c)
if [[ "$left" == 0 ]]; then
    pass "image-path-driver-big.efi: the middle byte lies inside the pad table"
else
    fail "image-path-driver-big.efi: the middle byte lies inside the pad table ($left other bytes)"
fi

# sshd's middle is inside the .axlpack payload, past its 40-byte header, so
# the flip is one the payload's checks see -- not one in the stub's code.
read -r pk_off pk_len <<<"$(axlpack_range "$SSHD")"
mid=$((SSHD_SIZE / 2))
if [[ "$pk_off" =~ ^[0-9]+$ ]] && (( mid >= pk_off + 40 && mid < pk_off + pk_len )); then
    pass "sshd.efi is packed and its middle byte lies inside the payload"
else
    fail "sshd.efi is packed and its middle byte lies inside the payload (.axlpack: $pk_off $pk_len, middle $mid)"
fi

test_add_efi "$FS_DRV"
test_add_efi "$APP"
test_add_efi "$BLOB" "vm/blob.bin"
test_add_efi "$BIG_DRV" "vm/big-drv.efi"
test_add_efi "$SSHD" "vm/sshd.efi"
test_add_efi "$BLOB" "vm/ioerr-blob.bin"
test_add_efi "$BIG_DRV" "vm/ioerr-drv.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "load fs0:\\vmedia-fs-driver.efi"
    echo "echo SHA_BEGIN"
    echo "vmedia-check.efi sha AXLVM:\\blob.bin"
    echo "echo SHA_END"
    echo "echo DRV_BEGIN"
    echo "vmedia-check.efi load AXLVM:\\big-drv.efi"
    echo "echo DRV_END"
    echo "echo TOOL_BEGIN"
    echo "AXLVM:\\sshd.efi --version"
    echo "echo TOOL_END"
    echo "echo RE_BEGIN"
    echo "vmedia-check.efi readerr AXLVM:\\ioerr-blob.bin"
    echo "echo RE_END"
    echo "echo FR_BEGIN"
    echo "vmedia-check.efi fread AXLVM:\\ioerr-blob.bin"
    echo "echo FR_END"
    echo "echo RF_BEGIN"
    echo "vmedia-check.efi refuse AXLVM:\\ioerr-drv.efi"
    echo "echo RF_END"
    echo "echo VM_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== reads from media that corrupts large reads ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 120

test_clean_log

echo "--- serial log (SHA_BEGIN .. VM_DONE) ---"
sed -n '/^SHA_BEGIN$/,/^VM_DONE$/p' "$TEST_CLEAN_LOG" | sed 's/^/  /'

# The boot runs the modern Shell, which always has SetMap: a missing map is a
# regression in axl_volume_set_map (or the fixture), never a reason to skip.
if grep -q '^VMFS: SKIP no-shell-map' "$TEST_CLEAN_LOG"; then
    fail "VMFS: the fixture volume could not be mapped (VMFS: SKIP no-shell-map)"
fi
if grep -qx 'VMFS: READY volume=AXLVM files=5' "$TEST_CLEAN_LOG"; then
    pass "VMFS: READY volume=AXLVM files=5"
else
    fail "VMFS: READY volume=AXLVM files=5"
fi

# Exactly N lines strictly between markers $2 and $3 equal $4 (a FIXED
# string, whole line). A missing marker FAILS: a count of 0 over a window
# that was never printed would otherwise pass every "none of these" check.
expect_line() {
    local name="$1" from="$2" to="$3" line="$4" want="$5" got
    if ! test_slice_log "$from" "$to" "$TEST_TMPDIR/slice"; then
        fail "$name (window $from..$to missing)"
        return 0
    fi
    got=$(grep -cFx -- "$line" "$TEST_TMPDIR/slice" || true)
    if [[ "$got" == "$want" ]]; then
        pass "$name"
    else
        fail "$name (want $want, got $got)"
    fi
}
# The same, for lines matching the ERE $4.
expect_count() {
    local name="$1" from="$2" to="$3" re="$4" want="$5" got
    if ! test_slice_log "$from" "$to" "$TEST_TMPDIR/slice"; then
        fail "$name (window $from..$to missing)"
        return 0
    fi
    got=$(grep -cE -- "$re" "$TEST_TMPDIR/slice" || true)
    if [[ "$got" == "$want" ]]; then
        pass "$name"
    else
        fail "$name (want $want, got $got)"
    fi
}

# --- contents: exact bytes, and no read the media could spoil.
expect_line "contents: vmedia-check sha passes 2/2" SHA_BEGIN SHA_END \
    "=== Results: 2 passed, 0 failed ===" 1
expect_line "contents: the guest's digest is the host's" SHA_BEGIN SHA_END \
    "VMCHECK: bytes=$BLOB_SIZE sha256=$BLOB_SHA" 1
expect_count "contents: no single read over 64 KB" SHA_BEGIN SHA_END '^VMFS: corrupted ' 0

# --- driver: intact, started, and loaded with the file's own path.
expect_line "driver: vmedia-check load passes 2/2" DRV_BEGIN DRV_END \
    "=== Results: 2 passed, 0 failed ===" 1
expect_line "driver: the pad table arrived intact" DRV_BEGIN DRV_END \
    "IMGPATH: pad=ok" 1
# A buffer load's synthesized identity reports self=(null). The shell names
# the volume by its own fsN alias, whose number depends on how many volumes
# the boot has, so that one field is the only thing left free.
expect_count "driver: it reports its own file path" DRV_BEGIN DRV_END \
    '^IMGPATH: self=FS[0-9]+:\\big-drv\.efi$' 1
expect_count "driver: no single read over 64 KB" DRV_BEGIN DRV_END '^VMFS: corrupted ' 0

# --- packed: the firmware's read is spoiled; the stub re-reads and runs.
expect_line "packed: the firmware's one large read was corrupted (control)" TOOL_BEGIN TOOL_END \
    "VMFS: corrupted a $SSHD_SIZE-byte read of sshd.efi" 1
expect_count "packed: no other read over 64 KB" TOOL_BEGIN TOOL_END '^VMFS: corrupted ' 1
expect_line "packed: one re-read line" TOOL_BEGIN TOOL_END \
    "axl-pack: packed image is corrupt -- re-reading from the volume (attempt 1 of 2)" 1
expect_count "packed: no other axl-pack line" TOOL_BEGIN TOOL_END '^axl-pack: ' 1
expect_line "packed: sshd --version ran" TOOL_BEGIN TOOL_END "sshd $VER" 1

# --- readerr: an error part-way through is an error, not a short file.
expect_line "readerr: vmedia-check readerr passes 2/2" RE_BEGIN RE_END \
    "=== Results: 2 passed, 0 failed ===" 1
expect_line "readerr: the media failed the second chunk (control)" RE_BEGIN RE_END \
    "VMFS: failed a read of ioerr-blob.bin at 65536" 1
expect_count "readerr: no truncated contents returned" RE_BEGIN RE_END '^VMCHECK: returned ' 0

# --- fread: the bytes before the error arrive, and the error is flagged.
expect_line "fread: vmedia-check fread passes 2/2" FR_BEGIN FR_END \
    "=== Results: 2 passed, 0 failed ===" 1
expect_line "fread: 65536 bytes delivered, then the error" FR_BEGIN FR_END \
    "VMCHECK: fread=65536 error=1" 1

# --- refuse: AXL's own read failed, so the driver is refused -- and the
#     firmware is not handed the path to read in one large call instead.
expect_line "refuse: vmedia-check refuse passes 2/2" RF_BEGIN RF_END \
    "=== Results: 2 passed, 0 failed ===" 1
expect_line "refuse: the media failed the second chunk (control)" RF_BEGIN RF_END \
    "VMFS: failed a read of ioerr-drv.efi at 65536" 1
expect_count "refuse: one warning naming the file" RF_BEGIN RF_END \
    '^[0-9:.]+ \[WARN\]  driver: cannot load AXLVM:\\ioerr-drv\.efi: the file could not be read whole$' 1
expect_count "refuse: no single read over 64 KB" RF_BEGIN RF_END '^VMFS: corrupted ' 0
expect_count "refuse: no driver code ran" RF_BEGIN RF_END '^IMGPATH: ' 0

if grep -Fxq VM_DONE "$TEST_CLEAN_LOG"; then
    pass "script ran to VM_DONE"
else
    fail "script ran to VM_DONE"
fi

if test_check_leaks; then
    pass "leak gate"
else
    fail "leak gate"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
if [[ "$FAIL" -eq 0 ]]; then
    echo "vmedia-reads test: OK"
    exit 0
fi
echo "vmedia-reads test: FAIL"
exit 1
