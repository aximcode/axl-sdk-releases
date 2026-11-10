#!/bin/bash
# pool-or-boot.sh -- run a command on a leased pool guest; on a POOL miss run
# it booted instead (docs/AXL-Test-Guest-Pool-Design.md §6.7).
#
# A pool failure -- the wrapper's (no slot, the service gone), or one the
# command's own harness met on the guest -- is marked AXLPOOL-MISS:
# (POOL_MISS_MARKER, test/pool/axlpool/service.py), and nothing else is: a
# 77 WITHOUT the marker is the command's own SKIP. A miss is never the
# command's verdict, and never a quiet SKIP either (that would drop the
# coverage whenever the pool hiccupped): it is said, its log kept, and the
# command run booted.
#
# Sourced: pool_attempt, for a caller with more to do around the attempt
# (lib/run-one.sh: the skip cache, its retry).
# Run: the whole thing, for a caller with nothing else to do (the pooled
# unit.<App> registration).
#
#   lib/pool-or-boot.sh --class CLASS --arch ARCH --tree TREE --log FILE
#                       [--timeout N] -- CMD...
#     The pool is AXL_ONE_POOL_SOCK's (unset or empty: CMD runs booted, at
#     once). The pooled attempt's output goes to FILE and to stdout as it
#     comes (a ctest TIMEOUT that kills the run still shows how far it got);
#     a miss keeps it as <FILE less .log>.pool-miss.log, as run-one.sh names
#     one, and clears the attempt's TEST_COUNT_FILE (a booted rerun that
#     fails before writing must not leave a "pooled" count behind).
#     --timeout N bounds each run, pooled (the attempt, the lease wrapper
#     with it) or booted, at N seconds -- a positive integer: GNU timeout
#     reads 0 as NO limit. Exit: CMD's status (booted or pooled), 124 when
#     the limit cut it off; 2 on a usage error; 128 + N when signal N
#     stopped it (SIGNALS below).
#
# SIGNALS (lib/fwd-signal.sh). A TERM, INT or HUP to this script -- ctest's
# TIMEOUT backstop sends a TERM to it and nothing else -- reaches the lease
# client at once, as a TERM: the client passes it to the command's group and
# releases the lease. Nothing is run after it: no booted rerun.

if ! declare -F fwd_run > /dev/null; then
    # shellcheck source=fwd-signal.sh
    source "$(dirname "${BASH_SOURCE[0]}")/fwd-signal.sh" || {
        echo "pool-or-boot: cannot load fwd-signal.sh beside it" >&2
        exit 2   # fatal sourced too: without it a TERM would orphan the client
    }
fi

# _pool_in_dir DIR CMD... -- CMD run from DIR, in place of this (sub)shell:
# started in the background by fwd_start, the PID it records is CMD's own.
_pool_in_dir() {
    cd "$1" || exit 125
    shift
    exec "$@"
}

