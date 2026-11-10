#!/bin/bash
# verify.sh — the pre-commit gate set, as a front end over CTest.
#
# It CHOOSES and REFUSES; CTest RUNS (slice 5a step 6, decision 1;
# docs/AXL-Build-System-Design.md §10.8). Its jobs are sets of CTest tests,
# run as THREE ctest calls and rendered as one table from their JUnit files
# (scripts/ctest-report.py verify):
#   - the x64 tree (build/x64-debug), CONCURRENTLY with the aa64 tree:
#       x64      when the axlpool service serves the arch (THE UNITS ON THE
#                POOL, below): every unit binary its own CTest test
#                (unit.<App>) on a leased guest, plus the inventory, the
#                ratchet and budget summed over them; else unit.one-boot:
#                test-axl.sh, every unit binary in ONE guest, with its
#                ratchet (it writes the baseline on a green x64 run) and its
#                wall-clock budget; under --apps, the named binaries one
#                CTest test each (unit.<App>), booted, plus the inventory
#       lint     gate.lint (scripts/lint.sh: clang-tidy over the x64-lint
#                preset, clang -Wall -Wextra)
#       gates    the gate.check-* source and compile gates (the `make` job
#                until 5.0.0, named for the Makefile the gates once ran)
#       imggates the gate.check-* labelled artifact (they read the tree's
#                images and archives, which the prep built)
#       docs     gate.docs (scripts/build-docs.sh, zero-warning Sphinx)
#   - the aa64 tree (build/aa64-debug):
#       aa64     the same on aa64 under TEST_SKIP_RATCHET=1: no ratchet and
#                no budget there (as before); cross-arch compares the counts
#       gates, imggates  that tree's own gate.check-* (the ones that read an
#                arch's compiler or images register in each arch's tree)
#   - then, ALONE, once both are done:
#       qloop    gate.qloop -- see THE QUIET SLOT below
# The registration carries what this script used to hand its jobs: nice on
# the gates, TEST_TIMEOUT_SCALE=2 on the per-binary (--apps) tests. The
# one-boot run's scales are this script's to declare (AXL_UNIT_TIMEOUT_SCALE
# / AXL_UNIT_BUDGET_SCALE, tree_env below).
# One run per tree: a second is refused by name.
#
# Without the pool the x64/aa64 jobs are ONE BOOT per arch (Mike, 2026-11-06,
# on measurement): per binary BOOTED -- 49 guest boots per arch, TCG on aa64
# -- verify took 357s against 173s for this. Per binary POOLED boots nothing,
# and took 71-92 s for both arches (2026-11-07). The x64-suite/aa64-suite
# presets run per binary and exclude unit.one-boot.
#
# Usage:
#   ./scripts/verify.sh              # everything
#   ./scripts/verify.sh --no-docs    # skip the Sphinx build (slowest to rebuild)
#   ./scripts/verify.sh --only=docs  # run ONLY these jobs (comma-separated):
#                                    # x64 aa64 lint gates imggates qloop docs
#   ./scripts/verify.sh --apps=AxlTestSsh   # x64/aa64 run ONLY these binaries
#   ./scripts/verify.sh --print-ctest       # the ctest calls, run nothing
#   AXL_VERIFY_KEEP=1 ./scripts/verify.sh   # keep the per-job logs on success
#   AXL_VERIFY_POOL=0 ./scripts/verify.sh   # the units and qloop boot, pool or not
#                       (default auto; or a pool socket path, as AXL_IT_POOL)
#   AXL_VERIFY_POOL_AGENT=tree              # pooled guests run THIS tree's agent
#                       (AXL_IT_POOL_AGENT, as the CI presets set it)
#   AXL_VERIFY_UNIT_BUDGET=190              # the unit wall-clock budget, declared
#                       (TEST_MAX_DURATION to the unit runs; inherited, it is scrubbed)
#   AXL_VERIFY_PRINT_FAILURES=1             # on failure, print each run's ctest
#                       output (failing tests' included) after the table
#
# --apps narrows the two arch jobs to named unit binaries. They run all 49
# otherwise, ~110-120s per arch, when the change at issue is in one of them --
# AxlTestSsh alone is a couple of seconds. The names are validated against
# `test-axl.sh --list-apps`, which is the suite's own list, and an unknown
# name is REFUSED: "matched nothing" and "you asked for something that is not
# here" are the same empty set and opposite facts. So is a name the TREE does
# not register (a binary added since it was configured): reconfigure.
#
# WHAT --apps GIVES UP, and it is not small:
#   - THE RATCHET. A subset cannot be compared against .last-pass-count (it is
#     legitimately far short of it), so the ratchet reports PARTIAL and
#     NEITHER arch writes the baseline. A subset that wrote it would lower the bar
#     for every later full run -- silently, since a lowered ratchet is green.
#   - THE WALL-CLOCK BUDGET. It measures one boot running everything; a
#     per-binary run has nothing for it to measure.
#   - MOST OF THE CROSS-ARCH CHECK. The two subset counts are still compared
#     to each other -- worth doing, it is the only check that sees an arch gate
#     with no balancer -- but over the subset, not the suite. The line says so.
# All three are named in the summary, because a scoped log that reads like a
# full one is the failure this whole file is arranged against.
#
# --only exists because the jobs are not equally able to SEE a given change.
# A markdown-only edit is invisible to x64/aa64/lint/gates, so running them
# costs two arch builds plus a full AXL_CPP=1 lint build for no information --
# and build-docs.sh runs INSIDE this script, so calling it separately first
# pays for Sphinx twice. `--only=docs` is the whole gate for a prose change.
#
# A filtered run NEVER prints a bare "ALL GREEN": it names what did not run.
# Otherwise a --only run and a full run are indistinguishable in a log, which
# is the failure mode this tree keeps meeting -- a gate that cannot see the
# change reporting the same green as one that can.
#
# THE QUIET SLOT (job qloop; docs/AXL-Build-System-Design.md §10.14).
# AxlTestLoop's driver idle-budget timing cases
# cannot be judged under the trees' concurrent load, so qloop is a third ctest
# call, made after both trees' calls return, when the box is as quiet as this
# script ever makes it (RUN_SERIAL alone would serialise it within ONE ctest
# process only, and the aa64 tree is another). That script holds the verdict and why (it is
# CTest's gate.qloop too): PASS needs the test's `LOOP-TIMING: judged` marker
# and a clean run; no marker, or a FAIL made only of timing bounds missed
# while the guest measurably waited for a CPU, reads UNJUDGED -- the run is
# NOT green (exit 3), and the row says why. It runs on a leased `unit` guest
# when the pool serves (no boot), after asking the pool to settle; its row
# says where it ran.
#
# Exit non-zero if any job fails or the arches disagree; 3 if nothing failed
# but the quiet slot could not judge.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$(dirname "$SCRIPT_DIR")" || exit 1

