#!/bin/bash
# test-meta: arch=none needs= est=4 local-only=0
# test-rsod-decode-pe-map.sh -- rsod-decode.py on the MSVC PE + linker-map path.
#
# WHY THIS EXISTS. Everything this tree builds is ELF with DWARF, and every
# rsod-decode.py path that mattered was tuned for it. The other workflow -- an
# MSVC-built UEFI image shipped as a PE with a `/MAP` beside it, no ELF, no
# `.debug`, no PDB, crashing into a raw serial capture -- had NO test at all,
# and a consumer investigation found eight separate gaps in one afternoon. Two
# of them cost the day:
#
#   * `--detail` printed NO disassembly and said nothing about it. The faulting
#     instruction WAS the answer; objdump reads the PE perfectly well, the tool
#     just drove it against `Image.elf`, which is "" for a PE with no sibling
#     .debug. Silence read as "nothing to show".
#   * Handed a DIFFERENT BUILD of the same source, the tool emitted specific,
#     confident, entirely fictional symbols with no signal anywhere that the
#     image and the dump were unrelated.
#
# The captured data that exposed this is vendor firmware and cannot live here,
# so lib/make-rsod-fixture.py synthesizes an equivalent: a real PE32+ objdump
# can disassemble, an MSVC-shaped map, and a whole PuTTY capture with the dump
# buried in it. Its geometry is chosen to discriminate -- see that file.
#
# WHAT THE ASSERTIONS PIN. Whole rendered LINES, whitespace-squeezed and with
# ANSI stripped: column padding is cosmetic but the content is the contract. A
# substring like "TestFaulty" would still match the exact regressions this
# guards (the RVA-repeated offset, the fabricated frame, the fictional symbol),
# which is the reason this file does not use one.
#
# Host-only: no QEMU, no build. Needs python3 and objdump.
#
# Usage: ./test/integration/test-rsod-decode-pe-map.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
# common-test.sh sets -e, which is wrong for a suite whose subject is a tool's
# EXIT STATUS as much as its output: a decoder that dies on the map-only run
# must be reported as one FAIL among many, not abort the file and take the
# remaining checks' silence with it.
set +e
set -uo pipefail

DECODE="$PROJECT_DIR/scripts/rsod-decode.py"
MAKE_FIXTURE="$SCRIPT_DIR/lib/make-rsod-fixture.py"

WORK="$(mktemp -d -t axl-rsodpe.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

[[ -f "$DECODE" ]] || { echo "  FAIL: $DECODE missing"; exit 1; }
command -v objdump >/dev/null || { echo "SKIP: objdump not on PATH"; exit 0; }

python3 "$MAKE_FIXTURE" "$WORK" --wrong --relocated 0x7E120000 \
    || { echo "  FAIL: fixture build"; exit 1; }

APP="$WORK/app.efi"
MAP="$WORK/app.map"
WRONG="$WORK/wrong-build.efi"
LOG="$WORK/console.log"
RELOG="$WORK/console-reloc.log"

# ── assertion helpers ─────────────────────────────────────────
#
# Compare whole lines, not substrings. `norm` strips ANSI and squeezes runs of
# whitespace so the padding may be retuned without silently voiding the check.

norm() { sed -e 's/\x1b\[[0-9;]*m//g' -e 's/[[:space:]]\+/ /g' -e 's/^ //' -e 's/ $//'; }

# run_decode <outfile> <label> [args...] -- run the decoder and REQUIRE that it
# actually produced a report.
#
# Every `refute_` below is worthless without this. A decoder that dies on an
# unknown flag writes an empty file, and an empty file refutes everything: the
# first draft of this suite reported seven green refutes against runs that had
# never started. "The tool could not run" and "the tool ran and found nothing"
# are the same empty string and opposite facts, so the exit status and the
# report header are both checked before any assertion reads the file.
run_decode() {
    local out="$1" label="$2"; shift 2
    # `timeout`, because the decoder reads stdin when given no dump text and
    # stdin is not a tty -- which is every script, this one included. That is a
    # HANG, and run-integration.sh runs a parallel pool under one clock, so an
    # unbounded invocation here starves every test after it. Bounded, the same
    # regression is one loud FAIL.
    timeout 60 python3 "$DECODE" "$@" > "$out.raw" 2>"$out.err"
    local rc=$?
    norm < "$out.raw" > "$out"
    if [[ $rc -ne 0 ]]; then
        test_host_fail "$label (decoder exited $rc)"
        sed 's/^/      /' "$out.err" | head -3
        return 1
    fi
    if ! grep -q '^RSOD Decoder' "$out"; then
        test_host_fail "$label (no report emitted)"
        return 1
    fi
    test_host_pass "$label"
    return 0
}

# assert_line <file> <exact line> <label>
assert_line() {
    if grep -qxF -- "$2" "$1"; then
        test_host_pass "$3"
    else
        test_host_fail "$3"
        echo "      wanted line: $2"
    fi
}

# An absent thing is only absent from output that EXISTS. Nothing is missing
# from an empty file, so a refute over one is a green light with no lamp behind
# it -- the second thing this suite got wrong about itself.
_have_output() {
    [[ -s "$1" ]] && return 0
    test_host_fail "$2 (no output to check)"
    return 1
}

# refute_line <file> <exact line> <label>
refute_line() {
    _have_output "$1" "$3" || return
    if grep -qxF -- "$2" "$1"; then
        test_host_fail "$3"
        echo "      unwanted line present: $2"
    else
        test_host_pass "$3"
    fi
}

# assert_match <file> <grep -E pattern> <label>  -- for a line of this SHAPE.
# Used where an exact line would pin formatting that is not the point (a
# warning that embeds several hex numbers, say); assert_line stays the default
# so a reworded banner still fails loudly.
assert_match() {
    _have_output "$1" "$3" || return
    if grep -qE -- "$2" "$1"; then
        test_host_pass "$3"
    else
        test_host_fail "$3"
        echo "      wanted match: $2"
    fi
}

# refute_match <file> <grep -E pattern> <label>  -- for "no line of this SHAPE"
refute_match() {
    _have_output "$1" "$3" || return
    if grep -qE -- "$2" "$1"; then
        test_host_fail "$3"
        echo "      unwanted match: $(grep -m1 -E -- "$2" "$1")"
    else
        test_host_pass "$3"
    fi
}

echo "=== rsod-decode PE + .map workflow ==="
echo ""