# pool_attempt SOCK TREE CLASS ARCH LOG TIMEOUT CMD... -- one pooled run of
# CMD into LOG, under `timeout TIMEOUT` (0: none) -- and to stdout as well
# when POOL_TEE=1. Its status is CMD's (or the wrapper's); POOL_MISS is set
# to the miss's reason when the run was a POOL miss, else empty.
pool_attempt() {
    local sock="$1" tree="$2" class="$3" arch="$4" log="$5" limit="$6" rc
    shift 6
    POOL_MISS=""
    local -a tmo=() agent=()
    # AXL_IT_POOL_AGENT=tree (the CI presets): the guest runs this TREE's own
    # agent build, so a CI run exercises the commit's agent, not whatever the
    # service's checkout last built.
    [[ -n "${AXL_IT_POOL_AGENT:-}" ]] && agent=(--agent "$AXL_IT_POOL_AGENT")
    # AXL_POOL_CODE_ID: the service's code identity as this RUN found it
    # (asked once, by the CTest registration). A lease from a service running
    # other code -- restarted mid-run -- is then a pool MISS, so the test
    # boots and a pooled skip-cache key naming that identity is only ever
    # committed by a test that ran on it (lib/test-cache.sh).
    [[ -n "${AXL_POOL_CODE_ID:-}" ]] && agent+=(--expect-code "$AXL_POOL_CODE_ID")
    # No -k: its KILL would reach the lease wrapper alone (the test runs in a
    # process group of its own) and orphan the test on a guest being reset.
    # The wrapper forwards the TERM and KILLs a group that ignores it
    # (client.py CHILD_TERM_GRACE_S), then releases.
    [[ "$limit" != 0 ]] && tmo=(timeout "$limit")
    # Truncated once, then appended to (`>>`, `tee -a`): a TIMING line a
    # process appends to LOG by path (AXL_TEST_TIMING_LOG, lib/run-one.sh)
    # must not be overwritten by the writer holding the log's offset.
    : > "$log"
    local -a run=(_pool_in_dir "$tree/test/pool" ${tmo[@]+"${tmo[@]}"} python3 -m axlpool run
        --socket "$sock" --tree "$tree" --class "$class" --arch "$arch" ${agent[@]+"${agent[@]}"} -- "$@")
    # In the BACKGROUND (lib/fwd-signal.sh), so a TERM to this shell reaches
    # the client now rather than after it -- which is why the tee is a
    # `tail --pid` beside it and no longer a pipeline: a pipeline's PID is
    # its last command's, and the TERM would have gone to the tee.
    if [[ "${POOL_TEE:-0}" == 1 ]]; then
        if fwd_start "${run[@]}" >> "$log" 2>&1; then
            local tp
            tail -n +1 -s 0.2 --pid="$FWD_PID" -f "$log" &
            tp=$!
            fwd_wait; rc=$?
            # tail ends by itself once the client has (--pid); a signal
            # interrupting this wait must not leave its last lines unprinted.
            while kill -0 "$tp" 2>/dev/null; do wait "$tp"; done
        else
            rc=143
        fi
    else
        fwd_run "${run[@]}" >> "$log" 2>&1
        rc=$?
    fi
    # Stopped by a signal: whatever the client said, it was no pool miss.
    [[ -n "$FWD_SIGNAL" ]] && return "$rc"
    if [[ $rc -eq 77 ]] && grep -q '^AXLPOOL-MISS: ' "$log"; then
        POOL_MISS=$(grep -m1 '^AXLPOOL-MISS: ' "$log" | cut -c15-110)
        # The marker IS the miss; a reason is a courtesy. Callers test for
        # a non-empty POOL_MISS, so an empty reason must not read as none.
        [[ -n "$POOL_MISS" ]] || POOL_MISS="no reason given"
    fi
    return "$rc"
}

# Script mode only when executed, not when sourced.
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    set -uo pipefail
    usage() {
        echo "usage: pool-or-boot.sh --class CLASS --arch ARCH --tree TREE --log FILE [--timeout N] -- CMD..." >&2
        exit 2
    }
    class="" arch="" tree="" log="" limit=0 dashdash=0
    while (( $# )); do
        case "$1" in
            --class|--arch|--tree|--log|--timeout)
                (( $# >= 2 )) || usage
                case "$1" in
                    --class) class="$2" ;; --arch) arch="$2" ;;
                    --tree) tree="$2" ;; --log) log="$2" ;;
                    --timeout) [[ "$2" =~ ^[1-9][0-9]*$ ]] || usage; limit="$2" ;;
                esac
                shift 2 ;;
            --) shift; dashdash=1; break ;;
            *)  usage ;;
        esac
    done
    if [[ $dashdash -eq 0 || -z "$class" || -z "$arch" || -z "$tree" || -z "$log" || $# -eq 0 ]]; then
        usage
    fi
    missf="${log%.log}.pool-miss.log"
    rm -f "$missf"   # this call's alone, as run-one.sh's
    booted=("$@")
    [[ "$limit" != 0 ]] && booted=(timeout "$limit" "$@")
    sock="${AXL_ONE_POOL_SOCK:-}"
    if [[ -z "$sock" ]]; then
        # exec'd, so ctest's TERM reaches the command (or its `timeout`)
        # itself: test-axl.sh acts on it at once (common-test.sh).
        exec "${booted[@]}"
    fi
    fwd_signal_init
    POOL_TEE=1 pool_attempt "$sock" "$tree" "$class" "$arch" "$log" "$limit" "$@"
    rc=$?
    if [[ -n "$FWD_SIGNAL" ]]; then
        fwd_signal_status; exit $?
    fi
    if [[ -z "$POOL_MISS" ]]; then
        exit "$rc"
    fi
    cp -f "$log" "$missf"
    [[ -n "${TEST_COUNT_FILE:-}" ]] && rm -f "$TEST_COUNT_FILE"
    echo "pool-or-boot: pool miss ($POOL_MISS); running it booted"
    [[ -z "$FWD_SIGNAL" ]] || { fwd_signal_status; exit $?; }
    exec "${booted[@]}"
fi
