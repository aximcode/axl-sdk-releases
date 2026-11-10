#!/bin/bash
# test-meta: arch=none needs= est=5 local-only=0 builds=ARCH=x64,axl-efi-pack
# test-efi-pack-host.sh -- the host packer `axl-efi-pack` (P4g Task 3).
#
# The packer is the ONLY place the wrap threshold lives (wrap iff
# raw - final >= 16384 AND final <= 0.80 * raw, final = stub + align8(40 +
# packed)); the CMake build and axl-cc call it with no tuning flags. This pins
# the CLI contract: exact stdout lines, exit codes 0 (wrapped) / 2 (refused or
# error) / 3 (kept raw), --force, the non-PE and unsupported-machine refusals,
# and a round trip through the hidden --verify.
#
# Host-only: no QEMU. Synthetic PE files are generated deterministically.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

WORK="$(mktemp -d -t axl-efipack.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

"$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 axl-efi-pack >/dev/null || { echo "FAIL: axl-efi-pack did not build"; exit 1; }
PACK="$(test_build_dir x64)/build/axl-efi-pack"
[[ -x "$PACK" ]] || { echo "FAIL: $PACK missing"; exit 1; }

# synth_pe FILE MACHINE_LO MACHINE_HI TOTAL_BYTES -- DOS header with e_lfanew
# = 0x80, "PE\0\0" and the Machine word there, then a repeating body (deterministic,
# compresses well) up to TOTAL_BYTES.
synth_pe() {
    local f="$1" lo="$2" hi="$3" total="$4"
    {
        printf 'MZ'
        head -c 58 /dev/zero
        printf '\200\000\000\000'
        head -c 64 /dev/zero
        printf 'PE\000\000'
        printf "\\$lo\\$hi"
        yes 'AXL packed image test body line 0123456789abcdef' | head -c "$total"
    } | head -c "$total" > "$f"
}

# synth_mixed FILE RANDOM_BYTES TOTAL -- x64 PE header, RANDOM_BYTES of
# deterministic incompressible data, then the repeating body up to TOTAL.
synth_mixed() {
    {
        printf 'MZ'; head -c 58 /dev/zero; printf '\200\000\000\000'
        head -c 64 /dev/zero; printf 'PE\000\000\144\206'
        openssl enc -aes-128-ctr -pass pass:axl -pbkdf2 -nosalt < /dev/zero 2>/dev/null | head -c "$2"
        yes 'AXL packed image test body line 0123456789abcdef'
    } 2>/dev/null | head -c "$3" > "$1"
}

synth_pe "$WORK/big.efi"   144 206 204800   # 0x8664 = bytes 0x64 0x86 -> octal 144 206
synth_pe "$WORK/small.efi" 144 206 8192
synth_pe "$WORK/arm.efi"   144 252 204800   # 0xAA64 -> bytes 0x64 0xAA
printf 'not a pe image\n' > "$WORK/junk.efi"
synth_pe "$WORK/odd.efi"   001 001 204800   # Machine 0x0101: unsupported

run() { # run ARGS... ; sets OUT and RC (stdout+stderr merged)
    OUT="$("$PACK" "$@" 2>&1)"; RC=$?
}
check() { # check LABEL EXPECT_OUT EXPECT_RC
    if [[ "$OUT" == "$2" && $RC -eq $3 ]]; then test_host_pass "$1"
    else test_host_fail "$1 (rc=$RC want $3, out='$OUT' want '$2')"; fi
}

cd "$WORK" || exit 1
STUB=16384

fsize() { stat -c %s "$1"; }

# Expected wrap line, derived from the files rather than from the tool: raw is
# the input size, final = stub + align8(.pack size), pct rounds DOWN.
expect_wrap() { # expect_wrap NAME PACKFILE RAWFILE
    local raw final
    raw=$(fsize "$3"); final=$(( STUB + ( $(fsize "$2") + 7 ) / 8 * 8 ))
    printf 'axl-efi-pack: %s: wrap %d -> %d (-%d%%)' "$1" "$raw" "$final" $(( (raw - final) * 100 / raw ))
}

