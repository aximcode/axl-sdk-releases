#!/bin/bash
# test-meta: arch=both needs= est=25 local-only=0 builds=pack-fixture-w,pack-load,kbtune-drv-test guest=none
# guest=none: it asserts image paths rooted at the boot volume (fs0:\...),
#   which on a leased guest is not the test's disk.
# test-efi-pack-drivers-qemu.sh -- AXL's own driver loaders take a packed
# (wrapped) image exactly as they take a raw one, deciding by CONTENT, and the
# handle they return is the ORIGINAL image's: no axl-pack-stub image is ever
# left resident by an AXL load.
#
# A wrapped .efi is valid PE (the stub), so a firmware-reads-the-file load
# would succeed and load the STUB, which then loads the original beside itself.
# The loaders instead read the file's section table, and when it names
# .axlpack they unpack in-process and LoadImage the original with the file's
# own device path. The discriminator is the Shell's `dh -v -p LoadedImage`:
# every image's PdbFileName is listed, and the stub's is `axl-pack-stub`.
#
#   mixed-dir   drivers/<arch>/ holds a raw, a wrapped, and a raw driver
#               whose section table ends past the loader's 1 KB sniff;
#               load_dir loads all three, each reports its own real path,
#               and nothing logs a "packed image" warning
#   buffer      a wrapped file's bytes through load_buffer_with_image_info
#   relative    a relative path: the loader's read-it-ourselves route
#   corrupt     a wrapped file with a damaged payload is REFUSED, with the
#               pack status in the log -- never loaded as the stub
#   bad-raw     a RAW file with a corrupt e_lfanew fails to load with NO
#               "packed image" warning: malformed headers prove nothing
#               about packing, so the firmware gives its own verdict
#   reload      the shared-driver ensure (sibling locate), twice: one
#               resident copy
#   dh          after all of the above: one image per loaded file, the inner
#               images' PdbFileName, and no axl-pack-stub at all
#   unload      the sibling driver unloaded by its protocol GUID: gone from dh
#   sibling     kbtune-drv-test.efi's own lifecycle against a wrapped
#               kbtune-drv.efi beside it
#
# One boot. Usage: ./test/integration/test-efi-pack-drivers-qemu.sh [--arch X64|AARCH64]

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
        pack-fixture-w pack-load kbtune-drv-test > "$TEST_TMPDIR/make.log" 2>&1; then
    tail -20 "$TEST_TMPDIR/make.log"
    echo "efi-pack-drivers test: FAIL (build)"
    exit 1
fi

NATIVE_DIR="$(test_build_dir "$_native_arch")"
RAW_DRV="$NATIVE_DIR/image-path-driver.efi"
WRAP_DRV="$NATIVE_DIR/pack-driver-w.efi"
WRAP_KBT="$NATIVE_DIR/pack-kbtune-drv-w.efi"
WRAP_SDSIB="$NATIVE_DIR/pack-sdsib-drv-w.efi"
APP="$NATIVE_DIR/pack-load-test.efi"
KBT_APP="$NATIVE_DIR/kbtune-drv-test.efi"

FAIL=0
PASS=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# File offset of the .axlpack section's bytes (PointerToRawData), or NONE.
axlpack_offset() {
    local img="$1" pe nsec opt tbl i ent name
    pe=$(od -An -tu4 -j60 -N4 "$img" | tr -d ' ')
    nsec=$(od -An -tu2 -j$((pe + 6)) -N2 "$img" | tr -d ' ')
    opt=$(od -An -tu2 -j$((pe + 20)) -N2 "$img" | tr -d ' ')
    tbl=$((pe + 24 + opt))
    for ((i = 0; i < nsec; i++)); do
        ent=$((tbl + 40 * i))
        name=$(dd if="$img" bs=1 skip="$ent" count=8 status=none | tr -d '\0')
        if [[ "$name" == ".axlpack" ]]; then
            od -An -tu4 -j$((ent + 20)) -N4 "$img" | tr -d ' '
            return
        fi
    done
    echo NONE
}