WANT_DOCS=1
PRINT_CTEST=0
ONLY=""; ONLY_SEEN=0
APPS=""; APPS_SEEN=0
ALL_JOBS=(x64 aa64 lint gates imggates qloop docs)
for a in "$@"; do
    case "$a" in
        --no-docs) WANT_DOCS=0 ;;
        --print-ctest) PRINT_CTEST=1 ;;
        --only=*)  ONLY="${a#--only=}"; ONLY_SEEN=1 ;;
        # SEEN is tracked apart from the VALUE. `--apps=` and `--apps="$V"`
        # with V unset are the same string, and the second is how it happens;
        # without this the flag silently does not apply and a full run is
        # reported as the scoped one the caller asked for.
        --apps=*)  APPS="${a#--apps=}"; APPS_SEEN=1 ;;
        --lint-files=*)
            echo "verify.sh: --lint-files is gone. The lint job is incremental:" \
                 "it re-checks exactly the TUs a change can reach, headers" \
                 "included, which a file list never did." >&2
            exit 2 ;;
        # DERIVED, not a line range. `sed -n '2,38p'` was the header's length
        # written down a second time, and it is wrong the moment a line is
        # added above it -- help that silently truncates is the same shape of
        # defect as a gate that silently narrows.
        -h|--help) awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next }
                        NR>1 { exit }' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "verify.sh: unknown option $a" >&2; exit 2 ;;
    esac
done

if [[ $ONLY_SEEN -eq 1 && -z "$ONLY" ]]; then
    echo "verify.sh: --only= was given an EMPTY value. That is a typo (or an" \
         "unset variable), not a request to run every job." >&2
    exit 2
fi

