#!/bin/bash
# test-meta: arch=x64 needs= est=35 local-only=0
# test-flushfail-tools-qemu.sh -- a TOOL's write path against media whose
# flush fails.
#
# The library's whole-file writers now flush-and-check before reporting
# success, because a close cannot report anything (EFI_FILE_PROTOCOL.Close is
# specified to return only EFI_SUCCESS). The TOOLS that stream their output
# through AxlStream had the same hole, and `tar -c` is the one with real
# consequences: a truncated archive announced as a successful create is the
# same failure mode as promoting a temp file that never landed.
#
# flushfail-fs-driver.efi publishes an AxlFsProvider whose flush always fails
# and stays RESIDENT, so `AXLFF:` is a live volume for the rest of the shell
# session and tar can be run against it directly. (A resident app driving tar
# through EFI_SHELL_PROTOCOL.Execute does NOT work: Execute spawns a nested
# shell that re-enumerates the map and keeps its own %lasterror%.)
#
# Assertions:
#   1. tar SAYS the archive did not reach the volume;
#   2. tar EXITS non-zero (%lasterror%), because a diagnostic a script cannot
#      see is not a report;
#   3. tar did NOT report a short write, and the shell's own `ls` shows the
#      archive at its full 10240-byte length -- so every byte WAS accepted
#      and the flush is the only thing that failed. Without this the test
#      could not tell a durability bug from an ordinary write error;
#   4. the same two halves for `tar -x`, which is do_create's twin 150
#      lines down the same file and had the identical hole: it wrote each
#      extracted file, closed it unchecked, listed it under -v and
#      exited 0.
#
# (1) and (2) were both RED before the fix: tar closed the archive unchecked
# and returned 0.
#
# Auxiliary; opts out of the test-axl.sh ratchet. x64 only -- nothing here is
# arch-specific, but it is a single-tool smoke test and a second boot costs
# more than the coverage adds.
set -u

ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        *) echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

case "$ARCH" in
    X64)     OUT="out/native-x64";  MAKE_ARCH="x64" ;;
    AARCH64) OUT="out/native-aa64"; MAKE_ARCH="aa64" ;;
    *) echo "unknown arch $ARCH"; exit 1 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_DIR"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
DRV="$OUT/flushfail-fs-driver.efi"
TAR="$OUT/tools/tar.efi"

export TEST_SKIP_RATCHET=1
PASS=0; FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL + 1)); }

[[ -f "$DRV" ]] || make flushfail-fs-driver ARCH="$MAKE_ARCH" 2>&1 | tail -3
[[ -f "$TAR" ]] || make tools ARCH="$MAKE_ARCH" 2>&1 | tail -3
[[ -f "$DRV" ]] || { echo "FAIL: $DRV not found"; exit 1; }
[[ -f "$TAR" ]] || { echo "FAIL: $TAR not found"; exit 1; }

NSH="$(mktemp --suffix=.nsh)"
LOG="$(mktemp)"
trap 'rm -f "$NSH" "$LOG"' EXIT

# One shell session throughout: `load` publishes AXLFF: and seeds the source,
# then tar runs against it and %lasterror% is read in the same instance.
cat > "$NSH" <<'EOF'
@echo -off
fs0:
cd \
load fs0:\flushfail-fs-driver.efi
fs0:\tar.efi -c AXLFF:\a.tar fs0:\ffsrc.txt
echo TOOLS: TAR-CREATE-LASTERROR=[%lasterror%]
ls AXLFF:\
fs0:\tar.efi -c fs0:\ref.tar fs0:\ffsrc.txt
fs0:\tar.efi -x fs0:\ref.tar -C AXLFF:\
echo TOOLS: TAR-EXTRACT-LASTERROR=[%lasterror%]
reset -s
EOF

echo "=== flush-failing volume vs. a tool's write path ($ARCH) ==="
# --extra stages tar.efi beside the driver on the boot FAT; only the first
# positional is the "app" run-qemu knows how to launch, and the .nsh above
# launches everything itself.
timeout 120s "$RUN_QEMU" --arch "$ARCH" --nsh "$NSH" --extra "$TAR" "$DRV" \
    > "$LOG" 2>&1 || true