# ═══════════════════════════════════════════════════════════════
# The headline run: everything the fixture's README calls "fixed" comes from
# this single command -- no :BASE, a PE whose only symbols are the map.
# ═══════════════════════════════════════════════════════════════
echo "-- PE + map, base inferred, --detail --"
OUT="$WORK/out-detail.txt"
run_decode "$OUT" "PE + map + --detail run completes" --image "$APP" --rsod "$LOG" --detail

# S4: the base is stated by the PE header, the map and the dump's image list.
# Requiring :BASE when all three agree is the tool refusing to read its inputs.
assert_line "$OUT" "#0 ?TestFaulty@@YAPEAXPEAI@Z + 0xa [0x14002100a + 0x2100a]" \
    "S4 base inferred with no :BASE (frame resolves)"
refute_match "$OUT" 'faulting PC .. invalid address|faulting PC -- invalid address' \
    "S4 faulting PC is not called invalid"

# S5: `+ 0xa` is how far into the function the fault landed. The old output
# repeated the RVA, which the reader already had on the line.
assert_line "$OUT" "#0 ?TestFaulty@@YAPEAXPEAI@Z + 0xa [0x14002100a + 0x2100a]" \
    "S5 function + offset, not the RVA repeated"

# S1: the whole investigation. objdump reads the PE; the tool never asked it to.
assert_line "$OUT" "Disassembly:" "S1 --detail emits a disassembly block"
assert_line "$OUT" ">>> 14002100a: mov %gs:0x58,%rax" \
    "S1 faulting instruction shown and marked"
assert_line "$OUT" "140021000: mov %rbx,0x8(%rsp)" \
    "S1 disassembly starts at the function entry from the map"
# Bounded trailing context, cut on an instruction boundary. Stopping at a fixed
# byte count lands objdump mid-instruction and it renders the tail as `.byte`,
# which reads like the disassembler lost its place at the crash site.
assert_line "$OUT" "14002102b: pop %rdi" "S1 disassembly keeps 4 trailing instructions"
refute_line "$OUT" "14002102c: ret" "S1 disassembly stops after those 4"
refute_match "$OUT" '\.byte |\(bad\)' "S1 no mid-instruction truncation artifact"

# S7: exact, free, and better than any heuristic -- the recorded branch target
# IS the faulting function's entry, which is what proves the fault happened on
# its first call, in the prologue.
assert_line "$OUT" "Branch records (last branch taken, most recent first):" \
    "S7 branch records section present"
assert_line "$OUT" "from ?TestCaller@@YAHH@Z + 0x4e [0x14002004e]" \
    "S7 branch source resolved to the caller"
assert_line "$OUT" "to ?TestFaulty@@YAPEAXPEAI@Z + 0x0 [0x140021000] (function entry)" \
    "S7 branch target resolved, entry called out"

# S6: BP is ODD -- it cannot be a frame pointer. The walk followed it anyway
# and printed a fabricated frame ABOVE the stack scan, which was correct.
refute_match "$OUT" '^#1 ' "S6 no fabricated frame from the odd BP"
refute_match "$OUT" 'outside all known images' "S6 no fictional out-of-image frame"
refute_match "$OUT" 'recovered via frame-pointer chain' \
    "S6 no heuristic trace claimed from an unusable BP"

# S6b: the stack SCAN reconstructed a genuine chain and was demoted below the
# heuristic AND hidden behind --detail. With no frame list it is the best
# evidence there is, so it leads.
assert_line "$OUT" "Stack scan (return addresses found in the stack dump):" \
    "S6b stack scan section present"
assert_line "$OUT" "?TestCaller@@YAHH@Z + 0x53 [0x140020053] (stack scan)" \
    "S6b stack scan resolves the caller's return address"

# S3 (negative control): the RIGHT image must NOT be accused.
refute_match "$OUT" 'IMAGE DOES NOT MATCH' "S3 correct image raises no mismatch warning"

# The scan's promotion has to be checked on a run WITHOUT --detail, or the
# assertions above prove only that it still appears where it always did. A
# sabotage that put it back behind the flag went undetected until this ran.
echo ""
echo "-- same dump, no --detail --"
OUTP="$WORK/out-plain.txt"
run_decode "$OUTP" "plain run completes" --image "$APP" --rsod "$LOG"
assert_line "$OUTP" "Stack scan (return addresses found in the stack dump):" \
    "S6b stack scan leads without --detail when there is no frame list"
assert_line "$OUTP" "?TestCaller@@YAHH@Z + 0x53 [0x140020053] (stack scan)" \
    "S6b stack scan resolves the caller without --detail"

# ═══════════════════════════════════════════════════════════════
# S3 -- the one that burned the day.
# ═══════════════════════════════════════════════════════════════
echo ""
echo "-- wrong-but-plausible image --"
OUTW="$WORK/out-wrong.txt"
run_decode "$OUTW" "wrong-image run completes" --image "$WRONG:0x140000000" --rsod "$LOG"

assert_line "$OUTW" "!! IMAGE DOES NOT MATCH THE DUMP - symbols below are probably fiction:" \
    "S3 mismatch is announced loudly at the top"
assert_line "$OUTW" "- SizeOfImage is 0x40000, but the dump's loaded-image list records 0x30000 at base 0x140000000" \
    "S3 size gate names both sizes and the base"
assert_line "$OUTW" "- faulting PC 0x14002100a does not land on an instruction boundary" \
    "S3 instruction-boundary gate fires"
# The fictional symbol is still printed -- the tool answers, but no longer
# silently. What must never happen again is printing it with NO signal.
assert_line "$OUTW" "#0 ?WrongFunc@@YAXXZ + 0xa [0x14002100a + 0x2100a]" \
    "S3 the fictional symbol is still shown, but now flagged"

# ═══════════════════════════════════════════════════════════════
# S2 -- SizeOfImage. Every address in this fixture is past the old 128 KB
# guess, so a decoder that never reads the PE header rejects all of them.
# ═══════════════════════════════════════════════════════════════
echo ""
echo "-- SizeOfImage read from the PE (no map) --"
NOMAP="$WORK/nomap"; mkdir -p "$NOMAP"; cp "$APP" "$NOMAP/app.efi"
OUTN="$WORK/out-nomap.txt"
run_decode "$OUTN" "image-only run completes" --image "$NOMAP/app.efi:0x140000000" --rsod "$LOG" --detail

refute_match "$OUTN" 'faulting PC .. invalid address|outside all known images' \
    "S2 PC past 128 KB is inside the image"
# S4 (image alone): no symbols, but the faulting instruction is still the answer.
assert_line "$OUTN" ">>> 14002100a: mov %gs:0x58,%rax" \
    "S4 image-only run still disassembles"

