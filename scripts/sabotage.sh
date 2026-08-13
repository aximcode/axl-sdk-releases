#!/bin/bash
# sabotage.sh — apply a deliberate defect, run a command, restore safely.
#
# CLAUDE.md requires sabotage verification: break the code on purpose and
# confirm the test notices. Doing that by hand has two footguns, and both have
# already produced a wrong answer in this tree:
#
#   1. RESTORE MTIME. `sed -i.bak file && mv file.bak file` gives the restored
#      source the BACKUP's mtime, which is older than the object built from the
#      sabotaged source. make then sees the .o as up to date and skips the
#      rebuild -- so the next run links the PREVIOUS sabotage and reports
#      failures belonging to an edit that is no longer in the file. This script
#      always `touch`es on restore.
#
#   2. A SABOTAGE THAT SABOTAGES NOTHING. A sed that matches no line leaves the
#      file untouched; the suite passes; the natural reading is "no test covers
#      this", which is the exact opposite of the truth. This script asserts the
#      file actually changed before running anything.
#
# It also restores on Ctrl-C / failure (trap), and verifies the restored file is
# byte-identical to the original via sha256 -- a restore that silently did not
# restore is worse than no restore at all.
#
# Usage:
#   scripts/sabotage.sh -s FILE:SED_EXPR [-s ...] [--expect-fail] -- CMD [ARGS...]
#   scripts/sabotage.sh -p PATCH_FILE    [-p ...] [--expect-fail] -- CMD [ARGS...]
#
# Options:
#   -s FILE:SED    apply SED_EXPR to FILE with `sed -i` (no .bak needed --
#                  this script keeps its own snapshot outside the tree)
#   -p PATCH       apply PATCH with `git apply`; every file it touches is
#                  snapshotted and restored
#   --expect-fail  invert the exit status: succeed only if CMD FAILED. This is
#                  what sabotage verification actually asserts -- "the suite
#                  must notice". Without it the script propagates CMD's status.
#   -k, --keep     leave the sabotage in place (skip restore); for debugging
#
# Examples:
#   scripts/sabotage.sh -s 'src/util/axl-tar.c:s/sum += hdr\[i\]/sum += 0/' \
#       --expect-fail -- ./test/integration/test-axl.sh
#
#   scripts/sabotage.sh -p /tmp/break-it.diff --expect-fail -- make check-nul
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

SEDS=()
PATCHES=()
EXPECT_FAIL=0
KEEP=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -s) SEDS+=("$2"); shift 2 ;;
        -p) PATCHES+=("$2"); shift 2 ;;
        --expect-fail) EXPECT_FAIL=1; shift ;;
        -k|--keep) KEEP=1; shift ;;
        -h|--help) sed -n '2,40p' "${BASH_SOURCE[0]}"; exit 0 ;;
        --) shift; break ;;
        *) echo "sabotage.sh: unknown option $1 (did you forget '--'?)" >&2; exit 2 ;;
    esac
done