if grep -q "FFDRV: SKIP no-shell-map" "$LOG"; then
    # No shell to map the published volume through: tar could not have named
    # the volume, so nothing was exercised. Reported, not passed.
    echo "SKIP: no shell map for the published volume (nothing ran)"
    exit 0
fi
if ! grep -q "FFDRV: READY volume=AXLFF" "$LOG"; then
    fail "the flush-failing volume was never published" "$(tail -20 "$LOG")"
    printf "\nResults: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$ARCH"
    exit 1
fi

# 1. tar says so. The message is asserted, not merely "some output": a silent
#    truncation is precisely the bug. Match do_create's OWN wording -- the
#    extract half below prints a `tar: flush ... failed` line too, and a bare
#    match on that prefix would let either side satisfy both checks.
if grep -q "tar: flush .* failed (archive did not reach the volume)" "$LOG"; then
    pass "tar reports that the archive did not reach the volume"
else
    fail "tar printed no flush diagnostic" "$(grep -i '^tar:' "$LOG" | head -5)"
fi

# 2. ...and exits non-zero. The shell renders %lasterror% as an EFI_STATUS in
#    hex ("0x0" on success), so compare against both spellings of zero.
LE=$(sed -n 's/.*TOOLS: TAR-CREATE-LASTERROR=\[\(.*\)\].*/\1/p' "$LOG" | head -1)
if [[ -n "$LE" && "$LE" != "0" && "$LE" != "0x0" ]]; then
    pass "tar -c exits non-zero (%lasterror%=$LE)"
else
    fail "tar -c exited with %lasterror%='$LE'; expected non-zero"
fi

# 3. The failure is the FLUSH's alone: no short-write diagnostic, and the
#    archive tar just wrote reads back complete through tar -t.
if grep -q "tar: write .* failed" "$LOG"; then
    fail "tar reported a short WRITE; the test is exercising the wrong failure" \
         "$(grep -i '^tar:' "$LOG" | head -5)"
else
    pass "no short-write error -- every byte was accepted, only the flush failed"
fi
# The shell's own `ls` of the fixture volume -- not the tool under test --
# is the oracle for how many bytes actually landed. tar pads to a
# 10240-byte record, so a full-length archive means every write was taken.
if grep -qE "10,?240[[:space:]]+a\.tar" "$LOG"; then
    pass "the volume holds tar's whole 10240-byte archive -- only the flush failed"
else
    fail "the archive is not full-length on the volume" "$(grep -A3 'Directory of' "$LOG" | head -8)"
fi

# 4. tar -x is do_create's twin, 150 lines down the same file: it opened
#    each extracted file, wrote, closed unchecked and returned 0, so an
#    extract onto a full volume produced truncated files, listed them under
#    -v, and exited 0. Same two halves asserted as the create side.
# Match do_extract's own wording, not a count of two `tar: flush` lines: a
# create that printed twice would satisfy `-ge 2` while the extract side said
# nothing at all. The two messages differ in their tail -- "archive" vs
# "extracted file" -- which is a stabler discriminator than the member's path
# spelling.
if grep -q "tar: flush .* failed (extracted file did not reach the volume)" "$LOG"; then
    pass "tar -x reports that an extracted file did not reach the volume"
else
    fail "tar -x printed no flush diagnostic" "$(grep -i '^tar:' "$LOG" | head -5)"
fi
LEX=$(sed -n 's/.*TOOLS: TAR-EXTRACT-LASTERROR=\[\(.*\)\].*/\1/p' "$LOG" | head -1)
if [[ -n "$LEX" && "$LEX" != "0" && "$LEX" != "0x0" ]]; then
    pass "tar -x exits non-zero (%lasterror%=$LEX)"
else
    fail "tar -x exited with %lasterror%='$LEX'; expected non-zero"
fi

echo ""
printf "Results: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$ARCH"
[[ $FAIL -eq 0 ]] || { echo ""; echo "--- log tail ---"; tail -40 "$LOG"; exit 1; }
exit 0
