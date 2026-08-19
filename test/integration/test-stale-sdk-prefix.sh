#!/bin/bash
# test-meta: arch=none needs= est=4 local-only=0
# test-stale-sdk-prefix.sh — a staged SDK left INSIDE the source tree must not
# be able to serve itself as the SDK.
#
# WHY THIS EXISTS. `scripts/axl-cc` resolves SDK_DIR to the repo root. That is
# correct for a staged prefix (stage/bin/axl-cc -> stage/) and meaningless for
# the repo -- until the repo root happens to LOOK like a prefix, which is
# exactly what an old `install.sh --prefix .` leaves behind:
#
#   bin/  lib/  share/axl/     all gitignored (.gitignore root-anchors each),
#                              so the tree reads as clean in `git status`
#
# A consumer building against the dev tip then got:
#
#   $ .../axl-sdk/scripts/axl-cc --version
#   axl-cc 2.8.7 (gcc, built 2026-07-08)
#
# -- six weeks and two major versions behind the checkout it lives in, on a
# tree whose `install.sh --arch all` had just staged 4.2.0 into stage/ and said
# nothing. Everything compiles, links, boots and passes; the only symptom is a
# version string nobody reads twice. The failure corrupts a MEASUREMENT, not a
# build, which is the same shape as the build-state-signature bug AXL_TLS was
# folded into the signature for.
#
# Two independent guards, and this pins both:
#
#   1. install.sh WARNS about a staged SDK it can see but does not own. It
#      already did this for the pre-O1 ./out default; the repo root is the
#      OTHER historical prefix and was uncovered.
#   2. scripts/axl-cc REFUSES rather than guessing, so the guard does not
#      depend on anyone running install.sh.
#
# The removal advice differs between the two candidates and that is
# load-bearing, not cosmetic: at the repo root `share/` and `include/` hold
# TRACKED SOURCES (share/jedec.json5, include/axl/*.h), so the ./out block's
# `rm -rf .../{bin,lib,include,share}` would delete the source tree. Section 3
# asserts the repo-root message never carries it.
#
# Host-only: no QEMU, no build. Never mutates the repo -- the suite runs a
# parallel pool and a stale prefix appearing at the repo root mid-run would
# poison every other test that resolves an SDK.
#
# Usage: ./test/integration/test-stale-sdk-prefix.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

source "$PROJECT_DIR/scripts/axl-common.sh"

WORK="$(mktemp -d -t axl-stale-prefix.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# check_has <haystack> <needle> <msg> — substring, for prose we do not pin
check_has()  { [[ "$1" == *"$2"* ]] && pass "$3" || { fail "$3"; printf '%s\n' "$1" | head -6 | sed 's/^/      /'; }; }
check_lacks() { [[ "$1" != *"$2"* ]] && pass "$3" || { fail "$3"; printf '%s\n' "$1" | head -6 | sed 's/^/      /'; }; }

# stage_fake <dir> <kind> — build a directory that LOOKS like a staged SDK.
#   version  only share/axl/version+build-date  (what axl-cc --version reads)
#   libs     only lib/axl/<arch>/libaxl.a       (what axl-cc links against)
#   driver   only bin/axl-cc                    (what the ./out check keyed on)
stage_fake() {
    local d="$1" kind="$2"
    case "$kind" in
        version) mkdir -p "$d/share/axl"
                 echo "2.8.7"      > "$d/share/axl/version"
                 echo "2026-07-08" > "$d/share/axl/build-date" ;;
        libs)    mkdir -p "$d/lib/axl/x64"; : > "$d/lib/axl/x64/libaxl.a" ;;
        driver)  mkdir -p "$d/bin"; printf '#!/bin/sh\n' > "$d/bin/axl-cc"
                 chmod +x "$d/bin/axl-cc" ;;
    esac
}

echo "=== 1. install.sh's stale-prefix warning: what it SEES ==="

# The three leftovers are asserted separately because the reported failure was
# NOT the one the ./out check looks for. The consumer invoked scripts/axl-cc,
# which reads share/axl/version and lib/axl/<arch> and never touches
# bin/axl-cc -- so a predicate keyed on the driver alone misses a half-cleaned
# tree that still lies about its version.
for kind in version libs driver; do
    root="$WORK/see-$kind"; mkdir -p "$root"
    stage_fake "$root" "$kind"
    out="$(axl_warn_stale_sdk_prefix "$WORK/elsewhere" "$root" 2>&1)"
    check_has "$out" "$root" "a leftover '$kind' is seen"
done