OUT=$(mktemp -d -t axl-verify.XXXXXX)
# Every `exit 2` refusal below this line used to leak this directory: 402 of
# them were sitting in /tmp when a review counted, 268 from one day alone,
# because the gate-scoping self-test drives ~10 refusals a run and this box
# ran the suite on every push (until 2026-10-21; now on every dispatch).
#
# ONLY the refusals. A run that actually starts jobs KEEPS its logs when it
# FAILS -- that is deliberate (see the tail of this script, which removes
# $OUT only when nothing failed), and it is the whole reason the directory
# has a name worth printing. So the trap fires only while no job has begun.
_verify_started=0
_verify_cleanup() { (( _verify_started )) || rm -rf "$OUT"; }
trap _verify_cleanup EXIT
SKIPPED=()
if [[ -n "$ONLY" ]]; then
    IFS=',' read -r -a JOBS <<< "$ONLY"
    # A typo must FAIL, not quietly select nothing and report ALL GREEN.
    for j in "${JOBS[@]}"; do
        # The two gate jobs were `make` and `makeimg` until the Makefile went
        # (5.0.0): an old name is told the new one, not "unknown".
        case "$j" in
            make)    echo "verify.sh: the 'make' job is 'gates' since 5.0.0 (the Makefile is gone)" >&2; exit 2 ;;
            makeimg) echo "verify.sh: the 'makeimg' job is 'imggates' since 5.0.0 (the Makefile is gone)" >&2; exit 2 ;;
        esac
        case " ${ALL_JOBS[*]} " in
            *" $j "*) ;;
            *) echo "verify.sh: --only names unknown job '$j'" \
                    "(known: ${ALL_JOBS[*]})" >&2; exit 2 ;;
        esac
    done
    for j in "${ALL_JOBS[@]}"; do
        case " ${JOBS[*]} " in *" $j "*) ;; *) SKIPPED+=("$j") ;; esac
    done
else
    JOBS=(x64 aa64 lint gates imggates qloop)
    [[ $WANT_DOCS -eq 1 ]] && JOBS+=(docs) || SKIPPED+=(docs)
fi

want() {
    local j
    for j in "${JOBS[@]}"; do [[ "$j" == "$1" ]] && return 0; done
    return 1
}

# --apps: validate BEFORE anything runs, against the suite's own list.
#
# ASKED FOR, not listed -- the same arrangement as the gates, which each tree registers. A copy of
# TEST_APPS here would drift the first time a binary was added, and a
# validator that accepts a name the suite does not run refuses nothing: the
# subsequent run would boot zero binaries, print no Results line and be
# reported by this table as a job that could not be read.
if [[ $APPS_SEEN -eq 1 && -z "$APPS" ]]; then
    echo "verify.sh: --apps= was given an EMPTY value. That is a typo (or an" \
         "unset variable), not a request to run everything." >&2
    exit 2
fi

APPS_SPACED=""
KNOWN_APPS=()
if [[ -n "$APPS" ]]; then
    # A subset flag that selects no job it can affect is a typo, not a no-op.
    if ! want x64 && ! want aa64; then
        echo "verify.sh: --apps=$APPS names unit binaries, but neither the" \
             "x64 nor the aa64 job is selected -- it would narrow nothing." \
             "Add x64 and/or aa64 to --only, or drop --apps." >&2
        exit 2
    fi
    # THE EXIT STATUS, SEPARATELY FROM THE OUTPUT. `mapfile < <(cmd)` leaves
    # $? as mapfile's, so a --list-apps that could not RUN and one that ran
    # and printed nothing are the same empty array and opposite facts -- and
    # only the second is a question about the list.
    _known_out=$(./test/integration/test-axl.sh --list-apps); _known_rc=$?
    if (( _known_rc != 0 )); then
        echo "verify.sh: refusing to run -- 'test-axl.sh --list-apps' FAILED" \
             "(exit $_known_rc). --apps was never validated against anything;" \
             "this is not 'the list is empty'." >&2
        exit 2
    fi
    # `mapfile <<< "$s"` on an EMPTY string yields a ONE-element array holding
    # "", so the `== 0` guard below could never fire and an empty list was
    # reported as "unknown binary 'X'. known: " -- the wrong cause, stated as
    # confidently as the right one. A process substitution of `printf '%s'`
    # feeds mapfile nothing when there is nothing, which is what makes the
    # guard reachable. (The sibling half of this line, mapfile's exit status
    # standing in for the script's, was fixed one commit earlier.)
    mapfile -t KNOWN_APPS < <(printf '%s' "$_known_out")
    if ! _skip_out=$(./test/integration/test-axl.sh --list-apps-skipped); then
        echo "verify.sh: refusing to run -- 'test-axl.sh --list-apps-skipped'" \
             "FAILED; --apps cannot tell an excluded binary from an unknown" \
             "one." >&2
        exit 2
    fi
    mapfile -t SKIP_APPS < <(printf '%s' "$_skip_out")
    if (( ${#KNOWN_APPS[@]} == 0 )); then
        echo "verify.sh: refusing to run -- 'test-axl.sh --list-apps' succeeded" \
             "but named NO binaries, so --apps cannot be validated." >&2
        exit 2
    fi
    APPS_SPACED="${APPS//,/ }"
    for _a in $APPS_SPACED; do
        case " ${KNOWN_APPS[*]} " in
            *" $_a "*) ;;
            *) case " ${SKIP_APPS[*]} " in
                   *" $_a "*)
                       echo "verify.sh: --apps names '$_a', which the suite" \
                            "deliberately does NOT run (TEST_APPS_SKIP in" \
                            "test-axl.sh records why). It is excluded, not" \
                            "unknown; run its own test instead." >&2 ;;
                   *) echo "verify.sh: --apps names unknown unit binary '$_a'." >&2
                      echo "           known: ${KNOWN_APPS[*]}" >&2 ;;
               esac
               exit 2 ;;
        esac
    done
