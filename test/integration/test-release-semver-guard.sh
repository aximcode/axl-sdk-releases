#!/bin/bash
# test-meta: arch=none needs= est=2 local-only=0
# test-release-semver-guard.sh — the release cut refuses to ship a documented
# breaking change under a non-major version.
#
# Regression guard for the v3.2.3 incident. cut-release.sh had exactly one
# CHANGELOG precondition -- that a "## Unreleased" heading EXISTS -- and then
# dated whatever sat under it. "## Unreleased" is branch-wide state while a
# release is a commit range, and those agree only when the release is
# "everything on main since the last tag". This project stopped working that
# way three releases ago, so a patch cut from main dated 43 commits of
# in-progress work, two of them under a "### Breaking" heading, and published
# eight assets before anyone noticed.
#
# The information was never missing: the script prints the whole commit range
# before its confirm prompt, and the tag message it generated listed the
# breaking entries by name. It did not help, and --yes skips the prompt
# entirely. So this is a REFUSAL, not a louder banner -- the same doctrine as
# check-log-levels' marker, where the burden is inverted onto the author.
#
# Host-only: needs no QEMU, no build. Runs the guard against fixtures.
#
# Usage: ./test/integration/test-release-semver-guard.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
GUARD="$PROJECT_DIR/scripts/check-release-semver.sh"

WORK="$(mktemp -d)"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Build a fixture repo root: VERSION + CHANGELOG.md are all the guard reads.
# $1 current version, $2 the "## Unreleased" body.
fixture() {
    rm -rf "$WORK/repo"; mkdir -p "$WORK/repo"
    echo "$1" > "$WORK/repo/VERSION"
    { echo "# Changelog"; echo; echo "## Unreleased"; echo; printf '%s\n' "$2";
      echo; echo "## 1.0.0 — 2020-01-01"; echo; echo "### Changed"; echo
      echo "- something old that IS breaking-labelled, below the section"
      echo "### Breaking"; echo "- an OLD breaking entry, already released"; } \
        > "$WORK/repo/CHANGELOG.md"
}

# $1 label, $2 target version, $3 expected exit (0 ok / 1 refuse), $4.. extra args
check() {
    local label="$1" target="$2" want="$3"; shift 3
    local out rc
    out="$(cd "$WORK/repo" && "$GUARD" "$target" "$@" 2>&1)" && rc=0 || rc=$?
    if [[ "$rc" -eq "$want" ]]; then
        pass "$label"
    else
        fail "$label (wanted exit $want, got $rc)"
        printf '        %s\n' "$out" | head -4
    fi
}

echo "=== release semver guard ==="

# --- the incident itself -------------------------------------------------
fixture 3.2.2 "$(printf '### Changed\n- a log sweep\n\n### Breaking\n- axl-cc --depfile is removed\n')"
check "refuses a PATCH carrying '### Breaking'"        3.2.3 1
check "refuses a MINOR carrying '### Breaking'"        3.3.0 1
check "allows a MAJOR carrying '### Breaking'"         4.0.0 0
check "--allow-breaking overrides the patch refusal"   3.2.3 0 --allow-breaking

# --- it must not fire on the ordinary case -------------------------------
fixture 3.2.2 "$(printf '### Changed\n- a log sweep\n\n### Fixed\n- a bug\n')"
check "allows a PATCH with no breaking entries"        3.2.3 0
check "allows a MINOR with no breaking entries"        3.3.0 0

# --- scoping: only the section about to be DATED counts -------------------
#
# The fixture's already-released 1.0.0 section carries its own "### Breaking".
# A guard that grepped the whole file would refuse every patch release this
# project will ever cut, which is the failure mode that gets a check deleted.
fixture 3.2.2 "$(printf '### Fixed\n- a bug\n')"
check "ignores '### Breaking' in an ALREADY-RELEASED section" 3.2.3 0

# --- a heading that merely mentions the word ------------------------------
fixture 3.2.2 "$(printf '### Fixed\n- restored a thing that was breaking builds\n')"
check "prose containing 'breaking' is not a Breaking heading" 3.2.3 0

echo ""
printf 'release semver guard: %d passed, %d failed\n' "$PASS" "$FAIL"
[[ $FAIL -eq 0 && $PASS -gt 0 ]]
