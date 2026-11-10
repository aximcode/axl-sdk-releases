#!/bin/bash
# test-meta: arch=none needs=ninja est=10 local-only=0 builds=ARCH=aa64,hello
# test-pe-set-debug-strtab.sh -- an image carries no COFF string table.
#
# THE DEFECT. aa64's linker scripts name the relocation section `.rela.dyn`
# (load-bearing there: elf_aarch64_efi.lds says why), and `.rela.dyn` is nine
# characters. A PE section header holds eight, so objcopy's ELF -> PE step
# writes the name into a COFF STRING TABLE after the last section, names the
# section `/4` (offset 4 in that table), and sets PointerToSymbolTable to the
# table with NumberOfSymbols ZERO -- `--strip-all` removed the symbols, not
# the table the long name needs. check-pe-stripped flagged every aa64 image
# for it, so the gate ran on x64 only.
#
# THE RULE. PE/COFF spec, section 3.4 (Section Table): "Executable images do
# not use a string table and do not support section names longer than 8
# characters. Long names in object files are truncated if they are emitted to
# an executable file." pe-set-debug (scripts/pe-set-debug.c), which every image goes through (the CMake
# link, axl-cc, the pack wrap), applies it: the long name becomes its first
# eight bytes (`.rela.dy`), PointerToSymbolTable becomes zero, and the table
# (the file's tail) is cut off. Nothing reads a section by name at load time:
# the crt0 finds its relocations through DT_RELA in .dynamic.
#
# THE REAL INPUT: the objcopy step for hello.efi is read off the aa64 tree's
# build graph (`ninja -t commands`) and replayed into this test's directory,
# so the PE under test is exactly what the build hands pe-set-debug.
#
# Host-only. Usage: ./test/integration/test-pe-set-debug-strtab.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

WORK="$(mktemp -d -t axl-pestrtab.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}
finish() {
    echo ""
    echo "pe-set-debug-strtab: $PASS passed, $FAIL failed"
    [[ $FAIL -eq 0 ]]
    exit $?
}

if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH=aa64 hello > "$WORK/build.log" 2>&1; then
    tail -20 "$WORK/build.log"
    check "the aa64 tree builds hello" "failed" "built"
    finish
fi
TREE="$("$PROJECT_DIR/scripts/axl-build.sh" --print-prefix --abs ARCH=aa64)"
PSD="$TREE/build/pe-set-debug"

# The objcopy half of hello.efi's link step: `cd ... && mkdir && ld ...`,
# `cd ... && mkdir && objcopy ... hello.so hello.efi && pe-set-debug ...`.
# Exit 2 when ninja could not answer -- not the same as no such step.
objcopy_cmd() {
    local all
    all="$(ninja -C "$TREE" -t commands hello.efi 2>&1)" || { echo "$all" >&2; return 2; }
    grep -F -- '--output-target=pei-' <<< "$all" | tail -1 \
        | sed -E 's/.* && ([^ ]*objcopy .*) && [^ ]*pe-set-debug .*/\1/'
}
if ! OC="$(objcopy_cmd)"; then
    check "ninja -t commands answers for $TREE" "failed" "answered"
    finish
fi
check "the aa64 graph has hello.efi's objcopy step" \
    "$([[ "$OC" == *objcopy\ * && "$OC" == *" $TREE/hello.efi" ]] && echo found)" "found"
# Replay it into this test's directory: only the output moves.
RAW="$WORK/hello.efi"
OC="${OC% "$TREE/hello.efi"} $RAW"
check "the replay writes only into the test's directory" \
    "$([[ "$OC" == *" $RAW" && "$OC" != *" $TREE/hello.efi"* ]] && echo redirected)" "redirected"
eval "$OC" || { check "the objcopy step replays" "failed" "ran"; finish; }

PES="$SCRIPT_DIR/lib/pe-sections.py"
coff() { python3 "$PES" coff "$1"; }
sections() { python3 "$PES" sections "$1"; }

