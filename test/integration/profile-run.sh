#!/bin/bash
# profile-run.sh — where did an integration run's wall clock actually go?
#
# Usage:
#   ./test/integration/profile-run.sh <runner-output.log> [logdir]
#   ./test/integration/profile-run.sh --last <runner-output.log>
#
# `logdir` defaults to the "logs: /tmp/tmp.XXXX" path the runner prints in the
# log you pass; --last uses the newest /tmp/tmp.* instead.
#
# WHY THIS EXISTS. "The suite takes too long" is a claim nobody had measured,
# and the intuition was wrong: the run this was written against spent 6.8 of
# its 6.9 minutes in the parallel test phase and about a second in the
# pre-build, which put it at ~97% of the best a perfect scheduler could do.
# Optimising the runner would therefore have bought nothing. What sets the
# floor is the WORK, and this prints the two numbers that say so:
#
#   sum/J     the parallel floor from total test time over J jobs
#   longest   the single slowest test, which no amount of parallelism beats
#
# The floor is max() of those. When `longest` is the larger, the only way to
# go faster is to make that ONE test cheaper -- a fact that is invisible in a
# sorted list of durations, because the list looks like a smooth tail.
#
# It also prints est-vs-actual drift. Those estimates are not decoration:
# `run-integration.sh --shard` packs shards by `est=` (longest-processing-time
# first), so estimates that have gone stale silently unbalance CI, and the
# error is in the direction that hurts -- a test whose est is half its real
# cost drags one shard long while the others idle.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

USE_LAST=0
if [[ "${1:-}" == "--last" ]]; then USE_LAST=1; shift; fi
SUMMARY="${1:-}"
if [[ -z "$SUMMARY" || ! -r "$SUMMARY" ]]; then
    echo "usage: $0 [--last] <runner-output.log> [logdir]" >&2
    exit 2
fi

LOGDIR="${2:-}"
if [[ -z "$LOGDIR" ]]; then
    if [[ "$USE_LAST" -eq 1 ]]; then
        LOGDIR="$(ls -dt /tmp/tmp.* 2>/dev/null | head -1 || true)"
    else
        LOGDIR="$(grep -oE '^logs: .*' "$SUMMARY" | tail -1 | awk '{print $2}' || true)"
    fi
fi

# Jobs: the runner does not record -j, so take it from the log if present and
# fall back to the core count. Printed, because the floor depends on it and a
# wrong J would quietly misattribute the whole analysis.
JOBS="$(grep -oE '\-j *[0-9]+' "$SUMMARY" | head -1 | tr -dc '0-9' || true)"
[[ -n "$JOBS" ]] || JOBS="$(nproc 2>/dev/null || echo 8)"

echo "=== integration run profile ==="
echo "summary : $SUMMARY"
echo "logdir  : ${LOGDIR:-(not found -- timeline unavailable)}"
echo "jobs    : $JOBS"
echo

# --- durations, from the runner's own verdict lines -------------------------
tmp_d="$(mktemp)"; trap 'rm -f "$tmp_d"' EXIT
grep -oE "[A-Za-z0-9_.-]+\.sh (PASS|FAIL|TIMEOUT) [0-9]+s" "$SUMMARY" \
    | awk '{d=$3; sub(/s$/,"",d); print d"\t"$1"\t"$2}' > "$tmp_d" || true

if [[ ! -s "$tmp_d" ]]; then
    echo "no per-test verdict lines found in $SUMMARY" >&2
    exit 1
fi

awk -v J="$JOBS" -F'\t' '
{ n++; tot+=$1; if ($1+0 > max) { max=$1+0; who=$2 } }
END {
  floor_par = tot / J
  floor     = (floor_par > max) ? floor_par : max
  binding   = (floor_par > max) ? "total work / jobs" : "the single slowest test"
  printf "tests                  %d\n", n
  printf "serial test time       %.1f min   (sum of every test)\n", tot/60
  printf "parallel floor (sum/J) %.1f min\n", floor_par/60
  printf "longest single test    %.1f min   %s\n", max/60, who
  printf "\nBEST POSSIBLE          %.1f min   bound by %s\n", floor/60, binding
}' "$tmp_d"

