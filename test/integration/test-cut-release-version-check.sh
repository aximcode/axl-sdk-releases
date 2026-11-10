#!/bin/bash
# test-meta: arch=none needs=git est=5 local-only=0
# test-cut-release-version-check.sh -- cut-release.sh's version check (step
# 6) STOPS a cut whose bump left VERSION and axl-version.h disagreeing.
#
# The line was `make check-version && note ...` until slice 5c, and under
# errexit a failing first command of a && list does not exit: the check could
# never stop a cut. It became `scripts/check-version.sh || die ...`, and no
# test reached it -- every other cut-release test stops at argument parsing
# or a precondition. This one drives the real script that far, in a fixture
# repository, with --dry-run (so it reverts and stops before any commit or
# push), --no-docs-check, --tag-untested (a decision that needs no CI) and a
# stub `gh` on PATH. Nothing touches the network or this tree.
#
# The bump is a stub (scripts/bump-version.sh in the fixture): it writes
# VERSION, and the header only when told to. CONTROL: with both written the
# same cut reaches "check-version OK", so the refusal below is the check's.
#
# Host-only. Usage: ./test/integration/test-cut-release-version-check.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}

# The caller's release-gate seams must not reach the fixture's run.
unset AXL_GH AXL_STAMP_FILE AXL_MAX_BRANCH_COMMITS
WORK="$(mktemp -d -t axl-cutver.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT
R="$WORK/repo"
mkdir -p "$R/scripts" "$R/test/integration/lib" "$R/include/axl" "$WORK/bin"
for f in cut-release.sh axl-common.sh check-release-semver.sh check-version.sh; do
    cp "$PROJECT_DIR/scripts/$f" "$R/scripts/"
done
cp "$PROJECT_DIR/test/integration/lib/release-gate.sh" "$R/test/integration/lib/"
cat > "$R/scripts/bump-version.sh" <<'EOF'
#!/bin/bash
# Stub: VERSION always; the header only with BUMP_HEADER=1.
echo "$1" > VERSION
[[ "${BUMP_HEADER:-0}" == 1 ]] && \
    sed -i "s/^#define AXL_VERSION_STRING .*/#define AXL_VERSION_STRING  \"$1\"/" include/axl/axl-version.h
exit 0
EOF
chmod +x "$R/scripts/"*.sh
echo 0.0.1 > "$R/VERSION"
printf '#define AXL_VERSION_STRING  "0.0.1"\n' > "$R/include/axl/axl-version.h"
printf '# Changelog\n\n## Unreleased\n\n### Fixed\n\n- **A fix.**\n\n## 0.0.1 — 2026-01-01\n\n- first\n' \
    > "$R/CHANGELOG.md"
# gh: the API answers as a user; every other call answers nothing, as a
# repository with no runs would.
cat > "$WORK/bin/gh" <<'EOF'
#!/bin/bash
[[ "$1 $2" == "api user" ]] && { echo fixture-user; exit 0; }
exit 0
EOF
chmod +x "$WORK/bin/gh"

git init -q --bare "$WORK/origin.git"
(
    cd "$R" || exit 1
    git init -q -b main .
    git config user.email t@t; git config user.name t
    git config commit.gpgsign false; git config tag.gpgsign false
    git add -A; git commit -qm "0.0.1"; git tag v0.0.1
    printf 'x\n' > fix.txt; git add fix.txt; git commit -qm "a fix"
    git remote add origin "$WORK/origin.git"
    git push -q origin main --tags
) || { check "the fixture repository is made" "failed" "made"; exit 1; }

cut() {   # cut [ENV...] -> stdout+stderr of a dry-run cut of 0.0.2; its status
    (cd "$R" && env PATH="$WORK/bin:$PATH" "$@" \
        ./scripts/cut-release.sh 0.0.2 --dry-run --yes --no-docs-check --tag-untested 2>&1)
}

echo "=== cut-release.sh: the version check stops a mismatched bump ==="
out="$(cut BUMP_HEADER=1)"; rc=$?
check "control: a bump that writes both reaches the check, and passes it" \
    "$rc $(grep -cxF '  check-version OK' <<< "$out")" "0 1"

out="$(cut BUMP_HEADER=0)"; rc=$?
check "a bump that leaves the header behind stops the cut (1)" "$rc" "1"
check "...at the version check, saying so" "$(tail -1 <<< "$out")" \
    "ERROR: VERSION and axl-version.h disagree (above)"
check "...with the check's own reason above it" \
    "$(grep -cxF 'ERROR: VERSION (0.0.2) != axl-version.h (0.0.1)' <<< "$out")" "1"
check "...and never reports the check passed" "$(grep -c 'check-version OK' <<< "$out")" "0"
check "...and the half-done bump is reverted: VERSION and the CHANGELOG as committed" \
    "$(cd "$R" && git status --porcelain --untracked-files=no)" ""

echo ""
echo "cut-release-version-check: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
