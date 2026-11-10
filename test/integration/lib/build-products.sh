# shellcheck shell=bash
# build-products.sh -- the files a test's declared `builds=` targets produce,
# read from the CMake tree's own build graph. Sourced by lib/test-cache.sh,
# whose key hashes them; run as a script by scripts/check-test-inputs.py,
# which reads the same answer rather than keeping a second reader.
#
# WHY. A test stages what its builds= targets made -- `cp "$(test_build_dir)/
# sshd-t.efi" "$TEST_STAGING/"` -- and the skip cache keyed only what a test
# RECORDED, so a fixture staged without test_record_input was outside its key:
# an sshd change left the test reading green (09147d6b found two by hand). The
# runner pre-builds the union of builds= before it decides the cache
# (lib/suite-prep.sh), so these files are current when the key is computed.
#
# WHAT A PRODUCT IS. From each target, walk the graph through PHONY nodes
# only (a goal, a CMake utility target: its explicit, implicit AND order-only
# inputs -- CMake hangs a utility target's dependencies on `||`, and a
# service's inner image, built so, is a file a test stages; measured over
# the tree, `||` adds only such inner images and axl-legacy-ip4cfg.efi) and
# stop at the first node a real rule makes: that file is a product. So
# `sshd` gives tools/sshd.efi and tools/raw/sshd.efi, not every object under
# them. Of those, `*.o *.obj *.a *.so` are dropped: intermediates whose bytes
# reach a guest only through an image that is itself a product -- and
# libaxl.a, a product of nearly every goal, would make every library edit
# miss every builds= key even where --gc-sections left the image
# byte-identical (the reason the cache exists, AXL-CI-Release-Speed-Design.md
# §12.15). A report goal's command (tool-sizes) makes no file: its symbolic
# output is skipped like any utility target's (below), so what it reports on
# is what it keys on.
#
# WHICH TREE. builds= is one list: its VAR=VALUE entries apply to all its
# targets (lib/suite-prep.sh groups it the same way). ARCH= overrides the
# run's arch, BUILD= and RTK_USB_UNDI= pick the tree -- named by
# scripts/axl-build.sh --print-prefix, the build's own answer. AXL_CPP=1
# means nothing to the build and is skipped; any other variable is an ERROR.
#
# EVERY ERROR IS AN ERROR, never an empty list: an unconfigured tree, a
# target ninja does not know, a product that does not exist (the build did
# not make it), a ninja that could not run. An empty answer the caller took
# for "no products" would key over less, which is a false hit.
#
# COST. The graph walk is one `ninja -t query` per level (~20 ms each), so
# its answer is MEMOISED per tree in $AXL_TEST_CACHE/.products/, keyed by
# build.ninja's identity (device, inode, size, mtime to the nanosecond):
# CMake rewrites it by rename on every regeneration, so a new graph is never
# read from an old memo. The files themselves are hashed fresh by the caller
# every time. Concurrent writers each rename a complete file into place.
#
# Script mode:
#   build-products.sh [--tree DIR] ARCH TEST...
#     prints `TEST<TAB>PRODUCT` for every product of every TEST's builds=
#     (absolute paths). --tree DIR reads every test's targets in DIR, whatever
#     its ARCH=/BUILD= (the gate matches by NAME, and one configured tree is
#     all it is given). Exit 0 printed; 1 on any error (stderr says which).

_BP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_BP_REPO="$(cd "$_BP_DIR/../../.." && pwd)"
_BP_SUFFIX_RE='\.(o|obj|a|so)$'
# A utility target's command (add_custom_target ... COMMAND) declares no
# file: ninja names it by a SYMBOLIC output, <dir>/CMakeFiles/<target>, which
# never exists. Real outputs under CMakeFiles/ sit deeper (<target>.dir/...).
# What such a command writes elsewhere is invisible to the graph, so to this.
_BP_SYMBOLIC_RE='(^|/)CMakeFiles/[^/]+$'

