#!/bin/bash
# axl-desc: derive what links into a size-gated image; check its declared paths cover it
#
# size-gate-inputs.sh -- for an integration test that declares
# `size-gate=<goal>[,...]` in its `# test-meta:` header, derive every file in
# the source tree that can change the bytes of that goal's images, from the
# CMake build's own record of them.
#
# ROUTING DOES NOT USE THIS. scripts/which-gates.sh routes a change to a
# size-gated test by a literal prefix match against the test's DECLARED
# `size-gate-paths=`, less its `size-gate-exclude=` globs (lib/discover.sh:
# size_gate_routes) -- no build, no cache, no environment, so it cannot go
# blind or stale. This script is the DRIFT CHECK behind that declaration
# (gate.check-size-gate-paths): `--check` fails, naming the file, when
# anything the build links into a size-gated goal is not ROUTED -- outside the
# declared prefixes, or matched by an exclusion -- or when a submodule it
# links from is not routed as a bare path (the spelling a submodule bump
# arrives in). The day sshd links a new deps/ library or a non-sshd tool file,
# the gate says so. A hole in the derivation can only make that check LESS
# complete; it can no longer make routing wrong.
#
# THE DERIVATION, from a built CMake tree (--tree DIR; default the x64 DEBUG
# tree, scripts/axl-build.sh --print-prefix): ninja already knows, exactly --
#   1. `ninja -t inputs <goal>`: every source, object, image and script the
#      goal's images are made from, transitively (cmake/AxlGoals.cmake
#      declares the goals);
#   2. `ninja -t deps` over those objects: every header each one included,
#      from the deps log;
#   3. the build definition: the inputs of build.ninja's regeneration edge
#      (every CMakeLists.txt and cmake/ file, CMakePresets.json), narrowed to
#      what can reach the goal -- they hold the flags.
# Only files in the source tree count (not the build tree, not the
# toolchain's headers). The tree must be BUILT: an object with no deps record
# fails the derivation rather than dropping its headers -- a short list here
# would make the check pass. (The Makefile's own derivation, `make -p` walked
# by hand, retired with it in 5.0.0.)
#
# It does NOT know what --gc-sections drops, so every libaxl.a member counts.
#
# Usage:
#   scripts/size-gate-inputs.sh [--tree DIR] <test-script>      print the derived inputs
#   scripts/size-gate-inputs.sh [--tree DIR] --check [test...]  the drift check;
#                                                  with no test, every size-gated one
# Exit: 0 inputs printed / check clean; 1 the derivation failed or the check
# found drift (stderr says which -- an empty answer is never printed as
# success); 2 usage, or a test declares no size-gate=.
set -uo pipefail

CHECK=0
TREE=""
if [[ "${1:-}" == --tree ]]; then
    TREE="$(realpath "${2:?--tree needs a directory}")"; shift 2
else
    # Every input stated, none inherited: a BUILD or RTK_USB_UNDI in the
    # caller's shell would otherwise name another tree.
    TREE="$(AXL_BUILD_BACKEND=cmake "$(dirname "$0")/axl-build.sh" --print-prefix --abs \
            ARCH=x64 BUILD=DEBUG RTK_USB_UNDI=)" \
        || { echo "size-gate-inputs: cannot name the x64 DEBUG tree" >&2; exit 1; }
fi
[[ -f "$TREE/build.ninja" ]] || { echo "size-gate-inputs: $TREE is not a configured CMake tree" >&2; exit 1; }
if [[ "${1:-}" == --check ]]; then CHECK=1; shift; fi
declare -a TESTS=()
for a in "$@"; do
    if [[ ! -f "$a" ]]; then
        echo "usage: size-gate-inputs.sh [--check] <test-script>..." >&2
        exit 2
    fi
    # Resolved BEFORE the cd below, so a path relative to the caller still
    # names the file the caller meant.
    TESTS+=("$(realpath "$a")")