CMD=("$@")
if [[ ${#CMD[@]} -eq 0 ]]; then
    echo "sabotage.sh: no command given (expected '-- CMD ...')" >&2; exit 2
fi
if [[ ${#SEDS[@]} -eq 0 && ${#PATCHES[@]} -eq 0 ]]; then
    echo "sabotage.sh: no sabotage given (expected -s or -p)" >&2; exit 2
fi

# ---- collect the files this sabotage will touch ---------------------------
FILES=()
for spec in ${SEDS[@]+"${SEDS[@]}"}; do
    FILES+=("${spec%%:*}")
done
for patch in ${PATCHES[@]+"${PATCHES[@]}"}; do
    [[ -f "$patch" ]] || { echo "sabotage.sh: no such patch: $patch" >&2; exit 2; }
    while IFS= read -r f; do FILES+=("$f"); done \
        < <(git apply --numstat "$patch" | awk '{print $3}')
done

SNAP=$(mktemp -d -t axl-sabotage.XXXXXX)
declare -A ORIG_SUM=()

for f in "${FILES[@]}"; do
    [[ -f "$f" ]] || { echo "sabotage.sh: no such file: $f" >&2; rm -rf "$SNAP"; exit 2; }
    mkdir -p "$SNAP/$(dirname "$f")"
    cp -p -- "$f" "$SNAP/$f"
    ORIG_SUM["$f"]=$(sha256sum -- "$f" | cut -d' ' -f1)
done

# ---- restore is unconditional: normal exit, failure, or Ctrl-C -------------
restore() {
    [[ $KEEP -eq 1 ]] && { echo "sabotage.sh: --keep given; sabotage LEFT IN PLACE"; return; }
    local bad=0
    for f in "${FILES[@]}"; do
        cp -- "$SNAP/$f" "$f"
        # touch AFTER the copy: cp preserves nothing here (no -p on purpose),
        # but be explicit -- make must see the restored source as NEWER than
        # any object built from the sabotaged one, or it skips the rebuild and
        # the next run silently tests the old sabotage.
        touch -- "$f"
        local now
        now=$(sha256sum -- "$f" | cut -d' ' -f1)
        if [[ "$now" != "${ORIG_SUM[$f]}" ]]; then
            echo "sabotage.sh: *** RESTORE FAILED for $f (sha mismatch) ***" >&2
            echo "sabotage.sh: original preserved at $SNAP/$f" >&2
            bad=1
        fi
    done
    if [[ $bad -eq 0 ]]; then
        rm -rf "$SNAP"
        echo "sabotage.sh: restored ${#FILES[@]} file(s), byte-identical, mtime bumped"
    fi
}
trap restore EXIT
trap 'echo "sabotage.sh: interrupted"; exit 130' INT TERM

# ---- apply ----------------------------------------------------------------
for spec in ${SEDS[@]+"${SEDS[@]}"}; do
    f="${spec%%:*}"; expr="${spec#*:}"
    sed -i -e "$expr" -- "$f" || { echo "sabotage.sh: sed failed on $f" >&2; exit 2; }
done
for patch in ${PATCHES[@]+"${PATCHES[@]}"}; do
    git apply -- "$patch" || { echo "sabotage.sh: git apply failed: $patch" >&2; exit 2; }
done

# ---- assert the sabotage IS a sabotage ------------------------------------
unchanged=()
for f in "${FILES[@]}"; do
    now=$(sha256sum -- "$f" | cut -d' ' -f1)
    [[ "$now" == "${ORIG_SUM[$f]}" ]] && unchanged+=("$f")
done
if [[ ${#unchanged[@]} -eq ${#FILES[@]} ]]; then
    cat >&2 <<EOF
sabotage.sh: *** NO-OP SABOTAGE — nothing changed ***
  ${unchanged[*]}
  The edit matched nothing, so running the suite would prove nothing: a pass
  would read as "no test covers this" when the truth is "no defect was
  introduced". Fix the expression and re-run.
EOF
    exit 3
fi
# Also bump mtime forward now, so the sabotaged source is unambiguously newer
# than any object already built from the pristine one.
touch -- "${FILES[@]}"

echo "sabotage.sh: sabotaged ${#FILES[@]} file(s); running: ${CMD[*]}"
echo "---------------------------------------------------------------"
"${CMD[@]}"
rc=$?
echo "---------------------------------------------------------------"
echo "sabotage.sh: command exited $rc"

if [[ $EXPECT_FAIL -eq 1 ]]; then
    if [[ $rc -eq 0 ]]; then
        echo "sabotage.sh: *** FAIL — the sabotage was NOT detected ***" >&2
        echo "  The code is broken and the command still succeeded, so nothing" >&2
        echo "  here actually tests this path." >&2
        exit 1
    fi
    echo "sabotage.sh: OK — the sabotage was detected (expected failure)."
    exit 0
fi
exit $rc
