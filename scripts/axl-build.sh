#!/bin/bash
# axl-build.sh -- build named goals of this tree, with its build system,
# CMake + Ninja (docs/AXL-Build-System-Design.md).
#
# Usage:
#   scripts/axl-build.sh [-s] [-jN] [VAR=VALUE...] GOAL...
#   scripts/axl-build.sh --print-prefix [--abs] [VAR=VALUE...]
#
# The arguments keep the shape of the `make` command lines every caller used
# before the Makefile was retired (5.0.0), so
#   "$PROJECT_DIR/scripts/axl-build.sh" ARCH=aa64 pack-fixture
# builds the goal pack-fixture (cmake/AxlGoals.cmake) in the preset for ARCH
# x BUILD: `cmake --build build/<preset> --target <goal>...`. --print-prefix
# names that tree.
#
# ARCH and BUILD come from the arguments, else the environment. Any other
# variable is REFUSED rather than dropped: a variable the build would
# silently ignore is a test that measures something other than it says.
# RTK_USB_UNDI is the one other it knows -- it configures the NIC driver into
# a build tree of its own -- and SSHD_RTK_OUT, where that build's goals copy
# their images.
#
# AXL_BUILD_BACKEND chose between make and cmake until the Makefile went; any
# value but `cmake` is refused, by name.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "${AXL_BUILD_BACKEND:-cmake}" in
    cmake) ;;
    make)  echo "axl-build.sh: AXL_BUILD_BACKEND=make -- the Makefile was retired in 5.0.0;" \
                "the build is CMake + Ninja (unset AXL_BUILD_BACKEND)" >&2
           exit 2 ;;
    *)     echo "axl-build.sh: AXL_BUILD_BACKEND='$AXL_BUILD_BACKEND' -- the only build is cmake" >&2
           exit 2 ;;
esac

# The CMake build tree for ARCH x BUILD (and an RTK driver, which gets a
# tree of its own): `build/<preset>[-rtk]`, relative to the repo. Exit 2 on
# an arch or build it has no preset for. Shared by the build and the
# fingerprint, so the tree the skip watches is the tree the call builds.
_cmake_dir() {   # _cmake_dir ARCH BUILD RTK
    local arch cfg
    case "$1" in
        x64|X64)                arch=x64 ;;
        aa64|AARCH64|aarch64)   arch=aa64 ;;
        *) echo "axl-build.sh: ARCH='$1' -- expected x64 or aa64" >&2; return 2 ;;
    esac
    case "$2" in
        DEBUG)   cfg=debug ;;
        RELEASE) cfg=release ;;
        *) echo "axl-build.sh: BUILD='$2' -- expected DEBUG or RELEASE" >&2; return 2 ;;
    esac
    printf 'build/%s-%s%s\n' "$arch" "$cfg" "${3:+-rtk}"
}

# The tree a call builds in, absolute: the CMake tree for ARCH x BUILD (and
# RTK_USB_UNDI), from the call's arguments, else the environment. Empty when
# the arch or build names no tree (the build call itself refuses those).
# Shared by the fingerprint and the build lock, so the tree the skip watches
# is the tree the lock guards.
_call_tree() {   # _call_tree CALL-ARGS... -> one path, or nothing
    local a c_arch="${ARCH:-x64}" c_build="${BUILD:-DEBUG}"
    local c_rtk="${RTK_USB_UNDI:-}" c_dir
    for a in "$@"; do
        case "$a" in
            ARCH=*)   c_arch="${a#ARCH=}" ;;
            BUILD=*)  c_build="${a#BUILD=}" ;;
            RTK_USB_UNDI=*) c_rtk="${a#RTK_USB_UNDI=}" ;;
        esac
    done
    c_dir="$(_cmake_dir "$c_arch" "$c_build" "$c_rtk" 2>/dev/null)" || return 0
    printf '%s/%s\n' "$REPO" "$c_dir"
}