# Little-endian field access for the PE edits below.
u32() { od -An -tu4 -j"$2" -N4 "$1" | tr -d ' '; }
u16() { od -An -tu2 -j"$2" -N2 "$1" | tr -d ' '; }
put32() {
    local v=$3
    printf '%b' "$(printf '\\%03o\\%03o\\%03o\\%03o' $((v & 255)) $(((v >> 8) & 255)) \
        $(((v >> 16) & 255)) $(((v >> 24) & 255)))" |
        dd of="$1" bs=1 seek="$2" conv=notrunc status=none
}
# Copy raw PE $1 to $2 with its NT headers moved to file offset 1024, so the
# section table ends past a 1 KB prefix. Everything after the headers shifts
# by 1024 bytes; SizeOfHeaders, every PointerToRawData and the debug
# directory's file offsets follow it.
widen_headers() {
    local src="$1" dst="$2" shift=1024 L N O H nl ht i ent ptr dd_rva dd_size va vs fo k e
    L=$(u32 "$src" 60); N=$(u16 "$src" $((L + 6))); O=$(u16 "$src" $((L + 20)))
    H=$(u32 "$src" $((L + 24 + 60)))
    nl=$shift
    head -c "$L" "$src" > "$dst"
    truncate -s "$nl" "$dst"
    dd if="$src" bs=1 skip="$L" count=$((24 + O + 40 * N)) status=none >> "$dst"
    truncate -s $((H + shift)) "$dst"
    tail -c +$((H + 1)) "$src" >> "$dst"
    put32 "$dst" 60 "$nl"
    put32 "$dst" $((nl + 24 + 60)) $((H + shift))
    ht=$((nl + 24 + O))
    for ((i = 0; i < N; i++)); do
        ent=$((ht + 40 * i)); ptr=$(u32 "$dst" $((ent + 20)))
        (( ptr != 0 )) && put32 "$dst" $((ent + 20)) $((ptr + shift))
    done
    dd_rva=$(u32 "$dst" $((nl + 24 + 112 + 6 * 8))); dd_size=$(u32 "$dst" $((nl + 24 + 112 + 6 * 8 + 4)))
    (( dd_rva == 0 )) && return 0
    for ((i = 0; i < N; i++)); do
        ent=$((ht + 40 * i)); va=$(u32 "$dst" $((ent + 12))); vs=$(u32 "$dst" $((ent + 8)))
        if (( dd_rva >= va && dd_rva < va + vs )); then
            fo=$((dd_rva - va + $(u32 "$dst" $((ent + 20)))))
            for ((k = 0; k < dd_size; k += 28)); do
                e=$(u32 "$dst" $((fo + k + 24)))
                (( e != 0 )) && put32 "$dst" $((fo + k + 24)) $((e + shift))
            done
        fi
    done
}

# ---------------------------------------------------------------------------
# Host-side: both drivers really are wrapped, and the corrupt copy.
# ---------------------------------------------------------------------------
for img in "$WRAP_DRV" "$WRAP_KBT" "$WRAP_SDSIB"; do
    off=$(axlpack_offset "$img")
    if [[ "$off" =~ ^[0-9]+$ ]] &&
       [[ "$(dd if="$img" bs=1 skip="$off" count=8 status=none)" == AXLPACK1 ]]; then
        pass "$(basename "$img") has an .axlpack section holding a packed payload"
    else
        fail "$(basename "$img") has an .axlpack section holding a packed payload (got $off)"
    fi
done