fi

# ---------------------------------------------------------------------------
# THE RUN IS CTEST'S. Everything above chose the jobs and refused what it
# should; from here this only translates them into ctest calls and renders
# the result.
# ---------------------------------------------------------------------------
REPO="$(pwd -P)"
# The pinned CMake and ctest (scripts/axl-cmake-pin.sh), for this script and
# every job it starts; cts_tree_current redoes a tree another cmake made.
# shellcheck source=axl-cmake-pin.sh
source "$REPO/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 2
# shellcheck source=../test/integration/lib/ctest-session.sh
source "$REPO/test/integration/lib/ctest-session.sh"
CTS_WHO=verify.sh
TREE_X64="$REPO/build/x64-debug" TREE_AA64="$REPO/build/aa64-debug"

# What this script does not set it does not pass on (as run-integration.sh;
# the list is the library's cts_inherited): an inherited AXL_IT_* /
# AXL_PREP_* / AXL_ONE_* / AXL_UNIT_* / AXL_TEST_CACHE would change what the
# trees' runs do.
declare -a SCRUB=()
while IFS= read -r _v; do SCRUB+=(-u "$_v"); done < <(cts_inherited)

want_tree() {   # TREE-NAME -> does any selected job run on it
    case "$1" in
        x64)  want x64 || want lint || want gates || want imggates || want docs ;;
        aa64) want aa64 || want gates || want imggates ;;
        qloop) want qloop ;;
    esac
}

# A tree's environment: its own log directory, and the prep told that no
# integration test was selected (AXL_PREP_TESTS set but empty: the shared
# targets the unit binaries need, no fixture, no restage).
# On an arch tree, the one-boot run's scales (unit.one-boot is strict without
# them; common-test.sh's TEST_TIMEOUT_SCALE / TEST_BUDGET_SCALE). Beside
# lint/gates/docs and the other arch a suite's wall time is ~1.6x idle, and the
# hang timeout is wall seconds: x2 when more than one of x64/aa64/lint/gates/
# imggates/docs runs. imggates counts now: it used to run AFTER the guests, and
# runs beside them in the x64 tree (a cold image build). The budget x1.40
# only when lint, gates, imggates or docs run beside the guests --
# the pressure it was measured under: the guest ran 1.41-1.49x idle (113.6 /
# 120.1 s against 80.8 s) with contention reading 1.00, so 110 s x 1.40 =
# 154 s leaves ~28% over the worst. A lone arch, or two guests alone, stay
# strict at 110 s.
# THE UNITS ON THE POOL (docs/AXL-Test-Guest-Pool-Design.md §6.7): when the
# user's axlpool service serves an arch, that arch's full unit job is the
# per-binary unit.<App> tests, each on a leased `unit` guest (spread over the
# class's idle guests by ctest), plus the inventory -- no boot; else the
# one-boot run. The REGISTRATION decides, not this script: each arch run is
# handed AXL_IT_POOL (AXL_VERIFY_POOL, default auto), and the tree's own
# listing then says whether it pooled the units (a guest_unit slot on each)
# and, when it did not, why (its integration.pool line). A second check here
# could disagree with it, and once did in effect: "pooled" said while every
# binary booted. --apps stays booted: a few binaries, not worth a lease each.
# And the RUN gets the socket the listing resolved, not `auto` again: ctest
# evaluates the registration afresh for the run, and a service that failed in
# between would unpool the units silently -- 49 boots per arch behind a
# "pooled" line. Given the socket, the run's own pool check fails instead.
VERIFY_POOL="${AXL_VERIFY_POOL:-auto}"
if [[ -n "${AXL_VERIFY_UNIT_BUDGET:-}" ]]; then
    [[ "$AXL_VERIFY_UNIT_BUDGET" =~ ^[0-9]+$ ]] || {
        echo "verify.sh: AXL_VERIFY_UNIT_BUDGET='$AXL_VERIFY_UNIT_BUDGET' -- whole seconds expected" >&2
        exit 2; }
    echo "verify: unit budget declared by the caller: ${AXL_VERIFY_UNIT_BUDGET} s (AXL_VERIFY_UNIT_BUDGET)" >&2