# _bp_ninja_walk TREE GOAL... -- `GOAL<TAB>REL` for each product of each GOAL
# (relative to TREE, or absolute when ninja names it so); a goal with none
# prints `GOAL<TAB>`. 1 when ninja fails.
_bp_ninja_walk() {
    local tree="$1"; shift
    local -A rule=() kids=() asked=()
    local -a todo=("$@") batch
    local out parsed node="" sec="" dep n
    while (( ${#todo[@]} )); do
        batch=()
        for n in "${todo[@]}"; do
            [[ -z "${asked[$n]:-}" ]] || continue
            asked[$n]=1; batch+=("$n")
        done
        todo=()
        (( ${#batch[@]} )) || break
        # stdout only: a warning on stderr is not graph. On a failure, ask
        # again for the reason.
        out=$(ninja -C "$tree" -t query "${batch[@]}" 2>/dev/null) || {
            out=$(ninja -C "$tree" -t query "${batch[@]}" 2>&1 >/dev/null)
            echo "build-products: ninja -t query failed in $tree: ${out##*$'\n'}" >&2
            return 1
        }
        # awk first: a product's own entry lists every object it links
        # (~3,000 lines for sshd's), and bash reading each was 0.4 s a walk.
        # Out: `N<TAB>node<TAB>rule`, and `K<TAB>node<TAB>dep` for a phony
        # node's inputs, order-only (`||`) ones too -- never a dropped suffix,
        # which cannot be a product. (An absolute path is asked about like
        # any other: a source file answers with no rule and is skipped.)
        parsed=$(_BP_SFX="$_BP_SUFFIX_RE" awk '
            /^[^ \t].*:$/ { node = substr($0, 1, length($0) - 1); rule = ""; sec = ""
                            printf "N\t%s\t\n", node; next }
            /^  input: / { rule = substr($0, 10); sec = "in"; printf "N\t%s\t%s\n", node, rule; next }
            /^  outputs:$/ { sec = "out"; next }
            sec == "in" && rule == "phony" && /^    / {
                d = substr($0, 5)
                sub(/^\|\|? /, "", d)
                if (d ~ ENVIRON["_BP_SFX"]) next
                printf "K\t%s\t%s\n", node, d
            }' <<< "$out") || { echo "build-products: could not parse ninja -t query's answer" >&2; return 1; }
        while IFS=$'\t' read -r sec node dep; do
            if [[ "$sec" == N ]]; then
                rule[$node]="$dep"
            elif [[ "$sec" == K ]]; then
                kids[$node]+="$dep"$'\n'
                todo+=("$dep")
            fi
        done <<< "$parsed"
        # Every node asked about must have been answered: one missing would
        # read as a source file -- no products -- and key over less.
        for n in "${batch[@]}"; do
            [[ -n "${rule[$n]+set}" ]] || {
                echo "build-products: ninja -t query said nothing about $n in $tree" >&2
                return 1
            }
        done
    done
    local g cur found
    local -a stack
    for g in "$@"; do
        local -A seen=()
        stack=("$g"); found=0
        while (( ${#stack[@]} )); do
            cur="${stack[-1]}"; unset 'stack[-1]'
            [[ -z "${seen[$cur]:-}" ]] || continue
            seen[$cur]=1
            case "${rule[$cur]:-}" in
                "")     ;;                               # a source file: not made
                phony)  while IFS= read -r dep; do
                            [[ -n "$dep" ]] && stack+=("$dep")
                        done <<< "${kids[$cur]:-}" ;;
                *)      [[ "$cur" =~ $_BP_SYMBOLIC_RE ]] && continue  # (suffixes: never kids)
                        printf '%s\t%s\n' "$g" "$cur"; found=1 ;;
            esac
        done
        unset seen
        (( found )) || printf '%s\t\n' "$g"
    done
}

# _bp_goal_products TREE GOAL... -- the products of every GOAL, relative to
# TREE (or absolute when ninja names them so), into the array _BP_REL; one
# memo file per tree and goal when $AXL_TEST_CACHE names a cache, its first
# line the build.ninja identity it was read from. 1 on any error.
# _bp_memo_name GOAL -- a file name for GOAL that no other goal shares (`_`
# escaped before `/` becomes one: a/b and a_b must not share a memo).
_bp_memo_name() {
    local n="${1//_/__}"
    printf '%s\n' "${n//\//_s}"
}

_BP_REL=() _BP_EMPTY=()
_bp_goal_products() {
    local tree="$1"; shift
    local sig memo="" g f line walked
    local -a missing=() m
    _BP_REL=() _BP_EMPTY=()
    sig=$(stat -c '%d:%i:%s:%.9Y' "$tree/build.ninja" 2>/dev/null) || {
        echo "build-products: $tree is not a configured CMake tree (no build.ninja)" >&2
        return 1
    }
    [[ -n "${AXL_TEST_CACHE:-}" && -d "$AXL_TEST_CACHE" ]] \
        && memo="$AXL_TEST_CACHE/.products/$(_bp_memo_name "$tree")"
    for g in "$@"; do
        m=()
        f="$memo/$(_bp_memo_name "$g")"
        [[ -n "$memo" && -f "$f" ]] && mapfile -t m < "$f"
        if [[ "${m[0]:-}" == "sig $sig" ]]; then
            _BP_REL+=("${m[@]:1}")
            (( ${#m[@]} > 1 )) || _BP_EMPTY+=("$g")
        else
            missing+=("$g")
        fi
    done
    (( ${#missing[@]} )) || return 0
    walked=$(_bp_ninja_walk "$tree" "${missing[@]}") || return 1
    local -A per=()
    while IFS= read -r line; do
        [[ -n "$line" ]] || continue
        g="${line%%$'\t'*}"; f="${line#*$'\t'}"
        per[$g]+="${per[$g]+$'\n'}$f"
    done <<< "$walked"
    for g in "${missing[@]}"; do
        mapfile -t m <<< "${per[$g]:-}"
        for f in "${m[@]}"; do [[ -n "$f" ]] && _BP_REL+=("$f"); done
        [[ -n "${per[$g]:-}" ]] || _BP_EMPTY+=("$g")
        [[ -n "$memo" ]] || continue
        mkdir -p "$memo" 2>/dev/null
        f="$memo/$(_bp_memo_name "$g")"
        { printf 'sig %s\n' "$sig"; [[ -z "${per[$g]:-}" ]] || printf '%s\n' "${per[$g]}"; } \
            > "$f.$BASHPID" 2>/dev/null && mv -f "$f.$BASHPID" "$f" 2>/dev/null
        rm -f "$f.$BASHPID" 2>/dev/null
    done
    return 0
}

# bp_tree_for ARCH VARS... -- the absolute tree a builds= list with these
# VAR=VALUE entries builds in (ARCH: the run's, X64 | AARCH64 | x64 | aa64).
# Only what builds= itself names is passed: BUILD and RTK_USB_UNDI otherwise
# come from the environment, exactly as for the runner's pre-build
# (lib/suite-prep.sh) and the tests' own scripts/build-prefix.sh -- forcing
# BUILD=DEBUG here keyed the debug tree while a BUILD=RELEASE run staged the
# release one.
bp_tree_for() {
    local arch="$1" v
    local -a pass=()
    shift
    for v in "$@"; do
        case "$v" in
            ARCH=*)                 arch="${v#ARCH=}" ;;
            BUILD=*|RTK_USB_UNDI=*) pass+=("$v") ;;
            AXL_CPP=1)              ;;
            *) echo "build-products: builds= variable '$v' is not one this reader knows" >&2
               return 1 ;;
        esac
    done
    # The PROJECT the run builds (AXL_ONE_PROJECT_DIR: a stub project in the
    # runner's self-test), not necessarily this checkout -- the pre-build
    # built its fixtures there.
    "${AXL_ONE_PROJECT_DIR:-$_BP_REPO}/scripts/axl-build.sh" --print-prefix --abs \
        ARCH="$arch" ${pass[@]+"${pass[@]}"}
}

# _bp_split SCRIPT -- SCRIPT's builds= into _BP_VARS (its VAR=VALUE entries)
# and _BP_GOALS (its targets), by discover.sh's reader of the header. (Its
# test_meta_builds forks to split what this splits in bash: the same list,
# ~3 ms less a key.)
_BP_VARS=() _BP_GOALS=()
_bp_split() {
    local e
    local -a list=()
    _BP_VARS=() _BP_GOALS=()
    declare -F test_meta_field >/dev/null || source "$_BP_DIR/discover.sh"
    # A reader that did not load is no answer -- not "declares no builds=".
    declare -F test_meta_field >/dev/null || {
        echo "build-products: cannot read test-meta headers ($_BP_DIR/discover.sh)" >&2
        return 1
    }
    IFS=, read -r -a list <<< "$(test_meta_field "$1" builds)"
    for e in ${list[@]+"${list[@]}"}; do
        [[ -n "$e" ]] || continue
        if [[ "$e" == *=* ]]; then _BP_VARS+=("$e"); else _BP_GOALS+=("$e"); fi
    done
}

# _bp_ninja_name TREE GOAL -- GOAL as ninja names it: a FILE under the tree
# is relative to it (scripts/axl-build.sh strips the same two prefixes).
_bp_ninja_name() {
    local g="${2#"$_BP_REPO/"}"
    printf '%s\n' "${g#"${1#"$_BP_REPO/"}/"}"
}

# bp_products ARCH SCRIPT [TREE [names]] -- every product of SCRIPT's
# builds=, one absolute path per line, sorted (nothing when it declares none).
# TREE, when given, replaces the tree its variables name; `names` skips the
# check that each product EXISTS (the gate matches names, in a tree that need
# not be built). 1 on any error.
bp_products() {
    local arch="$1" script="$2" tree="${3:-}" names="${4:-}" g
    _bp_split "$script" || return 1
    (( ${#_BP_GOALS[@]} )) || return 0
    if [[ -z "$tree" ]]; then
        tree=$(bp_tree_for "$arch" ${_BP_VARS[@]+"${_BP_VARS[@]}"}) || return 1
    fi
    local -a goals=()
    for g in "${_BP_GOALS[@]}"; do goals+=("$(_bp_ninja_name "$tree" "$g")"); done
    local rel p
    local -a out=()
    _bp_goal_products "$tree" "${goals[@]}" || return 1
    # A target that makes no file this reader sees (a utility command whose
    # output is symbolic) keys nothing: no answer, so no key -- unless the
    # gate is only matching names.
    if [[ -z "$names" ]] && (( ${#_BP_EMPTY[@]} )); then
        echo "build-products: $script: builds= target(s) ${_BP_EMPTY[*]} produce no file in $tree" >&2
        return 1
    fi
    for rel in ${_BP_REL[@]+"${_BP_REL[@]}"}; do
        if [[ "$rel" == /* ]]; then p="$rel"; else p="$tree/$rel"; fi
        [[ -n "$names" || -f "$p" ]] || {
            echo "build-products: $script: builds= product $p does not exist (not built?)" >&2
            return 1
        }
        out+=("$p")
    done
    (( ${#out[@]} )) || return 0
    printf '%s\n' "${out[@]}" | LC_ALL=C sort -u
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    set -uo pipefail
    _bp_tree=""
    if [[ "${1:-}" == --tree ]]; then
        _bp_tree="$(realpath "${2:?--tree needs a directory}")" || exit 1
        shift 2
    fi
    [[ $# -ge 2 ]] || { echo "usage: build-products.sh [--tree DIR] ARCH TEST..." >&2; exit 2; }
    _bp_arch="$1"; shift
    _bp_names=""
    if [[ -n "$_bp_tree" ]]; then
        # One tree for every test: walk the union of their targets ONCE,
        # into a private memo the per-test reads then answer from (~0.1 s a
        # test walked alone, ~350 tests). Names only: the gate's tree need
        # not be built.
        _bp_names=names
        if [[ -z "${AXL_TEST_CACHE:-}" ]]; then
            AXL_TEST_CACHE="$(mktemp -d)" || exit 1
            export AXL_TEST_CACHE
            trap 'rm -rf "$AXL_TEST_CACHE"' EXIT
        fi
        declare -A _bp_all=()
        for _bp_t in "$@"; do
            _bp_split "$_bp_t" || exit 1
            for _bp_g in ${_BP_GOALS[@]+"${_BP_GOALS[@]}"}; do
                _bp_all["$(_bp_ninja_name "$_bp_tree" "$_bp_g")"]=1
            done
        done
        if (( ${#_bp_all[@]} )); then
            _bp_goal_products "$_bp_tree" "${!_bp_all[@]}" || exit 1
        fi
    fi
    _bp_rc=0
    for _bp_t in "$@"; do
        if ! _bp_out=$(bp_products "$_bp_arch" "$_bp_t" "$_bp_tree" "$_bp_names"); then
            _bp_rc=1; continue
        fi
        while IFS= read -r _bp_p; do
            [[ -n "$_bp_p" ]] && printf '%s\t%s\n' "$_bp_t" "$_bp_p"
        done <<< "$_bp_out"
    done
    exit "$_bp_rc"
fi
