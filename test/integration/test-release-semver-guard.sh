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

# $1 label, $2 target version, $3.. substrings the refusal output MUST name.
# Separate from check() because an exit code is not the whole contract: the
# refusal is a REPORT a human reads to decide --allow-breaking, so what it
# LISTS is load-bearing. A truncated list reads as a complete one.
check_names() {
    local label="$1" target="$2"; shift 2
    local out; out="$(cd "$WORK/repo" && "$GUARD" "$target" 2>&1)" || true
    local missing=""
    for want in "$@"; do
        [[ "$out" == *"$want"* ]] || missing+=" '$want'"
    done
    if [[ -z "$missing" ]]; then
        pass "$label"
    else
        fail "$label (never named:$missing)"
        printf '        %s\n' "$out" | head -8
    fi
}

echo "=== release semver guard ==="

# --- the incident itself -------------------------------------------------
fixture 3.2.2 "$(printf '### Changed\n- a log sweep\n\n### Breaking\n- axl-cc --depfile is removed\n')"
check "refuses a PATCH carrying '### Breaking'"        3.2.3 1
check "refuses a MINOR carrying '### Breaking'"        3.3.0 1
check "allows a MAJOR carrying '### Breaking'"         4.0.0 0
check "--allow-breaking overrides the patch refusal"   3.2.3 0 --allow-breaking

# --- packaging breaks are a DIFFERENT bucket ------------------------------
#
# Semver's MAJOR rule is about incompatible API changes. Renaming a release
# asset, or ceasing to publish one, breaks a consumer's BUILD SCRIPTS while
# their code still compiles and links unchanged -- a real break, and not the
# one semver mandates a major for. Conflating the two forced 4.5.0 to become
# 5.0.0 for a release in which axl.h did not change.
#
# So "### Breaking (packaging)" is permitted below a major. It is NOT silent:
# the guard must still list the entries, because the whole doctrine here is
# that the exceptional case says so out loud.
fixture 4.4.0 "$(printf '### Breaking (packaging)\n- every release asset is renamed\n- the .deb and .rpm are retired\n')"
check "allows a MINOR carrying '### Breaking (packaging)'"   4.5.0 0
check "allows a MAJOR carrying '### Breaking (packaging)'"   5.0.0 0
check "allows a PATCH carrying '### Breaking (packaging)'"   4.4.1 0
check_names "a packaging break is still LISTED, not passed silently" 4.5.0 \
      "every release asset is renamed" "the .deb and .rpm are retired"

# ...and the plain heading is unaffected by the new one existing.
fixture 4.4.0 "$(printf '### Breaking\n- axl_foo() is removed\n\n### Breaking (packaging)\n- an asset is renamed\n')"
check "an API break still forces a major even beside a packaging one" 4.5.0 1

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

# --- the refusal must name EVERY breaking entry, not the first ------------
#
# Real entries run to several paragraphs. A listing that reads only a few lines
# past the heading names the first bullet and silently drops the rest -- and the
# reader deciding whether to pass --allow-breaking sees a list that looks
# complete. That is the original incident again, one layer in: the information
# is present in the file and absent from the report.
fixture 4.2.0 "$(printf '### Breaking\n\n- **FIRST breaking entry.** Lorem ipsum dolor sit amet, a body\n  long enough to run past the heading by several lines.\n\n  A second paragraph, because real entries have them.\n\n  | a | table |\n  |---|---|\n  | of | numbers |\n\n- **SECOND breaking entry.** Also multi-line, and the one a\n  short listing drops.\n\n  With its own trailing paragraph.\n\n### Fixed\n\n- **NOT a breaking entry** and must not be listed as one.\n')"
check       "refuses a MINOR carrying multi-paragraph breaking entries" 4.3.0 1
check_names "the refusal names EVERY breaking entry, not just the first" 4.3.0 \
            "FIRST breaking entry" "SECOND breaking entry"

# ...and does not spill into the next section. A listing that ran to the end of
# the body would report Fixed entries as breaking, which is the same defect
# pointing the other way and just as misleading.
out="$(cd "$WORK/repo" && "$GUARD" 4.3.0 2>&1)" || true
if [[ "$out" != *"NOT a breaking entry"* ]]; then
    pass "...and stops at the next '###' heading"
else
    fail "the listing ran past '### Breaking' into the following section"
fi

echo ""
printf 'release semver guard: %d passed, %d failed\n' "$PASS" "$FAIL"
[[ $FAIL -eq 0 && $PASS -gt 0 ]]