done
if [[ $CHECK -eq 0 && ${#TESTS[@]} -ne 1 ]]; then
    echo "usage: size-gate-inputs.sh <test-script> | --check [test-script...]" >&2
    exit 2
fi

cd "$(dirname "$0")/.." || exit 1

# shellcheck source=../test/integration/lib/discover.sh
source test/integration/lib/discover.sh

# derive_tree <test-script> -- the derivation from the CMake tree $TREE.
derive_tree() {
local TEST="$1" GOALS out deps line root
GOALS=$(test_meta_field "$TEST" size-gate | tr ',' ' ')
if [[ -z "${GOALS// /}" ]]; then
    echo "size-gate-inputs: $TEST declares no size-gate= in its # test-meta: header" >&2
    return 2
fi
root="$(pwd)"
# shellcheck disable=SC2086  # GOALS is a word list
if ! out=$(ninja -C "$TREE" -t inputs $GOALS 2>&1); then
    echo "size-gate-inputs: ninja -t inputs $GOALS failed in $TREE:" >&2
    sed 's/^/    /' <<< "$out" | head -10 >&2
    return 1
fi
local -a objs=()
mapfile -t objs < <(grep -E '\.obj$' <<< "$out")
if (( ${#objs[@]} == 0 )); then
    echo "size-gate-inputs: ninja named no object for $GOALS in $TREE" >&2
    return 1
fi
if ! deps=$(ninja -C "$TREE" -t deps "${objs[@]}" 2>&1); then
    echo "size-gate-inputs: ninja -t deps failed in $TREE:" >&2
    sed 's/^/    /' <<< "$deps" | head -10 >&2
    return 1
fi
# Every COMPILED object (*.obj -- the published build/*.o are copies of them,
# made by a copy step with no depfile) needs a VALID deps record; one without
# has unknown headers.
local nrec
nrec=$(grep -cE '^[^ ].*: #deps [0-9]+, deps mtime [0-9]+ \(VALID\)$' <<< "$deps")
if (( nrec != ${#objs[@]} )); then
    echo "size-gate-inputs: only $nrec of ${#objs[@]} objects have a valid deps record in" >&2
    echo "  $TREE -- build it first (scripts/axl-build.sh), or the headers are unknown" >&2
    return 1
fi
local regen
# The edge's implicit inputs (after `|`). Its OUTPUT list varies by CMake
# version -- 3.31 writes `build build.ninja:`, 4.2 adds every
# cmake_install.cmake (`build build.ninja /abs/cmake_install.cmake ...:`),
# which the first spelling of this missed on a fresh ubuntu:26.04 -- and
# ninja may continue a line with `$`. mawk and gawk both read this.
regen=$(awk '
    /^build build\.ninja[^:]*: RERUN_CMAKE / { on = 1 }
    on { line = line $0; if ($0 ~ /\$$/) { sub(/\$$/, "", line); next } ; print line; exit }
' "$TREE/build.ninja" | sed -n 's/^[^|]*| //p')
if [[ -z "$regen" ]]; then
    echo "size-gate-inputs: no regeneration edge in $TREE/build.ninja" >&2
    return 1
fi
# A build definition NEWER than the graph read above means a CMakeLists or
# cmake/ edit the tree has not been reconfigured for -- a source it added to
# the goal is in no `ninja -t inputs` answer yet, and the check would pass
# without seeing it. (ninja -n cannot say: with CONFIGURE_DEPENDS globs every
# dry run plans a re-run.) NOT seen: a file newly matched by such a glob with
# no definition edited -- none of the size-gated images takes a globbed source.
local f stale=""
for f in $regen; do
    [[ "$f" == /* ]] || f="$TREE/$f"
    [[ -f "$f" && "$f" -nt "$TREE/build.ninja" ]] && { stale="$f"; break; }
done
if [[ -n "$stale" ]]; then
    echo "size-gate-inputs: $TREE is older than its build definition ($stale);" >&2
    echo "  reconfigure it first (scripts/axl-build.sh, or cmake --build $TREE)" >&2
    return 1
fi
# repo_rel -- stdin paths (absolute, or relative to the tree) to the sorted
# repo-relative files in the SOURCE tree; the build tree and anything outside
# the repo (the toolchain's headers, CMake's own modules) drop out.
repo_rel() {
    local line
    while IFS= read -r line; do
        [[ -n "$line" ]] || continue
        [[ "$line" == /* ]] || line="$TREE/$line"
        [[ -f "$line" ]] || continue
        line="$(realpath -s "$line")"
        [[ "$line" == "$root"/* && "$line" != "$TREE"/* ]] || continue
        printf '%s\n' "${line#"$root"/}"
    done | sort -u
}
local made
made=$( { grep -vE '\.(obj|o)$' <<< "$out"; sed -n 's/^    //p' <<< "$deps"; } | repo_rel)
# The build definition, narrowed to what can reach the goal: every
# non-CMakeLists input of the regeneration (cmake/, CMakePresets.json, the
# lists file, VERSION...), and a CMakeLists.txt only at the root or in a
# directory that holds -- or is above -- one of the goal's own inputs. Every
# directory's CMakeLists.txt regenerates the build, but test/unit's cannot
# change sshd's bytes, and demanding it be routed would route every unit-test
# edit to the size gate.
local dirs
dirs=$(sed 's#/[^/]*$##' <<< "$made" | sort -u)
{
    printf '%s\n' "$made"
    tr ' ' '\n' <<< "$regen" | repo_rel | while IFS= read -r line; do
        if [[ "$line" != CMakeLists.txt && "$line" == */CMakeLists.txt ]]; then
            d="${line%/CMakeLists.txt}"
            grep -qE "^${d//./\\.}(/|\$)" <<< "$dirs" || continue
        fi
        printf '%s\n' "$line"
    done
} | sort -u
}

# derive <test-script> -- print the derived inputs, or fail with the reason.
derive() { derive_tree "$1"; }

if [[ $CHECK -eq 0 ]]; then
    derive "${TESTS[0]}"
    exit $?
fi

# --check: every derived input must be ROUTED -- under a declared
# size-gate-paths= prefix and matched by no size-gate-exclude= glob -- or
# which-gates would not send a change to it to the size gate. And every
# submodule holding a derived input must be covered AS A PATH: a submodule
# bump reaches which-gates as the bare gitlink (deps/mbedtls), a spelling no
# derived file has, so narrowing deps/ to deps/mbedtls/ would keep every file
# covered and silently stop routing the bump.
if [[ ${#TESTS[@]} -eq 0 ]]; then
    while IFS= read -r t; do
        [[ -n "$(test_meta_field "$t" size-gate)" ]] && TESTS+=("$t")
    done < <(grep -l '^# test-meta:.*[[:space:]]size-gate=' test/integration/test-*.sh 2>/dev/null)
fi
if [[ ${#TESTS[@]} -eq 0 ]]; then
    echo "check-size-gate-paths: no test declares size-gate= -- nothing to check is not clean" >&2
    exit 1
fi
# The submodules are the INDEX's gitlinks (mode 160000), and nothing else.
# The index holds exactly the path a bump appears as in `git diff`, even for
# an uninitialised submodule. .gitmodules is NOT consulted: a list read from
# it can be non-empty and still wrong (a stale or edited file), and a wrong
# list reports clean just as an empty one did. The git call's exit status is
# read, not just its output: an index that cannot be read and a tree with no
# submodules are the same empty list and opposite facts.
bad=0
if ! index=$(git ls-files -s 2>&1); then
    echo "check-size-gate-paths: cannot read the git index, so cannot tell which" >&2
    echo "  submodules this tree has: $index" >&2
    exit 1
fi
SUBMODULES=$(awk '$1 == "160000" { print $4 }' <<< "$index")
# Said on every run, so the self-test (and a reader) can see what the
# submodule rule checked -- "none" included.
echo "check-size-gate-paths: submodules (index gitlinks): $(tr '\n' ' ' <<< "$SUBMODULES" | sed 's/ *$//;s/^$/none/')"
for t in "${TESTS[@]}"; do
    b=$(basename "$t")
    if ! inputs=$(derive "$t"); then
        echo "check-size-gate-paths: $b: the derivation failed (above)" >&2
        bad=1
        continue
    fi
    if [[ -z "$(size_gate_paths "$t")" ]]; then
        echo "check-size-gate-paths: $b declares size-gate= but no size-gate-paths=" >&2
        bad=1
        continue
    fi
    n=0 out="" excl="" subs=""
    while IFS= read -r f; do
        n=$((n + 1))
        if size_gate_excluded "$t" "$f"; then
            excl="$excl$f"$'\n'
        elif ! size_gate_covers "$t" "$f"; then
            out="$out$f"$'\n'
        fi
    done <<< "$inputs"
    while IFS= read -r m; do
        [[ -n "$m" ]] || continue
        grep -q -- "^$m/" <<< "$inputs" || continue
        size_gate_routes "$t" "$m" || subs="$subs$m"$'\n'
    done <<< "$SUBMODULES"
    if [[ -n "$out" ]]; then
        echo "check-size-gate-paths: $b links file(s) its size-gate-paths= does not cover," >&2
        echo "  so which-gates would not route a change to them to the size gate:" >&2
        printf '%s' "$out" | sed 's/^/    /' >&2
        echo "  Add a prefix to its # test-meta: size-gate-paths=." >&2
        bad=1
    fi
    if [[ -n "$excl" ]]; then
        echo "check-size-gate-paths: $b's size-gate-exclude= matches file(s) it LINKS," >&2
        echo "  so which-gates would skip a change that can grow the image:" >&2
        printf '%s' "$excl" | sed 's/^/    /' >&2
        echo "  Narrow its # test-meta: size-gate-exclude=." >&2
        bad=1
    fi
    if [[ -n "$subs" ]]; then
        echo "check-size-gate-paths: $b links from submodule(s) whose own path it does not route --" >&2
        echo "  a bump arrives as that bare path, so it would not reach the size gate:" >&2
        printf '%s' "$subs" | sed 's/^/    /' >&2
        echo "  Declare a prefix that covers the submodule path itself (deps/, not deps/x/)." >&2
        bad=1
    fi
    if [[ -z "$out$excl$subs" ]]; then
        echo "check-size-gate-paths: clean -- $b: all $n derived input(s) routed by its size-gate-paths="
    fi
done
exit $bad
