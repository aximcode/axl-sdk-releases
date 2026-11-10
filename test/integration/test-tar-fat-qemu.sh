#!/bin/bash
# test-meta: arch=x64 needs=virtiofsd est=15 local-only=0 guest=none
# guest=none: --mount: a FAT volume of its own that fills up, fixed when
#   QEMU starts.
# test-tar-fat-qemu.sh -- tar -x safe writes on FAT, where test-tar-qemu.sh
# (virtiofs) cannot reach: a volume that fills up, and 8.3 aliases.
#
# The archives come from the host over virtiofs (FS1); tar extracts onto
# FAT -- a 1 MiB RAM disk made by mkrd, and the boot disk fs0.
#
#   fill   fill.tar holds a.txt, big.bin (2 MiB) and z.txt, extracted onto
#          the 1 MiB RAM disk TINY:. big.bin's write fails part-way on a
#          full volume -- a failure tar goes ON from, to z.txt. Asserted:
#          big.bin is reported; a.txt and z.txt are there, z.txt whole; no
#          ~TAR*.TMP is left on the disk; tar's end-of-extract backstop
#          never fires ("tar: internal:" absent); exit 1. Without the
#          discard on the failure path, the next member's temporary would
#          take the slot and big.bin's temporary would stay on the disk.
#   alias  On FAT a temporary ~TAR1234ABCD.TMP also answers to its 8.3
#          alias ~TAR12~1.TMP, so a MEMBER of that name would, once the
#          temporary exists, resolve to the temporary itself, and putting
#          it in place would rename the temporary onto itself -- the data
#          then listed under the temporary's long name. The test seam
#          AXL_TAR_TEST_TEMP (a Shell variable tar reads) makes the first
#          temporary exactly ~TAR1234ABCD.TMP, so the collision is
#          deterministic. Asserted: tar notices and retries with another
#          temporary (its debug line); the member lands as ~TAR12~1.TMP
#          with its bytes; nothing else is in the directory; exit 0. Also
#          ~target.tmp and ~tarball.TMP, names near tar's own, extract
#          normally.
#
#   badseam  AXL_TAR_TEST_TEMP must be a plain leaf name. Two that are not:
#          "FS1:\seamx.TMP" (a volume -- the temporary would not even be
#          creatable beside the member) and "..\esc.TMP" (would put it in
#          the parent). Each is ignored with one warning and a random name
#          is used: the member extracts, nothing lands in the parent, exit 0.
#
# Usage: test/integration/test-tar-fat-qemu.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
PREFIX="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs x64)"
TAR="$PREFIX/tools/tar.efi"
MKRD="$PREFIX/tools/mkrd.efi"

export TEST_SKIP_RATCHET=1
PASS=0
FAIL=0
pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "$2"; FAIL=$((FAIL + 1)); }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [[ ! -x "$TAR" || ! -f "$MKRD" ]]; then
    "$PROJECT_DIR/scripts/axl-build.sh" ARCH=x64 tools 2>&1 | tail -1
fi
[[ -x "$TAR" && -f "$MKRD" ]] || { echo "FAIL: tar.efi / mkrd.efi not built"; exit 1; }

D="$WORK/data"
mkdir -p "$D/f" "$D/al"
printf 'a\n' > "$D/f/a.txt"
head -c 2097152 /dev/urandom > "$D/f/big.bin"
printf 'z-content\n' > "$D/f/z.txt"
tar -cf "$D/fill.tar" -C "$D/f" a.txt big.bin z.txt
printf 'alias-member\n' > "$D/al/~TAR12~1.TMP"
printf 'target\n' > "$D/al/~target.tmp"
printf 'tarball\n' > "$D/al/~tarball.TMP"
tar -cf "$D/alias.tar" -C "$D/al" '~TAR12~1.TMP' '~target.tmp' '~tarball.TMP'
printf 'seam-member\n' > "$D/sm.txt"
tar -cf "$D/seam.tar" -C "$D" sm.txt

cat > "$WORK/fat.nsh" <<'EOF'
fs0:
echo FAT:START
mkrd -s 1 TINY
echo FAT:STEP:fill
tar.efi -x FS1:\fill.tar -C TINY:\
echo FAT:RC-fill=%lasterror%
ls TINY:\
type TINY:\z.txt
echo FAT:STEP:alias
mkdir fs0:\al
set AXL_TAR_TEST_TEMP ~TAR1234ABCD.TMP
set AXL_LOG_LEVEL debug
tar.efi -x FS1:\alias.tar -C fs0:\al
echo FAT:RC-alias=%lasterror%
set -d AXL_TAR_TEST_TEMP
set -d AXL_LOG_LEVEL
ls fs0:\al
type fs0:\al\~TAR12~1.TMP
echo FAT:STEP:badseam
mkdir fs0:\b1
mkdir fs0:\b2
set AXL_TAR_TEST_TEMP FS1:\seamx.TMP
tar.efi -x FS1:\seam.tar -C fs0:\b1
echo FAT:RC-b1=%lasterror%
set AXL_TAR_TEST_TEMP ..\esc.TMP
tar.efi -x FS1:\seam.tar -C fs0:\b2
echo FAT:RC-b2=%lasterror%
set -d AXL_TAR_TEST_TEMP
type fs0:\b1\sm.txt
type fs0:\b2\sm.txt
ls fs0:\esc.TMP
echo FAT:END
reset -s
EOF