fi
declare -A POOL_RESOLVED=()   # tree -> the socket its listing pooled the units on

tree_env() {   # TREE-NAME -> fills TENV
    TENV=(${SCRUB[@]+"${SCRUB[@]}"} "AXL_IT_LOGDIR=$OUT/$1" "AXL_PREP_TESTS=")
    # The quiet slot leases a `unit` guest as the units do (gate.qloop's
    # registration, scripts/qloop-verdict.sh) -- in verify's own MODE, not the
    # socket the x64 listing resolved: auto lets a service that went away
    # since then make qloop miss and boot, where a named socket would fail
    # its pool check and leave gate.qloop not run.
    if [[ "$1" == qloop ]]; then
        TENV+=("AXL_IT_POOL=$VERIFY_POOL")
        [[ -n "${AXL_VERIFY_POOL_AGENT:-}" ]] && TENV+=("AXL_IT_POOL_AGENT=$AXL_VERIFY_POOL_AGENT")
        return 0
    fi
    [[ -n "${AXL_VERIFY_POOL_AGENT:-}" ]] && TENV+=("AXL_IT_POOL_AGENT=$AXL_VERIFY_POOL_AGENT")
    [[ "$1" == x64 || "$1" == aa64 ]] || return 0
    [[ -z "$APPS" ]] && TENV+=("AXL_IT_POOL=${POOL_RESOLVED[$1]:-$VERIFY_POOL}")
    # A unit budget the CALLER declares (CI's slower runner: release-suite.sh
    # passes its TEST_MAX_DURATION). Inherited it is scrubbed -- it would
    # loosen the budget unannounced -- so it is taken only by this name, and
    # said.
    [[ -n "${AXL_VERIFY_UNIT_BUDGET:-}" ]] && TENV+=("TEST_MAX_DURATION=$AXL_VERIFY_UNIT_BUDGET")
    local j conc=0
    for j in x64 aa64 lint gates imggates docs; do want "$j" && conc=$((conc + 1)); done
    (( conc > 1 )) && TENV+=("AXL_UNIT_TIMEOUT_SCALE=2")
    # ALWAYS declared: the pooled units' ratchet judges a budget only when a
    # front end says what the run shared the box with (unit-ratchet.sh).
    if want lint || want gates || want imggates || want docs; then
        TENV+=("AXL_UNIT_BUDGET_SCALE=1.40")
    else
        TENV+=("AXL_UNIT_BUDGET_SCALE=1")
    fi
    # --apps is a scoped run: the ratchet judges it but never writes the
    # baseline, even when it names every binary (unit-ratchet.sh).
    [[ -n "$APPS" ]] && TENV+=("AXL_UNIT_NO_WRITE=1")
    return 0
}