# THE BUILD'S FINGERPRINT, for "pre-built by the runner" below: everything
# outside a call's own arguments that decides what it builds --
#   - every environment variable the build definition READS, derived from
#     cmake/ and the root CMakeLists.txt ($ENV{VAR}, ${VAR}) rather than listed
#     by hand (a hand-picked list missed AXL_PACK_STATE_DIR and AXL_TOOLCHAIN
#     once); a variable the call sets on its command line is its argument, not
#     this;
#   - the tree;
#   - the state of the call's OWN tree: its build.ninja and
#     CMakeFiles/rules.ninja, which a reconfigure with other options (or
#     another compiler or launcher: rules.ninja alone) rewrites and a wipe
#     deletes. Only its own: another tree appearing later (install.sh, another
#     fixture group) must not kill every skip.
_env_fingerprint() {   # _env_fingerprint CALL-ARGS... -> one line
    local -A given=() names=() exported=()
    local a n v parts=() st c_tree c_state=()
    for a in "$@"; do
        [[ "$a" == [A-Za-z_]*=* ]] && given["${a%%=*}"]=1
    done
    # The ENVIRONMENT only -- never this script's own shell variables or
    # locals, which share names with the derived list.
    while IFS= read -r n; do exported["$n"]=1; done < <(compgen -e)
    while IFS= read -r n; do
        [[ -n "$n" ]] && names["$n"]=1
    done < <( { cat "$REPO"/cmake/*.cmake "$REPO"/CMakeLists.txt 2>/dev/null || true; } \
             | grep -oE '\$[({][A-Za-z_][A-Za-z0-9_]*|\$ENV\{[A-Za-z_][A-Za-z0-9_]*' \
             | sed -E 's/^\$[({]//; s/^\$ENV\{//' \
             || true)
    for n in $(printf '%s\n' "${!names[@]}" | sort); do
        [[ -n "${given[$n]:-}" ]] && continue
        [[ -n "${exported[$n]:-}" ]] || continue   # not in the environment: not part of it
        printf -v v '%q' "${!n}"
        parts+=("$n=$v")
    done
    # A missing file (a fresh tree) is a state of the inputs, never an abort:
    # under pipefail a failed `cat` once made this whole fingerprint the EMPTY
    # string -- which matched another empty one. An arch or build it cannot
    # name: no tree to watch, and the build call itself refuses it -- so the
    # state is simply "none".
    c_tree="$(_call_tree "$@")"
    # (A path that cannot exist, not an empty list: `cat` with no
    # arguments would read this script's stdin.)
    if [[ -z "$c_tree" ]]; then
        c_state=("$REPO/.no-tree")
    else
        c_state=("$c_tree/build.ninja" "$c_tree/CMakeFiles/rules.ninja")
    fi
    st=$( { cat "${c_state[@]}" 2>/dev/null || true; } | sha256sum | cut -c1-16)
    printf 'backend=cmake tree=%s state=%s %s\n' "$REPO" "$st" "${parts[*]:-}"
}
if [[ "${1:-}" == --env-fingerprint ]]; then
    shift
    _env_fingerprint "$@"
    exit 0
fi

# PRE-BUILT BY THE RUNNER. run-integration.sh builds every test's declared
# builds= once, up front, and lists what it built in AXL_PREBUILT_MANIFEST:
# one `ARCH|VARS|TARGETS|FINGERPRINT` line per build -- VARS and TARGETS
# space-separated, VARS sorted, FINGERPRINT the _env_fingerprint it built
# with. check-test-meta holds each test's own build line to builds=, so a
# test's call here is then a no-op build (~1 s, a third of a pooled test).
# Skipped ONLY when a line has this call's arch, exactly its variables,
# every target it asks for, and the same fingerprint -- test-sshd-rtk builds
# `sshd-rtk` with RTK_USB_UNDI in the environment, another image under the
# same name. Only -s, -jN (or -j N) and --no-print-directory are how rather
# than what; any other option means "build". Anything else builds as ever.
_prebuilt() {
    [[ -n "${AXL_PREBUILT_MANIFEST:-}" && -f "$AXL_PREBUILT_MANIFEST" ]] || return 1
    local a arch="${ARCH:-x64}" vars=() tgts=() l_arch l_vars l_tgts l_fp t want_vars fp_now skip_n=false
    for a in "$@"; do
        if $skip_n; then skip_n=false; [[ "$a" =~ ^[0-9]+$ ]] && continue; fi
        case "$a" in
            -s|--no-print-directory|-j[0-9]*) ;;
            -j)      skip_n=true ;;              # `-j N`: the N is not a target
            -*)      return 1 ;;                 # -B, -n, -C, --print-prefix...: not ours to skip
            ARCH=*)  arch="${a#ARCH=}" ;;
            *=*)     vars+=("$a") ;;
            *)       tgts+=("$a") ;;
        esac
    done
    (( ${#tgts[@]} > 0 )) || return 1
    want_vars=$( (( ${#vars[@]} )) && printf '%s\n' "${vars[@]}" | sort | paste -sd' ' - || true)
    fp_now=$(_env_fingerprint "$@")
    while IFS='|' read -r l_arch l_vars l_tgts l_fp; do
        [[ "$l_arch" == "$arch" && "$l_vars" == "$want_vars" && "$l_fp" == "$fp_now" ]] || continue
        for t in "${tgts[@]}"; do
            [[ " $l_tgts " == *" $t "* ]] || continue 2
        done
        echo "axl-build.sh: pre-built by the runner: ARCH=$arch${want_vars:+ $want_vars} ${tgts[*]}"
        return 0
    done < "$AXL_PREBUILT_MANIFEST"
    return 1
}
if _prebuilt "$@"; then
    exit 0
fi

# ONE BUILD AT A TIME IN A TREE. Ninja does not support two
# invocations in one build tree: each decides for itself what is stale, and
# then both run the SAME step -- two `objcopy ... X.efi.tmp && rename
# X.efi.tmp X.efi`, where one renames the other's temporary away or reads a
# .so the other is still linking ("input file ... is empty"). The
# integration suite starts a dozen tests at once, each building its fixtures
# through here, so the first stale output -- a reconfigure, an edit mid-run,
# a fixture the prep did not build -- had them collide (CI, 2026-11-09:
# test-stderr-row-qemu.sh, "subcommand failed"). Per-suite flocks once
# guarded a few callers each; this is the one place every build goes
# through, so the lock is here, per tree (two arches still build at once).
#
# Taken AFTER the pre-built skip, so a skipped call never waits, and only
# for a build (--print-prefix and --env-fingerprint only read). The fd is
# inherited by the build it execs and released when the last of its
# processes exits (so a build step that left a daemon behind would hold it:
# none does today); no timeout, because a wait that gave up would be the
# collision this prevents. install.sh's own lock (TREE.buildlock, held over
# its build AND its copies out of the tree) is a different file, taken
# first, so the two nest without deadlock. Test: test-axl-build-lock.sh.
_lock_tree() {   # _lock_tree CALL-ARGS...
    local tree lock
    tree="$(_call_tree "$@")"
    [[ -n "$tree" ]] || return 0        # no tree: the build refuses the call
    if ! command -v flock >/dev/null 2>&1; then
        echo "axl-build.sh: flock not found -- builds in $tree are not serialised" >&2
        return 0
    fi
    lock="$tree.axl-build.lock"
    mkdir -p "$(dirname "$lock")"
    exec {_axl_lock_fd}>"$lock"
    if ! flock -n "$_axl_lock_fd"; then
        echo "axl-build.sh: waiting for another build in ${tree#"$REPO/"}..." >&2
        flock "$_axl_lock_fd"
    fi
}


arch="${ARCH:-x64}"
build="${BUILD:-DEBUG}"
rtk="${RTK_USB_UNDI:-}"
print_prefix=false
abs=false
goals=()
skip_next=false
for a in "$@"; do
    if $skip_next; then skip_next=false; continue; fi
    case "$a" in
        -j)             skip_next=true ;; # `-j N`: the N is not a goal
        --print-prefix) print_prefix=true ;;
        --abs)          abs=true ;;
        -s|-j*|--no-print-directory) ;;   # ninja is parallel and terse already
        ARCH=*)         arch="${a#ARCH=}" ;;
        BUILD=*)        build="${a#BUILD=}" ;;
        RTK_USB_UNDI=*) rtk="${a#RTK_USB_UNDI=}" ;;
        # Where sshd-rtk's goals put their images: read by the build step
        # itself (cmake/AxlRtkOut.cmake), so it travels in the environment.
        SSHD_RTK_OUT=*) export SSHD_RTK_OUT="${a#SSHD_RTK_OUT=}" ;;
        AXL_CPP=1)      ;;                # the CMake build always builds C++
        -*|*=*)
            echo "axl-build.sh: '$a' has no meaning to the CMake build -- refusing" \
                 "rather than ignoring it" >&2
            exit 2 ;;
        *)              goals+=("$a") ;;
    esac
done

dir=$(_cmake_dir "$arch" "$build" "$rtk") || exit 2
preset="${dir#build/}"
preset="${preset%-rtk}"
cfg_args=(--preset "$preset")
if [[ -n "$rtk" ]]; then
    cfg_args+=(-B "$REPO/$dir" "-DAXL_RTK_USB_UNDI=$rtk")
fi

if $print_prefix; then
    if $abs; then printf '%s/%s\n' "$REPO" "$dir"; else printf '%s\n' "$dir"; fi
    exit 0
fi

# The pinned CMake, not the box's (scripts/axl-cmake-pin.sh). Only here: the
# paths above never run cmake.
# shellcheck source=axl-cmake-pin.sh
source "$REPO/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1

cd "$REPO"
_lock_tree "$@"
# Configure once; after that ninja re-runs CMake itself when a CMakeLists.txt
# changes. The RTK tree is reconfigured every time: its driver path is input.
# A tree ANOTHER cmake configured is reconfigured --fresh, under the lock:
# ninja's regeneration edge runs the cmake its cache names, so without this
# the box's cmake would keep owning every tree it ever configured.
# Its build.ninja and CTest registration go first: if the --fresh fails, the
# tree must read as unconfigured next time, not as current with the old
# cmake still in its regeneration rule.
if axl_cmake_tree_foreign "$dir"; then
    echo "axl-build.sh: $(axl_cmake_foreign_reason "$dir")" >&2
    rm -f "$dir/build.ninja" "$dir/CTestTestfile.cmake"
    cmake --fresh "${cfg_args[@]}" >/dev/null
elif [[ -n "$rtk" || ! -f "$dir/build.ninja" ]]; then
    cmake "${cfg_args[@]}" >/dev/null
fi
# `all` is the default target, which IS everything -- so with it, no --target
# at all (naming the others too would build only them).
targets=()
for g in "${goals[@]}"; do
    if [[ "$g" == all ]]; then targets=(); break; fi
    # A FILE under the tree (`<tree>/tools/cut.efi`): ninja names its outputs
    # relative to the build tree, so the same file is `tools/cut.efi` there.
    g="${g#"$REPO/"}"
    g="${g#"$dir/"}"
    targets+=(--target "$g")
done
exec cmake --build "$dir" "${targets[@]}"
