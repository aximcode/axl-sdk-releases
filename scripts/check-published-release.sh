#!/bin/bash
# check-published-release.sh — verify a release as a consumer receives it.
#
# WHY THIS EXISTS. AXL-Distribution-Design.md §16 counted what every other
# check proves and what none of them can: they all run BEFORE publication, on
# locally built files. "Nothing in the tree fetches releases/download/...".
# A truncated upload, a missing asset, or a SHA256SUMS that does not match
# would be found by a consumer first.
#
# That exposure rose the moment D2's renames landed, because the failure mode
# became "one asset kept its old name" -- which every local check passes and no
# local check can see.
#
# IT CANNOT GATE, and pretending otherwise would be worse than not running: by
# the time it can fetch anything the release is already public. What it turns
# is "a consumer finds it" into "we find it in minutes" (§16.2, job 2).
#
# VERSION-AGNOSTIC BY CONSTRUCTION. It does not carry a list of expected asset
# names, because a list here would be a fifth copy of the one in §14.1a and
# would go stale exactly when it mattered. Instead it cross-checks the release
# against ITSELF:
#
#   - every name SHA256SUMS lists is actually attached to the release;
#   - every attached asset is covered by SHA256SUMS (bar SHA256SUMS itself);
#   - every one downloads and matches its recorded hash;
#   - VERSION says what the tag says;
#   - the stable `latest/download/` URLs resolve.
#
# A rename that missed one asset shows up as a set mismatch in the first two,
# whichever direction it went, and it works on a release cut before the rename
# as well as after.
#
# Usage:
#   scripts/check-published-release.sh v4.5.0
#   scripts/check-published-release.sh v4.5.0 --base-url file:///tmp/rel
#   scripts/check-published-release.sh v4.5.0 --skip-latest   # prerelease
set -uo pipefail

REPO="${AXL_RELEASES_REPO:-aximcode/axl-sdk-releases}"
TAG=""; BASE_URL=""; SKIP_LATEST=0
ASSET_LIST=""            # override the attached-asset list (testing)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --base-url)    BASE_URL="${2:?--base-url needs a value}"; shift 2 ;;
        --base-url=*)  BASE_URL="${1#--base-url=}"; shift ;;
        --asset-list)  ASSET_LIST="${2:?--asset-list needs a file}"; shift 2 ;;
        --asset-list=*) ASSET_LIST="${1#--asset-list=}"; shift ;;
        --skip-latest) SKIP_LATEST=1; shift ;;
        -h|--help)     sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)            echo "ERROR: unknown option '$1'" >&2; exit 2 ;;
        *)             TAG="$1"; shift ;;
    esac
done
[[ -n "$TAG" ]] || { echo "usage: $0 <tag> [--base-url URL] [--skip-latest]" >&2; exit 2; }

VER="${TAG#v}"
DL="${BASE_URL:-https://github.com/$REPO/releases/download/$TAG}"
LATEST="https://github.com/$REPO/releases/latest/download"

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORK="$(mktemp -d -t axl-relcheck.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

echo "Checking published release $TAG of $REPO"
echo "  from: $DL"
echo ""

# ---- SHA256SUMS ----------------------------------------------------------
if ! curl -fsSL -o "$WORK/SHA256SUMS" "$DL/SHA256SUMS"; then
    fail "SHA256SUMS is not downloadable from $DL"
    echo ""; echo "published-release: $PASS passed, $FAIL failed"; exit 1
fi
# Strip any ./ prefix, the same way install.sh does.
awk '{ f = $NF; sub(/^\.\//, "", f); print f }' "$WORK/SHA256SUMS" | sort > "$WORK/summed.txt"
SUMMED_N=$(wc -l < "$WORK/summed.txt")
if [[ "$SUMMED_N" -ge 4 ]]; then
    pass "SHA256SUMS lists $SUMMED_N assets"
else
    fail "SHA256SUMS lists only $SUMMED_N assets -- refusing to certify that"
