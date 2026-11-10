#!/bin/bash
# ctest-tmpbase.sh -- own a CTest run's temp base, and refuse a second run on
# the same build tree while the first is live.
#
# The integration tests' setup / cleanup fixture pair (integration.tmp-setup,
# integration.tmp-cleanup; cmake/AxlIntegrationTests.cmake.in) runs this. The
# base is what run-integration.sh made with mktemp per run: run-qemu.sh's
# scratch (disk images, pflash copies, sockets) goes under it, and it is
# removed when the run ends.
#
# ONE RUN PER TREE. A run's tests share more than the base -- the build tree
# a prep writes, the registration, the make prefix -- so a second concurrent
# ctest on the same tree would corrupt the first. The owner is the ctest
# process (this script's parent), recorded in OWNER_FILE, which the
# registration keys on the TREE (its .run-owner), not on the base: a caller's
# own temp base (AXL_IT_QEMU_TMPDIR) separates nothing the refusal protects.
# Setup refuses while the recorded owner is alive, and takes over from one
# that is gone (an interrupted run). Cleanup removes only what its own ctest
# owns.
#
# --foreign: DIR is the caller's (the runner's rule for an AXL_QEMU_TMPDIR it
# was handed). It must exist, and it is never emptied, created or removed --
# only the owner record is managed.
#
# Usage: ctest-tmpbase.sh setup|cleanup DIR OWNER_FILE [--foreign]
set -uo pipefail

mode="${1:-}" dir="${2:-}" owner_file="${3:-}" foreign=0
[[ "${4:-}" == --foreign ]] && foreign=1
[[ -n "$dir" && "$dir" == /* && -n "$owner_file" && "$owner_file" == /* ]] \
    || { echo "ctest-tmpbase.sh: DIR and OWNER_FILE must be absolute paths" >&2; exit 2; }
me=$PPID   # the ctest running this fixture

live_owner() {   # -> the recorded owner's pid when it is a live process, else nothing
    local p
    p=$(cat "$owner_file" 2>/dev/null) || return 0
    [[ "$p" =~ ^[0-9]+$ ]] && kill -0 "$p" 2>/dev/null && printf '%s\n' "$p"
    return 0
}

case "$mode" in
    setup)
        o=$(live_owner)
        if [[ -n "$o" && "$o" != "$me" ]]; then
            echo "ctest-tmpbase.sh: another ctest run (pid $o) is using this build tree's" \
                 "integration tests (owner record $owner_file). One run per tree: the" \
                 "build tree and the registration are shared. Wait for it, or stop it." >&2
            exit 1
        fi
        if (( foreign )); then
            [[ -d "$dir" ]] || { echo "ctest-tmpbase.sh: no directory $dir (AXL_IT_QEMU_TMPDIR)" >&2; exit 1; }
        else
            # A base left by an interrupted run is taken over, emptied first.
            rm -rf "${dir:?}"
            mkdir -p "$dir" || exit 1
        fi
        mkdir -p "$(dirname "$owner_file")" && printf '%s\n' "$me" > "$owner_file"
        ;;
    cleanup)
        o=$(cat "$owner_file" 2>/dev/null || true)
        if [[ "$o" == "$me" ]]; then
            (( foreign )) || rm -rf "${dir:?}"
            rm -f "$owner_file"
        else
            echo "ctest-tmpbase.sh: this tree's run is not this ctest's (owner '${o:-none}', this run $me) -- left alone"
        fi
        ;;
    *)
        echo "usage: ctest-tmpbase.sh setup|cleanup DIR OWNER_FILE [--foreign]" >&2
        exit 2 ;;
esac
