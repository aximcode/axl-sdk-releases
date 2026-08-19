#!/bin/bash
# test-meta: arch=X64 needs= est=31 local-only=1
# test-tools-qemu — behavioral coverage for the CLI tools that the
# happy-path output test (test-tools.sh) doesn't reach: EXIT CODES
# (%lasterror%), ERROR/failure paths (missing file, bad args), and the
# same battery on BOTH the modern EDK2 shell AND the legacy EFI 1.x shell
# (Shell106), where behavior can diverge.
#
# Motivation: a round of hand-testing on the legacy shell surfaced a
# cluster of issues invisible to output-only checks — every non-zero tool
# exit showing as "Aborted", grep-no-match reported as a crash, find
# printing a nonexistent path, tools failing silently with no reason. This
# test pins the EXIT-CODE contract (small-int %lasterror%, never the
# EFI_ABORTED 0x15 collapse) and the "explain why you failed" contract.
#
# Each case runs a command, captures its output between markers, and reads
# %lasterror% right after. A case asserts: expected exit code, and
# optionally a substring that MUST appear (e.g. an error message) or MUST
# NOT appear in the output.
#
# LOCAL-ONLY: the legacy-shell half needs Shell106.efi (an x86 EFI Toolkit
# binary not redistributable here). Point AXL_OLD_SHELL_EFI at a copy;
# absent it, only the modern-shell half runs. Ratchet-exempt (scenario
# assertions, not a unit binary's count).
#
# Usage: [AXL_OLD_SHELL_EFI=/path/to/Shell106.efi] \
#            ./test/integration/test-tools-qemu.sh [--arch X64]

source "$(dirname "$0")/common-test.sh"
export TEST_SKIP_RATCHET=1

TEST_ARCH="${TEST_ARCH:-X64}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64]"; exit 1 ;;
    esac
done

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
_arch_lc=$(echo "$TEST_ARCH" | tr 'A-Z' 'a-z')
[[ "$_arch_lc" == "aarch64" ]] && _arch_lc="aa64"
NATIVE_DIR="$(test_build_dir)"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"

make -C "$PROJECT_DIR" ARCH="$_arch_lc" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    tools 2>&1 | tail -1

TOOLS_DIR="$NATIVE_DIR/tools"
if [[ ! -d "$TOOLS_DIR" ]]; then
    echo "WARN: tools not built at $TOOLS_DIR; skipping."
    echo "test-tools-qemu: SKIP"
    exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# Fixtures staged on the ESP root (fs0:).
# ---------------------------------------------------------------------------
printf 'AximCode axl-sdk sample\nsecond line has axl in it\nthe quick brown fox\n' \
    > "$WORK/sample.txt"
mkdir -p "$WORK/sub"
printf 'nested content\n' > "$WORK/sub/nested.txt"

# ---------------------------------------------------------------------------
# Case table. Populated by tc(); consumed to build the nsh and to assert.
#   tc <name> <expect_rc> <present|-> <absent|-> -- <shell command...>
# expect_rc: the %lasterror% the command must leave, or '*' to skip the code
#            check (smoke — just must not hang/crash).
# present:   substring that MUST appear in the command's output ('-' = skip).
# absent:    substring that MUST NOT appear ('-' = skip).
# ---------------------------------------------------------------------------
CASE_NAMES=(); CASE_CMDS=(); CASE_RC=(); CASE_PRESENT=(); CASE_ABSENT=()
tc() {
    local name="$1" rc="$2" present="$3" absent="$4"; shift 4
    [[ "$1" == "--" ]] && shift
    CASE_NAMES+=("$name"); CASE_RC+=("$rc")
    CASE_PRESENT+=("$present"); CASE_ABSENT+=("$absent"); CASE_CMDS+=("$*")
}