# THE SELECTION, asked of each tree's own registration (ctest --show-only),
# by name and label: the unit binaries (or --apps's), the inventory -- the
# ratchet and its reset come as fixtures -- and the gates by job.
select_tests() {   # TREE-NAME TREE-DIR -> test names, one per line
    local name="$1" dir="$2" listing
    tree_env "$name"
    listing=$(env "${TENV[@]}" ctest --test-dir "$dir" --show-only=json-v1 \
                  --output-junit /dev/null 2>/dev/null) || return 1
    # Which job families this tree serves, as plain flags. A full unit job is
    # the per-binary unit.<App> tests when the registration pools them (THE
    # UNITS ON THE POOL, above), else unit.one-boot -- every binary in ONE
    # guest, with its ratchet and budget (49 BOOTED per-binary runs cost
    # verify 357s against 162s); --apps runs the named binaries one test
    # each, booted, plus the inventory.
    local u=false l=false d=false m=false mi=false
    if [[ "$name" == x64 ]]; then
        want x64 && u=true; want lint && l=true; want docs && d=true
        want gates && m=true; want imggates && mi=true
    elif [[ "$name" == aa64 ]]; then
        want aa64 && u=true
        want gates && m=true; want imggates && mi=true
    fi
    # The registration's verdict on the units (THE UNITS ON THE POOL).
    local pooled=false why
    if [[ "$u" == true && -z "$APPS" ]]; then
        pooled=$(jq '[.tests[] | select(.name | startswith("unit.AxlTest"))
                      | select([.properties[]? | select(.name == "RESOURCE_GROUPS")
                                | .value | tostring | contains("guest_unit_")] | any)]
                     | length > 0' <<< "$listing")
        if [[ "$pooled" == true ]]; then
            POOL_RESOLVED[$name]=$(jq -r '[.tests[] | select(.name | startswith("unit.AxlTest"))
                | .properties[]? | select(.name == "ENVIRONMENT") | .value[]
                | select(startswith("AXL_ONE_POOL_SOCK=")) | ltrimstr("AXL_ONE_POOL_SOCK=")] | first // empty' <<< "$listing")
            echo "verify: $name units: pooled, one leased guest per binary (AXL_IT_POOL=$VERIFY_POOL)" >&2
        else
            why=$(jq -r '.tests[] | select(.name == "integration.pool") | .command | join(" ")' <<< "$listing" \
                  | sed -n "s/^[^ ]*bash -c echo '\\(.*\\)'\$/\\1/p")
            echo "verify: $name units: one boot (${why:-no pool service (AXL_IT_POOL=$VERIFY_POOL)})" >&2
        fi
    fi
    jq -r --arg apps " ${APPS_SPACED:-} " --argjson unit "$u" --argjson lint "$l" \
          --argjson pooled "$pooled" \
          --argjson docs "$d" --argjson gates "$m" --argjson imggates "$mi" '
        .tests[]
        | . as $t
        | ([.properties[]? | select(.name == "LABELS") | .value[]]) as $l
        | select(
            ($unit and $apps == "  " and ($pooled | not) and $t.name == "unit.one-boot")
            or ($unit and $apps == "  " and $pooled and ($t.name | startswith("unit."))
                   and ($t.name != "unit.counts-reset") and ($t.name != "unit.ratchet")
                   and ($t.name != "unit.one-boot"))
            or ($unit and $apps != "  " and ($t.name | startswith("unit."))
                   and ($t.name != "unit.counts-reset") and ($t.name != "unit.ratchet")
                   and ($t.name != "unit.one-boot")
                   and ($t.name == "unit.inventory"
                        or ($apps | contains(" " + ($t.name | ltrimstr("unit.")) + " "))))
            or ($lint and $t.name == "gate.lint")
            or ($docs and $t.name == "gate.docs")
            or ($gates and ($t.name | startswith("gate.check-")) and (($l | index("artifact")) == null))
            or ($imggates and ($t.name | startswith("gate.check-")) and (($l | index("artifact")) != null)))
        | .name' <<< "$listing"
}

# Concurrency: the two trees share the box, so each gets half of the
# measured nproc+4, both starting a test only while the load is under nproc-2.
#
# WHY verify KEEPS --test-load when the integration suite dropped it for a
# per-test CPU budget (docs/AXL-CI-Release-Speed-Design.md §15). A PROCESSORS
# budget is per ctest PROCESS, and verify runs two at once, one per tree, so
# neither can see what the other holds; and what they run are not integration
# tests with a measured cpu= but the unit guests and the gates, whose lint and
# docs jobs fan out to every CPU on their own. The load average is the one
# signal both trees share. Its one-minute lag cost the suite thousands of idle
# worker-seconds over ~10 minutes; verify's trees take ~2 minutes, its gate
# jobs are ordered after the prep, and its 173 s was measured with the gate on.
NCPU=$(nproc 2>/dev/null || echo 2)
J_TREE=$(( (NCPU + 4) / 2 )); (( J_TREE >= 1 )) || J_TREE=1
J_LOAD=$(( NCPU - 2 )); (( J_LOAD >= 1 )) || J_LOAD=1
tree_argv() {   # TREE-NAME TREE-DIR JOBS -> fills TARGV
    TARGV=(ctest --test-dir "$2" --tests-from-file "$OUT/$1-selected.txt" -j "$3")
    [[ "$1" != qloop ]] && TARGV+=(--test-load "$J_LOAD")
    # 8 MiB: ctest keeps the HEAD of a test's output, and the one-boot run's
    # Results line is its LAST, ~775 KB in (2026-11). ctest-report.py fails a
    # passing one-boot run whose Results line it cannot see.
    # --output-on-failure: a failing test's output lands in $OUT/<run>-ctest.txt
    # beside the JUnit file, where AXL_VERIFY_PRINT_FAILURES (below) finds it.
    TARGV+=(--no-tests=error --output-on-failure --output-junit "$OUT/$1.xml"
            --test-output-size-passed 8388608 --test-output-size-failed 8388608)
}