# CONTROL: the condition exists in what the build hands pe-set-debug, or the
# rest proves nothing.
read -r ptr nsym names <<< "$(coff "$RAW")"
check "control: objcopy's aa64 PE carries a string table and no symbols" \
    "$([[ "$ptr" != 0 ]] && echo set) $nsym" "set 0"
check "control: the relocation section is the long name /4" \
    "$(tr ' ' '\n' <<< "$names" | grep -cx '/4')" "1"
check "control: the string table is the file's tail" \
    "$(( $(stat -c%s "$RAW") - ptr ))" "14"

cp "$RAW" "$WORK/before.efi"
"$PSD" "$RAW" hello.efi > "$WORK/psd.out" 2>&1
check "pe-set-debug succeeds" "$?" "0"
read -r ptr nsym names <<< "$(coff "$RAW")"
check "PointerToSymbolTable and NumberOfSymbols are zero" "$ptr $nsym" "0 0"
check "the long name is truncated to its first eight bytes, as the spec says" \
    "$names" ".text .reloc .data .bss .dynamic .rela.dy .rodata .dynsym .dbgdir"
check "the file ends where the last section's raw data ends" \
    "$(stat -c%s "$RAW")" "$(sections "$RAW" | awk '$1 == "end" {print $2}')"
check "every section's bytes but .dbgdir's are unchanged" \
    "$(sections "$RAW" | grep -v '^end')" "$(sections "$WORK/before.efi" | grep -v '^end')"
check "check-pe-stripped passes it" \
    "$(python3 "$PROJECT_DIR/scripts/check-pe-stripped.py" "$RAW")" \
    "check-pe-stripped: clean — 1 image(s), no COFF symbol table."

cp "$RAW" "$WORK/once.efi"
"$PSD" "$RAW" hello.efi > /dev/null 2>&1
check "a second run changes nothing" "$(cmp "$RAW" "$WORK/once.efi" && echo same)" "same"
check "the patched image is what the tree ships" \
    "$(cmp "$RAW" "$TREE/hello.efi" && echo same)" "same"

# A string table that is NOT the file's tail is a layout nothing here
# produces: refused, never patched in place with bytes left behind.
cp "$WORK/before.efi" "$WORK/tail.efi"
printf 'X' >> "$WORK/tail.efi"
cp "$WORK/tail.efi" "$WORK/tail.orig"
out="$("$PSD" "$WORK/tail.efi" hello.efi 2>&1)"; rc=$?
check "a string table followed by other bytes is refused" "$rc $out" \
    "1 $WORK/tail.efi: COFF string table at 0x$(printf '%x' "$(( $(stat -c%s "$WORK/before.efi") - 14 ))") is not the end of the file -- refusing to cut it"
check "...and the refused file is untouched" \
    "$(cmp "$WORK/tail.efi" "$WORK/tail.orig" && echo same)" "same"

# A table that still has SYMBOLS is check-pe-stripped's to report, not this
# tool's to remove: the symbols' names live in it too.
OC_SYMS="${OC/ --strip-all/}"
OC_SYMS="${OC_SYMS% "$RAW"} $WORK/syms.efi"
check "control: the unstripped replay differs only in --strip-all" \
    "$([[ "$OC_SYMS" != "$OC" && "$OC" == *" --strip-all "* ]] && echo yes)" "yes"
eval "$OC_SYMS" || { check "the unstripped objcopy replays" "failed" "ran"; finish; }
read -r sptr snsym _ <<< "$(coff "$WORK/syms.efi")"
sid="$("$PSD" --print-build-id "$WORK/syms.efi" 2>&1)"
"$PSD" "$WORK/syms.efi" hello.efi > /dev/null 2>&1; rc=$?
read -r aptr ansym _ <<< "$(coff "$WORK/syms.efi")"
check "a symbol table with symbols is left as it was (and the image WAS patched)" \
    "$rc $([[ "$snsym" != 0 ]] && echo has-symbols) $aptr $ansym $([[ "$("$PSD" --print-build-id "$WORK/syms.efi")" != "$sid" ]] && echo id-written)" \
    "0 has-symbols $sptr $snsym id-written"