# --- real timeline, from per-test log mtimes --------------------------------
# The runner writes one log per test and does not timestamp its stdout, so
# mtime is the only end-time record. start = end - duration is an inference,
# but the SPAN it produces is exact and that is what the phase split needs.
if [[ -n "$LOGDIR" && -d "$LOGDIR" ]]; then
    pre_birth="$(stat -c %W "$LOGDIR/_prebuild.log" 2>/dev/null || echo 0)"
    [[ "$pre_birth" == "0" ]] && pre_birth="$(stat -c %Y "$LOGDIR/_prebuild.log" 2>/dev/null || echo 0)"
    pre_end="$(stat -c %Y "$LOGDIR/_prebuild.log" 2>/dev/null || echo 0)"
    first_test="$(find "$LOGDIR" -name '*.log' ! -name '_prebuild.log' -printf '%T@\n' 2>/dev/null \
                  | sort -n | head -1 | cut -d. -f1)"
    last_test="$(find "$LOGDIR" -name '*.log' ! -name '_prebuild.log' -printf '%T@\n' 2>/dev/null \
                 | sort -n | tail -1 | cut -d. -f1)"
    if [[ -n "$first_test" && -n "$last_test" && "$pre_birth" != "0" ]]; then
        awk -v pb="$pre_birth" -v pe="$pre_end" -v ft="$first_test" -v lt="$last_test" '
        BEGIN {
          printf "\n--- where the wall clock went ---\n"
          printf "pre-build              %.1f min\n", (pe-pb)/60
          printf "test phase             %.1f min\n", (lt-ft)/60
          printf "TOTAL                  %.1f min\n", (lt-pb)/60
        }'
        # Efficiency against the floor computed above.
        awk -v J="$JOBS" -v ft="$first_test" -v lt="$last_test" -F'\t' '
        { tot+=$1; if ($1+0>max) max=$1+0 }
        END {
          fp = tot/J; fl = (fp>max)?fp:max
          actual = lt-ft
          if (actual > 0)
            printf "\ntest phase is %.0f%% of the theoretical best (%.1f min vs %.1f min)\n", \
                   100*fl/actual, fl/60, actual/60
        }' "$tmp_d"
    fi
fi

echo
echo "--- top 10 by duration ---"
sort -rn "$tmp_d" | head -10 | awk -F'\t' '{printf "  %5ds  %s\n", $1, $2}'

# --- est drift --------------------------------------------------------------
# Every test declares est= in its test-meta header. Under the parallel pool a
# test routinely exceeds it, so a uniform overshoot is expected and fine --
# ORDERING is what the estimates are used for. What matters is an OUTLIER,
# because that is what unbalances a shard.
echo
echo "--- est= drift (worst 8; ratios cluster ~1.3x under load) ---"
while IFS=$'\t' read -r dur name _; do
    f="$SCRIPT_DIR/$name"
    [[ -r "$f" ]] || continue
    e="$(grep -m1 '^# test-meta:' "$f" | grep -oE 'est=[0-9]+' | cut -d= -f2 || true)"
    [[ "$e" =~ ^[0-9]+$ ]] && [[ "$e" -gt 0 ]] || continue
    awk -v e="$e" -v d="$dur" -v n="$name" 'BEGIN{printf "%.2f\t%s\t%s\t%s\n", d/e, e, d, n}'
done < "$tmp_d" | sort -rn | head -8 \
  | awk -F'\t' 'BEGIN{printf "  %6s %6s %6s  %s\n","ratio","est","actual","test"}
                {printf "  %5.1fx %6s %6s  %s\n", $1, $2, $3, $4}'