# ...and a source tree with none of them is silent. A warning that fires on a
# clean checkout is one every reader learns to skip.
root="$WORK/clean"; mkdir -p "$root/src" "$root/scripts" "$root/include/axl" "$root/share"
: > "$root/share/jedec.json5"
out="$(axl_warn_stale_sdk_prefix "$WORK/elsewhere" "$root" 2>&1)"
[[ -z "$out" ]] && pass "a clean source tree is silent" \
                || { fail "a clean source tree warned"; printf '%s\n' "$out" | sed 's/^/      /'; }

echo "=== 2. ...and what it must NOT flag ==="

# `install.sh --prefix .` is a deliberate in-tree install -- the gitignore
# entries exist FOR it. Warning about the prefix this run just wrote is noise.
root="$WORK/intree"; mkdir -p "$root"; stage_fake "$root" version
out="$(axl_warn_stale_sdk_prefix "$root" "$root" 2>&1)"
[[ -z "$out" ]] && pass "the prefix this run owns is not flagged" \
                || { fail "flagged its own prefix"; printf '%s\n' "$out" | sed 's/^/      /'; }

# Same install, spelled the way a human types it. PREFIX arrives from --prefix
# verbatim and is never normalised, so a literal `!=` compares "." against an
# absolute path and warns about the directory it just installed into.
out="$(cd "$root" && axl_warn_stale_sdk_prefix "." "$root" 2>&1)"
[[ -z "$out" ]] && pass "a relative --prefix . is recognised as the same tree" \
                || { fail "relative --prefix . was flagged"; printf '%s\n' "$out" | sed 's/^/      /'; }

# The pre-O1 ./out prefix keeps its own warning; this is the case that already
# worked and must not regress.
root="$WORK/without"; mkdir -p "$root"; stage_fake "$root/out" driver
out="$(axl_warn_stale_sdk_prefix "$WORK/elsewhere" "$root" 2>&1)"
check_has "$out" "$root/out" "the pre-O1 ./out prefix is still reported"

echo "=== 3. the removal advice is SAFE at the repo root ==="

# The ./out block says `rm -rf <dir>/{bin,lib,include,share}`. Reused verbatim
# one directory up that deletes include/axl/*.h and share/*.json5 -- TRACKED
# SOURCES. So the glob is what these assert against, in every form of the
# message: the advice may never name include/ or share/ unsuffixed.
GLOB='{bin,lib,include,share}'

# 3a. A git checkout. The four leftover paths are gitignored BY NAME
#     (.gitignore root-anchors /bin/ /lib/ /include/axl-sdk/ /share/axl/), so
#     `git clean -Xdf --` removes exactly them and can touch nothing tracked.
root="$WORK/advice"; mkdir -p "$root/.git"; stage_fake "$root" version
out="$(axl_warn_stale_sdk_prefix "$WORK/elsewhere" "$root" 2>&1)"
check_has   "$out" "git clean -Xdf" "in a checkout the fix is git clean"
check_lacks "$out" "$GLOB"          "...and never the {bin,lib,include,share} glob"
# The version it found is the fact that makes the warning actionable -- "2.8.7"
# next to a 4.2.0 checkout is what a reader reacts to, where a bare path is
# just another line of build output.
check_has   "$out" "2.8.7"          "the stale version is named"

# 3a'. A git WORKTREE keeps .git as a FILE, not a directory -- and this tree
#      uses worktrees. A `-d` test would quietly send those users down the
#      tarball branch below.
root="$WORK/advice-wt"; mkdir -p "$root"; : > "$root/.git"; stage_fake "$root" version
out="$(axl_warn_stale_sdk_prefix "$WORK/elsewhere" "$root" 2>&1)"
check_has   "$out" "git clean -Xdf" "a worktree (.git is a FILE) still gets git clean"
# The pathspecs are relative, so the command has to say where to run it.
check_has   "$out" "cd $root && git clean" "...and the command carries its own cd"

# 3b. Unpacked from a release source tarball there is no .git, and README.md
#     teaches exactly that install path -- so `git clean` there is a command
#     the reader cannot run. Name the four paths instead, still never the glob.
root="$WORK/advice-tar"; mkdir -p "$root"; stage_fake "$root" version
out="$(axl_warn_stale_sdk_prefix "$WORK/elsewhere" "$root" 2>&1)"
check_lacks "$out" "git clean"            "without a .git it does not say git clean"
check_has   "$out" "$root/include/axl-sdk" "...it names include/axl-sdk, not include"
check_has   "$out" "$root/share/axl"       "...and share/axl, not share"
check_lacks "$out" "$GLOB"                 "...and still never the glob"

# The ./out message keeps rm -rf and its glob: that directory holds no sources,
# and this is the path that already worked.
root="$WORK/advice-out"; mkdir -p "$root"; stage_fake "$root/out" driver
out="$(axl_warn_stale_sdk_prefix "$WORK/elsewhere" "$root" 2>&1)"
check_has "$out" "rm -rf" "the ./out message still says rm -rf"