# One flipped byte 256 into the LZMA stream: header intact, stream damaged.
CORRUPT="$TEST_TMPDIR/corrupt.efi"
cp "$WRAP_DRV" "$CORRUPT"
off=$(axlpack_offset "$CORRUPT")
if [[ "$off" =~ ^[0-9]+$ ]]; then
    flip_at=$((off + 40 + 256))
    byte=$(od -An -tu1 -j "$flip_at" -N1 "$CORRUPT" | tr -d ' ')
    printf '%b' "$(printf '\\%03o' $(( byte ^ 0xFF )))" |
        dd of="$CORRUPT" bs=1 seek="$flip_at" conv=notrunc status=none
fi
if ! cmp -s "$WRAP_DRV" "$CORRUPT"; then
    pass "corrupt copy differs from pack-driver-w.efi"
else
    fail "corrupt copy differs from pack-driver-w.efi"
fi

# A raw driver whose section table ends past the 1 KB sniff, and a raw one
# whose e_lfanew points past the end of the file.
WIDE="$TEST_TMPDIR/wide.efi"
widen_headers "$RAW_DRV" "$WIDE"
wl=$(u32 "$WIDE" 60)
if (( wl >= 1024 )) && [[ "$(dd if="$WIDE" bs=1 skip="$wl" count=2 status=none)" == PE ]]; then
    pass "wide.efi: PE headers start at $wl, past the 1 KB sniff"
else
    fail "wide.efi: PE headers start at $wl, past the 1 KB sniff"
fi
BADRAW="$TEST_TMPDIR/badraw.efi"
cp "$RAW_DRV" "$BADRAW"
put32 "$BADRAW" 60 $((0xFFFFFF00))
if [[ "$(u32 "$BADRAW" 60)" == $((0xFFFFFF00)) ]] && (( $(stat -c%s "$BADRAW") > 1024 )); then
    pass "badraw.efi: e_lfanew past the end, file larger than the sniff"
else
    fail "badraw.efi: e_lfanew past the end, file larger than the sniff"
fi

test_add_efi "$APP"
test_add_efi "$KBT_APP"
test_add_efi "$WRAP_KBT" "kbtune-drv.efi"
test_add_efi "$WRAP_SDSIB" "sdsib-drv.efi"
test_add_efi "$RAW_DRV" "drivers/$_native_arch/plain.efi"
test_add_efi "$WRAP_DRV" "drivers/$_native_arch/packed.efi"
test_add_efi "$WIDE" "drivers/$_native_arch/wide.efi"
test_add_efi "$WRAP_DRV" "rel-packed.efi"
test_add_efi "$CORRUPT"
test_add_efi "$BADRAW"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo DIR_BEGIN"
    echo "pack-load-test.efi dir fs0:\\drivers\\$_native_arch 3"
    echo "echo DIR_END"
    echo "echo BUF_BEGIN"
    echo "pack-load-test.efi buffer fs0:\\drivers\\$_native_arch\\packed.efi buffer-packed.efi"
    echo "echo BUF_END"
    echo "echo REL_BEGIN"
    echo "pack-load-test.efi path rel-packed.efi"
    echo "echo REL_END"
    echo "echo BAD_BEGIN"
    echo "pack-load-test.efi refuse fs0:\\corrupt.efi"
    echo "echo BAD_END"
    echo "echo BADRAW_BEGIN"
    echo "pack-load-test.efi refuse fs0:\\badraw.efi"
    echo "echo BADRAW_END"
    echo "echo ENS_BEGIN"
    echo "pack-load-test.efi ensure"
    echo "echo ENS_END"
    echo "echo DH1_BEGIN"
    echo "dh -v -p LoadedImage"
    echo "echo DH1_END"
    echo "echo UNL_BEGIN"
    echo "pack-load-test.efi unload"
    echo "echo UNL_END"
    echo "echo DH2_BEGIN"
    echo "dh -v -p LoadedImage"
    echo "echo DH2_END"
    echo "echo KBT_BEGIN"
    echo "kbtune-drv-test.efi"
    echo "echo KBT_END"
    echo "echo DH3_BEGIN"
    echo "dh -v -p LoadedImage"
    echo "echo DH3_END"
    echo "echo PACK_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== efi-pack AXL driver loaders ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 120