LOG="$WORK/guest.log"
timeout 120s "$RUN_QEMU" --mount "$D" --extra "$MKRD" --nsh "$WORK/fat.nsh" "$TAR" \
    > "$LOG" 2>&1 || true
CLEAN="$(tr -d '\r' < "$LOG")"
# One step's lines: after its FAT:STEP marker, up to the next.
step() { awk -v m="FAT:STEP:$1" '$0 == m { on = 1; next } on && /^FAT:(STEP|END)/ { exit } on' <<< "$CLEAN"; }

if grep -qx 'FAT:END' <<< "$CLEAN"; then
    pass "the guest ran both steps to the end"
else
    fail "the guest did not reach FAT:END" "$(tail -20 <<< "$CLEAN")"
fi

# --- fill ------------------------------------------------------------------
F="$(step fill)"
# The names `ls` prints, one per line, from its "date time size name" rows.
f_names="$(grep -E '^[0-9]{2}/[0-9]{2}/[0-9]{4}' <<< "$F" | awk '{print $NF}' | LC_ALL=C sort)"
f_ok=1
grep -qxF 'tar: write TINY:\big.bin failed; not extracted' <<< "$F" || f_ok=0
grep -qx 'FAT:RC-fill=0x1' <<< "$F" || f_ok=0
[[ "$f_names" == $'a.txt\nz.txt' ]] || f_ok=0
grep -qx 'z-content' <<< "$F" || f_ok=0
grep -q 'tar: internal:' <<< "$F" && f_ok=0
if [[ "$f_ok" == 1 ]]; then
    pass "a member that fills the volume is reported and leaves no temporary; the next one extracts"
else
    fail "tar -x mishandled a member that fills the volume" \
         "$F"$'\n'"--- names:"$'\n'"$f_names"
fi

# --- alias -----------------------------------------------------------------
A="$(step alias)"
a_names="$(grep -E '^[0-9]{2}/[0-9]{2}/[0-9]{4}' <<< "$A" | grep -v '<DIR>' | awk '{print $NF}' | LC_ALL=C sort)"
a_ok=1
grep -qE "temporary .*~TAR1234ABCD\.TMP is the member's own name; retrying" <<< "$A" || a_ok=0
grep -qx 'FAT:RC-alias=0x0' <<< "$A" || a_ok=0
[[ "$a_names" == $'~TAR12~1.TMP\n~tarball.TMP\n~target.tmp' ]] || a_ok=0
grep -qx 'alias-member' <<< "$A" || a_ok=0
grep -q 'tar: internal:' <<< "$A" && a_ok=0
if [[ "$a_ok" == 1 ]]; then
    pass "a member named like the temporary's 8.3 alias makes tar retry; it lands whole, under its own name"
else
    fail "tar -x mishandled a member named like its temporary's alias" \
         "$A"$'\n'"--- names:"$'\n'"$a_names"
fi

# --- badseam --------------------------------------------------------------
B="$(step badseam)"
b_ok=1
for v in 'FS1:\seamx.TMP' '..\esc.TMP'; do
    n=$(grep -cF "AXL_TAR_TEST_TEMP '$v' is not a plain file name; ignored" <<< "$B" || true)
    [[ "$n" == 1 ]] || { b_ok=0; echo "  warning for $v seen $n times"; }
done
grep -qx 'FAT:RC-b1=0x0' <<< "$B" || b_ok=0
grep -qx 'FAT:RC-b2=0x0' <<< "$B" || b_ok=0
[[ "$(grep -cx 'seam-member' <<< "$B")" == 2 ]] || b_ok=0
grep -q 'esc\.TMP' <<< "$(grep -E '^[0-9]{2}/[0-9]{2}/[0-9]{4}' <<< "$B")" && b_ok=0
if [[ "$b_ok" == 1 ]]; then
    pass "an AXL_TAR_TEST_TEMP that is not a plain leaf name is ignored with one warning; extraction works"
else
    fail "tar -x mishandled an invalid AXL_TAR_TEST_TEMP" "$B"
fi

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"
[[ "$FAIL" -eq 0 ]]