# Invoke via explicit `.efi` so the test exercises the TOOL uniformly on both
# shells, not the shell's own command/arg handling: e.g. the modern EDK2 shell
# intercepts a bare `cat <nonexistent>` and returns EFI_NOT_FOUND itself before
# cat runs, while the legacy shell runs the tool — a shell difference, not a
# tool bug. `.efi` sidesteps that so the tool's exit code + message are what's
# under test.
#
# Exit convention: 0 = success, 1 = "negative result" (no match / usage error
# via the args layer), 2 = trouble (couldn't open a file). None show "Aborted".
tc grep_match     0 "axl"            - -- grep.efi axl fs0:\\sample.txt
tc grep_nomatch   1 -                Aborted -- grep.efi zzznope fs0:\\sample.txt
tc grep_missing   2 "cannot open"    Aborted -- grep.efi axl fs0:\\nosuch.txt
tc grep_badargs   1 "Usage"          Aborted -- grep.efi
# --- find: nonexistent must NOT print the bogus path, must error ------------
tc find_existing  0 "sample.txt"     - -- find.efi fs0:\\sample.txt
tc find_missing   2 "no such"        - -- find.efi fs0:\\MIKE
# --- generic tool failures explain themselves, no "Aborted" -----------------
tc hexdump_missing 2 "annot"         Aborted -- hexdump.efi fs0:\\nosuch.txt
tc cat_missing     2 "annot"         Aborted -- cat.efi fs0:\\nosuch.txt
tc sed_missing     2 "annot"         Aborted -- sed.efi s/a/b/ fs0:\\nosuch.txt
# --- tar -f (GNU/BSD compat): create then list, in sequence -----------------
tc tar_create_f   0 -                Aborted -- tar.efi -cf fs0:\\t.tar sample.txt
tc tar_list_f     0 "sample.txt"     Aborted -- tar.efi -tf fs0:\\t.tar
# --- dmidecode: runs, records grouped by type (Type 0 before Type 1) --------
tc dmi_type0      0 "DMI type 0"     Aborted -- dmidecode.efi -t 0
# --- universal -b / --page: accepted (never "unknown flag"), output still
#     produced, and NO hang. Under a startup.nsh the shell's page break is
#     suppressed (EFI_SHELL_PROTOCOL.BatchIsActive), so paging can't block the
#     batch on a keystroke; on the legacy shell there's no page-break service
#     so -b is simply accepted. Interactive pagination is verified separately.
tc pagebreak_b   0 "DMI type 0"     Aborted -- dmidecode.efi -b -t 0
tc pagebreak_long 0 "DMI type 0"    Aborted -- dmidecode.efi --page -t 0
# --- smoke: common tools run clean (exit 0) ---------------------------------
tc sysinfo_smoke  0 -                Aborted -- sysinfo.efi arch
tc hexdump_smoke  0 "00000000"       Aborted -- hexdump.efi fs0:\\sample.txt
tc cat_smoke      0 "AximCode"       Aborted -- cat.efi fs0:\\sample.txt
tc grep_count     0 -                Aborted -- grep.efi -c axl fs0:\\sample.txt

# ---------------------------------------------------------------------------
# Build the diagnostic nsh from the case table. Markers avoid '-' and '#'
# (the legacy shell's `echo` parses those as flags).
# ---------------------------------------------------------------------------
build_nsh() {
    local nsh="$1"
    {
        echo "@echo -off"
        echo "fs0:"
        echo "cd \\"
        local i
        for i in "${!CASE_NAMES[@]}"; do
            echo "echo TCASE ${CASE_NAMES[$i]} BEGIN"
            echo "${CASE_CMDS[$i]}"
            echo "echo TCRC ${CASE_NAMES[$i]} [%lasterror%]"
            echo "echo TCASE ${CASE_NAMES[$i]} END"
        done
        echo "echo TALL_DONE"
        echo "reset -s"
    } > "$nsh"
}

# Extract a case's captured output (between its BEGIN and TCRC lines).
case_output() { # log, name
    awk -v s="TCASE $2 BEGIN" -v e="TCRC $2 " \
        'index($0,s){f=1;next} index($0,e){f=0} f' "$1" || true
}
# Extract the %lasterror% a case left, as a DECIMAL. The shell prints it hex,
# with a `0x` prefix on the modern shell (`[0x15]`) and bare on the legacy one
# (`[8000000000000015]`). Empty output => the case left no marker. Never fails
# (set -euo pipefail is active in the caller).
case_rc() { # log, name
    local raw
    raw=$(grep -aoE "TCRC $2 \[(0x)?[0-9A-Fa-f]+\]" "$1" | tail -1 \
          | sed -E 's/.*\[(0x)?([0-9A-Fa-f]+)\]/\2/') || true
    [[ -n "$raw" ]] && printf '%d' "$((16#$raw))" || true
}

# Assert every case against a captured log for one shell ($1 label, $2 log).
assert_cases() {
    local shell_label="$1" log="$2" i
    for i in "${!CASE_NAMES[@]}"; do
        local name="${CASE_NAMES[$i]}" want_rc="${CASE_RC[$i]}"
        local present="${CASE_PRESENT[$i]}" absent="${CASE_ABSENT[$i]}"
        local out got_dec ok=1 why=""
        out=$(case_output "$log" "$name") || true
        got_dec=$(case_rc "$log" "$name") || true
        if [[ -z "$got_dec" ]]; then
            ok=0; why="no TCRC marker (did not run / hung)"
        elif [[ "$want_rc" != "*" && "$got_dec" != "$want_rc" ]]; then
            ok=0; why="exit=$got_dec want=$want_rc"
        fi
        if [[ $ok -eq 1 && "$present" != "-" ]] \
           && ! echo "$out" | grep -qa "$present"; then
            ok=0; why="missing '$present' in output"
        fi
        if [[ $ok -eq 1 && "$absent" != "-" ]] \
           && echo "$out" | grep -qa "$absent"; then
            ok=0; why="unwanted '$absent' in output"
        fi
        if [[ $ok -eq 1 ]]; then
            test_host_pass "[$shell_label] $name"
        else
            test_host_fail "[$shell_label] $name ($why)"
        fi
    done
}