declare -A TDIR=([x64]="$TREE_X64" [aa64]="$TREE_AA64" [qloop]="$TREE_X64")
declare -A TJOBS=([x64]="$J_TREE" [aa64]="$J_TREE" [qloop]=1)
RUNS=()
for _t in x64 aa64 qloop; do want_tree "$_t" && RUNS+=("$_t"); done

for _t in "${RUNS[@]}"; do
    # The registration must be current before it can be asked anything.
    _preset=x64-debug; [[ "$_t" == aa64 ]] && _preset=aa64-debug
    if [[ $PRINT_CTEST -eq 0 ]]; then
        _busy=$(cts_tree_busy "${TDIR[$_t]}/integration-logs/.run-owner") && {
            echo "verify.sh: another ctest run (pid $_busy) is using ${TDIR[$_t]};" \
                 "one run per tree at a time." >&2
            exit 2
        }
        cts_tree_current "${TDIR[$_t]}" "$_preset" "$REPO" "$OUT/_configure-$_t.txt" || {
            # The configure log is the evidence: keep it, and say where.
            _verify_started=1
            echo "verify.sh: configuring ${TDIR[$_t]} failed -- see $OUT/_configure-$_t.txt" >&2
            exit 2
        }
    fi
    if [[ "$_t" == qloop ]]; then
        printf '%s\n' gate.qloop > "$OUT/qloop-selected.txt"
    else
        select_tests "$_t" "${TDIR[$_t]}" > "$OUT/$_t-selected.txt" || {
            echo "verify.sh: cannot list ${TDIR[$_t]}'s tests (ctest --show-only)" >&2; exit 2; }
        [[ -s "$OUT/$_t-selected.txt" ]] || {
            echo "verify.sh: the $_t tree registers none of the selected tests --" \
                 "an empty selection is not a pass" >&2; exit 2; }
        # --apps was checked against the SUITE's list; the selection is the
        # TREE's registration, made at configure time (a binary added to
        # TEST_APPS does not trigger one). A name the tree lacks would leave
        # unit.inventory alone, "0 unit binaries passed", green.
        # Only a tree whose UNIT job runs: gates/imggates bring the aa64 tree in
        # for its gates alone, where no unit test is selected.
        if [[ -n "$APPS" && "$_t" != qloop ]] && { { want x64 && [[ $_t == x64 ]]; } || { want aa64 && [[ $_t == aa64 ]]; }; }; then
            _missing=()
            for _a in $APPS_SPACED; do
                grep -qx "unit.$_a" "$OUT/$_t-selected.txt" || _missing+=("$_a")
            done
            (( ${#_missing[@]} == 0 )) || {
                echo "verify.sh: --apps names ${_missing[*]}, not registered in ${TDIR[$_t]}" \
                     "(a binary added since the tree was configured? reconfigure:" \
                     "cmake --preset $_preset)" >&2
                exit 2
            }
        fi
    fi
done

if [[ $PRINT_CTEST -eq 1 ]]; then
    for _t in "${RUNS[@]}"; do
        tree_env "$_t"; tree_argv "$_t" "${TDIR[$_t]}" "${TJOBS[$_t]}"
        printf "$_t-ctest-env: %s\n" "${TENV[@]}"
        sed "s/^/$_t-ctest-test: /" "$OUT/$_t-selected.txt"
        printf "$_t-ctest:"; printf ' %q' "${TARGV[@]}"; echo
    done
    rm -rf "$OUT"
    exit 0
fi

# Signals take every running ctest down, session and all (cts_take_down):
# ownership asked first, then the kill, then -- for a tree whose run was this
# script's -- its guests reaped and its temp base and owner record removed,
# as the cleanup fixture would have.
declare -A QBASE=()
_on_signal() {
    local sig="$1"
    cts_take_down
    echo "verify.sh: interrupted (SIG$sig) -- logs: $OUT" >&2
    case "$sig" in INT) exit 130 ;; HUP) exit 129 ;; *) exit 143 ;; esac
}
trap '_on_signal INT' INT
trap '_on_signal TERM' TERM
trap '_on_signal HUP' HUP
trap 'cts_kill_all; _verify_cleanup' EXIT