fi

# ---- the attached set, from the release itself ---------------------------
# A rename that missed one asset is a DIFFERENCE between what is attached and
# what is checksummed, in whichever direction it went.
if [[ -n "$ASSET_LIST" ]]; then
    sort "$ASSET_LIST" > "$WORK/attached.txt"
elif command -v gh >/dev/null 2>&1; then
    if gh release view "$TAG" --repo "$REPO" --json assets \
           --jq '.assets[].name' 2>/dev/null | sort > "$WORK/attached.txt"; then
        :
    else
        : > "$WORK/attached.txt"
    fi
else
    : > "$WORK/attached.txt"
fi

if [[ -s "$WORK/attached.txt" ]]; then
    # SHA256SUMS does not list itself, by design.
    grep -vx 'SHA256SUMS' "$WORK/attached.txt" > "$WORK/attached-net.txt" || true
    if diff_out=$(diff "$WORK/summed.txt" "$WORK/attached-net.txt" 2>&1); then
        pass "every attached asset is checksummed, and vice versa"
    else
        fail "the attached asset set and SHA256SUMS disagree:"
        printf '%s\n' "$diff_out" | sed 's/^/        /'
        echo "        (< only in SHA256SUMS, > only attached to the release)"
    fi
else
    echo "  SKIP: could not list the release's attached assets (no gh, or no"
    echo "        network) -- the set comparison did not run. Hash + content"
    echo "        checks below still did."
fi

# ---- every asset downloads and matches ----------------------------------
bad=""
while read -r name; do
    [[ -n "$name" ]] || continue
    if ! curl -fsSL -o "$WORK/$name" "$DL/$name"; then
        bad="$bad $name(404)"; continue
    fi
    want=$(awk -v n="$name" '{ f = $NF; sub(/^\.\//, "", f) } f == n { print $1 }' "$WORK/SHA256SUMS")
    got=$(sha256sum "$WORK/$name" | awk '{print $1}')
    [[ "$want" == "$got" ]] || bad="$bad $name(hash)"
done < "$WORK/summed.txt"
if [[ -z "$bad" ]]; then
    pass "all $SUMMED_N assets download and match their recorded hash"
else
    fail "assets failed to download or verify:$bad"
fi

# ---- the release says which version it is -------------------------------
if [[ -f "$WORK/VERSION" ]]; then
    got=$(tr -d ' \t\r\n' < "$WORK/VERSION")
    if [[ "$got" == "$VER" ]]; then
        pass "VERSION says $VER, matching the tag"
    else
        fail "VERSION says '$got' but the tag is '$TAG' -- install.sh resolves"
        echo "        'latest' through this file, so a wrong value sends every"
        echo "        default install to the wrong release"
    fi
else
    echo "  SKIP: no VERSION asset (releases before D2 published none)"
fi

# ---- the two stable URLs a first-time user hits -------------------------
# These are the ONLY names that must never change (§14.1a). A release that
# publishes them under a versioned name breaks `curl .../latest/download/...`
# for everyone, and nothing local can see it.
if [[ "$SKIP_LATEST" -eq 1 || -n "$BASE_URL" ]]; then
    echo "  SKIP: latest/download checks (prerelease or --base-url)"
elif ! grep -qx 'install.sh' "$WORK/summed.txt"; then
    # A release cut before D2 published neither asset, so there is no contract
    # to break. Reporting one as broken there would be a false alarm that
    # trains the reader to ignore this check.
    echo "  SKIP: this release publishes no install.sh (pre-D2), so the"
    echo "        latest/download contract does not apply to it"
else
    for name in install.sh VERSION; do
        if curl -fsSL -o /dev/null "$LATEST/$name"; then
            pass "latest/download/$name resolves"
        else
            fail "latest/download/$name does NOT resolve -- the stable-URL"
            echo "        contract is broken; README's install command is dead"
        fi
    done
fi

echo ""
echo "published-release: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
