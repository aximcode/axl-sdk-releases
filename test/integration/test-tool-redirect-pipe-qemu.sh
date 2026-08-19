#!/bin/bash
# test-meta: arch=x64 needs= est=15 local-only=0
# test-tool-redirect-pipe-qemu.sh — every axl-sdk tool's stdout survives the
# four UEFI-shell output operators, and the byte encoding of each is correct.
#
# The four operators, and what each proves:
#   >    stdout to a file, UCS-2 (with BOM)   — the classic redirect
#   >a   stdout to a file, ASCII              — the ASCII redirect
#   |    pipe to another tool, UCS-2          — pipe-as-PRODUCER
#   |a   pipe to another tool, ASCII          — ASCII pipe
#
# Pipe-as-producer (`tool | other`) is the load-bearing case: the UEFI shell
# wires EFI_SHELL_PARAMETERS_PROTOCOL.StdOut for a `|` but does NOT swap
# gST->ConOut, so a tool that only wrote ConOut printed to the SCREEN and the
# downstream stage got nothing. axl_stdout now writes UCS-2 to the StdOut
# handle when stdout is non-interactive, so the output traverses the pipe.
#
# Deterministic, hardware-independent output: every tool's `--version` prints
# exactly "<tool> <VERSION>" (the AXL_TOOL_MAIN stamp), so this test asserts
# the same string across every tool regardless of platform — the same trick
# test-tool-version-qemu.sh uses to cover "all tools".
#
# Three parts:
#   A. BYTES  — one representative tool (cat) through all four operators,
#               hexdumped, so the exact UCS-2-vs-ASCII bytes are asserted
#               (this is where `>a` / `|a` are proven to really be ASCII).
#   B. BREADTH— every versioned tool through all four operators, asserting
#               its "<tool> <VERSION>" line survives each.
#   C. STDIN  — the filter tools (cat/grep/hexdump/sed/clip+paste) consume a
#               `<` redirect and a `|` pipe as the RHS.
#
# Auxiliary single-binary test — opts out of the test-axl.sh ratchet.
# x86 only for now: OVMF ships EDK2 ShellPkg with full `|`/`>a` support and is
# validated here; AArch64 AAVMF is not yet validated for shell piping (matching
# test-shell-pipe.sh's scope).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1