_verify_started=1   # past every refusal: from here a failing run KEEPS $OUT
declare -A WALL=()
start_run() {   # TREE-NAME
    tree_env "$1"; tree_argv "$1" "${TDIR[$1]}" "${TJOBS[$1]}"
    local foreign=1   # unreadable: never remove what we cannot name
    if cts_tree_base "${TDIR[$1]}" TENV; then
        QBASE[$1]="$CTS_BASE"; foreign="$CTS_BASE_FOREIGN"
    fi
    cts_own "$1" "${TDIR[$1]}/integration-logs/.run-owner" "${QBASE[$1]:-}" "$foreign"
    WALL[$1]=$SECONDS
    cts_start "$1" "$OUT/$1-ctest.txt" TENV TARGV
}
echo "verify: running ${RUNS[*]} ($(for _t in "${RUNS[@]}"; do printf '%s ' "$_t=$(grep -c . "$OUT/$_t-selected.txt")"; done)tests)"
declare -A RC=()
for _t in x64 aa64; do [[ " ${RUNS[*]} " == *" $_t "* ]] && start_run "$_t"; done
# Each run's wall ends when THAT run ends, whichever finishes first.
while (( ${#CTS_PID[@]} )); do
    cts_wait_any; _rc=$?
    [[ -n "$CTS_DONE" ]] || break
    RC[$CTS_DONE]=$_rc; WALL[$CTS_DONE]=$(( SECONDS - WALL[$CTS_DONE] ))
done
if [[ " ${RUNS[*]} " == *" qloop "* ]]; then
    touch "$OUT/.qloop-start"; sleep 1   # the verdict file must be NEWER
    start_run qloop; cts_wait qloop; RC[qloop]=$?; WALL[qloop]=$(( SECONDS - WALL[qloop] ))
fi
for _t in "${RUNS[@]}"; do
    [[ -n "${RC[$_t]:-}" ]] || WALL[$_t]="?"
    echo "verify: $_t ctest exit ${RC[$_t]:-? (not collected)}, ${WALL[$_t]}s wall"
    # A guest that outlived its test, reaped -- only if THIS run's own setup
    # fixture ran (another run's base sits at the same path).
    cts_reap_after "$OUT/$_t.xml" "${QBASE[$_t]:-}"
done

# The table and the verdict. qloop's verdict counts only if THIS run wrote it.
_qv="$TREE_X64/qloop-logs/qloop-verdict.txt"
declare -a REP=(--jobs "$(IFS=,; echo "${JOBS[*]}")" --logs "$OUT" --known-apps "${#KNOWN_APPS[@]}")
[[ -n "$APPS" ]] && REP+=(--apps "$APPS")
for _t in "${RUNS[@]}"; do REP+=("--$_t" "$OUT/$_t.xml" --ctest-rc "$_t=${RC[$_t]:-?}"); done
[[ -f "$_qv" && "$_qv" -nt "$OUT/.qloop-start" ]] && REP+=(--qloop-verdict "$_qv")
python3 "$REPO/scripts/ctest-report.py" verify "${REP[@]}"
rc=$?
if (( rc == 2 )); then
    echo "verify.sh: a run left no report it could read -- see $OUT" >&2
    rc=1
fi
# A caller whose log is all anyone will see (CI: release-suite.sh) asks for
# the failing tests' output inline; otherwise it stays in $OUT, kept.
if (( rc != 0 )) && [[ "${AXL_VERIFY_PRINT_FAILURES:-0}" == 1 ]]; then
    for _t in "${RUNS[@]}"; do
        echo "=== verify: the $_t run's ctest output (with each failing test's) ==="
        cat "$OUT/$_t-ctest.txt" 2>/dev/null || echo "(no output file: $OUT/$_t-ctest.txt)"
    done
fi
# Green and not asked to keep them: the logs go. Kept, they are named.
if (( rc == 0 )); then
    if [[ "${AXL_VERIFY_KEEP:-0}" != 1 ]]; then
        _verify_started=0
    else
        echo "logs: $OUT (kept: AXL_VERIFY_KEEP=1)"
    fi
fi
exit "$rc"
