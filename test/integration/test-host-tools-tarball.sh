#!/bin/bash
# test-meta: arch=none needs= est=6 local-only=0
# test-host-tools-tarball.sh -- the host-tools tarball, built and unpacked.
#
# WHY THIS EXISTS. This archive was assembled INLINE in release.yml, and
# make-sdk-tarball.sh's own header named that as the defect: "the host-tools
# tarball IS assembled inline below and is exactly the kind of thing that then
# has no local reproduction." Everything about it -- its name, its layout, that
# its commands run -- could only be checked by cutting a release.
#
# WHAT IS PINNED, and both were broken before D2:
#
#   - ONE versioned top-level directory. §14.1c measured SIX top-level entries
#     in the v4.4.0 archive, which is why the consumer README has to say
#     `mkdir -p ~/axl-sdk-host-tools && tar xf ... -C` -- the caller creates
#     the directory because the archive will not. Extracted anywhere with
#     other content it scatters over it.
#   - The name is the settled one, `axl-sdk-host-tools-<ver>.tar.gz` (§14.1a),
#     with NO arch field: the payload is shell and Python, so an arch there
#     would either be a lie or force two byte-identical uploads.
#
# It also RUNS every command the dispatcher lists rather than stat-ing them.
# The predecessor in release.yml asserted `test -x rsod-decode.py`, which stays
# true of a script that dies on import -- and it did: shipping without
# axl_version.py broke all four Python tools while that check still passed.
#
# Host-only, no QEMU, no compiler: it copies scripts and tars them.
#
# Usage: ./test/integration/test-host-tools-tarball.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail

MAKER="$PROJECT_DIR/scripts/make-host-tools-tarball.sh"
VERSION="$(cat "$PROJECT_DIR/VERSION")"

WORK="$(mktemp -d -t axl-httar.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT

# check <rc> <message>. Defined HERE, above every caller: a function
# definition returns 0, so one placed between an assertion and the `$?` that
# reads it silently swallows the result. That is exactly what happened to
# "bin/axl is executable", which could never have failed.
check() {
    if [[ "$1" -eq 0 ]]; then test_host_pass "$2"; else test_host_fail "$2"; fi
    return 0
}

echo "=== host-tools tarball ==="
echo ""

if [[ ! -x "$MAKER" ]]; then
    test_host_fail "scripts/make-host-tools-tarball.sh is executable"
    test_host_summary "host-tools-tarball"
    exit 1
fi
test_host_pass "scripts/make-host-tools-tarball.sh is executable"

OUT="$WORK/dist"
if ! "$MAKER" --out "$OUT" > "$WORK/build.log" 2>&1; then
    test_host_fail "make-host-tools-tarball.sh succeeds"
    tail -12 "$WORK/build.log" | sed 's/^/      /'
    test_host_summary "host-tools-tarball"
    exit 1
fi
test_host_pass "make-host-tools-tarball.sh succeeds"

TARBALL="$OUT/axl-sdk-host-tools-${VERSION}.tar.gz"
[[ -f "$TARBALL" ]]
if [[ $? -ne 0 ]]; then
    test_host_fail "produces axl-sdk-host-tools-${VERSION}.tar.gz"
    ls -1 "$OUT" 2>/dev/null | sed 's/^/      /'
    test_host_summary "host-tools-tarball"
    exit 1
fi
test_host_pass "produces axl-sdk-host-tools-${VERSION}.tar.gz"

# ---------------------------------------------------------------------------
# Shape: one versioned root, named after the archive.
# ---------------------------------------------------------------------------
LISTING="$WORK/listing.txt"
tar tzf "$TARBALL" > "$LISTING"

roots="$(awk -F/ '{print $1}' "$LISTING" | sort -u)"
[[ "$roots" == "axl-sdk-host-tools-${VERSION}" ]]
if [[ $? -eq 0 ]]; then
    test_host_pass "extracts to the single root axl-sdk-host-tools-${VERSION}/"
else
    test_host_fail "extracts to the single root axl-sdk-host-tools-${VERSION}/ (got: $(echo "$roots" | tr '\n' ' '))"
fi

for want in bin/axl libexec/axl/axl-common.sh libexec/axl/run-qemu.sh \
            libexec/axl/axl_version.py libexec/axl/install.sh \
            share/axl/version VERSION LICENSE; do
    grep -qx "axl-sdk-host-tools-${VERSION}/${want}" "$LISTING"
    if [[ $? -eq 0 ]]; then
        test_host_pass "carries $want"
    else
        test_host_fail "carries $want"
    fi
done

# ---------------------------------------------------------------------------
# Behaviour: extract it somewhere it has never been and RUN what it ships.
# ---------------------------------------------------------------------------
EXTRACT="$WORK/extract"
mkdir -p "$EXTRACT"
tar xzf "$TARBALL" -C "$EXTRACT"
ROOT="$EXTRACT/axl-sdk-host-tools-${VERSION}"

[[ -x "$ROOT/bin/axl" ]]
check $? "bin/axl is executable"

got="$("$ROOT/bin/axl" --print-version 2>&1)"
[[ "$got" == "$VERSION" ]]
check $? "axl --print-version says $VERSION (got '$got')"