run --stub-size $STUB -o big.pack big.efi
check "wraps the 200 KB compressible image" "$(expect_wrap big.efi big.pack big.efi)" 0
[[ -s big.pack ]] && test_host_pass "big.pack written" || test_host_fail "big.pack written"
# Literal pin of the exact line for this fixture (200 KB body, 210-byte pack).
[[ "$OUT" == "axl-efi-pack: big.efi: wrap 204800 -> 16600 (-91%)" ]] \
    && test_host_pass "wrap line literal" || test_host_fail "wrap line literal: $OUT"

run --verify big.pack big.efi
check "--verify round-trips x64" "axl-efi-pack: verify ok" 0

run --force --stub-size $STUB -o small.pack small.efi
[[ $RC -eq 0 && -s small.pack && "$OUT" == axl-efi-pack:\ small.efi:\ wrap\ 8192\ -\>* ]] \
    && test_host_pass "--force wraps the small image" || test_host_fail "--force wraps the small image (rc=$RC '$OUT')"
SAVES=$(( 8192 - ( STUB + ( $(fsize small.pack) + 7 ) / 8 * 8 ) ))   # signed: a stub bigger than the image saves < 0
run --stub-size $STUB -o keep.pack small.efi
check "keeps the 8 KB image raw" "axl-efi-pack: small.efi: keep raw 8192 (saves $SAVES bytes, under the threshold)" 3
[[ ! -e keep.pack ]] && test_host_pass "keep writes nothing" || test_host_fail "keep writes nothing"

run --verify small.pack small.efi
check "--verify round-trips the forced pack" "axl-efi-pack: verify ok" 0

run --stub-size $STUB -o arm.pack arm.efi
[[ $RC -eq 0 && "$OUT" == axl-efi-pack:\ arm.efi:\ wrap\ 204800\ -\>* ]] \
    && test_host_pass "wraps an AArch64 image" || test_host_fail "wraps an AArch64 image (rc=$RC '$OUT')"
run --verify arm.pack arm.efi
check "--verify round-trips AArch64" "axl-efi-pack: verify ok" 0
# Header byte 12 is the filter: 1 = X86 for Machine 0x8664, 2 = ARM64 for 0xAA64.
[[ "$(od -An -tu1 -j12 -N1 big.pack | tr -d ' ')" == 1 && "$(od -An -tu1 -j12 -N1 arm.pack | tr -d ' ')" == 2 ]] \
    && test_host_pass "filter byte follows Machine (x64 -> 1, AArch64 -> 2)" \
    || test_host_fail "filter byte follows Machine"
cmp -s big.pack arm.pack && test_host_fail "x64 and AArch64 packs differ (filter chosen from Machine)" \
    || test_host_pass "x64 and AArch64 packs differ (filter chosen from Machine)"

run --stub-size $STUB -o junk.pack junk.efi
check "refuses a non-PE input" "axl-efi-pack: junk.efi: not a PE/COFF image" 2
[[ ! -e junk.pack ]] && test_host_pass "refusal writes nothing" || test_host_fail "refusal writes nothing"

run --stub-size $STUB -o odd.pack odd.efi
check "refuses an unsupported Machine" "axl-efi-pack: odd.efi: unsupported PE machine 0x0101" 2

# A pack for one machine must not verify against an image of another.
cp big.pack mismatch.pack
run --verify mismatch.pack arm.efi
[[ $RC -ne 0 && "$OUT" != "axl-efi-pack: verify ok" ]] \
    && test_host_pass "--verify rejects a pack that is not this image" || test_host_fail "--verify mismatch (rc=$RC '$OUT')"