# ═══════════════════════════════════════════════════════════════
# S4 -- a map alone holds every symbol AND the preferred load address.
# ═══════════════════════════════════════════════════════════════
echo ""
echo "-- map alone --"
OUTM="$WORK/out-map.txt"
run_decode "$OUTM" "map-only run completes" --map "$MAP" --rsod "$LOG"

assert_line "$OUTM" "#0 ?TestFaulty@@YAPEAXPEAI@Z + 0xa [0x14002100a + 0x2100a]" \
    "S4 map-only run resolves the frame"
refute_match "$OUTM" 'unrecognized file type' "S4 a .map is not rejected"

# A .map handed to --image must work too -- the report named --image as the
# thing a user reaches for, and rejecting the file outright is the failure.
OUTMI="$WORK/out-map-image.txt"
run_decode "$OUTMI" "map-as---image run completes" --image "$MAP" --rsod "$LOG"
assert_line "$OUTMI" "#0 ?TestFaulty@@YAPEAXPEAI@Z + 0xa [0x14002100a + 0x2100a]" \
    "S4 a .map is accepted as --image too"

# A map-only run cannot disassemble; say so rather than printing nothing.
OUTMD="$WORK/out-map-detail.txt"
run_decode "$OUTMD" "map-only --detail run completes" --map "$MAP" --rsod "$LOG" --detail
assert_line "$OUTMD" "Disassembly: unavailable (no image file for this module -- pass --image)" \
    "S1 --detail explains an absent disassembly instead of staying silent"

# ═══════════════════════════════════════════════════════════════
# S4 -- a RELOCATED image. Where the dump's module list disagrees with the PE
# header, the module list is the truth: the header states where the image WANTED
# to go and the firmware states where it went. Getting this backwards resolves
# every address to a symbol that is off by the relocation delta -- wrong, and
# wrong in a way that still looks like a plausible function name.
# ═══════════════════════════════════════════════════════════════
echo ""
echo "-- relocated image (dump base overrides the PE header) --"
OUTR="$WORK/out-reloc.txt"
run_decode "$OUTR" "relocated run completes" --image "$APP" --rsod "$RELOG"

assert_line "$OUTR" "#0 ?TestFaulty@@YAPEAXPEAI@Z + 0xa [0x7e14100a + 0x2100a]" \
    "S4 relocated image resolves at the dump's base, not the PE header's"
refute_match "$OUTR" 'IMAGE DOES NOT MATCH' \
    "S3 a relocated image is not mistaken for the wrong image"

# Disassembling a relocated image means mapping the runtime address back
# through the preferred base; get that wrong and objdump decodes empty space.
OUTRD="$WORK/out-reloc-detail.txt"
run_decode "$OUTRD" "relocated --detail run completes" \
    --image "$APP" --rsod "$RELOG" --detail
assert_line "$OUTRD" ">>> 14002100a: mov %gs:0x58,%rax" \
    "S1 relocated image disassembles at its LINK address"

# --dump had the same shape of bug as the disassembler: it drove nm with
# `img.elf`, so a map-only image dumped an empty section instead of the symbols
# it plainly holds. A map-only run also needs no architecture, and refusing to
# pick one made `--map x.map --dump` impossible.
echo ""
echo "-- --dump from a map alone --"
DUMPO="$WORK/dump.txt"
if timeout 60 python3 "$DECODE" --map "$MAP" --dump > "$DUMPO.raw" 2>"$DUMPO.err"; then
    norm < "$DUMPO.raw" > "$DUMPO"
    test_host_pass "map-only --dump completes with no --arch"
    assert_line "$DUMPO" "0x0000000000021000 ?TestFaulty@@YAPEAXPEAI@Z" \
        "map-only --dump lists symbols at their RVA"
    refute_match "$DUMPO" '0x-' "no negative RVA from absolute map symbols"
else
    test_host_fail "map-only --dump completes with no --arch"
    sed 's/^/      /' "$DUMPO.err" | head -3
fi

# ═══════════════════════════════════════════════════════════════
# PDB as a symbol source (P1-P3).
#
# On MSVC the PDB is the ONLY route to source lines -- /MAPINFO:LINES is a
# fatal error on 14.36 (LNK1117), so a map cannot carry them. 4.3.2 resolved
# PDB lines already, but only by letting llvm-addr2line DISCOVER the file
# under the basename the PE embeds, and said nothing when that failed. A
# renamed PDB is the normal case, not an edge case: release artifacts get
# versioned names when they are archived, so the PDB sits right there, matched
# and unused, and the reader sees a decode with no lines and no reason given.
#
# Line RESOLUTION is not tested here -- that needs a real MSVC PDB, which this
# tree cannot build. The synthetic .pdb is a valid MSF carrying a real
# CodeView identity and nothing else, which is exactly enough to exercise
# every DECISION: which file to use, whether to refuse it, and what to say.
# ═══════════════════════════════════════════════════════════════
echo ""
echo "-- PDB as a symbol source --"
PD="$WORK/pdb"; mkdir -p "$PD"
python3 "$MAKE_FIXTURE" "$PD" --pdb >/dev/null 2>&1 \
    || test_host_fail "PDB fixture builds"
PAPP="$PD/app.efi"
PLOG="$PD/console.log"

# The PE must actually carry the CodeView record the rest of this depends on.
if "$DECODE" --image "$PAPP" --rsod "$PLOG" --detail 2>&1 >/dev/null \
        | grep -q 'app.pdb'; then
    test_host_pass "the PE's embedded PDB name is read"
else
    test_host_fail "the PE's embedded PDB name is read"
fi

# The report must say HOW the PDB was found, not merely that one was. Without
# this the two discovery paths are indistinguishable in the output, and
# disabling the exact-name lookup changed nothing detectable -- the GUID scan
# quietly found the same file, so the fast path was untested.
OUTB="$WORK/out-pdb-beside.txt"
run_decode "$OUTB" "PDB beside the image, embedded name" \
    --image "$PAPP" --rsod "$PLOG" --detail
assert_line "$OUTB" "Symbol sources: app.map (map, linked 0x6a8f3db9), app.pdb (PDB, beside the image)" \
    "P3 a PDB under the embedded name is found by name, not by scan"

# P2/B: the PDB is present under a DIFFERENT name. 4.3.2 resolved nothing and
# said nothing; the file is right there.
mv "$PD/app.pdb" "$PD/app-1.2.3.efi.pdb"

# P1/A: --pdb names it explicitly.
OUTP="$WORK/out-pdb-named.txt"
run_decode "$OUTP" "--pdb names a PDB" \
    --image "$PAPP" --pdb "$PD/app-1.2.3.efi.pdb" --rsod "$PLOG" --detail