test_clean_log

echo "--- serial log (DIR_BEGIN .. ENS_END, UNL_BEGIN .. UNL_END, KBT_BEGIN .. KBT_END) ---"
sed -n '/^DIR_BEGIN$/,/^ENS_END$/p; /^UNL_BEGIN$/,/^UNL_END$/p; /^KBT_BEGIN$/,/^KBT_END$/p' \
    "$TEST_CLEAN_LOG" | sed 's/^/  /'

# Exactly N lines strictly between markers $2 and $3 match the anchored ERE
# $4. A missing marker FAILS the assertion: a count of 0 over a window that
# was never printed (the guest hung before it) would otherwise pass every
# "none of these" check.
expect_count() {
    local name="$1" from="$2" to="$3" re="$4" want="$5" got
    if ! test_slice_log "$from" "$to" "$TEST_TMPDIR/slice"; then
        fail "$name (window $from..$to missing)"
        return 0
    fi
    got=$(grep -cE "$re" "$TEST_TMPDIR/slice" || true)
    if [[ "$got" == "$want" ]]; then
        pass "$name"
    else
        fail "$name (want $want, got $got)"
    fi
}

A="$_native_arch"
RESULTS_OK() { printf '^=== Results: %d passed, 0 failed ===$' "$1"; }

# --- mixed-dir: both loaded and started; each reports its own real path, so
#     the wrapped one was loaded with the FILE's device path, not a buffer's.
expect_count "mixed-dir: pack-load-test dir passes 3/3" DIR_BEGIN DIR_END "$(RESULTS_OK 3)" 1
expect_count "mixed-dir: LOADDIR: loaded=3" DIR_BEGIN DIR_END '^LOADDIR: loaded=3$' 1
expect_count "mixed-dir: plain.efi reports its own path" DIR_BEGIN DIR_END \
    "^IMGPATH: self=FS0:\\\\drivers\\\\$A\\\\plain\\.efi\$" 1
expect_count "mixed-dir: packed.efi reports its own path" DIR_BEGIN DIR_END \
    "^IMGPATH: self=FS0:\\\\drivers\\\\$A\\\\packed\\.efi\$" 1
expect_count "mixed-dir: wide.efi reports its own path" DIR_BEGIN DIR_END \
    "^IMGPATH: self=FS0:\\\\drivers\\\\$A\\\\wide\\.efi\$" 1
expect_count "mixed-dir: no warning at all" DIR_BEGIN DIR_END '\[WARN\]' 0

# --- buffer / relative: a synthesized identity (no file of its own).
expect_count "buffer: pack-load-test buffer passes 3/3" BUF_BEGIN BUF_END "$(RESULTS_OK 3)" 1
expect_count "buffer: the inner driver ran (IMGPATH: self=(null))" BUF_BEGIN BUF_END \
    '^IMGPATH: self=\(null\)$' 1
expect_count "relative: pack-load-test path passes 2/2" REL_BEGIN REL_END "$(RESULTS_OK 2)" 1
expect_count "relative: the inner driver ran (IMGPATH: self=(null))" REL_BEGIN REL_END \
    '^IMGPATH: self=\(null\)$' 1

# --- corrupt: refused, one exact log line naming the file and the status,
#     and nothing of the driver ran.
expect_count "corrupt: pack-load-test refuse passes 2/2" BAD_BEGIN BAD_END "$(RESULTS_OK 2)" 1
expect_count "corrupt: one log line with the pack status" BAD_BEGIN BAD_END \
    '^[0-9:.]+ \[WARN\]  driver: cannot load fs0:\\corrupt\.efi: packed image is corrupt$' 1
expect_count "corrupt: no driver code ran" BAD_BEGIN BAD_END '^IMGPATH: ' 0