# A long name that does not index the table, or a table pointer inside a
# section's data: refused by name, the file untouched.
NAMEOFF=$(python3 "$PES" nameoff "$WORK/before.efi" /4)
refused() {   # refused LABEL FILE WANT-MESSAGE
    local out rc
    cp "$2" "$2.orig"
    out="$("$PSD" "$2" hello.efi 2>&1)"; rc=$?
    check "$1" "$rc $out" "1 $3"
    check "...and the refused file is untouched ($1)" "$(cmp "$2" "$2.orig" && echo same)" "same"
}
for bad in '/99' '/x' '/0'; do
    cp "$WORK/before.efi" "$WORK/bad.efi"
    printf '%s\0\0\0\0\0\0' "$bad" | dd of="$WORK/bad.efi" bs=1 seek="$NAMEOFF" count=8 conv=notrunc status=none
    refused "a long name '$bad' that does not index the table is refused" "$WORK/bad.efi" \
        "$WORK/bad.efi: section 5's long name '$bad' does not index the string table -- refusing to cut it"
done
cp "$WORK/before.efi" "$WORK/inside.efi"
lastraw=$(python3 "$PES" lastraw "$WORK/inside.efi")
printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((lastraw & 255)) $((lastraw >> 8 & 255)) $((lastraw >> 16 & 255)) $((lastraw >> 24 & 255)))" \
    | dd of="$WORK/inside.efi" bs=1 seek="$(python3 "$PES" symptr-off "$WORK/inside.efi")" count=4 conv=notrunc status=none
check "control: the planted pointer is the last section's data" \
    "$(coff "$WORK/inside.efi" | cut -d' ' -f1)" "$lastraw"
refused "a table pointer inside a section's data is refused" "$WORK/inside.efi" \
    "$WORK/inside.efi: COFF string table at 0x$(printf '%x' "$lastraw") overlaps section 8's data -- refusing to cut it"

# A consumer that looked the section up by its ELF name stops finding it:
# check-reloc-coverage.py's "present at the wrong size" diagnosis compared the
# PE name with `.rela.dyn` and, once the name was cut, misreported a short
# table as "did not survive objcopy -- add -j .rela.dyn". A shipped image
# (with the .so beside it) whose .rela.dy VirtualSize is cut to 16 bytes:
mkdir -p "$WORK/short"
cp "$TREE/hello.efi" "$TREE/hello.so" "$WORK/short/"
printf '\x10\x00\x00\x00' | dd of="$WORK/short/hello.efi" bs=1 conv=notrunc status=none \
    seek=$(( $(python3 "$PES" nameoff "$WORK/short/hello.efi" .rela.dy) + 8 ))
# The tree's own binutils, as the CTest gate passes them: the host has none
# that read an AArch64 PE. The expected table comes from the .so's own
# dynamic section, read here independently of the script under test.
CROSS_AA64="$(sed -n 's/^AXL_TC_BINUTILS_PREFIX:INTERNAL=//p' "$TREE/CMakeCache.txt")"
check "control: the aa64 tree names its binutils" "$([[ -n "$CROSS_AA64" ]] && echo named)" "named"
dyn() { "${CROSS_AA64}objdump" -p "$WORK/short/hello.so" | awk -v k="$1" '$1 == k { print $2 }'; }
rela=$(printf '%#x' "$(dyn RELA)"); relasz=$(printf '%#x' "$(dyn RELASZ)")
rc=0
out=$(CROSS="$CROSS_AA64" python3 "$PROJECT_DIR/scripts/check-reloc-coverage.py" \
          "$WORK/short/hello.efi" 2>&1) || rc=$?
check "check-reloc-coverage finds a short .rela.dy by its eight-byte PE name" \
    "$rc $(sed -n 2p <<< "$out")" \
    "1   hello.efi: .rela.dyn is present but 0x10 bytes at $rela, not $relasz at $rela"

finish