assert_line "$OUTP" "Symbol sources: app.map (map, linked 0x6a8f3db9), app-1.2.3.efi.pdb (PDB, named)" \
    "P1 the named PDB is reported as the symbol source"

# P3/C: with no --pdb, a renamed PDB is still found -- by CodeView GUID/age,
# not by filename. This is the case that makes --pdb unnecessary in practice.
OUTD="$WORK/out-pdb-guid.txt"
run_decode "$OUTD" "renamed PDB run completes" \
    --image "$PAPP" --rsod "$PLOG" --detail
assert_line "$OUTD" "Symbol sources: app.map (map, linked 0x6a8f3db9), app-1.2.3.efi.pdb (PDB, matched by GUID)" \
    "P3 a renamed PDB is found by its CodeView GUID"

# P3/C: a PDB from a DIFFERENT build must be refused, not trusted. Same class
# of protection as the image/dump gates -- a wrong PDB gives wrong LINES,
# which are believed precisely because they are so specific.
OUTM="$WORK/out-pdb-mismatch.txt"
run_decode "$OUTM" "mismatched PDB run completes" \
    --image "$PAPP" --pdb "$PD/mismatched.pdb" --rsod "$PLOG" --detail
assert_line "$OUTM" "!! PDB DOES NOT MATCH THE IMAGE - ignoring it for line numbers:" \
    "P3 a mismatched PDB is refused loudly"
refute_match "$OUTM" 'mismatched\.pdb \(PDB' \
    "P3 a refused PDB is not then used as a symbol source"

# P2/B: with no PDB at all, say so and say why -- the silent case.
rm -f "$PD/app-1.2.3.efi.pdb" "$PD/mismatched.pdb"
OUTN="$WORK/out-pdb-absent.txt"
run_decode "$OUTN" "no-PDB run completes" --image "$PAPP" --rsod "$PLOG" --detail
assert_line "$OUTN" "No PDB: image embeds 'app.pdb', not found beside the image - no line numbers" \
    "P2 an absent PDB is explained, not silent"
assert_line "$OUTN" "Symbol sources: app.map (map, linked 0x6a8f3db9)" \
    "P2 the sources actually used are named"

# A .pdb is NOT self-sufficient the way a .map is -- it carries no section
# table, and llvm-symbolizer refuses it as an object outright. Naming one with
# no image must say that, rather than accepting the run and printing a report
# with no symbols in it, which is what "accept a .pdb like a .map" first did.
python3 "$MAKE_FIXTURE" "$PD" --pdb >/dev/null 2>&1
POERR="$WORK/pdb-only.err"
if timeout 60 python3 "$DECODE" --image "$PD/app.pdb" --rsod "$PLOG" \
        >/dev/null 2>"$POERR"; then
    test_host_fail "a .pdb alone is refused, not silently useless"
else
    if grep -q 'resolves lines only alongside its image' "$POERR"; then
        test_host_pass "a .pdb alone is refused, not silently useless"
    else
        test_host_fail "a .pdb alone is refused with a useful message"
        sed 's/^/      /' "$POERR" | head -2
    fi
fi
POERR2="$WORK/pdb-flag-only.err"
if timeout 60 python3 "$DECODE" --pdb "$PD/app.pdb" --rsod "$PLOG" \
        >/dev/null 2>"$POERR2"; then
    test_host_fail "--pdb with no --image is refused"
elif grep -q 'needs the image it belongs to' "$POERR2"; then
    test_host_pass "--pdb with no --image is refused"
else
    test_host_fail "--pdb with no --image is refused with a useful message"
fi

# ═══════════════════════════════════════════════════════════════
# Real MSVC PE + PDB, opt-in.
#
# Everything above proves which PDB gets CHOSEN. None of it proves a line
# number is right, because a PDB that resolves lines cannot be synthesized
# without MSVC -- the fixture's .pdb is a valid MSF carrying an identity and
# no line table at all, deliberately, so that it cannot appear to pass this.
#
# Point RSOD_PDB_FIXTURE at a directory holding a matched PE/PDB/map trio and
# a dump, plus RSOD_PDB_EXPECT set to the `file:line` the faulting frame must
# resolve to. No default path: the corpus lives outside the repo, and baking
# in someone's home directory would make this test pass on one machine and
# skip everywhere else while looking identical.
# ═══════════════════════════════════════════════════════════════
echo ""
echo "-- real MSVC PDB (opt-in) --"
if [[ -z "${RSOD_PDB_FIXTURE:-}" ]]; then
    echo "  SKIP: RSOD_PDB_FIXTURE not set (real MSVC PE+PDB line numbers)"
elif [[ ! -d "$RSOD_PDB_FIXTURE" ]]; then
    test_host_fail "RSOD_PDB_FIXTURE is a directory"
elif [[ -z "${RSOD_PDB_EXPECT:-}" ]]; then
    test_host_fail "RSOD_PDB_EXPECT names the expected file:line"