# Stage every tool the cases invoke + the fixtures, run one boot, capture.
run_battery() { # shell_label, extra run-qemu args...
    local label="$1"; shift
    local nsh="$WORK/tools-$label.nsh"
    build_nsh "$nsh"
    local extras=(--extra "$WORK/sample.txt:sample.txt"
                  --extra "$WORK/sub/nested.txt:sub/nested.txt")
    local t
    for t in grep find hexdump cat sed sysinfo tar dmidecode; do
        extras+=(--extra "$TOOLS_DIR/$t.efi:$t.efi")
    done
    # `|| true`: run-qemu / timeout may exit non-zero (reset -s, timeout);
    # the captured log + TALL_DONE marker are the real success signal, and
    # common-test.sh's `set -euo pipefail` would otherwise abort the run.
    { timeout 150 "$RUN_QEMU" --arch "$TEST_ARCH" --timeout 90 --nsh "$nsh" \
        "$@" "${extras[@]}" "$TOOLS_DIR/sysinfo.efi" 2>&1 \
        | sed 's/\x1b\[[0-9;]*[A-Za-z]//g'; } || true
}

# ---------------------------------------------------------------------------
# Modern EDK2 shell (always).
# ---------------------------------------------------------------------------
echo "=== tools on the modern EDK2 shell ($TEST_ARCH) ==="
MODERN_LOG="$WORK/modern.log"
run_battery modern > "$MODERN_LOG"
if ! grep -qa "TALL_DONE" "$MODERN_LOG"; then
    test_host_fail "[modern] battery completed (TALL_DONE)"
fi
assert_cases modern "$MODERN_LOG"

# ---------------------------------------------------------------------------
# Legacy EFI 1.x shell (Shell106) — only when available and on X64.
# ---------------------------------------------------------------------------
OLD_SHELL="${AXL_OLD_SHELL_EFI:-$HOME/work/dell/delldiags/local/legacy-corpus/EFI/Boot/Shell106.efi}"
if [[ "$TEST_ARCH" == "X64" && -f "$OLD_SHELL" ]]; then
    echo "=== tools on the legacy EFI 1.x shell (Shell106) ==="
    OLD_LOG="$WORK/old.log"
    run_battery old --shell "$OLD_SHELL" > "$OLD_LOG"
    if ! grep -qa "TALL_DONE" "$OLD_LOG"; then
        test_host_fail "[legacy] battery completed (TALL_DONE)"
    fi
    assert_cases legacy "$OLD_LOG"
else
    echo "legacy-shell half: SKIP (need X64 + AXL_OLD_SHELL_EFI / Shell106.efi)"
fi

# ---------------------------------------------------------------------------
# mkrd must NOT clobber the user's %path%. Creating a RAM disk runs the shell's
# `map -r` to make the disk usable; on the legacy shell that command also
# rewrites %path%. axl_map_refresh now snapshots + restores it. Separate boot
# (mkrd mutates the volume map, which would disturb the battery above), legacy
# shell only — that's where the clobber happens and where the fix matters.
# ---------------------------------------------------------------------------
mkrd_path_scenario() { # shell_label, extra args...
    local label="$1"; shift
    local nsh="$WORK/mkrd-$label.nsh"
    cat > "$nsh" <<'NSH'
@echo -off
fs0:
cd \
echo MKRD_PATH_BEFORE=[%path%]
mkrd TESTRD
echo MKRD_PATH_AFTER=[%path%]
mkrd -d TESTRD
echo MKRD_PATH_DONE
reset -s
NSH
    { timeout 120 "$RUN_QEMU" --arch "$TEST_ARCH" --timeout 80 --nsh "$nsh" \
        "$@" --extra "$TOOLS_DIR/mkrd.efi:mkrd.efi" \
        "$TOOLS_DIR/mkrd.efi" 2>&1 | sed 's/\x1b\[[0-9;]*[A-Za-z]//g'; } || true
}

if [[ "$TEST_ARCH" == "X64" && -f "$OLD_SHELL" ]]; then
    echo "=== mkrd %path% preservation (legacy shell) ==="
    MKRD_LOG="$WORK/mkrd.log"
    mkrd_path_scenario old --shell "$OLD_SHELL" > "$MKRD_LOG"
    grep -aE "MKRD_PATH_(BEFORE|AFTER)|RAM disk created" "$MKRD_LOG" | sed 's/^/  /'
    # The disk must actually get created (so map -r ran), and %path% must read
    # back IDENTICAL after — without the fix, the legacy shell's map -r rewrites
    # it from the device-alias form to the fsN form.
    _pbefore=$(grep -aoE 'MKRD_PATH_BEFORE=\[[^]]*\]' "$MKRD_LOG" | tail -1)
    _pafter=$(grep -aoE 'MKRD_PATH_AFTER=\[[^]]*\]' "$MKRD_LOG"  | tail -1)
    _pbefore=${_pbefore#MKRD_PATH_BEFORE=}
    _pafter=${_pafter#MKRD_PATH_AFTER=}
    if grep -qa "RAM disk created" "$MKRD_LOG" \
       && [[ -n "$_pafter" && "$_pbefore" == "$_pafter" ]]; then
        test_host_pass "[legacy] mkrd preserves %path% across its map -r"
    else
        test_host_fail "[legacy] mkrd preserves %path% (before=$_pbefore after=$_pafter)"
    fi
fi

test_host_summary "tool behavior ($TEST_ARCH)"