echo "=== 4. install.sh wires it up ==="

# A helper install.sh does not call is not a guard. Asserted structurally
# because the alternative -- staging a fake prefix at the real repo root and
# running install.sh -- would corrupt every concurrently running test.
calls=$(grep -c 'axl_warn_stale_sdk_prefix' "$PROJECT_DIR/scripts/install.sh")
[[ "$calls" -eq 1 ]] && pass "install.sh calls the helper exactly once" \
                     || fail "install.sh has $calls calls to the helper, expected 1"

# And does not keep a hand-rolled second copy of the check next to it. The two
# candidates must come from ONE derivation or they drift, which is how the
# repo root went uncovered for the whole life of the ./out warning.
stale=$(grep -c '_old_default' "$PROJECT_DIR/scripts/install.sh")
[[ "$stale" -eq 0 ]] && pass "the inline ./out block is gone, not duplicated" \
                     || fail "install.sh still carries $stale _old_default line(s)"

echo "=== 5. scripts/axl-cc refuses to act as a staged SDK ==="

AXL_CC="$PROJECT_DIR/scripts/axl-cc"
printf 'int main(void){return 0;}\n' > "$WORK/t.c"

# --version is the call that LIED. It reported 2.8.7 against a 4.2.0 checkout
# and exited 0; on a cleaned tree it answers "unknown" and still exits 0.
# Neither is an answer, and both are silent.
out="$("$AXL_CC" --version 2>&1)" && rc=0 || rc=$?
[[ "$rc" -ne 0 ]] && pass "--version from the source tree refuses" \
                  || { fail "--version exited 0 (said: $out)"; }
check_has "$out" "stage/bin/axl-cc" "--version names the driver to run instead"

# ...and the compile path, which is where a stale library would actually be
# linked in. README.md promises this fails -- true only on a CLEAN checkout,
# where it fails for the incidental reason that lib/axl/<arch> is missing.
out="$("$AXL_CC" "$WORK/t.c" -o "$WORK/t.efi" 2>&1)" && rc=0 || rc=$?
[[ "$rc" -ne 0 ]] && pass "a compile from the source tree refuses" \
                  || fail "a compile from the source tree succeeded"
check_has "$out" "stage/bin/axl-cc" "the compile refusal names the staged driver"

# The C++ alias must name ITSELF. `axl-c++` execs this script, and telling a
# C++ user to go run axl-cc is the exact bug AXL_CC_PROGNAME was added to fix
# -- a refusal is no place to reintroduce it.
out="$("$PROJECT_DIR/scripts/axl-c++" --version 2>&1)" && rc=0 || rc=$?
[[ "$rc" -ne 0 ]] && pass "the axl-c++ alias refuses as well" \
                  || fail "axl-c++ --version exited 0 (said: $out)"
check_has "$out" "stage/bin/axl-c++" "...and names its OWN staged driver"

echo "=== 6. ...and a REAL staged prefix still works ==="

# The other half of the predicate, and the one that breaks consumers if it is
# wrong. A refusal that also fires on an installed SDK is worse than the bug.
# Built here rather than borrowed from ./stage so this asserts the predicate,
# not whatever the last install left lying around.
PFX="$WORK/pfx"; mkdir -p "$PFX/bin" "$PFX/share/axl"
install -m 755 "$AXL_CC" "$PFX/bin/axl-cc"
echo "9.9.9"      > "$PFX/share/axl/version"
echo "2026-01-01" > "$PFX/share/axl/build-date"
out="$("$PFX/bin/axl-cc" --version 2>&1)" && rc=0 || rc=$?
if [[ "$rc" -eq 0 && "$out" == "axl-cc 9.9.9 (gcc, built 2026-01-01)" ]]; then
    pass "a staged prefix answers --version verbatim"
else
    fail "staged --version gave rc=$rc: '$out'"
fi

# The distro layout that would be misread by a sloppier predicate: an SDK
# installed to a prefix that HAS a src/ (/usr/src is on most systems). The
# discriminator is this script sitting in scripts/ next to a Makefile, not the
# presence of a src/ directory.
mkdir -p "$PFX/src"
out="$("$PFX/bin/axl-cc" --version 2>&1)" && rc=0 || rc=$?
if [[ "$rc" -eq 0 && "$out" == "axl-cc 9.9.9 (gcc, built 2026-01-01)" ]]; then
    pass "a prefix containing src/ is not mistaken for a checkout"
else
    fail "prefix with src/ gave rc=$rc: '$out'"
fi

echo
echo "stale-sdk-prefix: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