else
    RF="$WORK/realpdb"; rm -rf "$RF"; cp -r "$RSOD_PDB_FIXTURE" "$RF"
    R_EFI=$(ls "$RF"/*.efi 2>/dev/null | head -1)
    R_PDB=$(ls "$RF"/*.pdb 2>/dev/null | head -1)
    R_LOG=$(ls "$RF"/*rsod*.log "$RF"/*.log 2>/dev/null | head -1)
    if [[ -z "$R_EFI" || -z "$R_PDB" || -z "$R_LOG" ]]; then
        test_host_fail "fixture has a .efi, a .pdb and a dump"
    else
        test_host_pass "fixture has a .efi, a .pdb and a dump"

        # (i) discovered under the name the PE embeds.
        OUTR1="$WORK/out-realpdb-1.txt"
        run_decode "$OUTR1" "real PDB, discovered" --image "$R_EFI" --rsod "$R_LOG"
        if grep -qF -- "$RSOD_PDB_EXPECT" "$OUTR1"; then
            test_host_pass "real PDB resolves $RSOD_PDB_EXPECT"
        else
            test_host_fail "real PDB resolves $RSOD_PDB_EXPECT"
            grep -m1 '^#0 ' "$OUTR1" | sed 's/^/      got: /'
        fi

        # (ii) RENAMED, with no --pdb. This is the reported gap: archived
        # artifacts get versioned names, and the file is then present, matched
        # and silently unused. Found by CodeView identity, not by filename.
        mv "$R_PDB" "$RF/renamed-9.9.9.efi.pdb"
        OUTR2="$WORK/out-realpdb-2.txt"
        run_decode "$OUTR2" "real PDB, renamed" --image "$R_EFI" --rsod "$R_LOG"
        if grep -qF -- "$RSOD_PDB_EXPECT" "$OUTR2"; then
            test_host_pass "a renamed real PDB still resolves, unaided"
        else
            test_host_fail "a renamed real PDB still resolves, unaided"
            grep -m1 '^#0 ' "$OUTR2" | sed 's/^/      got: /'
        fi

        # (iii) named explicitly.
        OUTR3="$WORK/out-realpdb-3.txt"
        run_decode "$OUTR3" "real PDB, named" \
            --image "$R_EFI" --pdb "$RF/renamed-9.9.9.efi.pdb" --rsod "$R_LOG"
        if grep -qF -- "$RSOD_PDB_EXPECT" "$OUTR3"; then
            test_host_pass "--pdb resolves the renamed real PDB"
        else
            test_host_fail "--pdb resolves the renamed real PDB"
            grep -m1 '^#0 ' "$OUTR3" | sed 's/^/      got: /'
        fi

        # (iv) absent: no lines, and the reason said out loud.
        mkdir -p "$RF/nopdb" && cp "$R_EFI" "$R_LOG" "$RF/nopdb/" 2>/dev/null
        cp "$RF"/*.map "$RF/nopdb/" 2>/dev/null || true
        OUTR4="$WORK/out-realpdb-4.txt"
        run_decode "$OUTR4" "real image, no PDB" \
            --image "$RF/nopdb/$(basename "$R_EFI")" \
            --rsod "$RF/nopdb/$(basename "$R_LOG")"
        refute_match "$OUTR4" "$(printf '%s' "$RSOD_PDB_EXPECT" | sed 's/[].[^$\\*]/\\&/g')" \
            "no PDB means no line numbers"
        if grep -qE '^No PDB: image embeds .* - no line numbers$' "$OUTR4"; then
            test_host_pass "the absent PDB is explained by name"
        else
            test_host_fail "the absent PDB is explained by name"
        fi
    fi
fi

# ═══════════════════════════════════════════════════════════════
# S8 + the --file/--rsod rename.
# ═══════════════════════════════════════════════════════════════
echo ""
echo "-- CLI surface --"
HELP="$WORK/help.txt"
timeout 60 python3 "$DECODE" --help 2>/dev/null | norm > "$HELP"

assert_line "$HELP" "# PE with a sibling linker map - no ELF, no PDB" \
    "S8 help shows the PE + map workflow"
assert_line "$HELP" "rsod-decode.py --map app.map --rsod console.log" \
    "S8 help shows a map-only run"
assert_line "$HELP" "# Raw terminal capture: the dump is embedded in unrelated console output" \
    "S8 help states --rsod may be a whole console capture"

# `--rsod` is the name now; `--file` keeps working so existing scripts do not
# break on a patch release.
OUTF="$WORK/out-file-alias.txt"
run_decode "$OUTF" "--file alias run completes" --image "$APP" --file "$LOG"
assert_line "$OUTF" "#0 ?TestFaulty@@YAPEAXPEAI@Z + 0xa [0x14002100a + 0x2100a]" \
    "--file still accepted as an alias for --rsod"

# ═══════════════════════════════════════════════════════════════
# The mismatch has to survive the machine-readable path too, or a CI consumer
# gets the fiction with none of the signal.
# ═══════════════════════════════════════════════════════════════
echo ""
echo "-- json --"
JSONW="$WORK/wrong.json"
timeout 60 python3 "$DECODE" --image "$WRONG:0x140000000" --rsod "$LOG" --json 2>/dev/null > "$JSONW"
if python3 -c "
import json,sys
d = json.load(open('$JSONW'))
w = d.get('image_warnings') or []
sys.exit(0 if any('SizeOfImage' in x for x in w) else 1)
" 2>/dev/null; then
    test_host_pass "S3 mismatch reaches --json as image_warnings"
else
    test_host_fail "S3 mismatch reaches --json as image_warnings"
fi

# ═══════════════════════════════════════════════════════════════
# The ELF/DWARF path is what everything in THIS tree produces, and it had no
# test either. It is not what the PE work changed, which is exactly why it
# needs one: a refactor of the shared resolver, the shared disassembler or the
# shared frame renderer breaks it silently and no fixture here would notice.
# ═══════════════════════════════════════════════════════════════
echo ""
echo "-- ELF/DWARF path still works --"
if ! command -v gcc >/dev/null; then
    echo "  SKIP: gcc not on PATH (ELF regression guard)"
else
    cat > "$WORK/elfprobe.c" <<'EOF'
int deep_function(int x) { volatile int *p = 0; return *p + x; }
int middle_function(int x) { return deep_function(x + 1); }
int main(void) { return middle_function(41); }
EOF
    # -no-pie so the link addresses are absolute and stable, as a UEFI .debug's
    # are; the RSOD below is written against them.
    if ! gcc -g -O0 -no-pie -o "$WORK/elfprobe" "$WORK/elfprobe.c" 2>/dev/null; then
        test_host_fail "ELF probe builds"
    else
        FAULT_VA=$(nm "$WORK/elfprobe" | awk '$3 == "deep_function" { print $1 }')
        FAULT_VA=$((0x$FAULT_VA + 0xf))     # into the body, past the prologue
        CALLER_VA=$(nm "$WORK/elfprobe" | awk '$3 == "middle_function" { print $1 }')
        cat > "$WORK/elf-rsod.txt" <<EOF
!!!! X64 Exception Type - 0e(#PF)  CPU Apic ID - 00000000 !!!!
RIP  - $(printf '%016X' $FAULT_VA), CS  - 0000000000000038
RAX  - 0000000000000000, RCX - ${CALLER_VA}
CR2 - 0000000000000000
EOF
        OUTE="$WORK/out-elf.txt"
        run_decode "$OUTE" "ELF run completes" \
            --image "$WORK/elfprobe:0x0" --rsod "$WORK/elf-rsod.txt" --detail

        # DWARF gives what a map never can: the exact source line.
        # The path is rendered shortened and lives under a mktemp dir, so it is
        # the one field here that cannot be pinned literally.
        if grep -qE '^#0 deep_function \S*elfprobe\.c:1 \[0x[0-9a-f]+ \+ 0x[0-9a-f]+\]$' "$OUTE"; then
            test_host_pass "ELF frame resolves to function + source line"
        else
            test_host_fail "ELF frame resolves to function + source line"
            grep -m1 '^#0 ' "$OUTE" | sed 's/^/      got: /'
        fi
        assert_line "$OUTE" "Cause: Page fault — NULL pointer dereference" \
            "ELF diagnosis still reads CR2"
        assert_line "$OUTE" "Disassembly:" "ELF --detail still disassembles"
        refute_match "$OUTE" 'IMAGE DOES NOT MATCH' \
            "an ELF with no dump image list is not accused of mismatching"

        # ── ELF WITH A SIBLING .map, on the binutils path ──────────
        #
        # An EDK2 build leaves a .map next to the module, so this pairing is
        # ordinary, not exotic. It is also the one case that distinguishes "a
        # PE, addressed against its link base" from "an ELF, already addressed
        # the way the offset is" -- and a shared helper that keyed off
        # `efi_path` alone got it wrong, adding the map's preferred base to an
        # ELF address and decoding empty space.
        #
        # The failing `gdb` shim is load-bearing twice over: gdb handles this
        # case itself and masks the bug entirely, AND a gdb that answers
        # nothing is what proves the objdump fallback is reached at all --
        # which a bare `return` used to prevent.
        mkdir -p "$WORK/nogdb"
        printf '#!/bin/sh\nexit 1\n' > "$WORK/nogdb/gdb"
        chmod +x "$WORK/nogdb/gdb"

        ELFMAP="$WORK/elfmap"; mkdir -p "$ELFMAP"
        cp "$WORK/elfprobe" "$ELFMAP/probe.so"
        FN_VA=$(nm "$WORK/elfprobe" | awk '$3 == "deep_function" { print $1 }')
        cat > "$ELFMAP/probe.map" <<EOF
 probe

 Preferred load address is 0000000000400000

  Address         Publics by Value              Rva+Base               Lib:Object

 0001:00000144       deep_function              ${FN_VA} f   probe.obj
EOF
        OUTEM="$WORK/out-elfmap.txt"
        PATH="$WORK/nogdb:$PATH" timeout 60 python3 "$DECODE" \
            --image "$ELFMAP/probe.so:0x0" --rsod "$WORK/elf-rsod.txt" --detail \
            > "$OUTEM.raw" 2>"$OUTEM.err"
        norm < "$OUTEM.raw" > "$OUTEM"
        if grep -q '^RSOD Decoder' "$OUTEM"; then
            test_host_pass "ELF + sibling .map run completes (no gdb)"
        else
            test_host_fail "ELF + sibling .map run completes (no gdb)"
            sed 's/^/      /' "$OUTEM.err" | head -3
        fi
        assert_line "$OUTEM" "Disassembly:" \
            "an ELF with a sibling .map still disassembles"
        refute_match "$OUTEM" 'Disassembly: unavailable' \
            "the map's preferred base is not added to an ELF address"
        refute_match "$OUTEM" 'IMAGE DOES NOT MATCH' \
            "an ELF with a sibling .map is not accused of mismatching"

        # ── AXL's OWN crash-report format ─────────────────────────
        #
        # drivers/crashhandler/report.c writes this, and it is the format the
        # decoder read WORST: the registers and the faulting PC came through
        # (they look like the Dell `REG=VALUE` shape) and everything that makes
        # the report worth writing did not -- the `Image:` line stating the
        # load base, the `Loaded Images:` table, and every frame of the `Stack
        # Trace:` section. The report's own last line tells the reader to run
        # this script.
        #
        # test-crashhandler.sh proves this end to end against a REAL crash, but
        # it needs QEMU and a built tree. This is the same claim in 2 seconds.
        #
        # The probe is relinked LOW here on purpose: a UEFI image is linked at
        # 0 and loaded high, so `offset = addr - base` is what indexes its
        # DWARF. A host binary linked at 0x400000 cannot model that.
        if gcc -g -O0 -nostdlib -e main -Wl,-Ttext=0x1000 \
                -o "$WORK/probelow" "$WORK/elfprobe.c" 2>/dev/null; then
            test_host_pass "low-linked probe builds (UEFI-shaped)"
            # The frame addresses must be REAL instruction boundaries.
            # Picking round offsets (+0x10, +0x8) put both mid-instruction and
            # the image/dump validator correctly called the report fiction --
            # the gate was right and the fixture was wrong, which is the good
            # way round, but only visible because the gate exists.
            insn_at() {   # <symbol> <1-based instruction index> -> hex offset
                objdump -d --no-show-raw-insn "$WORK/probelow" \
                    | awk -v sym="<$1>:" -v n="$2" '
                        index($0, sym) { f = 1; next }
                        f && /^$/ { exit }
                        f && /^ *[0-9a-f]+:/ { c++; if (c == n) {
                            sub(":", "", $1); print $1; exit } }'
            }
            CR_BASE=$((0x7E120000))
            OFF0=$((0x$(insn_at deep_function 5)))
            OFF1=$((0x$(insn_at middle_function 3)))
            PC0=$((CR_BASE + OFF0))
            PC1=$((CR_BASE + OFF1))
            cat > "$WORK/crash-report.txt" <<EOF
UEFI Crash Report
==================
Exception:    #UD (Invalid Opcode) at $(printf '0x%016X' $PC0)
Image:        probelow (base 0x7E120000, size 0x10000)
Offset:       $(printf '0x%X' $OFF0)
Architecture: X64

Registers:
  RAX=0000000000000000  RBX=0000000000000000
  RIP=$(printf '%016X' $PC0)  RFLAGS=0000000000010246
  CR2=0000000000000000  ErrCode=0000000000000000

Stack Trace:
  $(printf '0x%016X' $PC0)  probelow+$(printf '0x%X' $OFF0)
  $(printf '0x%016X' $PC1)  probelow+$(printf '0x%X' $OFF1)
  0x0000000000000000  ???

Loaded Images:
  Base               Size       Name
  0x000000007E120000 0x010000  probelow
  0x0000000047683000 0x0D2000  DxeCore

Decode with debug symbols:
  rsod-decode.py --image <build>/probelow.so --rsod crash-report.txt
EOF
            # No :BASE and no --base -- the report states the base twice over.
            OUTCR="$WORK/out-crashreport.txt"
            PATH="$WORK/nogdb:$PATH" timeout 60 python3 "$DECODE" \
                --image "$WORK/probelow" --rsod "$WORK/crash-report.txt" --detail \
                > "$OUTCR.raw" 2>"$OUTCR.err"
            norm < "$OUTCR.raw" > "$OUTCR"
            if grep -q '^RSOD Decoder' "$OUTCR"; then
                test_host_pass "AXL crash report decodes"
            else
                test_host_fail "AXL crash report decodes"
                sed 's/^/      /' "$OUTCR.err" | head -3
            fi
            # The base came from the report, so the frames resolve; without it
            # every address here is "outside all known images".
            if grep -qE "^#0 deep_function \S*elfprobe\.c:1 \[$(printf '0x%x' $PC0) \+ $(printf '0x%x' $OFF0)\]\$" "$OUTCR"; then
                test_host_pass "crash-report base inferred; frame 0 resolves"
            else
                test_host_fail "crash-report base inferred; frame 0 resolves"
                grep -m1 '^#0 ' "$OUTCR" | sed 's/^/      got: /'
            fi
            # The `Stack Trace:` section is a REAL frame list. Dropping it left
            # the FP-chain fallback to invent a one-frame trace instead.
            if grep -qE "^#1 middle_function \S*elfprobe\.c:2 \[$(printf '0x%x' $PC1) \+ $(printf '0x%x' $OFF1)\]\$" "$OUTCR"; then
                test_host_pass "crash-report stack trace is rendered, not re-derived"
            else
                test_host_fail "crash-report stack trace is rendered, not re-derived"
                grep -m1 '^#1 ' "$OUTCR" | sed 's/^/      got: /'
            fi
            refute_match "$OUTCR" 'IMAGE DOES NOT MATCH' \
                "the image the crash report names is not flagged as wrong"
            refute_match "$OUTCR" 'recovered via frame-pointer chain' \
                "a real frame list is not replaced by the FP heuristic"

            # ── one source at a time ──────────────────────────────
            #
            # The report states the load base TWICE -- on the `Image:` line and
            # again in the `Loaded Images:` table -- so the run above cannot
            # tell which one supplied it, and sabotaging either left the suite
            # green. Redundancy is the right design and a bad test: each
            # source needs a fixture where it is the only one.
            decode_variant() {   # <file>; sets $VOUT
                VOUT="$1.out"
                PATH="$WORK/nogdb:$PATH" timeout 60 python3 "$DECODE" \
                    --image "$WORK/probelow" --rsod "$1" --detail \
                    > "$VOUT.raw" 2>"$VOUT.err"
                norm < "$VOUT.raw" > "$VOUT"
            }
            # The expectation is ABSOLUTE, computed from the fixture rather
            # than read back from another run of the tool. Comparing a variant
            # to the baseline run cannot fail: a defect that breaks base
            # inference breaks both, and the two then agree on being wrong.
            want_frame0() {   # <file> <label>
                if grep -qE "^#0 deep_function \S*elfprobe\.c:1 \[$(printf '0x%x' $PC0) \+ $(printf '0x%x' $OFF0)\]\$" "$1"; then
                    test_host_pass "$2"
                else
                    test_host_fail "$2"
                    grep -m1 '^#0 ' "$1" | sed 's/^/      got: /'
                fi
            }

            # The base is stated THREE times, not two: the `Image:` line, the
            # loaded-image table, and every named stack frame (`name+0xoff`
            # gives base = pc - off). So each variant also blanks the frame
            # names to `???` -- the same thing the crash handler writes for a
            # frame it could not attribute -- or the trace supplies the base
            # and the variant proves nothing about the source it meant to
            # isolate. Both variants passed under sabotage until this.
            blank_frame_names() { sed 's/  probelow+0x[0-9A-Fa-f]*/  ???/'; }

            # (a) the `Image:` line alone -- no table, no attributed frames.
            sed '/^Loaded Images:/,/^$/d' "$WORK/crash-report.txt" \
                | blank_frame_names > "$WORK/cr-no-table.txt"
            refute_match "$WORK/cr-no-table.txt" '^Loaded Images:' \
                "variant (a) really has no loaded-image table"
            refute_match "$WORK/cr-no-table.txt" 'probelow\+0x' \
                "variant (a) really has no attributed frames"
            decode_variant "$WORK/cr-no-table.txt"
            want_frame0 "$VOUT" \
                "base comes from the Image: line when the table is absent"

            # (b) the loaded-image table alone -- no `Image:` line.
            sed '/^Image: /d' "$WORK/crash-report.txt" \
                | blank_frame_names > "$WORK/cr-no-image.txt"
            refute_match "$WORK/cr-no-image.txt" '^Image: ' \
                "variant (b) really has no Image: line"
            refute_match "$WORK/cr-no-image.txt" 'probelow\+0x' \
                "variant (b) really has no attributed frames"
            decode_variant "$WORK/cr-no-image.txt"
            want_frame0 "$VOUT" \
                "base comes from the loaded-image table when Image: is absent"

            # (c) TWO reports in one capture -- a reboot loop writes the report
            # every boot, so a console log holding several is the normal case,
            # not a contrived one. Section scoping is what stops the second
            # report's frames being appended to the first's.
            {
                cat "$WORK/crash-report.txt"
                sed 's/^  0x00000000000000/  0x00000000CAFEF0/' \
                    "$WORK/crash-report.txt"
            } > "$WORK/cr-twice.txt"
            decode_variant "$WORK/cr-twice.txt"
            refute_match "$VOUT" 'cafef0' \
                "a second report in the same capture does not leak frames"
        else
            test_host_fail "low-linked probe builds (UEFI-shaped)"
        fi
    fi
fi

# ═══════════════════════════════════════════════════════════════
# AARCH64. The PE + .map workflow is an MSVC/x64 shape, but this tree
# cross-builds aa64 with gcc and ships .so/.debug ELFs -- and the resolver, the
# disassembler and the frame-pointer walk are all shared. The toolchain the
# decoder reaches for is prefixed (aarch64-linux-gnu-nm / -objdump), so an
# x64-only suite proves nothing about whether it names them correctly.
# ═══════════════════════════════════════════════════════════════
echo ""
echo "-- AARCH64 .so path --"
if ! command -v aarch64-linux-gnu-gcc >/dev/null \
        || ! command -v aarch64-linux-gnu-objdump >/dev/null; then
    echo "  SKIP: aarch64-linux-gnu toolchain not on PATH"
else
    # -nostdlib: the cross toolchain here is freestanding and has no libc
    # headers, which is also what a UEFI module is built like.
    if ! aarch64-linux-gnu-gcc -g -O0 -nostdlib -e main \
            -o "$WORK/aaprobe.so" "$WORK/elfprobe.c" 2>/dev/null; then
        test_host_fail "AARCH64 probe builds"
    else
        AA_FAULT=$(aarch64-linux-gnu-objdump -d --no-show-raw-insn "$WORK/aaprobe.so" \
            | awk '/<deep_function>:/ { f=1; next } f && /ldr\tw1, \[x0\]/ { print $1; exit }' \
            | tr -d ':')
        if [[ -z "$AA_FAULT" ]]; then
            test_host_fail "AARCH64 probe has the expected faulting insn"
        else
            test_host_pass "AARCH64 probe has the expected faulting insn"
            cat > "$WORK/aa-rsod.txt" <<EOF
Synchronous Exception at 0x00000000${AA_FAULT}
X0 0x0000000000000000  X1 0x0000000000000000
FP 0x0000000000000000  LR 0x0000000000000000
SP 0x00000000401FFF00  ELR 0x00000000${AA_FAULT}
ESR 0x0000000096000004  FAR 0x0000000000000000
ESR : EC 0x25  IL 0x1  ISS 0x0000004
Data abort: Translation fault, third level
EOF
            # Hidden gdb again: without it this measures gdb, not the
            # cross-prefixed binutils the aa64 path actually depends on.
            OUTA="$WORK/out-aa64.txt"
            PATH="$WORK/nogdb:$PATH" timeout 60 python3 "$DECODE" \
                --image "$WORK/aaprobe.so:0x0" --rsod "$WORK/aa-rsod.txt" --detail \
                > "$OUTA.raw" 2>"$OUTA.err"
            norm < "$OUTA.raw" > "$OUTA"
            if grep -q '^RSOD Decoder — aaprobe (AARCH64)$' "$OUTA"; then
                test_host_pass "AARCH64 arch detected from the .so"
            else
                test_host_fail "AARCH64 arch detected from the .so"
                sed 's/^/      /' "$OUTA.err" | head -3
            fi
            assert_line "$OUTA" \
                "Cause: Data abort from same EL — Translation fault, level 0 — NULL pointer dereference" \
                "AARCH64 ESR decode still reads FAR"
            if grep -qE '^#0 deep_function \S*elfprobe\.c:1 \[0x[0-9a-f]+ \+ 0x[0-9a-f]+\]$' "$OUTA"; then
                test_host_pass "AARCH64 frame resolves via aarch64-linux-gnu binutils"
            else
                test_host_fail "AARCH64 frame resolves via aarch64-linux-gnu binutils"
                grep -m1 '^#0 ' "$OUTA" | sed 's/^/      got: /'
            fi
            assert_line "$OUTA" ">>> ${AA_FAULT}: ldr w1, [x0]" \
                "AARCH64 faulting instruction disassembled and marked"
            refute_match "$OUTA" 'Disassembly: unavailable' \
                "AARCH64 disassembly is not reported unavailable"
        fi
    fi
fi

# ═══════════════════════════════════════════════════════════════
# S9 -- the mismatch gate in MAP-ONLY mode.
#
# The gate that catches a wrong image was conditioned on SizeOfImage, which a
# .map does not have, so --map skipped it entirely: the same wrong build that
# --image refused produced a confident, fully-formatted decode with no banner
# at all. That cost a day of debugging on a real ePSA RSOD, twice, because the
# fiction is coherent -- plausible function names and a plausible call chain,
# since .text did not move between the builds even though the data did.
#
# The map-side bound is sound rather than heuristic: a symbol cannot live
# beyond the end of its own image, so a highest symbol offset at or past the
# size the dump records is a contradiction.
# ═══════════════════════════════════════════════════════════════
echo ""
echo "-- map-only: correct map (negative control) --"
WRONGMAP="$WORK/wrong-build.map"
OUTMC="$WORK/out-map-correct.txt"
run_decode "$OUTMC" "map-only correct run completes" --map "$MAP" --rsod "$LOG"

# The control matters more than usual here: without it, "no banner" on the
# wrong map would be indistinguishable from a gate that never fires at all.
refute_match "$OUTMC" 'DOES NOT MATCH' \
    "S9 correct map raises no mismatch warning"
assert_line "$OUTMC" "#0 ?TestFaulty@@YAPEAXPEAI@Z + 0xa [0x14002100a + 0x2100a]" \
    "S9 correct map still resolves the faulting frame"

echo ""
echo "-- map-only: WRONG map -- the bug --"
OUTMW="$WORK/out-map-wrong.txt"
run_decode "$OUTMW" "map-only wrong run completes" --map "$WRONGMAP" --rsod "$LOG"

assert_line "$OUTMW" "!! IMAGE DOES NOT MATCH THE DUMP - symbols below are probably fiction:" \
    "S9 wrong map is announced, where it used to be silent"
assert_match "$OUTMW" "the map's highest symbol is at \+0x38000, past the end of the 0x30000 image" \
    "S9 map-size gate names the symbol offset and the recorded size"
# Degraded output is fine; silence is not. The tool must still answer.
assert_line "$OUTMW" "#0 ?WrongFunc@@YAXXZ + 0xa [0x14002100a + 0x2100a]" \
    "S9 wrong map still produces a decode rather than dying"

echo ""
echo "-- a stale map sitting beside a CORRECT image --"
# Neither of the other gates sees this: the image is right so its size check
# passes, and the dump-side map bound does not run when a PE supplied the
# size. But the map wins symbol resolution, so the decode is fiction while
# every other signal says the artifacts are fine.
#
# Caught by the SAME invariant applied to the image instead of the dump: a
# symbol past the image's own SizeOfImage cannot belong to it. Deliberately
# NOT by the link stamp -- a PE's TimeDateStamp does not survive every build
# flow (every .efi in this very tree carries 0, written by the ELF-to-PE
# conversion), so a stamp difference is evidence and is reported as a note,
# while the gate itself stays proof.
STALE="$WORK/stale"; mkdir -p "$STALE"
cp "$APP" "$STALE/app.efi"; cp "$WRONGMAP" "$STALE/app.map"
OUTST="$WORK/out-stale.txt"
run_decode "$OUTST" "stale-map run completes" --image "$STALE/app.efi" --rsod "$LOG"
assert_match "$OUTST" "the map's highest symbol is at \+0x38000, past the end of this 0x30000 image" \
    "S9 stale map beside a correct image is caught by size, which is proof"
assert_match "$OUTST" "note: map linked 0x6a8f812a, image stamped 0x6a8f3db9" \
    "S9 the link-stamp difference is reported as a note, not as proof"
refute_match "$OUTST" "different builds$" \
    "S9 the stamp note does not claim different builds outright"

cp "$MAP" "$STALE/app.map"
OUTSM="$WORK/out-stale-ok.txt"
run_decode "$OUTSM" "matching-map run completes" --image "$STALE/app.efi" --rsod "$LOG"
refute_match "$OUTSM" 'DOES NOT MATCH' \
    "S9 a matching map beside the image stays silent"

# The build fingerprint a map-only user has nothing else to check by hand.
assert_match "$OUTMC" "app.map \(map, linked 0x6a8f3db9\)" \
    "S9 the map's link stamp is surfaced in Symbol sources"

test_host_summary "rsod-decode-pe-map"