# axl_version.py has no shebang: 0644 keeps `axl`'s command scan -- which lists
# the EXECUTABLES in libexec/axl -- from offering it as a verb.
perm="$(stat -c '%a' "$ROOT/libexec/axl/axl_version.py")"
[[ "$perm" == "644" ]]
check $? "axl_version.py is mode 0644, not a subcommand (got $perm)"

# Same reason for the mode, and a bigger reason for its presence: carrying the
# installer is what makes this component the MANAGER (§20 M2). `axl prune`
# never removes it and `axl use` never repoints it, so it is the one tree that
# can still self-update after any rollback -- but only if the installer its
# verbs exec is actually in it.
perm="$(stat -c '%a' "$ROOT/libexec/axl/install.sh")"
[[ "$perm" == "644" ]]
check $? "the staged install.sh is mode 0644, not a subcommand (got $perm)"

# THE RULE WAS APPLIED, asserted against every tool rather than against a list
# of names. `axl` offers exactly the EXECUTABLES in libexec/axl, so the mode
# bit is not a detail of the copy -- it IS the declaration "this is a command"
# -- and scripts/stage-host-tools.sh decides it from the file itself: a
# shebang is the manifest. This archive is the OTHER staging path for the same
# file list, and it used to install everything 0755 with one hand-written
# exception, so `axl common` (a sourced library) and `axl gdb-sample` (loaded
# INSIDE gdb, no shebang at all) both shipped 0755 in v4.7.0 and were offered
# as commands that cannot run.
#
# Naming the two here would have made this test a FOURTH copy of the same list,
# in the test for a change whose whole thesis is that the second copy is what
# drifted -- and it would not notice an eleventh tool arriving shebang-less and
# staged 755. Sourcing the rule and comparing against it is what proves the
# rule was applied.
# shellcheck source=../../scripts/stage-host-tools.sh
source "$PROJECT_DIR/scripts/stage-host-tools.sh"
mode_bad=""
mode_n=0
while read -r _f; do
    [[ -n "$_f" ]] || continue
    mode_n=$((mode_n + 1))
    want="$(axl_host_tool_mode "$PROJECT_DIR/scripts/$_f")"
    got="$(stat -c '%a' "$ROOT/libexec/axl/$_f" 2>/dev/null)"
    [[ "$got" == "$want" ]] || mode_bad="$mode_bad $_f($got,want $want)"
done < <(make -s -C "$PROJECT_DIR" print-HOST_TOOL_FILES | tr ' ' '\n')
[[ "$mode_n" -gt 5 && -z "$mode_bad" ]]
check $? "all $mode_n staged tools carry the mode the shebang rule gives them${mode_bad:+ --$mode_bad}"

# ---------------------------------------------------------------------------
# The dispatcher's listing: every entry described, and every entry runs.
# ---------------------------------------------------------------------------
#
# SCOPED TO THE `host commands:` SECTION and NOT requiring a description.
# The walk below used to read the listing with `s/^  \([a-z][a-z0-9-]*\) .*/\1/p`
# -- a regex whose space demands a description. The only entries without one
# were axl-common.sh and gdb-sample.py, so the two commands that could not run
# were the exact two this walk could not see, and it reported "all 7 dispatcher
# commands run" against an archive offering nine. A listing entry with no
# description is now its own failure, because that is the cheap tell.
CMDS="$WORK/cmds.txt"
"$ROOT/bin/axl" --help | sed -n '/^host commands:/,/^$/p' \
    | sed -n 's/^  \([a-z][a-z0-9-]*\).*/\1/p' > "$CMDS"

nodesc="$("$ROOT/bin/axl" --help | sed -n '/^host commands:/,/^$/p' \
          | sed -n 's/^  \([a-z][a-z0-9-]*\)[[:space:]]*$/\1/p' | tr '\n' ' ')"
[[ -z "$nodesc" ]]
check $? "every listed command carries an axl-desc description${nodesc:+ -- undescribed:$nodesc}"

# Every command the dispatcher offers must RUN and know its version. A command
# reports its PROGRAM name, which is not always the verb -- `axl`'s listing
# strips a leading `axl-`, so `axl prune` is axl-prune.sh and answers
# "axl-prune". Accept either spelling; what is asserted is that it ran.
bad=""
n=0
while read -r cmd; do
    [[ -n "$cmd" ]] || continue
    n=$((n + 1))
    got="$("$ROOT/bin/axl" "$cmd" --version 2>&1)"
    case "$got" in
        "$cmd $VERSION"|"axl-$cmd $VERSION") ;;
        *) bad="$bad $cmd" ;;
    esac
done < "$CMDS"

# EXACTLY the executables in libexec/axl, not a floor. `-gt 5` is the shape
# that let "all 7 dispatcher commands run" stand against an archive offering
# nine: the two that could not run were invisible to the walk, and a floor
# cannot tell a shrinking list from a correct one.
n_exec=$(find "$ROOT/libexec/axl" -maxdepth 1 -type f -perm -u+x | wc -l)
[[ "$n" -eq "$n_exec" && "$n" -gt 5 && -z "$bad" ]]
check $? "all $n dispatcher commands run and report $VERSION (${n_exec} executable in libexec)${bad:+ -- failed:$bad}"

test_host_summary "host-tools-tarball"