# --- bad-raw: refused by the firmware, and no pack verdict claimed for it.
expect_count "bad-raw: pack-load-test refuse passes 2/2" BADRAW_BEGIN BADRAW_END "$(RESULTS_OK 2)" 1
expect_count "bad-raw: no warning at all (no false 'packed image')" BADRAW_BEGIN BADRAW_END '\[WARN\]' 0

# --- reload: two locates, one load.
expect_count "reload: pack-load-test ensure passes 2/2" ENS_BEGIN ENS_END "$(RESULTS_OK 2)" 1

# --- dh while everything is resident.
PDB='^  PdbFileName\.\.\.: '
FP='^  FilePath\.\.\.\.\.\.: '
expect_count "dh: NO axl-pack-stub image resident" DH1_BEGIN DH1_END "${PDB}axl-pack-stub *\$" 0
expect_count "dh: five image-path-driver images (plain, packed, wide, buffer, relative)" \
    DH1_BEGIN DH1_END "${PDB}image-path-driver\\.efi *\$" 5
expect_count "dh: one image for drivers\\$A\\plain.efi" DH1_BEGIN DH1_END \
    "${FP}\\\\drivers\\\\$A\\\\plain\\.efi *\$" 1
expect_count "dh: one image for drivers\\$A\\packed.efi" DH1_BEGIN DH1_END \
    "${FP}\\\\drivers\\\\$A\\\\packed\\.efi *\$" 1
expect_count "dh: one image for drivers\\$A\\wide.efi" DH1_BEGIN DH1_END \
    "${FP}\\\\drivers\\\\$A\\\\wide\\.efi *\$" 1
expect_count "dh: nothing loaded badraw.efi" DH1_BEGIN DH1_END "${FP}\\\\badraw\\.efi *\$" 0
expect_count "dh: one image for the buffer load" DH1_BEGIN DH1_END "${FP}\\\\buffer-packed\\.efi *\$" 1
expect_count "dh: one image for the relative load" DH1_BEGIN DH1_END "${FP}\\\\rel-packed\\.efi *\$" 1
expect_count "dh: one sdsib-drv image after two ensures" DH1_BEGIN DH1_END "${FP}\\\\sdsib-drv\\.efi *\$" 1
expect_count "dh: nothing loaded the corrupt file" DH1_BEGIN DH1_END "${FP}\\\\corrupt\\.efi *\$" 0

# --- unload by GUID: the handle the protocol sits on is the image's.
expect_count "unload: pack-load-test unload passes 1/1" UNL_BEGIN UNL_END "$(RESULTS_OK 1)" 1
expect_count "unload: sdsib-drv gone from dh" DH2_BEGIN DH2_END "${FP}\\\\sdsib-drv\\.efi *\$" 0
expect_count "unload: no axl-pack-stub image" DH2_BEGIN DH2_END "${PDB}axl-pack-stub *\$" 0

# --- sibling: kbtune-drv-test's own lifecycle against the wrapped driver.
expect_count "sibling: kbtune-drv-test passes 14/14" KBT_BEGIN KBT_END "$(RESULTS_OK 14)" 1
expect_count "sibling: kbtune-drv gone from dh after its unload" DH3_BEGIN DH3_END \
    "${FP}\\\\kbtune-drv\\.efi *\$" 0
expect_count "sibling: no axl-pack-stub image" DH3_BEGIN DH3_END "${PDB}axl-pack-stub *\$" 0

if grep -Fxq PACK_DONE "$TEST_CLEAN_LOG"; then
    pass "script ran to PACK_DONE"
else
    fail "script ran to PACK_DONE"
fi

# Every app run above logs a teardown leak verdict; a missing or failing one
# fails the run.
if test_check_leaks; then
    pass "leak gate"
else
    fail "leak gate"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
if [[ "$FAIL" -eq 0 ]]; then
    echo "efi-pack-drivers test: OK"
    exit 0
fi
echo "efi-pack-drivers test: FAIL"
exit 1