# --- both branch converters, through the host packer's decoder -------------
# Firmware code (libaxl's loaders, the stub) decodes its OWN architecture's
# filter only, so the round trip of the OTHER converter lives here: the packer
# is built with both decoders. The text fixtures above never trigger either
# converter (no E8/E9 byte, no BL/ADRP word), so these are branch-dense: a call
# every 16 bytes to one of four targets, as AxlTestPack's own fixtures.
# synth_branchy FILE x64|aa64 RECORDS
synth_branchy() {
    local f="$1" arch="$2" n="$3" i off tgt w fmt
    local -a targets=(8192 13312 23168 31760)
    {
        printf 'MZ'; head -c 58 /dev/zero; printf '\200\000\000\000'
        head -c 64 /dev/zero; printf 'PE\000\000'
        if [[ "$arch" == x64 ]]; then printf '\144\206'; else printf '\144\252'; fi
        head -c 2 /dev/zero                       # records start 4-aligned (136)
        for (( i = 0; i < n; i++ )); do
            off=$(( 136 + i * 16 )); tgt=${targets[$(( i * 7 % 4 ))]}
            if [[ "$arch" == x64 ]]; then         # E8 rel32, 11 x NOP
                w=$(( (tgt - off - 5) & 0xFFFFFFFF ))
                printf -v fmt '\\xe8\\x%02x\\x%02x\\x%02x\\x%02x' $(( w & 255 )) \
                    $(( w >> 8 & 255 )) $(( w >> 16 & 255 )) $(( w >> 24 & 255 ))
                printf "$fmt"; printf '\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90'
            else                                  # BL imm26, 3 x NOP
                w=$(( 0x94000000 | ((tgt - off) >> 2 & 0x03FFFFFF) ))
                printf -v fmt '\\x%02x\\x%02x\\x%02x\\x%02x' $(( w & 255 )) \
                    $(( w >> 8 & 255 )) $(( w >> 16 & 255 )) $(( w >> 24 & 255 ))
                printf "$fmt"; printf '\x1f\x20\x03\xd5\x1f\x20\x03\xd5\x1f\x20\x03\xd5'
            fi
        done
    } > "$f"
}
synth_branchy x64br.efi x64 2048
synth_branchy a64br.efi aa64 2048
for a in x64br a64br; do
    run --force --stub-size 1 -o "$a.pack" "$a.efi"
    [[ $RC -eq 0 && -s "$a.pack" ]] && test_host_pass "$a: branch-dense image packs" \
        || test_host_fail "$a: branch-dense image packs (rc=$RC '$OUT')"
    run --verify "$a.pack" "$a.efi"
    check "$a: --verify round-trips through the branch converter" "axl-efi-pack: verify ok" 0
done
[[ "$(od -An -tu1 -j12 -N1 x64br.pack | tr -d ' ')" == 1 && "$(od -An -tu1 -j12 -N1 a64br.pack | tr -d ' ')" == 2 ]] \
    && test_host_pass "branch-dense packs carry X86 (1) and ARM64 (2)" \
    || test_host_fail "branch-dense packs carry X86 (1) and ARM64 (2)"

# --- the threshold, exactly ------------------------------------------------
# A = align8(pack size) of a fixture, from a --force run (the tool's own pack
# is the oracle for sizes; the DECISION is what is under test).
align_pack() { # align_pack IN.efi -> align8(.pack size)
    "$PACK" --force --stub-size 1 -o probe.pack "$1" >/dev/null 2>&1
    echo $(( ( $(fsize probe.pack) + 7 ) / 8 * 8 ))
}
expect_keep() { # expect_keep NAME RAW FINAL
    printf 'axl-efi-pack: %s: keep raw %d (saves %d bytes, under the threshold)' "$1" "$2" $(( $2 - $3 ))
}
# edge LABEL IN.efi STUB EXPECT(wrap|keep)
edge() {
    local label="$1" in="$2" stub="$3" want="$4" raw A final
    raw=$(fsize "$in"); A=$(align_pack "$in"); final=$(( stub + A ))
    run --stub-size "$stub" -o edge.pack "$in"
    if [[ "$want" == wrap ]]; then
        check "$label" "$(printf 'axl-efi-pack: %s: wrap %d -> %d (-%d%%)' "$in" "$raw" "$final" $(( (raw - final) * 100 / raw )))" 0
        [[ -s edge.pack ]] || test_host_fail "$label: pack not written"
    else
        check "$label" "$(expect_keep "$in" "$raw" "$final")" 3
        [[ ! -e edge.pack ]] || test_host_fail "$label: pack written on keep"
    fi
    rm -f edge.pack
}

# Saving edge: raw 65536, stub chosen so saved == 16384 exactly (final is then
# 75% of raw, so the ratio rule holds and only the saving rule decides).
synth_pe save.efi 144 206 65536
A=$(align_pack save.efi)
edge "saved == 16384 wraps"      save.efi $(( 65536 - 16384 - A ))     wrap
edge "saved == 16383 keeps"      save.efi $(( 65536 - 16384 - A + 1 )) keep

