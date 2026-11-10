# shellcheck shell=bash
# pack-lib.sh -- finding and damaging the packed payload of a wrapped .efi,
# for the suites that boot one (test-efi-pack-qemu.sh,
# test-stderr-row-qemu.sh). Never run on its own.

# pack_header_offset IMG MACHINE -- the offset of the packed payload's header
# inside a wrapped image, NONE when there is none, AMBIGUOUS when there are
# two. The magic string may also sit in the stub's own .rodata, so a
# candidate counts only if it is followed by version 1 and MACHINE, the PE
# machine number (34404 x64, 43620 aa64).
pack_header_offset() {
    local img="$1" machine="$2" off ver mach found=""
    while IFS=: read -r off _; do
        ver=$(od -An -tu2 -j $((off + 8)) -N2 "$img" | tr -d ' ')
        mach=$(od -An -tu2 -j $((off + 10)) -N2 "$img" | tr -d ' ')
        if [[ "$ver" == 1 && "$mach" == "$machine" ]]; then
            if [[ -n "$found" ]]; then
                echo "AMBIGUOUS"
                return
            fi
            found="$off"
        fi
    done < <(grep -obUa 'AXLPACK1' "$img" || true)
    echo "${found:-NONE}"
}

# pack_corrupt_copy SRC DST MACHINE -- DST is SRC with one byte 256 into the
# LZMA stream flipped: the header intact, the stream damaged. DST is a plain
# copy when SRC carries no single payload, which the caller's cmp catches.
pack_corrupt_copy() {
    local hdr flip_at byte
    cp "$1" "$2"
    hdr=$(pack_header_offset "$2" "$3")
    if [[ "$hdr" =~ ^[0-9]+$ ]]; then
        flip_at=$((hdr + 40 + 256))
        byte=$(od -An -tu1 -j "$flip_at" -N1 "$2" | tr -d ' ')
        printf '%b' "$(printf '\\%03o' $(( byte ^ 0xFF )))" |
            dd of="$2" bs=1 seek="$flip_at" conv=notrunc status=none
    fi
}