ARCH="X64"
while [[ $# -gt 0 ]]; do case "$1" in --arch) ARCH="$2"; shift 2;; *) shift;; esac; done
declare -A _M=([X64]=x64 [AARCH64]=aa64); NATIVE="${_M[$ARCH]:-x64}"

TOOLS="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$NATIVE")/tools"
VERSION="$(cat "$PROJECT_DIR/VERSION")"

make -C "$PROJECT_DIR" ARCH="$NATIVE" tools >/dev/null 2>&1 || true
[[ -d "$TOOLS" ]] || { echo "ERROR: $TOOLS not built"; exit 1; }

# Non-versioned tools (dev/bench, or resident drivers loaded not run): no
# "<tool> <VERSION>" line to assert. Same list test-tool-version-qemu.sh uses.
EXCLUDE="axbench crashtest kbtune-drv fbcon"

mapfile -t ALL < <(cd "$TOOLS" && ls *.efi 2>/dev/null | sed 's/\.efi$//' | sort)
VERSIONED=()
for t in "${ALL[@]}"; do
    skip=0; for x in $EXCLUDE; do [[ "$t" == "$x" ]] && skip=1; done
    [[ "$skip" == 0 ]] && VERSIONED+=("$t")
done
[[ "${#VERSIONED[@]}" -ge 20 ]] || { echo "ERROR: only ${#VERSIONED[@]} tools"; exit 1; }

# grep + hexdump drive the assertions; make sure both are in the set we stage.
for req in grep hexdump cat sed clip paste; do
    [[ -f "$TOOLS/$req.efi" ]] || { echo "ERROR: $req.efi not built"; exit 1; }
done

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
NSH="$TMP/startup.nsh"
LOG="$TMP/serial.log"

{
    echo '@echo -off'
    echo 'fs0:'
    echo 'cd \'

    # --- Part A: byte-level, representative tool (cat) ---
    echo 'echo A_BYTES_BEGIN'
    echo 'echo A_GT'
    echo 'cat.efi --version > cg.txt'
    echo 'hexdump.efi cg.txt'
    echo 'echo A_GTA'
    echo 'cat.efi --version >a ca.txt'
    echo 'hexdump.efi ca.txt'
    echo 'echo A_PIPE'
    echo 'cat.efi --version | hexdump.efi'
    echo 'echo A_PIPEA'
    echo 'cat.efi --version |a hexdump.efi'
    echo 'echo A_BYTES_END'

    # --- Part B: breadth — every tool through the four operators, each check
    #     CAN-FAIL. Redirect: a marker sits BETWEEN the redirect command and
    #     the `type`, so if the redirect silently failed the command's
    #     console-leak lands in the fenced-off region, NOT in the `type` output
    #     the assertion reads (and a stale file makes `type` show the wrong
    #     tool's line — also a fail). Pipe: assert a hexdump OFFSET line
    #     ("00000000:") which appears only if the tool's bytes reached the
    #     pipe; a producer that leaked to the console emits TEXT, not an offset.
    for t in "${VERSIONED[@]}"; do
        echo "echo B_${t}_gtc"
        echo "${t}.efi --version > og.txt"
        echo "echo B_${t}_gtt"
        echo 'type og.txt'
        echo "echo B_${t}_gac"
        echo "${t}.efi --version >a oa.txt"
        echo "echo B_${t}_gat"
        echo 'type oa.txt'
        echo "echo B_${t}_p"
        echo "${t}.efi --version | hexdump.efi"
        echo "echo B_${t}_pa"
        echo "${t}.efi --version |a hexdump.efi"
        echo "echo B_${t}_e"
    done

    # --- Part C: stdin consumers (< and | as RHS) ---
    echo 'echo C_STDIN_BEGIN'
    echo 'echo needle-xyz > sc.txt'
    echo 'echo C_GREP_LT'
    echo 'grep.efi needle-xyz < sc.txt'
    echo 'echo C_GREP_PIPE'
    echo 'echo needle-xyz | grep.efi needle-xyz'
    echo 'echo C_SED_PIPE'
    echo 'echo aaa | sed.efi s/aaa/bbb/'
    echo 'echo C_CAT_PIPE'
    echo 'echo catpipe | cat.efi'
    echo 'echo C_HEXDUMP_PIPE'
    echo 'echo hx | hexdump.efi'
    echo 'echo C_CLIP_PASTE'
    echo 'echo clipword | clip.efi'
    echo 'paste.efi'
    echo 'echo C_STDIN_END'

    echo 'echo ALL_DONE'
    echo 'reset -s'
} > "$NSH"

EXTRA=()
for t in "${VERSIONED[@]:1}"; do EXTRA+=(--extra "$TOOLS/$t.efi"); done

qto=100; [[ "$ARCH" == "AARCH64" || ! -r /dev/kvm ]] && qto=260
"$PROJECT_DIR/scripts/run-qemu.sh" --arch "$ARCH" --timeout "$qto" \
    --nsh "$NSH" "${EXTRA[@]}" "$TOOLS/${VERSIONED[0]}.efi" > "$LOG" 2>&1 || true

# ---------------------------------------------------------------------------
# Assertions — exact, section-fenced. `sect A B` extracts the log between two
# markers; every check greps only inside its own section so a right-looking
# line from another command cannot satisfy it.
# ---------------------------------------------------------------------------
PASS=0; FAIL=0
sect() { sed -n "/$1/,/$2/p" "$LOG"; }
ok()   { echo "  PASS: $1"; PASS=$((PASS+1)); }
no()   { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

echo ""
echo "  --- A. byte encoding (cat --version = 'cat $VERSION') ---"
# UCS-2 'cat ' = 63 00 61 00 74 00 20 00 ; ASCII 'cat ' = 63 61 74 20
UCS2='6300 6100 7400'
ASC='6361 7420'
sect A_GT A_GTA | grep -aq "fffe $UCS2" \
    && ok "> redirect wrote a BOM + UCS-2 file" || no "> redirect encoding"
sect A_GTA A_PIPE | grep -aq "$ASC" \
    && ! sect A_GTA A_PIPE | grep -aq "fffe $UCS2" \
    && ok ">a redirect wrote plain ASCII (no BOM/UCS-2)" || no ">a redirect encoding"
sect A_PIPE A_PIPEA | grep -aq "fffe $UCS2" \
    && ok "| pipe delivered the tool's output as UCS-2 (producer works)" \
    || no "| pipe-producer (tool output did not reach the pipe)"
sect A_PIPEA A_BYTES_END | grep -aq "$ASC" \
    && ! sect A_PIPEA A_BYTES_END | grep -aq "fffe $UCS2" \
    && ok "|a pipe delivered the tool's output as ASCII" || no "|a pipe encoding"

echo ""
echo "  --- B. all ${#VERSIONED[@]} tools: > / >a (type-fenced) + | / |a (hexdump offset) ---"
gtmiss=(); gamiss=(); pmiss=(); pamiss=()
for t in "${VERSIONED[@]}"; do
    # Redirect: assert the tool's line in the `type` output ONLY (the region
    # after the type-fence marker), so a redirect that leaked to the console
    # cannot satisfy it.
    sect "B_${t}_gtt" "B_${t}_gac" | grep -aqE "^${t} ${VERSION}([[:space:]]|$)" || gtmiss+=("$t")
    sect "B_${t}_gat" "B_${t}_p"   | grep -aqE "^${t} ${VERSION}([[:space:]]|$)" || gamiss+=("$t")
    # Pipe: a hexdump offset line proves bytes traversed the pipe.
    sect "B_${t}_p"   "B_${t}_pa"  | grep -aq  "00000000:" || pmiss+=("$t")
    sect "B_${t}_pa"  "B_${t}_e"   | grep -aq  "00000000:" || pamiss+=("$t")
done
[[ ${#gtmiss[@]} -eq 0 ]] && ok "all tools' output redirected with '>'"  || no "'>' lost from: ${gtmiss[*]}"
[[ ${#gamiss[@]} -eq 0 ]] && ok "all tools' output redirected with '>a'" || no "'>a' lost from: ${gamiss[*]}"
[[ ${#pmiss[@]}  -eq 0 ]] && ok "all tools' output piped with '|'"        || no "'|' lost from: ${pmiss[*]}"
[[ ${#pamiss[@]} -eq 0 ]] && ok "all tools' output piped with '|a'"       || no "'|a' lost from: ${pamiss[*]}"

echo ""
echo "  --- C. stdin consumers (< and |) ---"
sect C_GREP_LT   C_GREP_PIPE  | grep -aq 'needle-xyz' && ok "grep reads '< file'"        || no "grep < file"
sect C_GREP_PIPE C_SED_PIPE   | grep -aq 'needle-xyz' && ok "grep reads a '|' pipe"      || no "grep | pipe"
sect C_SED_PIPE  C_CAT_PIPE   | grep -aq 'bbb'        && ok "sed reads+transforms a pipe"|| no "sed | pipe"
sect C_CAT_PIPE  C_HEXDUMP_PIPE | grep -aq 'catpipe'  && ok "cat reads a '|' pipe"       || no "cat | pipe"
sect C_HEXDUMP_PIPE C_CLIP_PASTE | grep -aqi '68.*78\|hx' && ok "hexdump reads a '|' pipe" || no "hexdump | pipe"
sect C_CLIP_PASTE C_STDIN_END | grep -aq 'clipword'  && ok "clip reads stdin, paste emits it" || no "clip/paste stdin"

echo ""
printf "tool redirect/pipe: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$ARCH"
if [[ "$FAIL" -gt 0 ]]; then echo "--- serial tail ---"; tail -40 "$LOG"; fi
[[ "$FAIL" -eq 0 ]] && exit 0 || exit 1