# Ratio edge: raw 100000, stub chosen so final == 80000 == 0.80 * raw exactly
# (saved is 20000, so only the ratio rule decides).
synth_pe ratio-edge.efi 144 206 100000
A=$(align_pack ratio-edge.efi)
edge "final == 80% of raw wraps"        ratio-edge.efi $(( 80000 - A ))     wrap
edge "final == 80% of raw + 1 keeps"    ratio-edge.efi $(( 80000 - A + 1 ))  keep

# Each rule failing alone, well clear of its edge, with exact keep lines.
# Ratio only: 170 KB incompressible + 30 KB compressible, stub 1 (the least
# --stub-size accepts).
synth_mixed ratio.efi 170000 200000
A=$(align_pack ratio.efi)
run --stub-size 1 -o ratio.pack ratio.efi
check "keeps when the saving is large but the ratio is over 80%" "$(expect_keep ratio.efi 200000 $(( 1 + A )))" 3
# Saving only: 20 KB compressible image, stub 8192.
synth_pe floor.efi 144 206 20000
A=$(align_pack floor.efi)
run --stub-size 8192 -o floor.pack floor.efi
check "keeps when the ratio is fine but the saving is under 16384" "$(expect_keep floor.efi 20000 $(( 8192 + A )))" 3

# R6: OUT.pack exists afterwards only if this run wrapped.
run --stub-size $STUB -o stale.pack big.efi
[[ $RC -eq 0 && -s stale.pack ]] && test_host_pass "stale.pack written by a wrap" || test_host_fail "stale.pack written by a wrap"
run --stub-size 200000 -o stale.pack big.efi
[[ $RC -eq 3 && ! -e stale.pack ]] && test_host_pass "a keep run removes a stale OUT.pack" || test_host_fail "a keep run removes a stale OUT.pack (rc=$RC)"
cp big.pack stale.pack
run --stub-size $STUB -o stale.pack junk.efi
[[ $RC -eq 2 && ! -e stale.pack ]] && test_host_pass "a refusal removes a stale OUT.pack" || test_host_fail "a refusal removes a stale OUT.pack (rc=$RC)"
cp big.pack stale.pack
run --stub-size $STUB -o /nonexistent-dir/x.pack big.efi
[[ $RC -eq 2 ]] && test_host_pass "an unwritable OUT is an error (rc 2)" || test_host_fail "unwritable OUT (rc=$RC)"

# -o naming the input: refused BEFORE anything is removed, so the input
# survives -- by the same spelling, by another spelling, and by a hard link.
cp big.efi same.efi; cp big.efi same.orig
for o in same.efi ./same.efi "$WORK/same.efi"; do
    run --stub-size $STUB -o "$o" same.efi
    check "-o $o naming the input is refused" "axl-efi-pack: -o $o is the input file same.efi" 2
done
ln same.efi same-link.efi
run --stub-size $STUB -o same-link.efi same.efi
check "-o naming a hard link to the input is refused" "axl-efi-pack: -o same-link.efi is the input file same.efi" 2
cmp -s same.efi same.orig && test_host_pass "the input survives -o naming it" \
    || test_host_fail "the input survives -o naming it"
# Control: a different, existing -o is still replaced as before.
cp big.efi other.pack
run --stub-size $STUB -o other.pack same.efi
[[ $RC -eq 0 ]] && ! cmp -s other.pack big.efi && test_host_pass "a different existing -o is overwritten" \
    || test_host_fail "a different existing -o is overwritten (rc=$RC)"

# --stub-size must be a whole byte count in [1, 1 GiB]: empty, non-numeric,
# negative, overflowing and out-of-range values are refused, never read as 0.
for v in "" abc -1 0 99999999999999999999 1073741825 12x; do
    rm -f bad.pack
    run --stub-size "$v" -o bad.pack big.efi
    check "--stub-size '$v' is refused" "axl-efi-pack: --stub-size '$v': not a byte count in [1, 1073741824]" 2
    [[ ! -e bad.pack ]] || test_host_fail "--stub-size '$v' wrote a pack"
done
run --stub-size 1073741824 -o ok.pack big.efi
[[ $RC -eq 3 ]] && test_host_pass "--stub-size 1073741824 (the cap) is accepted" \
    || test_host_fail "--stub-size 1073741824 accepted (rc=$RC '$OUT')"

run --stub-size $STUB big.efi
check "missing -o is a usage error" "usage: axl-efi-pack [--force] --stub-size BYTES -o OUT.pack IN.efi" 2

test_host_summary "test-efi-pack-host"
