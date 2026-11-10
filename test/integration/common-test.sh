#!/bin/bash
# Shared test infrastructure for UdkLib QEMU-based tests.
# Sources common.sh for QEMU/firmware discovery, provides lightweight
# disk creation (FAT32, no GPT, no sudo) and QEMU run helpers.
#
# Usage in test scripts:
#   source "$(dirname "$0")/common-test.sh"
#   test_parse_args "$@"
#   test_setup
#   test_add_efi "$BUILD_DIR/MyApp.efi"
#   test_set_startup "MyApp.efi"
#   test_build_image
#   test_run_foreground 20    # timeout in seconds
#   test_count_results        # parse PASS/FAIL from serial log

set -euo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$TESTS_DIR")")"
source "$PROJECT_DIR/scripts/axl-common.sh"
# Inert unless AXL_TEST_PROFILE is set. See lib/profile.sh for what it records
# and why the unit is a guest BOOT rather than a test.
# shellcheck source=lib/profile.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/profile.sh"
# Inert unless AXL_TEST_CACHE names a directory. See lib/test-cache.sh.
# shellcheck source=lib/test-cache.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/test-cache.sh"

# ---------------------------------------------------------------------------
# Wall-time accounting, ALWAYS ON: a `TIMING: guest` line as each guest run
# ends and one `TIMING: test=` summary as test_cleanup begins. The formats and
# every field are defined once, in the "Wall-time accounting" block of
# scripts/axl-common.sh. Cheap by construction: a clock read is
# $EPOCHREALTIME (no fork), a booted foreground run's boot reading is one grep
# of its ts-stamped log after it ends, and a background run gets one watcher
# subshell (axl_timing_watch) -- nothing forks in a loop.
#
# The opt-in AXL_TEST_PROFILE records (lib/profile.sh) are kept, not folded
# in: they carry the launch census (argv, staged-file digests), which costs a
# sha256 per staged file and has no business in an always-on path, and their
# per-boot durations are a second view the census report already reads.
#
# A run is OWNED by the shell that started it: a subshell's test_cleanup
# closes only its own, and only the test's main shell prints the summary, so
# a `( test_setup ... )` helper (test-standard-guest.sh) cannot double-count.
# ---------------------------------------------------------------------------
axl_timing_now _TM_T0
_TM_FIRST="" _TM_LAST_END=0 _TM_GUEST_US=0 _TM_BOOT_US=0
_TM_BOOTED=0 _TM_POOLED=0 _TM_DONE=0 _TM_N=0 _TM_CUR=""
_TM_KIND=() _TM_VIA=() _TM_START=() _TM_OWNER=() _TM_WATCH=() _TM_WPID=() _TM_LOG=() _TM_PID=()
# Run directly from a terminal, the lines reach it -- this script's and its
# children's (run-qemu.sh's), which otherwise write to a terminal or to this
# file only, never into a captured stderr.
if [[ -z "${AXL_TEST_TIMING_LOG:-}" && -t 2 ]]; then
    export AXL_TEST_TIMING_LOG="/proc/$$/fd/2"
fi

# _tm_begin KIND VIA -- a guest run starts now; its index in _TM_CUR.
_tm_begin() {
    local i=$_TM_N now
    _TM_N=$(( i + 1 ))
    axl_timing_now now
    _TM_KIND[i]=$1 _TM_VIA[i]=$2 _TM_START[i]=$now _TM_OWNER[i]=$BASHPID
    _TM_WATCH[i]="" _TM_WPID[i]="" _TM_LOG[i]="" _TM_PID[i]=""
    [[ -n "$_TM_FIRST" ]] || _TM_FIRST=$now
    _TM_CUR=$i
}

# _tm_ts_boot VAR LOG -- the boot reading from a `ts -s`-stamped log: the
# stamp on the banner's line, which counts from the guest's launch. Empty
# when the banner is absent.
_tm_ts_boot() {
    local l frac v=""
    l=$(grep -m1 -a -- "$AXL_TIMING_BOOT_MARK" "$2" 2>/dev/null) || l=""
    if [[ "$l" =~ ^\[([0-9]+)\.([0-9]+)\] ]]; then
        frac="${BASH_REMATCH[2]}000000"
        v=$(( 10#${BASH_REMATCH[1]} * 1000000 + 10#${frac:0:6} ))
    fi
    printf -v "$1" '%s' "$v"
}

# _tm_close I END RC -- run I ended at END (us) with status RC: its line,
# and its share of the summary. A background run's watcher knows better than
# the caller when its guest really ended (a guest that exited by itself long
# before the harness looked) and when it booted; a ts-stamped log knows a
# foreground run's boot.
_tm_close() {
    local i=$1 end=$2 rc=$3 e="" b="" boot="" span g bs="-"
    [[ -n "${_TM_START[i]:-}" ]] || return 0
    # Its watcher stopped and GONE before its files are read -- and before
    # anything (test_cleanup's rm -rf) removes the directory it writes into.
    [[ -n "${_TM_WPID[i]}" ]] && axl_timing_unwatch "${_TM_WPID[i]}"
    if [[ -n "${_TM_WATCH[i]}" ]]; then
        axl_timing_read e "${_TM_WATCH[i]}.end"
        [[ -n "$e" ]] && (( e < end )) && end=$e
        axl_timing_read b "${_TM_WATCH[i]}.boot"
        [[ -n "$b" ]] && boot=$(( b - _TM_START[i] ))
    elif [[ -n "${_TM_LOG[i]}" ]]; then
        _tm_ts_boot boot "${_TM_LOG[i]}"
    fi
    span=$(( end - _TM_START[i] ))
    (( span < 0 )) && span=0
    axl_timing_secs g "$span"
    if [[ -n "$boot" ]]; then
        (( boot > span )) && boot=$span
        (( boot < 0 )) && boot=0
        axl_timing_secs bs "$boot"
        _TM_BOOT_US=$(( _TM_BOOT_US + boot ))
    fi
    _TM_GUEST_US=$(( _TM_GUEST_US + span ))
    if [[ "${_TM_KIND[i]}" == booted ]]; then
        _TM_BOOTED=$(( _TM_BOOTED + 1 ))
    else
        _TM_POOLED=$(( _TM_POOLED + 1 ))
    fi
    (( end > _TM_LAST_END )) && _TM_LAST_END=$end
    axl_timing_emit "TIMING: guest kind=${_TM_KIND[i]} via=${_TM_VIA[i]} guest=$g boot=$bs rc=$rc"
    unset '_TM_START[i]'
    return 0
}

# _tm_close_now I RC -- _tm_close at this instant.
_tm_close_now() {
    local now
    axl_timing_now now
    _tm_close "$1" "$now" "$2"
}

# _tm_close_open VIA_OR_PID RC -- close every open run of this shell whose
# pid (a booted background guest) or via matches.
_tm_close_open() {
    local i now
    axl_timing_now now
    for (( i = 0; i < _TM_N; i++ )); do
        [[ -n "${_TM_START[i]:-}" && "${_TM_OWNER[i]}" == "$BASHPID" ]] || continue
        [[ "${_TM_PID[i]}" == "$1" || "${_TM_KIND[i]}/${_TM_VIA[i]}" == "$1" ]] || continue
        _tm_close "$i" "$now" "$2"
    done
    return 0
}

# _tm_finish -- test_cleanup's first act: every run still open closed now
# (a background guest as `-`, the cleanup ends it; a foreground one as
# `unfinished`, the test died mid-run), then -- main shell, once -- the
# summary. A test calling test_cleanup itself before the trap does gets one.
_tm_finish() {
    local i now rc total setup="-" between="-" post="-" g b start="-" lease="-" span
    axl_timing_now now
    for (( i = 0; i < _TM_N; i++ )); do
        [[ -n "${_TM_START[i]:-}" && "${_TM_OWNER[i]}" == "$BASHPID" ]] || continue
        rc=unfinished
        [[ "${_TM_VIA[i]}" == bg ]] && rc=-
        _tm_close "$i" "$now" "$rc"
    done
    [[ "$BASHPID" == "$$" && "$_TM_DONE" == 0 ]] || return 0
    _TM_DONE=1
    axl_timing_secs total $(( now - _TM_T0 ))
    if [[ -n "$_TM_FIRST" ]]; then
        axl_timing_secs setup $(( _TM_FIRST - _TM_T0 ))
        axl_timing_secs post $(( now - _TM_LAST_END ))
        span=$(( _TM_LAST_END - _TM_FIRST - _TM_GUEST_US ))
        axl_timing_secs between "$span"
    fi
    axl_timing_secs g "$_TM_GUEST_US"
    axl_timing_secs b "$_TM_BOOT_US"
    if _test_pooled; then
        start="${AXL_POOL_START:--}" lease="${AXL_POOL_LEASE_MS:--}"
    fi
    axl_timing_emit "TIMING: test=${0##*/} total=$total setup=$setup guest=$g boot=$b between=$between post=$post booted=$_TM_BOOTED pooled=$_TM_POOLED start=$start lease=$lease"
    return 0
}

# test_timing_begin / test_timing_end [RC] -- for a test that launches a guest
# ITSELF rather than through test_run_foreground/test_run_background (a TCG
# crash guest, lib/stage-lib.sh's boots): bracket the launch so the run is
# counted (`via=direct`); RC defaults to `-`. test_timing_watch PID LOG, after
# a backgrounded launch, adds the boot reading from LOG (the guest's serial
# file) and the guest's real end; a launch under `timeout` in the foreground
# has no pid to watch, so its boot reads `-`.
test_timing_begin() {
    _tm_begin booted direct
    TEST_TIMING_RUN=$_TM_CUR
}
test_timing_watch() {
    [[ -n "${TEST_TIMING_RUN:-}" ]] || return 0
    _TM_WATCH[TEST_TIMING_RUN]="$TEST_TMPDIR/.timing.$TEST_TIMING_RUN"
    axl_timing_watch "$1" "$2" "${_TM_WATCH[TEST_TIMING_RUN]}"
    _TM_WPID[TEST_TIMING_RUN]=$AXL_TIMING_WATCH_PID
}
test_timing_end() {
    [[ -n "${TEST_TIMING_RUN:-}" ]] || return 0
    _tm_close_now "$TEST_TIMING_RUN" "${1:--}"
    TEST_TIMING_RUN=""
}

# ---------------------------------------------------------------------------
# Pooled mode (docs/AXL-Test-Guest-Pool-Design.md §6). Under `axlpool run`
# (AXL_POOL_AGENT set) a test does not boot: it runs its checked-in
# <TestName>.nsh on a leased, already-booted guest through the guest's agent.
# The same test runs unchanged without a pool, booting as it always did --
# test_use_nsh, test_build_image and test_run_foreground do the right thing
# in both. What a pooled guest CANNOT give a test -- another NIC, a device,
# typed input -- refuses loudly here (_test_unpooled) rather than being
# silently skipped: such a test must not declare a guest class.
# ---------------------------------------------------------------------------

# The guest's QEMU command line, built by test_build_qemu_cmd. Declared so a
# check of it under `set -u` holds before (or without) that call.
TEST_QEMU_CMD=()
TEST_POOL_FWDS=()   # test_add_port_forward's pairs on a pooled guest (HOST:GUEST)
# The file holding the firmware's DEBUG output (ASSERTs included) for the last
# run -- an AARCH64 guest's debug UART, an X64 guest's 0x402 debugcon
# (qemu_fw_debug_uart in axl-common.sh); neither is the console, so none of it
# is in TEST_LOG. Booted, the channel's own log; pooled, what the leased
# guest's wrote during the run (every pooled guest has one). Empty on a booted
# X64 guest unless the test asked for the debugcon (TEST_QEMU_DEBUGCON): a
# booted X64 run's default has no port.
TEST_FW_DEBUG_LOG=""

_test_pooled() {
    [[ -n "${AXL_POOL_AGENT:-}" ]]
}

# A POOL failure inside a pooled test: not the test's verdict. Marked
# exactly as the wrapper marks its own (POOL_MISS_MARKER in axlpool/
# service.py) so run-integration.sh reruns the test booted, and exit 77.
_test_pool_miss() {   # _test_pool_miss REASON
    echo "  pool: $1" >&2
    echo "AXLPOOL-MISS: $(basename "$0"): $1" >&2
    exit 77
}

_test_unpooled() {   # _test_unpooled WHAT -- refuse WHAT on a pooled guest
    if _test_pooled; then
        echo "  FAIL: $(basename "$0"): $1 is not available on a pooled guest" \
             "(its machine is fixed); this test cannot declare guest=" >&2
        exit 1
    fi
}

# One Shell line through the leased guest's agent (axlpool exec): its output
# on stdout, its exit 0 / 1 (failed) / 124 (timed out) / 77 (pool failure).
_test_pool_exec() {   # _test_pool_exec TIMEOUT LINE [--console]
    # exec: python IS the subshell, so a signal to it (a background run's
    # cleanup) reaches the client, not a shell that would leave it running.
    (cd "$PROJECT_DIR/test/pool" && exec python3 -m axlpool exec --timeout "$1" ${3:-} -- "$2")
}

# The leased guest back to its clean snapshot, the lease kept (axlpool
# restore; AXL_POOL_CLI replaces the command line, as for run-qemu.sh).
# _test_pool_restore_fresh: the leased guest restored for this test's next
# run, the console streamed from here on.
TEST_POOL_FRESH=""   # restored since the last run (test_build_image did it)
_test_pool_restore_fresh() {
    _test_pool_restore || _test_pool_miss "could not restore the guest after an earlier run (exit $?)"
    # The pool's restore drops every host forward but the agent's (pool.py
    # restore): the next run adds its own again.
    TEST_POOL_FWDS_ADDED=" "
    TEST_POOL_SERIAL_OFFSET=$(stat -c %s "$AXL_POOL_SERIAL" 2>/dev/null || echo 0)
    TEST_POOL_FRESH=1
}
_test_pool_restore() {
    if [[ -n "${AXL_POOL_CLI:-}" ]]; then
        "$AXL_POOL_CLI" restore > /dev/null 2>&1
    else
        (cd "$PROJECT_DIR/test/pool" && exec python3 -m axlpool restore) > /dev/null 2>&1
    fi
}
# Where this test's console stream starts: the lease's start, or the restore
# _test_pool_enter made after a pooled run-qemu.sh run.
TEST_POOL_SERIAL_OFFSET=""
TEST_POOL_LINE=""   # the line a pooled run gives the agent (_test_pool_enter)
TEST_POOL_NO_SCRIPT="AXL-POOL-NO-SCRIPT"   # the wrapper's marker: no test script
# Set once a pooled run of this test has started (a shell variable, not a
# file: nothing of the harness's may show in the guest's directory): the next
# run restores the guest first, and a rebuilt image replaces the share's files
# -- each run a fresh machine and disk, as each boot is.
TEST_POOL_RAN=""

# The build tree for this run's arch — ASKED OF THE BUILD, never composed here.
#
# 71 scripts used to spell out "$PROJECT_DIR/out/native-$arch" themselves,
# which is correct only while PREFIX depends on nothing but ARCH. It already
# depends on BUILD — so runs stop wiping the
# non-TLS objects and vice versa — would have silently pointed every one of
# those scripts at a tree nothing had built. The build owns the answer;
# this asks it. `scripts/axl-build.sh --print-prefix` honours ARCH and BUILD
# together, so this stays right through any future split.
#
# Memoized per arch: 71 scripts times several calls times a fork is
# measurable, and the answer cannot change inside one run.
# $1 (optional) names the arch when it is NOT this run's TEST_ARCH — a script
# that deliberately builds the other one, or that has already mapped TEST_ARCH
# to a build arch of its own. Accepts either spelling (X64/x64,
# AARCH64/aa64) because the scripts use both.
test_build_prefix() {
    local arch="${1:-${TEST_ARCH:-X64}}"
    case "$arch" in
        AARCH64|aarch64|aa64) arch=aa64 ;;
        *)                    arch=x64  ;;
    esac
    # Key the memo on the TLS state too, not just the arch: a caller may ask
    # for a differently-keyed prefix in a run whose
    # default is non-TLS, and an arch-only key would hand back the cached
    # non-TLS answer -- the exact class of bug this helper exists to prevent.
    local var="_TEST_BUILD_PREFIX_${arch}"
    if [[ -z "${!var:-}" ]]; then
        # Delegates rather than re-asking the build itself:
        # scripts/build-prefix.sh is the one definition, since 51 integration scripts source nothing
        # and can only call it as a command. This adds the memo on top.
        printf -v "$var" '%s' "$("$PROJECT_DIR/scripts/build-prefix.sh" "$arch")"
    fi
    printf '%s\n' "${!var}"
}

# Absolute form. The relative one above is what a build TARGET wants (targets
# are relative to PROJECT_DIR); this one is what a file test or a QEMU
# staging path wants. Both come from the same answer.
test_build_dir() {
    printf '%s/%s\n' "$PROJECT_DIR" "$(test_build_prefix "$@")"
}

# The STAGED SDK -- a different question from test_build_dir, with different
# inputs, which is why it is a separate helper rather than a mode of that one.
#
#   test_build_dir   build/<arch>-<debug|release>        objects and images,
#                    varies with ARCH x BUILD
#   test_sdk_dir     stage/{bin,lib,include,share}       what install.sh
#                    produces and a consumer consumes; varies with nothing
#
# Both once lived under out/ and both were called "the prefix", which is how
# AXL-Distribution-Design.md's P2 (separate the two) came to look like the same
# 149-file sweep as the CMake port's slice 3. Measured, the real overlap is
# seven files; P2's own surface is the handful of callers that ask this.
#
# No memo: scripts/sdk-prefix.sh forks no build -- it reads one environment
# variable -- so the cost test_build_prefix memoizes away does not exist here.
test_sdk_dir() {
    "$PROJECT_DIR/scripts/sdk-prefix.sh" --abs
}

# A block here used to warn when AXL_TLS was about to be toggled: the suite
# exported it, a hand-run test did not, and the Makefile's state-change rule
# then wiped every object -- surfacing as "driver build failed", which reads
# like a code error rather than a flag toggle. It cost an hour to diagnose
# once. There is no toggle now, and the .axl-tls-state file it read is gone.

# State variables (set by helpers)
TEST_ARCH="X64"
TEST_TMPDIR=""
TEST_STAGING=""
TEST_DISK=""
TEST_LOG=""
TEST_CLEAN_LOG=""
# How the last foreground guest ended, and under what limit. Set by
# test_run_foreground; read by test_count_results to tell a truncated log from
# a complete one, and -- the part that took a review to get right -- to tell
# the harness's OWN timeout from something else killing the guest. Both zero
# also covers "no foreground run happened", which is the honest default: not
# knowing is not the same as knowing it did not fire, and the message keyed
# off them claims nothing either way.
TEST_RUN_TIMED_OUT=0
TEST_RUN_KILLED_SIG=0
TEST_RUN_LIMIT=""
TEST_NVRAM=""
TEST_BOOT_NAME=""
TEST_QEMU_PID=0
TEST_GUEST_START_NS=""   # test_run_foreground: guest launch / exit, date +%s%N
TEST_GUEST_END_NS=""
TEST_SCHED_FILE=""       # host_load_sampler output: "<sum> <samples>" of procs_running
TEST_SCHED_PID=""
TEST_ECHO_PID=""
TEST_ECHO_PORT=""
TEST_UDP_ECHO_PID=""
TEST_SINK_PID=""
TEST_SINK_PORT=""
TEST_CPU_SPIKE=0

# ---------------------------------------------------------------------------
# Arg parsing
# ---------------------------------------------------------------------------

test_parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --arch) TEST_ARCH="$2"; shift 2 ;;
            *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
        esac
    done
}

# ---------------------------------------------------------------------------
# Setup / teardown
# ---------------------------------------------------------------------------

# A KILL IS NOT A VERDICT, and a killed test must say which one happened.
#
# The runner stops a test with `timeout`, which sends SIGTERM. This file used to
# trap TERM with the cleanup and nothing else -- and a bash trap handler that
# does not exit RESUMES the script where it was interrupted. So a test killed
# while polling for its guest's last line ran its cleanup (deleting the serial
# log), then went on polling the DELETED log until its own wait expired, and
# reported that the line never came. CI run 35348243557 read, verbatim:
#
#   PASS: the event loop is still dispatching (heartbeat 81 -> 84 in 6s)
#   Terminated
#   FAIL: the event loop never reached its deadline — it is wedged
#
# The heartbeat was advancing right up to the kill: a runner time limit,
# reported as a firmware wedge, which sends the reader hunting a hang that does
# not exist. It also cost the time it claimed: each attempt ran on for the rest
# of its wait after the limit.
#
# So INT and TERM report the kill and then END the script through
# axl_die_by_signal (scripts/axl-common.sh): it runs the EXIT trap -- the
# test's cleanup -- itself, once, and then dies by the signal. It has to run
# it itself. This handler's first version re-raised the signal and trusted
# bash to run the EXIT trap on the way out, and for a signal trapped before
# any EXIT trap exists (as these are, at source time) bash does not: every
# runner kill skipped every cleanup. test-runner-selftest.sh pins both halves
# -- the message and the cleanup -- for a runner kill and a direct TERM.
#
# Installed HERE, at source time, so every test that sources this file gets it
# -- including one that later replaces the EXIT trap with its own cleanup:
# traps are per signal, so `trap X EXIT` leaves INT and TERM alone, and the
# handler reads whatever the EXIT trap is when the signal arrives. What
# defeats it is a test trapping INT or TERM itself: install your cleanup with
# `trap X EXIT` and let these two handlers call it.
#
# run-integration.sh exports AXL_TEST_TIMEOUT, its per-test limit, so the
# message can name it -- and can tell a TERM at that limit from one that
# arrived early, which the limit did not send. $SECONDS is this shell's age;
# `timeout` starts it at once, so at the limit it reads the limit or one less.
# A kill that lands while a test is waiting on a command in ANOTHER process
# group (an inner `timeout 60 ssh ...` makes its own) is handled only once that
# command returns, which is why the message says when it was handled.
_test_killed() {
    local sig="$1" limit="${AXL_TEST_TIMEOUT:-}" name
    # No $(basename): a subshell here is killed by a repeat of the signal
    # (axl_die_by_signal, scripts/axl-common.sh), which has not yet ignored it.
    name=${0##*/}
    echo ""
    if [[ "$sig" == TERM && "$limit" =~ ^[0-9]+$ \
          && "$SECONDS" -ge $(( limit - 2 )) ]]; then
        echo "KILLED: $name was killed by the runner at its ${limit}s time limit" \
             "(SIGTERM, handled ${SECONDS}s in)."
    elif [[ "$limit" =~ ^[0-9]+$ ]]; then
        echo "KILLED: $name received SIG$sig ${SECONDS}s in, before the runner's" \
             "${limit}s time limit -- stopped from outside, not by the limit."
    else
        echo "KILLED: $name received SIG$sig ${SECONDS}s in -- stopped from" \
             "outside (no runner time limit is known to this run)."
    fi
    echo "        Not a test verdict: the run ends here, so no check after this" \
         "point passed or failed."
    axl_die_by_signal "$sig"
}
trap '_test_killed TERM' TERM
trap '_test_killed INT' INT

test_setup() {
    TEST_START_TIME=$(date +%s%N)
    # Build the per-test scratch (a ~40 MB QEMU image + pflash vars) on a tmpfs
    # when available — under the parallel pool, concurrent root-fs image builds
    # are an I/O bottleneck that starves booting guests. When run-integration.sh
    # set AXL_QEMU_TMPDIR, nest under it so the whole run's scratch is cleaned
    # wholesale even if a SIGTERM skips our own trap below; else prefer /dev/shm.
    _tt_base="${AXL_QEMU_TMPDIR:-}"
    if [[ -z "$_tt_base" && -d /dev/shm && -w /dev/shm ]]; then _tt_base=/dev/shm; fi
    if [[ -n "$_tt_base" ]]; then
        # Tests make UNIX sockets in here; refuse a base that leaves no room
        # for one BEFORE making the dir (axl_test_tmpdir_check, axl-common.sh).
        axl_test_tmpdir_check "$_tt_base" || exit 1
        TEST_TMPDIR=$(mktemp -d -p "$_tt_base" axl-ctest.XXXXXXXX)
    else
        # No /dev/shm: a plain mktemp -d, under $TMPDIR. Same check, naming
        # the knob that controls THIS path.
        axl_test_tmpdir_check "${TMPDIR:-/tmp}" TMPDIR tmp.XXXXXXXXXX || exit 1
        TEST_TMPDIR=$(mktemp -d)
    fi
    TEST_STAGING="$TEST_TMPDIR/staging"
    TEST_DISK="$TEST_TMPDIR/test.img"
    TEST_LOG="$TEST_TMPDIR/serial.log"
    TEST_CLEAN_LOG="$TEST_TMPDIR/clean.log"
    TEST_NVRAM="$TEST_TMPDIR/vars.fd"
    TEST_CURL_ERR="$TEST_TMPDIR/curl.err"   # test_curl's stderr; see there
    TEST_BOOT_NAME=$(boot_efi_name "$TEST_ARCH")

    # Resolve QEMU and firmware. find_qemu's `export QEMU_DIR` happens
    # in the $() subshell, so it doesn't reach our parent shell —
    # re-derive it here so find_firmware below can locate QEMU-bundled
    # firmware ($QEMU_DIR/../share/qemu/edk2-*-code.fd) when applicable.
    TEST_QEMU_BIN=$(find_qemu "$TEST_ARCH") || { echo "QEMU not found for $TEST_ARCH"; exit 1; }
    QEMU_DIR="$(dirname "$TEST_QEMU_BIN")"
    export QEMU_DIR
    find_firmware "$TEST_ARCH" || { echo "Firmware not found for $TEST_ARCH"; exit 1; }
    # Truncation happens in run-integration.sh's run_one, NOT here: several
    # tests call test_setup more than once (test-sd-sibling drives three
    # images), and truncating per setup would discard the earlier images'
    # inputs. Tests driven purely through run-qemu.sh never reach this function
    # at all, so the runner is the only place that sees every test exactly once.
    cache_record_input "$(basename "$0")" "${FW_CODE:-}"
    cache_record_input "$(basename "$0")" "${FW_VARS:-}"

    # Copy NVRAM template
    cp "$FW_VARS" "$TEST_NVRAM"

    # CPU-spike policy — shared with run-qemu.sh via cpu_policy_init (axl-common.sh),
    # which picks the KVM/TCG-aware threshold (0.5), warm-up and warn carve-out (see
    # its comment for the measured rationale). TEST_CPU_* env vars override per run.
    CPU_WARN="${TEST_CPU_WARN:-}"
    CPU_THRESHOLD="${TEST_CPU_THRESHOLD:-}"
    cpu_policy_init "$TEST_ARCH"
    CPU_WARMUP="${TEST_CPU_WARMUP:-$CPU_WARMUP}"
    CPU_SUSTAIN="${TEST_CPU_SUSTAIN:-$CPU_SUSTAIN}"

    # Cleanup trap. EXIT alone, and that covers a `timeout` wrapper's SIGTERM:
    # the INT/TERM handlers above report the kill, run this EXIT trap
    # themselves, and end the script. Trapping TERM here as well is what made
    # a killed test resume after its cleanup -- see _test_killed.
    trap 'test_cleanup' EXIT

    if _test_pooled && [[ "${AXL_POOL_ARCH:-}" != "$TEST_ARCH" ]]; then
        _test_pool_miss "leased an ${AXL_POOL_ARCH:-?} guest for a $TEST_ARCH test"
    fi
}

test_cleanup() {
    # First statement: the status the script is exiting with.
    local _exit_rc=$?
    # The TIMING summary first: before anything below can hang.
    _tm_finish
    _test_report_curl_errors "$_exit_rc"
    # A pooled run killed mid-exec, or a background one ending: its exec
    # client (whose guest-side program keeps running until the release
    # restores the guest) and its stream, through the one stop.
    if _test_pooled && [[ -n "${TEST_POOL_BG_PID:-}${TEST_POOL_SENTINEL:-}" ]]; then
        test_guest_stop
    fi
    _test_pooled && _test_pool_dump_del
    if [[ -n "${TEST_SCHED_PID:-}" ]]; then
        kill "$TEST_SCHED_PID" 2>/dev/null || true
        wait "$TEST_SCHED_PID" 2>/dev/null || true
        TEST_SCHED_PID=""
    fi
    # The witness's sampler too: on a pooled run its target, the leased
    # guest's QEMU, outlives the test, so it would never end by itself.
    if [[ -n "${TEST_STARVE_PID:-}" ]]; then
        kill "$TEST_STARVE_PID" 2>/dev/null || true
        wait "$TEST_STARVE_PID" 2>/dev/null || true
        TEST_STARVE_PID=""
    fi
    if [[ $TEST_QEMU_PID -gt 0 ]]; then
        kill "$TEST_QEMU_PID" 2>/dev/null || true
        wait "$TEST_QEMU_PID" 2>/dev/null || true
    fi
    # Any guest of this test still running now was stopped mid-boot: a
    # FOREGROUND guest (test_run_foreground) runs under its own `timeout`,
    # which puts it in a process group of its own, so the runner's group kill
    # never reaches it -- a killed test left its guest running to that
    # timeout, on a deleted disk. The group kill takes the wrapper subshell
    # that launched it, so no PID is left to signal; the guest is found the
    # way _test_qemu_pid finds it, by this test's disk path (unique per
    # TEST_TMPDIR) and a qemu-system comm. Guarded: an empty path would match
    # every process on the box.
    if [[ -n "$TEST_DISK" ]]; then
        local _gp
        for _gp in $(pgrep -f -- "$TEST_DISK" 2>/dev/null); do
            if [[ "$(cat "/proc/$_gp/comm" 2>/dev/null)" == qemu-system* ]]; then
                kill -TERM "$_gp" 2>/dev/null || true
            fi
        done
    fi
    # Summarize the CPU sampler for background runs, where QEMU only dies
    # here. Foreground runs already summarized in test_run_foreground; the
    # call is a no-op once the monitor globals are cleared.
    test_cpu_check || true
    if [[ -n "${TEST_ECHO_PID:-}" ]] && kill -0 "$TEST_ECHO_PID" 2>/dev/null; then
        kill "$TEST_ECHO_PID" 2>/dev/null || true
        wait "$TEST_ECHO_PID" 2>/dev/null || true
    fi
    if [[ -n "${TEST_UDP_ECHO_PID:-}" ]] && kill -0 "$TEST_UDP_ECHO_PID" 2>/dev/null; then
        kill "$TEST_UDP_ECHO_PID" 2>/dev/null || true
        wait "$TEST_UDP_ECHO_PID" 2>/dev/null || true
    fi
    if [[ -n "${TEST_SINK_PID:-}" ]] && kill -0 "$TEST_SINK_PID" 2>/dev/null; then
        kill "$TEST_SINK_PID" 2>/dev/null || true
        wait "$TEST_SINK_PID" 2>/dev/null || true
    fi
    #
    # TEST_KEEP_LOG=<path> preserves the raw serial log outside the
    # tmpdir so post-mortem inspection is possible after the
    # auto-cleanup runs. Useful when hunting stalls.
    #
    if [[ -n "${TEST_KEEP_LOG:-}" && -f "$TEST_LOG" ]]; then
        cp -f "$TEST_LOG" "$TEST_KEEP_LOG" 2>/dev/null || true
    fi
    [[ -n "$TEST_TMPDIR" ]] && rm -rf "$TEST_TMPDIR"
}

# ---------------------------------------------------------------------------
# Staging helpers
# ---------------------------------------------------------------------------

# Add an EFI binary to the image root (or a subdirectory)
test_add_efi() {
    local src="$1"
    local dest="${2:-$(basename "$1")}"

    if [[ ! -f "$src" ]]; then
        echo "Not found: $src"
        # Point at the right goal based on the missing artifact so the
        # next command actually builds the missing thing.
        local hint="scripts/axl-build.sh${TEST_ARCH:+ ARCH=${TEST_ARCH,,}}"
        case "$src" in
            */tools/*.efi)  hint+=" tools" ;;
            */AxlTest*.efi) hint+=" tests" ;;
            *)              hint+=" all" ;;
        esac
        echo "Build first: $hint"
        exit 1
    fi

    mkdir -p "$TEST_STAGING/$(dirname "$dest")"
    cp "$src" "$TEST_STAGING/$dest"
    _prof_efi "$src" "$dest"
    cache_record_input "$(basename "$0")" "$src"
}

# test_record_input <file>... -- name a tree file this test feeds the guest
# by some route other than the staging helpers (a cp or sed into
# $TEST_STAGING, a host-side read), so the skip cache keys on it. test_add_efi
# records its own; a bare `cp "$PROJECT_DIR/tools/sshd.conf" ...` did not, and
# an edit to the shipped sshd.conf left the tests that check it reading green.
test_record_input() {
    local f
    for f in "$@"; do
        cache_record_input "$(basename "$0")" "$f"
    done
}

# Add NIC drivers for the current arch
test_add_drivers() {
    _test_unpooled "NIC drivers loaded at boot (test_add_drivers)"
    local driver_dir="$PROJECT_DIR/build/staging/drivers/$(arch_dir "$TEST_ARCH")"

    if [[ -d "$driver_dir" ]]; then
        mkdir -p "$TEST_STAGING/drivers"
        cp "$driver_dir"/*.efi "$TEST_STAGING/drivers/" 2>/dev/null || true
        local d
        for d in "$driver_dir"/*.efi; do
            [[ -f "$d" ]] || continue   # an empty dir leaves the glob literal
            test_record_input "$d"
        done
    fi
}

# Write startup.nsh content
test_set_startup() {
    _test_unpooled "an inline startup.nsh (check in <TestName>.nsh and call test_use_nsh)"
    cat > "$TEST_STAGING/startup.nsh"
}

# test_use_nsh [ARG...] -- this test's commands are its checked-in
# <TestName>.nsh, beside its .sh, run with ARGs (%1..%9) from the directory
# holding its files. Booting, a startup.nsh runs it from fs0:\ and then
# `reset -s`; pooled, test_run_foreground runs it through the agent from the
# lease's share directory. The script must therefore name its files
# relatively and must not `reset` or `exit` (the agent refuses `exit`).
test_use_nsh() {
    TEST_NSH="$(basename "$0" .sh).nsh"
    local src end=$'reset -s\n'
    # --keep-up: the guest keeps running after the script returns (a resident
    # driver serving the host) -- booted, the startup ends at the Shell
    # prompt instead of `reset -s`; pooled, the guest stays up anyway.
    if [[ "${1:-}" == --keep-up ]]; then
        end=""
        shift
    fi
    src="$(cd "$(dirname "$0")" && pwd)/$TEST_NSH"
    if [[ ! -f "$src" ]]; then
        echo "  FAIL: $(basename "$0"): no $TEST_NSH beside it" >&2
        exit 1
    fi
    TEST_NSH_LINE="$TEST_NSH"
    local a
    for a in "$@"; do
        case "$a" in
            --keep-up)
                echo "  FAIL: $(basename "$0"): test_use_nsh --keep-up must come first" >&2
                exit 1 ;;
            *[[:space:]\"^%#]*|"")
                echo "  FAIL: $(basename "$0"): test_use_nsh argument [$a] needs Shell quoting;" \
                     "pass it as a file instead" >&2
                exit 1 ;;
        esac
        TEST_NSH_LINE+=" $a"
    done
    mkdir -p "$TEST_STAGING"
    cp "$src" "$TEST_STAGING/$TEST_NSH"
    # The test's commands live here now, not in its .sh: an edit to them
    # must invalidate a cached green run (lib/test-cache.sh).
    cache_record_input "$(basename "$0")" "$src"
    if ! _test_pooled; then
        printf '@echo -off\nfs0:\ncd \\\n%s\n%s' "$TEST_NSH_LINE" "$end" > "$TEST_STAGING/startup.nsh"
    fi
}

# test_use_nsh_text NAME -- as test_use_nsh, for a script this test GENERATES
# (stdin) rather than checks in: test-axl.sh's list of binaries depends on
# TEST_APPS_ONLY. Written to the staging dir as NAME.nsh; booting, a
# startup.nsh runs it from fs0:\ and then `reset -s`; pooled, the agent runs
# it from the lease's share directory. What generated it -- the test's own
# script, or a lib/*.sh it sources (lib/ssh-hostile-lib.sh's hostile_boot) --
# is already a cache input (lib/test-cache.sh hashes every lib/*.sh).
test_use_nsh_text() {
    TEST_NSH="$1.nsh"
    TEST_NSH_LINE="$TEST_NSH"
    mkdir -p "$TEST_STAGING"
    cat > "$TEST_STAGING/$TEST_NSH"
    if ! _test_pooled; then
        printf '@echo -off\nfs0:\ncd \\\n%s\nreset -s\n' "$TEST_NSH_LINE" > "$TEST_STAGING/startup.nsh"
    fi
}

# ---------------------------------------------------------------------------
# Image creation
# ---------------------------------------------------------------------------

test_build_image() {
    if _test_pooled; then
        # No image: the lease's share directory IS the guest's disk. A test
        # that builds again (another test_setup phase) gets a new disk, as a
        # boot does: the earlier image's files go (_test_pool_enter restores
        # the guest before the next run).
        if [[ -n "$TEST_POOL_RAN" ]]; then
            # The guest first: the earlier run's program may still be writing
            # to the share until the restore ends it.
            _test_pool_restore_fresh
            find "${AXL_POOL_HOST_DIR:?}" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
        fi
        cp -r "$TEST_STAGING"/. "$AXL_POOL_HOST_DIR"/
        return 0
    fi
    # Stage Shell.efi as boot binary if available. This prevents PXE
    # boot delays when networking is enabled and ensures the NIC option
    # ROM loads the network stack drivers.
    local shell_efi
    shell_efi=$(find_shell_efi "$TEST_ARCH") || true
    if [[ -n "$shell_efi" && -f "$shell_efi" ]]; then
        # Boot the Shell via the AXL launcher ("-delay 0", skips the 5 s startup
        # countdown); see stage_boot_shell in axl-common.sh. No trailing policy
        # argument, so an unset AXL_SHELL_LAUNCHER means OFF here: this harness
        # does not inspect its own serial log for the startup sentinel, so it
        # cannot notice a firmware that never reaches the Shell, and must not
        # opt anyone into a boot it could not undo. run-integration.sh sets
        # AXL_SHELL_LAUNCHER=1 explicitly, so the suite still takes the win.
        stage_boot_shell "$TEST_STAGING" "$TEST_ARCH" "$TEST_BOOT_NAME" "$shell_efi" || exit 1
    fi

    # Shared with run-qemu.sh — see qemu_stage_disk in axl-common.sh.
    qemu_stage_disk "$TEST_STAGING" "$TEST_DISK" TEST || exit 1
}

# ---------------------------------------------------------------------------
# QEMU command construction
# ---------------------------------------------------------------------------

# Build base QEMU command array. Caller can append extra args.
# Sets TEST_QEMU_CMD array.
test_build_qemu_cmd() {
    if _test_pooled; then
        # What these ask for is the MACHINE: a pooled one cannot give it, and
        # silently not giving it would test the wrong thing (a read-only
        # check passing on a writable share).
        [[ "${TEST_DISK_READONLY:-0}" == 1 ]] && _test_unpooled "a read-only boot disk (TEST_DISK_READONLY)"
        [[ -n "${TEST_DISK2:-}" ]] && _test_unpooled "a second disk (TEST_DISK2)"
        [[ -n "${TEST_QEMU_GDB:-}" ]] && _test_unpooled "a GDB stub (TEST_QEMU_GDB)"
        # TEST_QEMU_DEBUGCON is served: every pooled guest has the channel
        # (_test_pool_stream_start streams the run's part into it).
        _test_fw_debug_log_set
        TEST_QEMU_CMD=()             # the leased guest is already running
        return 0
    fi
    _test_fw_debug_log_set
    mapfile -d '' -t TEST_QEMU_CMD < <(
        build_qemu_base_cmd "$TEST_ARCH" "$TEST_QEMU_BIN" 512M "$TEST_NVRAM" "" standard
        qemu_boot_wait_args
    )
    # TEST_DISK_READONLY=1 attaches the boot disk as a READ-ONLY USB stick
    # instead (below, once the USB controller exists): BMC virtual media is
    # often read-only, and a tool that writes beside itself fails there.
    if [[ "${TEST_DISK_READONLY:-0}" == 1 ]]; then
        TEST_QEMU_CMD+=(-drive "if=none,id=axl_stick,format=raw,readonly=on,file=$TEST_DISK")
    else
        TEST_QEMU_CMD+=(-drive "format=raw,file=$TEST_DISK")
    fi
    TEST_QEMU_CMD+=(
        -nographic
        -no-reboot
    )
    # The PCI tree and the USB controller (axl_usb0) came with the base
    # command: qemu_standard_devices in axl-common.sh says what leans on each.
    # The read-only stick on port 3, and TEST_DISK2 (an image the test made,
    # writable) on port 4: the Shell maps them in port order, so with both
    # the stick is fs0: and TEST_DISK2 is fs1:.
    if [[ "${TEST_DISK_READONLY:-0}" == 1 ]]; then
        TEST_QEMU_CMD+=(-device "usb-storage,bus=axl_usb0.0,port=3,drive=axl_stick,removable=on")
    fi
    if [[ -n "${TEST_DISK2:-}" ]]; then
        TEST_QEMU_CMD+=(
            -drive "if=none,id=axl_disk2,format=raw,file=$TEST_DISK2"
            -device "usb-storage,bus=axl_usb0.0,port=4,drive=axl_disk2"
        )
    fi
    # Debug hooks — opt-in via env vars so the normal test path is
    # untouched. TEST_QEMU_GDB=PORT exposes the QEMU GDB stub for
    # interactive debugging; TEST_QEMU_DEBUGCON=FILE captures the firmware's
    # DEBUG output (X64: OVMF's port 0x402, which scripts/gdb-syms.py needs to
    # recover module load addresses) -- _test_fw_debug_uart adds it at launch.
    if [[ -n "${TEST_QEMU_GDB:-}" ]]; then
        # TCG fallback for single-stepping — shared with run-qemu.sh.
        qemu_strip_kvm TEST_QEMU_CMD
        TEST_QEMU_CMD+=(-gdb "tcp::${TEST_QEMU_GDB}")
        log_info "QEMU GDB stub on tcp::${TEST_QEMU_GDB} (KVM disabled, TCG)"
    fi
    if [[ -n "${TEST_QEMU_DEBUGCON:-}" && "$TEST_ARCH" == X64 ]]; then
        log_info "OVMF debugcon → ${TEST_QEMU_DEBUGCON}"
    fi
}

# _test_fw_debug_log_set: TEST_FW_DEBUG_LOG for the guest test_build_qemu_cmd
# builds or leases -- the TEST_QEMU_DEBUGCON file when a test names one, else
# the test's temp dir; on a booted X64 guest only the former (its default has
# no debugcon: the port costs a VM exit per byte).
_test_fw_debug_log_set() {
    TEST_FW_DEBUG_LOG=""
    if [[ "$TEST_ARCH" == AARCH64 ]] || _test_pooled; then
        TEST_FW_DEBUG_LOG="${TEST_QEMU_DEBUGCON:-$TEST_TMPDIR/firmware-debug.log}"
    elif [[ "$TEST_ARCH" == X64 ]]; then
        TEST_FW_DEBUG_LOG="${TEST_QEMU_DEBUGCON:-}"
    fi
}

# _test_fw_debug_uart: the firmware's DEBUG channel on a booted guest's
# command (an AARCH64 guest's debug UART, an X64 guest's debugcon when
# TEST_QEMU_DEBUGCON asked for it), its log truncated -- each launch's DEBUG
# output alone, as each TEST_LOG is.
# Every launch of TEST_QEMU_CMD calls it beside _test_default_nic, after the
# test's own additions: whether the console is `mon:stdio` or `stdio` depends
# on a monitor option the test may have added. Only a guest
# test_build_qemu_cmd built (TEST_FW_DEBUG_LOG set): a hand-built command
# chose its machine whole.
_test_fw_debug_uart() {
    [[ -n "$TEST_FW_DEBUG_LOG" ]] || return 0
    : > "$TEST_FW_DEBUG_LOG"
    qemu_fw_debug_uart TEST_QEMU_CMD "$TEST_ARCH" "$TEST_FW_DEBUG_LOG"
}

# NIC device appropriate for the architecture. TEST_NIC_MODEL overrides it
# (a -device spec without netdev=, e.g. "virtio-net-pci,romfile=").
_test_nic_device() {
    if [[ -n "${TEST_NIC_MODEL:-}" ]]; then
        echo "$TEST_NIC_MODEL"
    elif [[ "$TEST_ARCH" == "AARCH64" ]]; then
        echo "virtio-net-pci"
    else
        echo "e1000"
    fi
}

# Host-port allocation. A test derives each host port it needs as
# `test_port <slot>` (slot 0, 1, 2, ...). The same value drives both the
# host-side server and the number baked into the guest's startup.nsh, so the
# two always agree within a single test run — which is why each slot is
# resolved ONCE and cached for the life of the test.
#
# Two modes:
#
#   - TEST_PORT_BASE set explicitly  -> `TEST_PORT_BASE + slot`, verbatim.
#     A caller that pins the base has a reason (reproducing a capture,
#     matching a firewall rule) and owns the consequences.
#
#   - TEST_PORT_BASE unset (the norm) -> the base is claimed HERE, at source
#     time, as a contiguous run of TEST_PORT_SLOTS ports that are verified
#     free at that moment and held for the test's lifetime. This is what
#     makes two INDEPENDENT suite runs safe: an arithmetic base only keeps
#     one invocation self-consistent, and every invocation starts from the
#     same constants.
#
# The claim is taken here, in the test's own shell, and NOT inside
# test_port. Call sites are all `HOST_PORT=$(test_port 0)`, and a command
# substitution runs in a subshell that exits immediately — a claim made
# there would be released the instant it was granted. Claiming at source
# time keeps every port held for as long as the test runs, and test_port
# stays the pure arithmetic every call site already expects.
TEST_PORT_SLOTS="${TEST_PORT_SLOTS:-4}"
if [[ -z "${TEST_PORT_BASE:-}" ]]; then
    axl_alloc_host_port TEST_PORT_BASE "$TEST_PORT_SLOTS" || exit 1
fi
test_port() {
    echo $(( TEST_PORT_BASE + ${1:-0} ))
}

# Add port forwarding: test_add_port_forward HOST GUEST [HOST GUEST ...]
#
# Every pair lands on the SAME netdev — call this once with all of them. A
# second call would emit a second `-netdev id=net0` and QEMU would reject the
# duplicate id, so a test needing two forwards passes four arguments rather
# than calling twice.
test_add_port_forward() {
    # Pooled: recorded, and added to the leased guest by test_run_background
    # (QMP hostfwd_add; the pool removes them at release -- a forward lives
    # outside the guest's snapshot). The guest's NIC is the pool's.
    if _test_pooled; then
        while [[ $# -ge 2 ]]; do
            TEST_POOL_FWDS+=("$1:$2")
            shift 2
        done
        return 0
    fi
    local fwd=""
    while [[ $# -ge 2 ]]; do
        fwd+=",hostfwd=tcp::${1}-:${2}"
        shift 2
    done
    TEST_QEMU_CMD+=(
        -device "$(_test_nic_device),netdev=net0"
        -netdev "user,id=net0${fwd}"
    )
}

# test_add_net_dump FILE -- capture the guest NIC's traffic (net0) as a pcap
# in FILE: QEMU's filter-dump. Booted, a -object from QEMU's start; pooled,
# the same filter added to the leased guest's net0 over QMP when the run
# starts and removed when it stops (_test_pool_dump_del) -- a filter lives
# outside the guest's snapshot, so one left behind would keep writing into
# the next lease. The pooled capture also holds the agent's own traffic
# (guest port 4747): read it by the test's address and port.
TEST_POOL_DUMP=""      # pooled: the capture file to attach
TEST_POOL_DUMP_ON=""   # pooled: the filter is on the leased guest now
test_add_net_dump() {
    if _test_pooled; then
        TEST_POOL_DUMP="$1"
        return 0
    fi
    TEST_QEMU_CMD+=(-object "filter-dump,id=axl_dump0,netdev=net0,file=$1")
}
_test_pool_dump_add() {
    local out
    [[ -n "$TEST_POOL_DUMP" && -z "$TEST_POOL_DUMP_ON" ]] || return 0
    # Interpolated into JSON: only what a temp path is.
    if ! [[ "$TEST_POOL_DUMP" =~ ^[A-Za-z0-9_./+-]+$ ]]; then
        echo "  FAIL: $(basename "$0"): test_add_net_dump path [$TEST_POOL_DUMP] needs JSON quoting" >&2
        exit 1
    fi
    # A capture a killed test left behind (its EXIT trap never ran, and the
    # pool's release does not remove QOM objects): gone first.
    _test_pool_qmp "$AXL_POOL_QMP" object-del '{"id": "axl_dump0"}' > /dev/null 2>&1 || true
    if ! out=$(_test_pool_qmp "$AXL_POOL_QMP" object-add \
                   "{\"qom-type\": \"filter-dump\", \"id\": \"axl_dump0\", \"netdev\": \"net0\", \"file\": \"$TEST_POOL_DUMP\"}" 2>&1); then
        _test_pool_miss "could not add the packet capture to the leased guest: $out"
    fi
    TEST_POOL_DUMP_ON=1
}
_test_pool_dump_del() {
    [[ -n "$TEST_POOL_DUMP_ON" ]] || return 0
    TEST_POOL_DUMP_ON=""
    _test_pool_qmp "$AXL_POOL_QMP" object-del '{"id": "axl_dump0"}' > /dev/null 2>&1 \
        || echo "  pool: could not remove the packet capture from the leased guest" >&2
}

# Add networking (QEMU user-mode, no port forwarding)
test_add_network() {
    _test_pooled && return 0   # a pooled guest has the standard network already
    TEST_QEMU_CMD+=(
        -device "$(_test_nic_device),netdev=net0"
        -netdev "user,id=net0"
    )
}

# A guest whose test chose no network gets the STANDARD NIC, stated, rather
# than whatever QEMU defaults to (an e1000e with its PXE ROM on q35). Tests do
# rely on there being one -- the 9p suites reach the host over it without
# asking -- so "none chosen" cannot mean none; it means the standard guest's.
# Every launch of TEST_QEMU_CMD calls it last (then _test_fw_debug_uart), so
# every earlier choice is seen: test_run_foreground, test_run_background and
# lib/stage-lib.sh.
#
# ONLY a standard guest (test_build_qemu_cmd's, recognised by its xHCI): a test
# that hand-builds TEST_QEMU_CMD chose its machine whole, and a NIC appended to
# it can make the QEMU fail at startup -- `-M none` has no PCI bus -- which
# would turn a "the guest is gone" assertion into a pass for the wrong reason.
_test_default_nic() {
    local a standard=false
    for a in ${TEST_QEMU_CMD[@]+"${TEST_QEMU_CMD[@]}"}; do
        case "$a" in
            -net|-netdev|-nic) return 0 ;;
            qemu-xhci,id=axl_usb0,*) standard=true ;;
        esac
    done
    [[ "$standard" == true ]] && test_add_network
    return 0
}

# Slirp subnet IP that the unit-test runner reserves as the "echo
# target." Tests connect to this IP (any port) and slirp's guestfwd
# rules redirect the byte stream to a host-side stream-echo server.
# Picked inside slirp's default 10.0.2.0/24 subnet but distinct from
# the guest's own DHCP'd 10.0.2.15.
#
# Slirp does NOT intercept connections to the guest's own IP — those
# packets bypass slirp's TCP routing — so a non-self IP is required.
TEST_ECHO_HOST="10.0.2.100"

# UDP echo port. Slirp's `guestfwd` is TCP-only (no UDP form), but
# UDP datagrams sent from the guest to 10.0.2.2 (the slirp gateway)
# are delivered to host loopback natively by slirp. We pin a fixed
# port so the guest-side test code can target it directly without
# needing to plumb a runtime-allocated port number into the guest.
TEST_UDP_ECHO_PORT=35555

# Add networking with a stream-echo backstop reachable at
# $TEST_ECHO_HOST:<any port>. Tests can do
# `axl_tcp_connect("10.0.2.100", port, ...)` and get a working remote
# peer that echoes every byte back, without any real intra-guest
# networking.
#
# This helper:
#   1. Starts a host-side TCP echo server (test/integration/echo-stream.py)
#      on 127.0.0.1:<TEST_ECHO_PORT> as a background process. The PID
#      is captured in TEST_ECHO_PID so the cleanup trap kills it on
#      exit.
#   2. Installs `guestfwd` rules that intercept guest connections to
#      $TEST_ECHO_HOST:<port> at the slirp layer and forward them as
#      TCP to 127.0.0.1:<TEST_ECHO_PORT>. Both directions of byte
#      traffic flow transparently.
#
# Usage: test_add_network_with_echo <port1> [port2] [sink:port3] ...
#
# A `sink:PORT` argument forwards that port to a second server that reads
# every byte and answers none (echo-stream.py --sink): a peer that never
# replies. A guestfwd target is a chardev QEMU connects ONCE, at start, so
# every guest connection to one port shares a single host connection --
# which is why a sink must be a port of its own, not a mode one guest
# connection could ask the echo server for.
#
# A booted run uses a host-side listener here. The `guestfwd ... cmd:` form
# (a program spawned per connection) works too -- the pool's `unit` class
# uses it (test/pool/classes.toml, measured) -- but this path keeps its own
# servers.
test_add_network_with_echo() {
    _test_unpooled "a guest network with an echo server (test_add_network_with_echo)"
    local echo_script
    echo_script="$(dirname "${BASH_SOURCE[0]}")/echo-stream.py"

    # Pick a free host port for the echo server. This used to bind port 0,
    # read the kernel's choice and close again — which leaves the port
    # unclaimed for the whole gap before echo-stream.py binds it, and draws
    # from the ephemeral range where an outbound connection can take it.
    # The allocator claims it and holds the claim until this shell exits.
    if [[ -z "${TEST_ECHO_PORT:-}" ]]; then
        axl_alloc_host_port TEST_ECHO_PORT || return 1
        export TEST_ECHO_PORT
    fi

    # Start the TCP echo server in the background and stash its PID
    # for test_cleanup. Wait briefly for the listener to come up.
    python3 "$echo_script" --port "$TEST_ECHO_PORT" \
        > "$TEST_TMPDIR/echo-stream.out" 2>&1 &
    TEST_ECHO_PID=$!

    local i
    for i in $(seq 1 50); do
        if (echo > /dev/tcp/127.0.0.1/"$TEST_ECHO_PORT") 2>/dev/null; then
            break
        fi
        sleep 0.05
    done

    # Start the UDP echo on the pinned port. No readiness probe — UDP
    # is connectionless; if bind failed we'd see test failures, not
    # hangs.
    python3 "$echo_script" --udp --port "$TEST_UDP_ECHO_PORT" \
        > "$TEST_TMPDIR/echo-udp.out" 2>&1 &
    TEST_UDP_ECHO_PID=$!

    # The sink server, only when a port asks for it.
    local port
    for port in "$@"; do
        [[ "$port" == sink:* ]] || continue
        if [[ -z "${TEST_SINK_PORT:-}" ]]; then
            axl_alloc_host_port TEST_SINK_PORT || return 1
            export TEST_SINK_PORT
        fi
        python3 "$echo_script" --sink --port "$TEST_SINK_PORT" \
            > "$TEST_TMPDIR/echo-sink.out" 2>&1 &
        TEST_SINK_PID=$!
        for i in $(seq 1 50); do
            if (echo > /dev/tcp/127.0.0.1/"$TEST_SINK_PORT") 2>/dev/null; then
                break
            fi
            sleep 0.05
        done
        break
    done

    # Build the netdev string — one guestfwd rule per requested port.
    local netdev="user,id=net0"
    for port in "$@"; do
        if [[ "$port" == sink:* ]]; then
            netdev+=",guestfwd=tcp:${TEST_ECHO_HOST}:${port#sink:}-tcp:127.0.0.1:${TEST_SINK_PORT}"
        else
            netdev+=",guestfwd=tcp:${TEST_ECHO_HOST}:${port}-tcp:127.0.0.1:${TEST_ECHO_PORT}"
        fi
    done
    TEST_QEMU_CMD+=(
        -device "$(_test_nic_device),netdev=net0"
        -netdev "$netdev"
    )
}

# Add -net none (no networking)
test_add_no_network() {
    # Pooled: the standard guest has a NIC (its agent's); a test that wants
    # none tolerates one it does not use.
    _test_pooled && return 0
    TEST_QEMU_CMD+=(-net none)
}

# The device adders below append to TEST_QEMU_CMD on a pooled guest too: the
# test then runs only on a class whose machine has exactly those devices
# (_test_pool_machine_check), and refuses on any other.

# Attach QEMU's IPMI BMC simulator via the ISA KCS interface at the
# canonical x86 ports 0xCA2 (data) / 0xCA3 (cmd). Only supported on
# x86 (isa-ipmi-kcs is ISA). Lets AxlIpmi's KCS transport exercise
# the real FSM end-to-end against the simulator, which replies with
# CC=0xC7 for malformed wire framing — catches KCS protocol bugs
# that mock-callback unit tests cannot see.
test_add_ipmi_bmc_sim_kcs() {
    if [[ "$TEST_ARCH" != "X64" ]]; then
        log_warning "test_add_ipmi_bmc_sim_kcs: ISA KCS is x86-only; skipping on $TEST_ARCH"
        return
    fi
    TEST_QEMU_CMD+=(
        -device "ipmi-bmc-sim,id=bmc0"
        -device "isa-ipmi-kcs,bmc=bmc0,ioport=0xca2"
    )
}

# Attach QEMU's IPMI BMC simulator via SMBus. Requires a guest-side
# EFI_I2C_MASTER_PROTOCOL provider (sdk/examples/smbus-hc-shim.c)
# because stock OVMF doesn't publish one. x86-only: the ICH9 SMBus
# controller is on q35, and there's no equivalent on the aa64 virt
# machine.
#
# ipmi-bmc-sim defaults slave_addr=0x20 (the IPMI-standard 8-bit BMC
# address), which QEMU writes verbatim into SMBIOS Type 38's
# I2CSlaveAddress byte. AxlSmbus shifts that right by 1 to get the
# 7-bit SMBus address (0x10) and hands it to the shim. For the
# request to land, smbus-ipmi's `address` must equal that 7-bit
# form. QEMU's smbus-ipmi defaults address=0 (SMBus general-call) —
# useless — so we pin it to 0x10 explicitly. A mismatch there would
# manifest as an SSIF session that opens but times out on every
# command; SmbusHcShim's boot-time SMBIOS Type 38 dump surfaces it.
test_add_ipmi_bmc_sim_ssif() {
    if [[ "$TEST_ARCH" != "X64" ]]; then
        log_warning "test_add_ipmi_bmc_sim_ssif: ICH9 SMBus is x86-only; skipping on $TEST_ARCH"
        return
    fi
    TEST_QEMU_CMD+=(
        -device "ipmi-bmc-sim,id=bmc0"
        -device "smbus-ipmi,bmc=bmc0,address=0x10"
    )
}

# Attach an SPD EEPROM image to the platform SMBus at <address> (default
# 0x50). Requires the locally-patched QEMU build (scripts/qemu-patches/
# 0001-smbus-eeprom-add-memdev-link.patch) which adds a memdev= link
# property to smbus-eeprom; stock 10.x rejects the memdev= argument.
# The blob must be at least SMBUS_EEPROM_SIZE bytes (256) — the device
# copies the first 256 bytes into init_data at realize.
#
# Usage: test_add_smbus_eeprom <blob-path> [address]
test_add_smbus_eeprom() {
    local blob="$1"
    local addr="${2:-0x50}"
    if [[ "$TEST_ARCH" != "X64" ]]; then
        log_warning "test_add_smbus_eeprom: SMBus EEPROM injection is x86-only; skipping on $TEST_ARCH"
        return
    fi
    if [[ ! -f "$blob" ]]; then
        log_error "test_add_smbus_eeprom: blob '$blob' not found"
        return 1
    fi
    local id="spd_${addr//0x/}"
    # memory-backend-file rounds `size=` up to the host page; the blob
    # must be at least one page or QEMU rejects "backing store too
    # small". gen-spd.py pads to 4096 bytes for this reason. The
    # smbus-eeprom device only consumes the first 256 bytes; padding
    # is harmless.
    TEST_QEMU_CMD+=(
        -object "memory-backend-file,id=$id,mem-path=$blob,size=4096,share=off,readonly=on"
        -device "smbus-eeprom,address=$addr,memdev=$id"
    )
}

# ---------------------------------------------------------------------------
# QEMU execution
# ---------------------------------------------------------------------------

# test_scale_timeout <seconds> -- sets TEST_SCALED_S, the wall-clock limit this
# harness applies to a wait for the guest: the AArch64 allowance, then the
# caller's declared TEST_TIMEOUT_SCALE. ONE definition for the foreground run
# limit and test_wait_for, so a test that hard-codes "60" for a boot does not
# stop at 60 s on aa64 under load (the known aa64 startup stall) while the
# harness's own limit is 180.
TEST_SCALED_S=0
test_scale_timeout() {
    local t="$1"
    #
    # AArch64 QEMU under TCG boots noticeably slower than x86_64 and
    # the per-suite QEMU startup cost dominates. Every extra test
    # binary added to the aa64 sweep widens the gap. +60 over the
    # x86_64 budget covers the current 10-binary matrix with
    # headroom; bump if we add more.
    #
    # A POOLED guest does not boot: none of that applies, and its commands
    # ran no slower than x86_64's (measured 2026-11-04: the same test 2.8 s
    # pooled on AARCH64, 2.7 s on X64). Kept, the +60 made a hang cost a
    # pooled aa64 run 75 s instead of 15.
    [[ "$TEST_ARCH" == "AARCH64" ]] && ! _test_pooled && t=$((t + 60))
    #
    # TEST_TIMEOUT_SCALE: set by a caller that KNOWS it is sharing the box with
    # other heavy work (verify.sh, running six jobs at once). This is a hang
    # detector in wall seconds, and beside those jobs the x64 suite's 75 s
    # became 115-124 s against this limit's 120 -- cut off, 11-14k assertions
    # short, reported as a failure of the suite. A hang is still a hang at 2x.
    # Unset (the normal case) changes nothing. It is declared rather than
    # measured because the limit has to be chosen BEFORE the run; the duration
    # budget, which can wait, is the one that measures.
    #
    # VALIDATED, because both ways of getting it wrong are worse than no scale:
    # 0 makes `timeout 0`, which GNU timeout reads as NO LIMIT (the hang
    # detector silently disappears), and 1.5 is an arithmetic syntax error that
    # aborts the script mid-run. Unset is 1; set-but-empty is refused too.
    local _scale="${TEST_TIMEOUT_SCALE-1}"
    if ! [[ "$_scale" =~ ^[1-9][0-9]*$ ]]; then
        echo "FAIL: TEST_TIMEOUT_SCALE='$_scale' is not a positive integer" \
             "(0 would disable the hang timeout); refusing to run." >&2
        exit 2
    fi
    TEST_SCALED_S=$((t * _scale))
}

# Run QEMU in foreground with timeout. Serial output goes to TEST_LOG.
#
# If `ts` (moreutils) is available every output line is prefixed with
# the elapsed seconds since QEMU start, which lets test_count_results
# compute per-binary wall-clock times and pinpoint the binary that
# owned any stall. Falls back to untimestamped output if `ts` isn't
# installed.
test_run_foreground() {
    if _test_pooled; then
        _test_run_pooled "${1:-20}"
        return 0
    fi
    _prof_boot_start
    _test_default_nic
    _test_fw_debug_uart
    _prof_launch foreground "${TEST_QEMU_CMD[@]}"
    local timeout_sec="${1:-20}"
    test_scale_timeout "$timeout_sec"
    timeout_sec=$TEST_SCALED_S
    test_check_budget_scale

    # Backgrounded so the CPU sampler can attach to QEMU while it runs. The
    # pipeline and the `|| true` (a non-zero guest exit is not a harness
    # failure) are preserved exactly.
    _tm_begin booted fg
    local _tm_i=$_TM_CUR
    if command -v ts &>/dev/null; then
        _TM_LOG[_tm_i]="$TEST_LOG"   # stamped: the boot reading comes from it
        ( timeout "$timeout_sec" "${TEST_QEMU_CMD[@]}" 2>&1 \
            | ts -s '[%.s]' > "$TEST_LOG" ) &
    else
        ( timeout "$timeout_sec" "${TEST_QEMU_CMD[@]}" > "$TEST_LOG" 2>&1 ) &
    fi
    local _wrapper=$!
    TEST_GUEST_START_NS=$(date +%s%N)
    local _gpid; _gpid=$(_test_qemu_pid)
    cpu_monitor_start "$_gpid" "$TEST_TMPDIR"
    # How oversubscribed the host was while the guest ran, for the wall-clock
    # budget (see test_timing_breakdown). Sampled off the guest's own pid so it
    # stops with it.
    TEST_SCHED_FILE=""
    TEST_SCHED_PID=""
    # Only when a budget exists to scale: the pool runs ~25 guests at once and
    # nothing else reads this, so an unconditional sampler is ~50 awk forks a
    # second spent on a number nobody uses.
    if [[ -n "$_gpid" && -n "${TEST_MAX_DURATION:-}" ]]; then
        TEST_SCHED_FILE="$TEST_TMPDIR/host-load.txt"
        host_load_sampler "$_gpid" "$TEST_SCHED_FILE" &
        TEST_SCHED_PID=$!
    fi
    # How long the guest's QEMU threads were kept from running, for a caller
    # that judges timing and asked (verify.sh's quiet slot sets
    # TEST_VCPU_STARVE_FILE): read per thread, so a thread that exits mid-run
    # cannot take the run's wait with it (vcpu_starve_sampler);
    # vcpu_starve_report reads it once the guest is gone. Opt-in, for the
    # same reason as the sampler above.
    TEST_STARVE_PID="" TEST_STARVE_WHY=""
    if [[ -n "$_gpid" && -n "${TEST_VCPU_STARVE_FILE:-}" ]]; then
        rm -f "$TEST_VCPU_STARVE_FILE"
        vcpu_starve_sampler "$_gpid" "$TEST_VCPU_STARVE_FILE" &
        TEST_STARVE_PID=$!
    fi
    #
    # CAPTURE THE STATUS. This was `|| true`, and throwing it away is what let
    # the stall detector accuse the wrong binary: `timeout` exits 124 when it
    # kills its child, and that is the ONLY authoritative
    # statement that the log below is truncated rather than complete. Without
    # it, test_count_results reads a header with no footer and cannot tell a
    # binary that hung from one that merely happened to be in flight when the
    # harness ran out of time -- and it said "it hung or faulted (likely an
    # infinite loop)" for both. It named AxlTestSsh four times under box load,
    # against a binary that runs in 1.1 s of a 120 s limit.
    #
    # `set -o pipefail` is in force, so the subshell's status is the pipeline's
    # rightmost failure -- `timeout`'s 124, not `ts`'s 0.
    #
    # 124 AND ONLY 124 IS THE HARNESS'S OWN LIMIT, for the harness AS WRITTEN.
    # An earlier version also treated 137 as "our timeout fired, with -k".
    # The exact truth, measured here (coreutils 9.5), took two passes to get
    # right and both wrong versions were committed:
    #
    #   timeout      1 <anything>                     -> 124
    #   timeout -k 1 1 sleep 10                       -> 124  (TERM sufficed)
    #   timeout -k 1 1 bash -c 'trap "" TERM; sleep'  -> 137  (-k had to KILL)
    #
    # So "137 under -k" is true only when the child ignores SIGTERM, and
    # "124 whether or not -k is given" is equally over-general. Neither
    # matters for THIS code: the harness passes no -k (checked -- there is
    # none in common-test.sh, run-qemu.sh or axl-common.sh), so our limit is
    # always 124. What reaches 128+N is something ELSE killing the guest,
    # which on the loaded box this fix is about means the OOM killer.
    # Blaming that on our limit would be the same confident-wrong-cause
    # defect, mirrored, so it gets its own branch.
    #
    # ADD -k AND THIS NEEDS REVISITING: a killed-by-our-own-kill-after would
    # then arrive as 137 and read as an external kill.
    #
    # Any other non-zero status is still ignored, deliberately: a non-zero
    # guest exit is not a harness failure, which is what the `|| true` was
    # there for.
    local _wrc=0
    wait "$_wrapper" 2>/dev/null || _wrc=$?
    TEST_GUEST_END_NS=$(date +%s%N)
    _tm_close_now "$_tm_i" "$_wrc"
    # The sampler ends by itself once the guest is gone, but a wait that depends
    # on that is unbounded if the guest pid is ever reused or unkillable: stop it.
    # Its last reading was written atomically (tmp + mv), so killing is safe.
    if [[ -n "$TEST_SCHED_PID" ]]; then
        kill "$TEST_SCHED_PID" 2>/dev/null || true
        wait "$TEST_SCHED_PID" 2>/dev/null || true   # 143: killed on purpose; set -e
        TEST_SCHED_PID=""
    fi
    _test_starve_stop
    TEST_RUN_TIMED_OUT=0
    TEST_RUN_KILLED_SIG=0
    TEST_RUN_LIMIT="$timeout_sec"
    if [[ "$_wrc" -eq 124 ]]; then
        TEST_RUN_TIMED_OUT=1
    elif [[ "$_wrc" -gt 128 && "$_wrc" -lt 192 ]]; then
        TEST_RUN_KILLED_SIG=$(( _wrc - 128 ))
    fi
    # A foreground boot starts and ends inside this one call, so it is timed
    # exactly -- no trap and no pairing across functions. The third launcher
    # found while instrumenting: hooking only test_run_background left every
    # test that boots this way reporting ZERO boots, which reads as a cheap
    # test rather than an unmeasured one.
    _prof_boot_end "foreground"
    test_cpu_check || true
    axl_report_hostfwd_failure "$TEST_LOG" "$(basename "$0")" || true

    _test_check_dxe_assert
    _test_fw_debug_on_exception
}

# _test_fw_debug_on_exception: a foreground run that hit a firmware exception
# gets the debug UART's log appended to TEST_LOG, under a marker line. On
# AARCH64 only the dump's header (AXL_FW_EXCEPTION_RE) is the
# console's; its registers and backtrace are DEBUG output, which a decoder and
# a reader of the failure need where they read the rest. ASSERTs have their
# own checks (_test_check_dxe_assert, test_check_firmware_asserts), run
# before this so an ASSERT is not read twice. A background run's TEST_LOG is
# live, and gets nothing appended: read TEST_FW_DEBUG_LOG.
_test_fw_debug_on_exception() {
    [[ -s "$TEST_FW_DEBUG_LOG" ]] || return 0
    grep -aqE "$AXL_FW_EXCEPTION_RE" "$TEST_LOG" 2>/dev/null || return 0
    { printf -- '--- firmware debug UART ---\n'; cat "$TEST_FW_DEBUG_LOG"; } >> "$TEST_LOG"
}

# A firmware DXE-core pool/heap ASSERT (e.g. a double-FreePool) is NEVER
# acceptable — the guest's allocator state is corrupt. DEBUG firmware prints
# "ASSERT [DxeCore] ... Pool.c(...)"; RELEASE just spins at 100% CPU. Fail
# the whole test here rather than let it be scored on other criteria: the
# driver start-failure double-free shipped precisely because a run that
# printed this line was still judged on later greps. Global, so every
# foreground QEMU test is covered, booted or pooled. The EXIT trap still
# runs cleanup.
#
# Read where the firmware prints it: the console (TEST_LOG) and its DEBUG
# channel (TEST_FW_DEBUG_LOG) -- an AARCH64 guest's debug UART, an X64
# guest's debugcon (every pooled guest's; a booted X64 one's when the test
# asked for it), which is where an ASSERT goes. A debug log that was asked for and not
# written fails too: a guard that cannot read its input is blind, not clean.
_test_check_dxe_assert() {
    local hits
    if [[ -n "$TEST_FW_DEBUG_LOG" && ! -r "$TEST_FW_DEBUG_LOG" ]]; then
        echo "*** FAIL: $(basename "$0"): the firmware debug log $TEST_FW_DEBUG_LOG" \
             "was not written, so a firmware ASSERT could not be seen" >&2
        exit 1
    fi
    hits=$(grep -ahE 'ASSERT \[DxeCore\]' "$TEST_LOG" ${TEST_FW_DEBUG_LOG:+"$TEST_FW_DEBUG_LOG"} \
               2>/dev/null | tr -d '\r' || true)
    if [[ -n "$hits" ]]; then
        echo "*** FAIL: firmware DXE-core ASSERT (pool/heap corruption) in" \
             "$(basename "$0"):" >&2
        head -3 <<< "$hits" | sed 's/^/    /' >&2
        exit 1
    fi
}

# _test_starve_stop -- end the vCPU-starvation sampler a run started (the
# quiet slot's witness, TEST_VCPU_STARVE_FILE) and print its figure, the one
# line scripts/qloop-verdict.sh reads; nothing when no witness was asked for.
# A witness asked for but never started says so: unmeasured is not 0 ms.
_test_starve_stop() {
    [[ -n "${TEST_VCPU_STARVE_FILE:-}" ]] || return 0
    if [[ -z "${TEST_STARVE_PID:-}" ]]; then
        echo "vCPU starvation: not measured (${TEST_STARVE_WHY:-no guest QEMU pid})"
        return 0
    fi
    kill "$TEST_STARVE_PID" 2>/dev/null || true
    wait "$TEST_STARVE_PID" 2>/dev/null || true
    TEST_STARVE_PID=""
    local _starve
    if _starve=$(vcpu_starve_report "$TEST_VCPU_STARVE_FILE"); then
        printf 'vCPU starvation: %s ms run delay, %s ms host steal (all CPUs)\n' $_starve
    else
        echo "vCPU starvation: not measured (no two comparable readings of the guest's QEMU threads)"
    fi
}

# The pooled runs' shared prologue. _test_pool_enter: the checks a pooled
# run makes (a .nsh, no QEMU arguments of the test's own, a lease directory
# it can name), the inputs it records, and the Shell moved into the lease's
# directory (the Shell's `cd` cannot change volume, so the volume first).
_test_pool_enter() {   # _test_pool_enter fg|bg
    local vol dir rc=0 _in
    if [[ -z "${TEST_NSH_LINE:-}" ]]; then
        echo "  FAIL: $(basename "$0"): pooled, but no test_use_nsh" >&2
        exit 1
    fi
    # Anything the test appended to the command line itself (a device, an
    # adder's) asked for a machine of its own: it runs only on a class whose
    # machine is exactly that.
    _test_pool_machine_check
    # Interpolated into Shell lines below: only what a lease dir can be.
    if ! [[ "${AXL_POOL_GUEST_DIR:-}" =~ ^[A-Za-z0-9]+:\\[A-Za-z0-9._-]+$ ]]; then
        _test_pool_miss "unexpected AXL_POOL_GUEST_DIR [${AXL_POOL_GUEST_DIR:-}]"
    fi
    # What ran this test is an input too: the agent image. The pool's CODE is
    # in a pooled key as the identity the SERVICE reports (AXL_POOL_CODE_ID,
    # lib/test-cache.sh): the tree's own test/pool/ is harness, but the
    # long-lived service may be running other code.
    if [[ -f "${AXL_POOL_AGENT_IMAGE:-}" ]]; then
        cache_record_input "$(basename "$0")" "$AXL_POOL_AGENT_IMAGE"
    fi
    vol="${AXL_POOL_GUEST_DIR%%:*}:"
    dir="\\${AXL_POOL_GUEST_DIR#*:\\}"
    # A pooled scripts/run-qemu.sh call ran on this lease first (its rq-<n>
    # directory is in the share): the guest is as that run left it -- its
    # variables, `path`, a loaded driver, maybe a line still running -- so
    # restore it, as run-qemu.sh does before each of its own runs, and stream
    # the console from here rather than from the lease's start.
    # So does an earlier run of this test's own (another phase, or the same
    # image run again): a boot would give this run a fresh machine -- unless
    # test_build_image restored it already, before replacing the files.
    if [[ -n "${TEST_POOL_BG_PID:-}" ]]; then
        _test_unpooled "a run while a background run is live"
    fi
    if [[ -z "$TEST_POOL_FRESH" ]] && { compgen -G "$AXL_POOL_HOST_DIR/rq-*" > /dev/null || [[ -n "$TEST_POOL_RAN" ]]; }; then
        _test_pool_restore_fresh
        rm -rf "$AXL_POOL_HOST_DIR"/rq-*
    fi
    TEST_POOL_FRESH=""
    TEST_POOL_RAN=1
    _test_pool_dump_add
    TEST_GUEST_START_NS=$(date +%s%N)
    _tm_begin pooled "$1"
    _test_pool_exec 30 "$vol" > /dev/null || rc=$?
    [[ $rc -eq 0 ]] && { _test_pool_exec 30 "cd $dir" > /dev/null || rc=$?; }
    if [[ $rc -ne 0 ]]; then
        _test_pool_miss "could not enter $AXL_POOL_GUEST_DIR (exit $rc)"
    fi
    # The agent runs THIS, not the test's line: a wrapper ending in `@cd .`
    # (the one silent command that sets %lasterror% to 0). The agent reads a
    # script whose last status is 0xE as "not run" -- it cannot tell that
    # from a script never found (include/axl/axl-shell.h, the 0xE row) -- so
    # a test script ending with a missing-file `type`, or an app returning
    # 14, was taken for a pool failure and the test quietly rerun booted. A
    # boot's startup.nsh discards the status anyway. What the wrapper hides,
    # the test's own script not being there, it says itself: the marker
    # _test_pool_check_ran reads turns that into the pool miss it is.
    printf '@echo -off\r\nif not exist %s then\r\n  echo %s\r\nendif\r\n%s\r\n@cd .\r\n' \
        "$TEST_NSH" "$TEST_POOL_NO_SCRIPT" "$TEST_NSH_LINE" > "$AXL_POOL_HOST_DIR/axl-pool-run.nsh"
    TEST_POOL_LINE="axl-pool-run.nsh"
}

# _test_pool_machine_check: a pooled test whose TEST_QEMU_CMD is not empty
# (test_build_qemu_cmd leaves it so on a lease) asked for the standard guest
# plus those arguments -- with no standard NIC when they bring their own, as
# _test_default_nic boots it. The lease's class must be that machine, as
# run-qemu.sh's pooled mode judges its own runs (_AXL_RQ_MACHINE_SIG=compare
# against AXL_POOL_CLASS_ARGS); any other lease refuses the test, loudly --
# it declared the wrong class.
_test_pool_machine_check() {
    local a why rc=0 what="its own QEMU arguments" nic=(--net --nic-model "$(_test_nic_device)")
    # Nothing added asks for the standard guest, which the standard class is
    # and the unit class extends (test-axl.sh's TCP peers and RAM, which no
    # test adds); on any other class it is checked like the rest.
    if (( ${#TEST_QEMU_CMD[@]} == 0 )); then
        case "${AXL_POOL_CLASS:-}" in standard|unit) return 0 ;; esac
        what="the standard guest"
    fi
    local -a m=(--arch "$TEST_ARCH" --bridges)
    for a in ${TEST_QEMU_CMD[@]+"${TEST_QEMU_CMD[@]}"}; do
        case "$a" in -net|-netdev|-nic) nic=() ;; esac
        m+=(--qemu-arg "$a")
    done
    why=$(_AXL_RQ_MACHINE_SIG=compare bash "$PROJECT_DIR/scripts/run-qemu.sh" \
              ${nic[@]+"${nic[@]}"} "${m[@]}") || rc=$?
    case $rc in
        0) return 0 ;;
        3) _test_unpooled "$what (a machine that is not the ${AXL_POOL_CLASS:-?} class's: $why)" ;;
        *) _test_unpooled "$what (the lease names no machine to compare them with)" ;;
    esac
}

# _test_pool_check_ran: a pooled run whose wrapper found no test script is a
# pool failure (the share did not give the guest the staged file), not the
# test's verdict.
_test_pool_check_ran() {
    # The stream stamps each line ("[<seconds>] "), and the guest ends it CRLF.
    # Read with escape sequences stripped: a terminal escape at the line's
    # start (an attribute reset) must not hide the marker. Process
    # substitution, not a pipe: under pipefail `grep -q` leaving early would
    # fail the pipeline exactly when it matched.
    if grep -aqE "^(\[[0-9.]+\] )?$TEST_POOL_NO_SCRIPT"$'\r'"?\$" \
            < <(axl_strip_escapes "$TEST_LOG" 2>/dev/null); then
        _test_pool_miss "the guest did not find $TEST_NSH in $AXL_POOL_GUEST_DIR"
    fi
}

# _test_pool_stream_start: the guest's serial log from the lease's offset,
# STREAMED through the same `ts -s` a booted run's QEMU output goes through
# (every line carries its real "[<seconds>] " stamp, as tests that match
# whole raw lines expect) into TEST_LOG, until _test_pool_stream_stop; the
# host-load sampler and the quiet slot's witness run beside it. tail follows
# until the sentinel ends, then drains and exits.
_test_pool_stream_start() {   # [bg]: no samplers -- a booted background run has none
    local sentinel fwoff=0
    # The guest's firmware DEBUG output, from here on, into
    # TEST_FW_DEBUG_LOG: its log (an AARCH64 guest's debug UART, an X64
    # guest's debugcon), beside its console in the guest's state dir
    # (scripts/run-qemu.sh --state-dir). One the lease does not have would
    # leave the ASSERT checks blind: a pool miss, not a pass.
    [[ -n "$TEST_FW_DEBUG_LOG" ]] || _test_fw_debug_log_set
    TEST_POOL_FW_SRC="$(dirname "$AXL_POOL_SERIAL")/firmware-debug.log"
    [[ -f "$TEST_POOL_FW_SRC" ]] \
        || _test_pool_miss "the leased $TEST_ARCH guest has no firmware debug log ($TEST_POOL_FW_SRC)"
    # Read now, not in the tail's own arguments: a background job expands
    # those after the fork, by when the run may have written.
    fwoff=$(stat -c %s "$TEST_POOL_FW_SRC" 2>/dev/null || echo 0)
    sleep 3600 & sentinel=$!
    TEST_POOL_SENTINEL=$sentinel
    # How oversubscribed the host was meanwhile, for the budget, as a booted
    # run samples it (test_run_foreground) -- off the sentinel, which lives
    # exactly as long as the run. Without it every pooled count file said
    # contention 1.00, the idle box's figure, under any load.
    TEST_SCHED_FILE=""
    TEST_SCHED_PID=""
    if [[ -n "${TEST_MAX_DURATION:-}" && "${1:-}" != bg ]]; then
        TEST_SCHED_FILE="$TEST_TMPDIR/host-load.txt"
        host_load_sampler "$sentinel" "$TEST_SCHED_FILE" &
        TEST_SCHED_PID=$!
    fi
    # The quiet slot's witness, as a booted run starts it -- off the leased
    # guest's own QEMU, which the pool names (AXL_POOL_QEMU_PID). That process
    # outlives the run, so the sampler is stopped below, not left to end. A
    # long-lived QEMU retires idle I/O workers mid-run: the per-thread reading
    # is what keeps that from reading as a negative wait.
    TEST_STARVE_PID="" TEST_STARVE_WHY=""
    [[ "${1:-}" == bg ]] && TEST_STARVE_WHY="a background run samples no witness"
    if [[ -n "${TEST_VCPU_STARVE_FILE:-}" && "${1:-}" != bg ]]; then
        if [[ "${AXL_POOL_QEMU_PID:-}" =~ ^[1-9][0-9]*$ ]]; then
            rm -f "$TEST_VCPU_STARVE_FILE"
            vcpu_starve_sampler "$AXL_POOL_QEMU_PID" "$TEST_VCPU_STARVE_FILE" &
            TEST_STARVE_PID=$!
        else
            TEST_STARVE_WHY="the pool named no guest QEMU pid (AXL_POOL_QEMU_PID='${AXL_POOL_QEMU_PID:-}')"
        fi
    fi
    if command -v ts &>/dev/null; then
        ( tail -c "+$(( ${TEST_POOL_SERIAL_OFFSET:-${AXL_POOL_SERIAL_OFFSET:-0}} + 1 ))" -s 0.05 --pid="$sentinel" -f "$AXL_POOL_SERIAL" \
            | ts -s '[%.s]' > "$TEST_LOG" ) &
    else
        ( tail -c "+$(( ${TEST_POOL_SERIAL_OFFSET:-${AXL_POOL_SERIAL_OFFSET:-0}} + 1 ))" -s 0.05 --pid="$sentinel" -f "$AXL_POOL_SERIAL" \
            > "$TEST_LOG" ) &
    fi
    TEST_POOL_TAILER=$!
    tail -c "+$(( fwoff + 1 ))" -s 0.05 --pid="$sentinel" -f "$TEST_POOL_FW_SRC" \
        > "$TEST_FW_DEBUG_LOG" &
    TEST_POOL_FW_TAILER=$!
}

# _test_pool_stream_stop: end the stream and its samplers, the log complete.
_test_pool_stream_stop() {
    _tm_close_open pooled/bg -       # a background run ends with its stream
    sleep 0.2                       # the console's last bytes reach the file
    if [[ -n "${TEST_POOL_SENTINEL:-}" ]]; then
        kill "$TEST_POOL_SENTINEL" 2>/dev/null || true
        wait "$TEST_POOL_SENTINEL" 2>/dev/null || true
        TEST_POOL_SENTINEL=""
    fi
    if [[ -n "${TEST_SCHED_PID:-}" ]]; then
        kill "$TEST_SCHED_PID" 2>/dev/null || true
        wait "$TEST_SCHED_PID" 2>/dev/null || true   # 143: killed on purpose
        TEST_SCHED_PID=""
    fi
    _test_starve_stop
    if [[ -n "${TEST_POOL_TAILER:-}" ]]; then
        wait "$TEST_POOL_TAILER" 2>/dev/null || true
        TEST_POOL_TAILER=""
    fi
    if [[ -n "${TEST_POOL_FW_TAILER:-}" ]]; then
        wait "$TEST_POOL_FW_TAILER" 2>/dev/null || true
        TEST_POOL_FW_TAILER=""
    fi
}

# The pooled run of test_run_background: the test's .nsh started on the
# leased guest and left running (axlpool exec --console in the background),
# its serial log streamed into TEST_LOG as a booted guest's is, and every
# test_add_port_forward pair added to the guest's user network on 127.0.0.1
# (QMP hostfwd_add) -- so the host reaches the guest's server exactly as it
# reaches a booted one. No QEMU is the test's to kill: TEST_QEMU_PID is empty,
# test_cleanup ends the stream and the exec, and the release resets the guest
# and removes the forwards (axlpool/guest.py drop_forwards).
# One QMP command to the leased guest's QEMU (axlpool qmp): its answer on
# stdout, or why not.
_test_pool_qmp() {   # _test_pool_qmp SOCKET COMMAND ARGS-JSON
    (cd "$PROJECT_DIR/test/pool" && exec python3 -m axlpool qmp "$@")
}
TEST_POOL_FWDS_ADDED=" "   # the forwards already on the leased guest (" H:G H:G ")
_test_run_pooled_background() {
    local f host guest out
    # One LIVE at a time: the agent runs the first until it is stopped, so a
    # second would queue behind it. Once test_guest_stop has ended it, the
    # next one restores the guest (_test_pool_enter), as a boot is a fresh
    # machine -- the first run's server goes with the restore.
    if [[ -n "${TEST_POOL_BG_PID:-}" ]]; then
        _test_unpooled "a second background run while the first is live (test_run_background twice)"
    fi
    _test_pool_enter bg
    for f in ${TEST_POOL_FWDS[@]+"${TEST_POOL_FWDS[@]}"}; do
        host="${f%%:*}" guest="${f#*:}"
        # Once per guest state: QEMU refuses a forward twice, and only a
        # restore (which drops them) clears the record.
        [[ "$TEST_POOL_FWDS_ADDED" == *" $f "* ]] && continue
        if ! out=$(_test_pool_qmp "$AXL_POOL_QMP" human-monitor-command \
                       "{\"command-line\": \"hostfwd_add net0 tcp:127.0.0.1:${host}-:${guest}\"}" 2>&1) \
           || [[ "$out" != '""' ]]; then
            # QEMU answers "" when the rule went in, its reason otherwise (a
            # host port another process holds): the test's FAIL, as a booted
            # run's "Could not set up host forwarding rule" is.
            echo "  FAIL: $(basename "$0"): could not forward host port $host to guest port $guest: $out" >&2
            exit 1
        fi
        TEST_POOL_FWDS_ADDED+="$f "
    done
    _test_pool_stream_start bg
    _test_pool_exec 3600 "$TEST_POOL_LINE" --console > "$TEST_TMPDIR/exec.out" 2>&1 &
    TEST_POOL_BG_PID=$!
    # Said to this test's children: a pooled scripts/run-qemu.sh call now
    # boots instead of restoring the guest this run's server lives on.
    export AXL_POOL_BG_LIVE=1
    # EMPTY, not 0: `kill 0` signals the caller's whole process group -- the
    # test itself and its host servers -- and `kill -0 0` always succeeds.
    # A test that kills or polls the guest uses test_guest_stop instead.
    TEST_QEMU_PID=""
}

# _test_pool_bg_ended: the background exec has ended (the guest's script
# returned, or the agent could not run it). Its status is read, the stream
# drained, and what exec itself said appended to TEST_LOG; a POOL failure
# (77) is a miss -- the test reruns booted -- never "the server did not
# start" after a full wait.
_test_pool_bg_ended() {
    local rc=0
    wait "$TEST_POOL_BG_PID" 2>/dev/null || rc=$?
    TEST_POOL_BG_PID=""
    unset AXL_POOL_BG_LIVE
    _test_pool_stream_stop
    _test_pool_dump_del
    cat "$TEST_TMPDIR/exec.out" >> "$TEST_LOG" 2>/dev/null || true
    if [[ $rc -eq 77 ]]; then
        _test_pool_miss "the agent could not run $TEST_POOL_LINE for $TEST_NSH ($(grep -m1 '^axlpool exec:' "$TEST_TMPDIR/exec.out" || echo 'exit 77'))"
    fi
    _test_pool_check_ran
    return 0
}

# test_guest_stop: end the guest's run now -- a booted guest's QEMU killed
# and reaped; on a pooled one the stream stopped and the exec client ended
# (its program keeps running until the release restores the guest). The log
# is complete afterwards either way.
test_guest_stop() {
    if _test_pooled; then
        if [[ -n "${TEST_POOL_BG_PID:-}" ]]; then
            kill "$TEST_POOL_BG_PID" 2>/dev/null || true
            wait "$TEST_POOL_BG_PID" 2>/dev/null || true
            TEST_POOL_BG_PID=""
        fi
        unset AXL_POOL_BG_LIVE
        _test_pool_stream_stop
        _test_pool_dump_del
        return 0
    fi
    if [[ "${TEST_QEMU_PID:-0}" =~ ^[1-9][0-9]*$ ]]; then
        kill "$TEST_QEMU_PID" 2>/dev/null || true
        wait "$TEST_QEMU_PID" 2>/dev/null || true
        _tm_close_open "$TEST_QEMU_PID" -
        TEST_QEMU_PID=0
    fi
}

# test_guest_running: the background run's guest is still running its
# script -- a booted guest's QEMU alive; on a pooled one the exec client still
# attached (it ends when the script returns). `kill -0 "$TEST_QEMU_PID"` is
# NOT that test: pooled, TEST_QEMU_PID is empty, so it reads "gone" from the
# start.
test_guest_running() {
    if _test_pooled; then
        [[ -n "${TEST_POOL_BG_PID:-}" ]] && kill -0 "$TEST_POOL_BG_PID" 2>/dev/null
        return
    fi
    [[ "${TEST_QEMU_PID:-0}" =~ ^[1-9][0-9]*$ ]] && kill -0 "$TEST_QEMU_PID" 2>/dev/null
}

# test_guest_qemu_pid: the PID of the QEMU the guest runs in, for a test that
# samples its CPU from /proc -- the booted run's own, or the leased guest's
# (AXL_POOL_QEMU_PID). Prints nothing, and fails, when there is none.
test_guest_qemu_pid() {
    local pid="${TEST_QEMU_PID:-}"
    _test_pooled && pid="${AXL_POOL_QEMU_PID:-}"
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1
    echo "$pid"
}

# The pooled run of test_run_foreground: into the lease's share directory
# (the Shell's `cd` cannot change volume, so the volume first), then the
# test's .nsh with its output on the guest's CONSOLE (axlpool exec --console),
# not captured. TEST_LOG is then the guest's serial log from the lease's
# offset, streamed through `ts` as a booted run's is: what the script printed
# and what the firmware printed (DEBUG output, an exception dump),
# interleaved as they happened and stamped -- the same log a booted test
# reads, and complete up to a crash or hang, which a capture read after the
# line ends would lose.
_test_run_pooled() {
    local timeout_sec="$1" rc=0
    test_scale_timeout "$timeout_sec"
    timeout_sec=$TEST_SCALED_S
    _test_pool_enter fg
    _test_pool_stream_start
    _test_pool_exec "$timeout_sec" "$TEST_POOL_LINE" --console > "$TEST_TMPDIR/exec.out" 2>&1 || rc=$?
    TEST_GUEST_END_NS=$(date +%s%N)
    _tm_close_open pooled/fg "$rc"
    _test_pool_stream_stop
    cat "$TEST_TMPDIR/exec.out" >> "$TEST_LOG"   # what exec itself said (a timeout, a refusal)
    TEST_RUN_TIMED_OUT=0
    TEST_RUN_KILLED_SIG=0
    TEST_RUN_LIMIT="$timeout_sec"
    case "$rc" in
        124) TEST_RUN_TIMED_OUT=1 ;;
        77)  _test_pool_miss "the agent could not run $TEST_POOL_LINE for $TEST_NSH ($(grep -m1 '^axlpool exec:' "$TEST_TMPDIR/exec.out" || echo 'exit 77'))" ;;
    esac
    _test_pool_check_ran
    _test_check_dxe_assert
    _test_fw_debug_on_exception
}

# Resolve THIS test's QEMU pid. Matched by the test's own disk image path,
# which is unique per TEST_TMPDIR — `pgrep -P` cannot be used through the
# `timeout`+`ts` pipeline, and the comm check keeps the `timeout` wrapper
# (whose argv also contains the path) from being mistaken for QEMU.
_test_qemu_pid() {
    local i pid comm
    for i in 1 2 3 4 5; do
        for pid in $(pgrep -f "$TEST_DISK" 2>/dev/null); do
            comm=$(cat "/proc/$pid/comm" 2>/dev/null || true)
            if [[ "$comm" == qemu-system* ]]; then
                printf '%s' "$pid"
                return 0
            fi
        done
        sleep 0.2
    done
    printf ''
}

# test_timing_breakdown <elapsed_ms> -- print one "Timing:" line splitting the
# suite's wall time into setup (image build, before the guest exists) and the
# guest run, with how oversubscribed the host was meanwhile; sets
# TEST_CONTENTION (the factor, 1.00 = idle box) and TEST_BUDGET_S (the budget
# after scaling).
#
# WHY THE BUDGET SCALES. It exists to notice that the suite got slower. Wall
# time cannot tell that from "the box was busy": beside verify.sh's other five
# jobs the same suite measured 75 s alone and 115-124 s together (host load
# 19-21 on 12 cores). Alone it is judged against TEST_MAX_DURATION unchanged;
# oversubscribed, against TEST_MAX_DURATION x host_contention_factor (axl-common.sh,
# which derives and justifies using the raw ratio; capped at 3x). Under
# verify.sh the suite (setup + guest, as it was graded then) took 107-134 s against budgets of 133-216 s (24-85%
# margin; 27% idle). The idle run is the one that catches a regression. If the load was not captured the factor is 1.00 and
# the line says so: raw wall time, the stricter direction, never a guessed
# discount.
# test_check_budget_scale -- TEST_BUDGET_SCALE (declared by a caller that knows
# it shares the box, see below) must be a decimal in [1, 3] with a nonzero leading
# digit: 0 or 0.5 would shrink the budget below its idle value (stricter than the
# idle run, a false failure by construction) and junk would abort arithmetic
# mid-run. Unset is 1. Exits 2, naming the variable, like TEST_TIMEOUT_SCALE.
test_check_budget_scale() {
    local v="${TEST_BUDGET_SCALE-1}"
    if ! budget_scale_ok "$v"; then
        echo "FAIL: TEST_BUDGET_SCALE='$v' is not a decimal between 1 and 3" \
             "(3 is the cap on the measured factor too; more would cancel the budget);" \
             "refusing to run." >&2
        exit 2
    fi
}

TEST_CONTENTION="1.00"
TEST_CONTENTION_MEASURED=0   # 1: TEST_CONTENTION came from a sample, not the fallback
TEST_BUDGET_S=""
TEST_JUDGED_MS=0   # what the budget grades: the guest run (see below)
test_timing_breakdown() {
    local elapsed_ms="$1" setup_ms=0 guest_ms=0 note=""
    if [[ -n "$TEST_GUEST_START_NS" && -n "$TEST_GUEST_END_NS" ]]; then
        setup_ms=$(( (TEST_GUEST_START_NS - TEST_START_TIME) / 1000000 ))
        guest_ms=$(( (TEST_GUEST_END_NS - TEST_GUEST_START_NS) / 1000000 ))
    fi
    if [[ -n "$TEST_SCHED_FILE" ]] \
        && TEST_CONTENTION=$(host_contention_factor "$TEST_SCHED_FILE"); then
        TEST_CONTENTION_MEASURED=1
    else
        TEST_CONTENTION="1.00"
        TEST_CONTENTION_MEASURED=0
        note=" (host load not captured: budget judges raw wall time)"
    fi
    # THE BUDGET GRADES THE GUEST RUN, NOT SETUP. Setup is the image build; after
    # a merge or on a clean tree it rebuilt for 64 s of a 158 s total while the
    # guest took 93 s, and a budget that graded the total failed a suite whose
    # tests had not slowed at all. A slow image build is not a slow test. Setup
    # stays on the Timing line and has no budget of its own: it has no hang mode
    # worth a limit (the outer runner timeout bounds it) and its cost is a cold
    # cache, which a separate generous number would only teach people to ignore.
    # A test that launched its guest another way has no split, so it is judged
    # on the whole elapsed time as before.
    TEST_JUDGED_MS=$elapsed_ms
    if [[ -n "$TEST_GUEST_START_NS" && -n "$TEST_GUEST_END_NS" ]]; then
        TEST_JUDGED_MS=$guest_ms
    fi
    # TWO SOURCES OF "THE BOX WAS BUSY", and the budget takes the larger.
    # Measured: the runnable-task ratio (TEST_CONTENTION) sees oversubscription,
    # but under verify.sh's concurrent jobs on 12 cores it reads 1.00 while the
    # guest still takes 1.4-1.5x as long (113.6 s and 120.1 s against 80.8 s
    # idle): memory-bandwidth/cache/IO contention, which no runqueue counter
    # sees. The guest's own CPU time cannot stand in for it -- a CPU-bound
    # regression inflates that too, which is exactly what the budget is for.
    # So a caller that KNOWS it launched concurrent jobs (verify.sh) declares
    # TEST_BUDGET_SCALE, as it declares TEST_TIMEOUT_SCALE for the hang limit.
    # Unset (an idle or standalone run) it is 1 and the budget stays strict.
    test_check_budget_scale
    local _eff="$TEST_CONTENTION"
    _eff=$(awk -v a="$TEST_CONTENTION" -v b="${TEST_BUDGET_SCALE-1}" \
        'BEGIN { printf "%.2f", (b > a) ? b : a }')
    if [[ -n "${TEST_MAX_DURATION:-}" ]]; then
        TEST_BUDGET_S=$(awk -v b="$TEST_MAX_DURATION" -v f="$_eff" \
            'BEGIN { printf "%d", b * f + 0.5 }')
    fi
    if [[ "${TEST_BUDGET_SCALE-1}" != 1 ]]; then
        note="$note; declared budget scale x${TEST_BUDGET_SCALE}"
    fi
    # Only a foreground guest has a setup/guest split to report; a test that
    # launched its guest another way would print "0.000s + 0.000s" here, a
    # measurement that was never made.
    if [[ -n "$TEST_GUEST_START_NS" && -n "$TEST_GUEST_END_NS" ]]; then
        printf 'Timing: %d.%03ds setup + %d.%03ds guest; host contention x%s%s\n' \
            $((setup_ms / 1000)) $((setup_ms % 1000)) $((guest_ms / 1000)) \
            $((guest_ms % 1000)) "$TEST_CONTENTION" "$note"
    fi
}

# Summarize the CPU sampler. WARNS but does NOT fail the test by default.
#
# The integration pool runs ~25 VMs concurrently and the 1.5-core threshold is
# one that firmware boot legitimately brushes (measured: a large cluster of
# short-lived guests peaks 1.4-1.65 cores, all of it inside the warm-up
# window). Failing on that would make the suite flaky for a signal that is
# advisory, so the default here is a WARN line plus TEST_CPU_SPIKE=1 — unlike
# run-qemu.sh, which keeps failing with CPU_SPIKE_EXIT. Set
# TEST_CPU_SPIKE_FAIL=1 to opt a specific test into failing.
test_cpu_check() {
    local rc=0
    cpu_monitor_finish || rc=$?
    [[ $rc -eq 0 ]] && return 0
    TEST_CPU_SPIKE=1
    if [[ "${TEST_CPU_SPIKE_FAIL:-0}" == "1" ]]; then
        return "$rc"
    fi
    return 0
}

# Suppress the CPU advisory for a suite where the spike is STRUCTURAL.
#
# The sampler cannot tell AXL's CPU use from the firmware's, and for a suite
# whose cost is dominated by firmware boot the warning fires on every single
# run (observed 6/6 on x64: peak ~1.05-1.10 cores, sustained 4.6-5.8s against
# a 2s threshold). A warning that always fires is not a signal, it is noise
# that teaches people to skim past the summary -- so such a suite opts out and
# uses TEST_MAX_DURATION instead, which measures something actionable.
#
# This does NOT weaken the sampler elsewhere: run-qemu.sh still fails on a
# spike (CPU_SPIKE_EXIT), test-cpu-spike-qemu.sh still covers the machinery,
# and any test can still opt into failing with TEST_CPU_SPIKE_FAIL=1.
test_cpu_advisory_off() {
    CPU_WARN=false
}

# Run QEMU in background. Sets TEST_QEMU_PID.
#
# QEMU's own stderr lands in TEST_LOG alongside the guest serial stream, so a
# refused -netdev is visible here — but only if someone looks. Nothing did:
# the test went straight to test_wait_for and reported "server did not start
# within 60s" a minute later. Check the moment QEMU has had a chance to fail.
#
# QEMU dies with the test shell (axl_pdeath_init's setpriv prefix, as
# run-qemu.sh's background guests do): a test SIGKILLed by the runner's limit
# or by hand runs no EXIT trap, and its guest used to survive at ppid 1.
# test-run-background-pdeathsig.sh pins it.
test_run_background() {
    if _test_pooled; then
        _test_run_pooled_background
        return
    fi
    _prof_boot_start
    _test_default_nic
    _test_fw_debug_uart
    _prof_launch background "${TEST_QEMU_CMD[@]}"
    axl_pdeath_init
    _tm_begin booted bg
    if [[ -n "${TEST_STDIN_FD:-}" ]]; then
        ${AXL_PDEATH[@]+"${AXL_PDEATH[@]}"} "${TEST_QEMU_CMD[@]}" > "$TEST_LOG" 2>&1 <&"$TEST_STDIN_FD" &
    else
        ${AXL_PDEATH[@]+"${AXL_PDEATH[@]}"} "${TEST_QEMU_CMD[@]}" > "$TEST_LOG" 2>&1 &
    fi
    TEST_QEMU_PID=$!
    # Its end and boot reading come from a watcher: the guest may exit by
    # itself long before anything here looks.
    _TM_PID[_TM_CUR]=$TEST_QEMU_PID
    _TM_WATCH[_TM_CUR]="$TEST_TMPDIR/.timing.$_TM_CUR"
    axl_timing_watch "$TEST_QEMU_PID" "$TEST_LOG" "${_TM_WATCH[_TM_CUR]}"
    _TM_WPID[_TM_CUR]=$AXL_TIMING_WATCH_PID
    local i
    for i in $(seq 1 20); do
        kill -0 "$TEST_QEMU_PID" 2>/dev/null || break   # died: log is final
        grep -qa "Could not set up host forwarding rule" "$TEST_LOG" 2>/dev/null && break
        sleep 0.1
    done
    if ! axl_report_hostfwd_failure "$TEST_LOG" "$(basename "$0")"; then
        return 1
    fi
    # TEST_QEMU_PID is QEMU itself here (no timeout wrapper), so the sampler
    # can attach directly. test_cleanup summarizes once QEMU is killed.
    cpu_monitor_start "$TEST_QEMU_PID" "$TEST_TMPDIR"
}

# ---------------------------------------------------------------------------
# Guest console INPUT (background runs)
# ---------------------------------------------------------------------------
#
# The harness is otherwise write-once: startup.nsh drives the guest and the
# host only reads serial. That is enough for anything scriptable, but not for
# the class of behavior a key press IS — a foreground server ended by Ctrl-C,
# say, whose whole contract is what happens when a byte arrives on ConIn.
#
# test_enable_stdin routes QEMU's stdin from a FIFO this script holds open;
# `-nographic` multiplexes the guest serial on stdio, so a byte written here
# is delivered to the guest as a keystroke. QEMU's stdio chardev reserves
# Ctrl-A (0x01) as its own escape prefix and passes everything else straight
# through, so 0x03 arrives at ConIn as a real serial Ctrl-C.
#
# Call BEFORE test_run_background; the FIFO is opened read-WRITE on this end
# so QEMU never sees EOF between writes (a write-only open would close after
# each printf and QEMU would stop watching stdin).
test_enable_stdin() {
    _test_unpooled "typed input (test_enable_stdin)"
    local fifo="$TEST_TMPDIR/qemu-stdin.fifo"
    rm -f "$fifo"
    mkfifo "$fifo"
    exec {TEST_STDIN_FD}<>"$fifo"
}

# test_send_stdin <printf-format>
#
# Write raw bytes to the guest console; the argument goes through
# `printf '%b'`, so escapes like '\003' work. No-op-safe only after
# test_enable_stdin — calling it without one is a harness bug, and the
# `set -u` bare expansion below is what makes that loud instead of silent.
test_send_stdin() {
    _test_unpooled "typed input (test_send_stdin)"
    printf '%b' "$1" >&"$TEST_STDIN_FD"
}

# Wait for a string to appear in the serial log (for background QEMU).
# Returns 0 on success, 1 on timeout. Two entry points, because they answer
# different questions:
#
#   test_wait_for  PATTERN SECS   a BOUNDED wait: the guest is already up and the
#                                 event must come within SECS. The bound is often
#                                 the assertion (rebind-storm's "FREE-DONE within
#                                 8 s" is its "returns bounded" check), so it is
#                                 used EXACTLY as given -- never widened.
#   test_wait_boot PATTERN SECS   a wait that COVERS THE GUEST'S BOOT (the first
#                                 wait after test_run_background, or a pattern
#                                 only the whole run produces). SECS is scaled
#                                 like the foreground run limit: aarch64 +60 s,
#                                 then TEST_TIMEOUT_SCALE. aarch64 TCG startup
#                                 stalls at the firmware banner under load, and
#                                 a hard-coded 60 s there is a flake, not a test.
#
# Both set TEST_WAITED_S to the limit actually used, so a failure message can
# say what was waited (a hard-coded "60s" is wrong the moment it is scaled).
TEST_WAITED_S=0
_test_wait_poll() {
    local pattern="$1" timeout_sec="$2" i
    TEST_WAITED_S=$timeout_sec
    for i in $(seq 1 "$timeout_sec"); do
        # A pooled background run whose exec has ENDED will print nothing
        # more: decided now, not after the whole wait (_test_pool_bg_ended).
        if [[ -n "${TEST_POOL_BG_PID:-}" ]] && ! kill -0 "$TEST_POOL_BG_PID" 2>/dev/null; then
            _test_pool_bg_ended
            grep -q "$pattern" "$TEST_LOG" 2>/dev/null && { _prof_boot_end "$pattern"; return 0; }
            return 1
        fi
        if grep -q "$pattern" "$TEST_LOG" 2>/dev/null; then
            # Profiled here rather than at test_cleanup: a test with several
            # scenarios boots and waits repeatedly, and cleanup runs once. This
            # is the point where THIS guest finished doing what was asked.
            _prof_boot_end "$pattern"
            return 0
        fi
        sleep 1
    done
    # A boot that never produced its pattern still burned the wall clock, and
    # is usually the most expensive one in the run -- record it rather than
    # dropping it and under-reporting the total.
    _prof_boot_end "TIMEOUT:$pattern"
    # Timed out. Before the caller reports "the server never came up", say so
    # if the guest never had a network to come up on.
    axl_report_hostfwd_failure "$TEST_LOG" "$(basename "$0")" || true
    return 1
}

test_wait_for() {
    _test_wait_poll "$1" "${2:-60}"
}

test_wait_boot() {
    test_scale_timeout "${2:-60}"
    _test_wait_poll "$1" "$TEST_SCALED_S"
}

# ---------------------------------------------------------------------------
# Serial log processing
# ---------------------------------------------------------------------------

# Strip ANSI escape codes AND the `ts` per-line timestamp prefix
# ("[SSS.sss] ") from the serial log so downstream PASS/FAIL grep
# anchored with ^ still matches. The raw TEST_LOG keeps the
# timestamps for per-binary timing parsing.
test_clean_log() {
    axl_strip_escapes "$TEST_LOG" | sed -E 's/^\[[0-9]+\.[0-9]+\] //' \
        | tr -d '\r' > "$TEST_CLEAN_LOG"
}

# test_slice_log <start-marker> <end-marker> <outfile>
#
# Extract the lines strictly between two exact-match `echo` markers in
# TEST_CLEAN_LOG into <outfile>. Pass "" as <end-marker> to slice from
# <start-marker> to the end of the log instead of to a second marker.
#
# For binding an assertion to the ONE command that must have produced a
# line, when the same line (a repeated `status` label, say) can legally
# appear more than once across a run — a whole-log grep would also be
# satisfied by an earlier/later occurrence that proves nothing. Neither
# marker line itself is included in the slice.
#
# A marker that is not in the log is a HARD error, not an empty slice:
# renaming an `echo` marker in a startup.nsh without renaming it here would
# otherwise silently turn every assertion bound to that slice into a no-op —
# an absence check over zero lines passes, and a presence check fails with a
# message that blames the tool rather than the harness. Returns 1 (fatal
# under the `set -e` this file installs) after naming the missing marker.
test_slice_log() {
    local start="$1" end="$2" outfile="$3"
    local m
    for m in "$start" "$end"; do
        [[ -z "$m" ]] && continue
        if ! grep -Fxq "$m" "$TEST_CLEAN_LOG"; then
            echo "  FAIL: test_slice_log: marker '$m' is not in the serial log" >&2
            echo "        (the slice would be empty and every assertion over it vacuous)" >&2
            return 1
        fi
    done
    if [[ -z "$end" ]]; then
        awk -v s="$start" '$0 == s { on = 1; next } on' \
            "$TEST_CLEAN_LOG" > "$outfile"
    else
        awk -v s="$start" -v e="$end" \
            '$0 == e { on = 0 } on; $0 == s { on = 1 }' \
            "$TEST_CLEAN_LOG" > "$outfile"
    fi
}

# ---------------------------------------------------------------------------
# Memory-leak gate
# ---------------------------------------------------------------------------
#
# Every AXL image prints a leak verdict from its teardown path (_axl_cleanup,
# or the minimal CRT0) under AXL_MEM_DEBUG: either "mem: no leaks detected" or
# a report listing every block still live AFTER atexit callbacks and the tier-1
# registry sweep have run. Nothing frees anything after that point, so a report
# there is a leak, full stop.
#
# Until this gate existed nothing read those lines. Two leaked streams sat in
# test/unit/axl-test-io.c until a human happened to read the code, and the
# measurement that produced this function found five more the same way.
#
# The two anchors below are load-bearing and are pinned on the guest side:
#
#   TEST_LEAK_MARKER      The teardown report. axl_mem_dump_leaks() called by a
#                         RUNNING program prints "=== AxlMem leak report (live
#                         allocations): ..." instead — live blocks are not
#                         leaked blocks, and a diagnostic dump must not fail a
#                         run. That infix is asserted exactly by test_leak_dump
#                         (test/unit/axl-test-mem.c), so collapsing the two
#                         spellings back together breaks a unit test rather
#                         than silently unarming this grep.
#
#   TEST_LEAK_OK_MARKER   The clean verdict. Used as a POSITIVE CONTROL: a
#                         guest that reached teardown printed one marker or the
#                         other, so an ABSENT verdict means this gate saw
#                         nothing rather than nothing being wrong. Every cause
#                         is reported as a failure (the message enumerates
#                         them). A gate that silently no-ops is worse than no
#                         gate; that is the whole point of the exercise.
#
# The control is applied PER BINARY when the log carries "=== Results:"
# footers, and whole-log otherwise. Per-binary is what catches the interesting
# case: AxlTestLog used to silence its console mid-run and never restore it, so
# that one binary's verdict never reached the serial log and a leak in it would
# have been invisible while 38 siblings kept a whole-log control green.
#
# TEST_SKIP_LEAK_GATE=1 opts a suite out (RELEASE guests, or a scenario whose
# whole subject is a crash before teardown). Say why at the call site.
TEST_LEAK_MARKER='=== AxlMem leak report:'
TEST_LEAK_OK_MARKER='mem: no leaks detected'

# Verdict on TEST_CLEAN_LOG. Returns 0 clean, 1 on leaks or a blind gate.
# Caller must have run test_clean_log.
test_check_leaks() {
    if [[ "${TEST_SKIP_LEAK_GATE:-0}" == "1" ]]; then
        return 0
    fi

    local n_leak n_ok rc=0
    n_leak=$(grep -acF "$TEST_LEAK_MARKER" "$TEST_CLEAN_LOG" || true)
    n_ok=$(grep -acF "$TEST_LEAK_OK_MARKER" "$TEST_CLEAN_LOG" || true)

    # --- positive control -------------------------------------------------
    #
    # Keyed on the "=== Results:" FOOTER, not the "=== NAME Tests ===" header:
    # a footer is what says a binary ran to completion, and the verdict is the
    # next thing it prints. Keying on the header instead would let a binary
    # that forgot to print one merge into its predecessor's window, so the
    # predecessor's verdict would vouch for both -- precisely the silent blind
    # spot this control exists to remove. (Two binaries were in that state
    # until the headers went into axl-test-vterm.c / axl-test-9p.c.) The
    # header is used only to NAME the offender; a nameless one still counts.
    local silent
    if grep -qa '=== Results:' "$TEST_CLEAN_LOG"; then
        silent=$(awk -v leak="$TEST_LEAK_MARKER" -v ok="$TEST_LEAK_OK_MARKER" '
            match($0, /=== .+ Tests ===/) {
                name = $0
                sub(/.*=== /, "", name); sub(/ Tests ===.*/, "", name)
                cur = name
            }
            /=== Results:/ {
                if (pending) { print label }
                n++
                label = (cur == "" ? sprintf("(binary #%d, no \"=== NAME Tests ===\" header)", n) : cur)
                cur = ""; pending = 1
                next
            }
            # Only AFTER a footer: the teardown verdict is printed once main
            # has returned, so a mid-run axl_mem_dump_leaks() is not one.
            pending && (index($0, leak) || index($0, ok)) { pending = 0 }
            END { if (pending) print label }
        ' "$TEST_CLEAN_LOG")
    elif (( n_leak == 0 && n_ok == 0 )); then
        silent="(the whole run)"
    fi

    if [[ -n "${silent:-}" ]]; then
        echo ""
        echo "FAIL: the leak gate saw NO memory verdict from:"
        local _b
        while IFS= read -r _b; do echo "      - $_b"; done <<< "$silent"
        echo "  An AXL_APP image prints one at teardown under AXL_MEM_DEBUG."
        echo "  Absent, the guest is one of:"
        echo "    - a RELEASE build (the build defines AXL_MEM_DEBUG only"
        echo "      for BUILD=DEBUG);"
        echo "    - an image that silenced the console and never restored it"
        echo "      (axl_log_set_console_enabled), or raised the log level"
        echo "      above INFO so the clean verdict is filtered;"
        echo "    - an AXL_DRIVER, which has no _axl_cleanup and so never"
        echo "      emits a teardown verdict at all;"
        echo "    - a fault before teardown;"
        echo "    - a wording change in src/mem/axl-mem.c that this grep"
        echo "      anchors on."
        echo "  Fix it or set TEST_SKIP_LEAK_GATE=1 with a reason — do NOT"
        echo "  leave the gate blind."
        rc=1
    fi

    # --- the verdict itself ----------------------------------------------
    if (( n_leak == 0 )); then
        return "$rc"
    fi

    #
    # Attribute each report to the test binary that was running, the same way
    # the stalled-binary detector does: the most recent "=== NAME Tests ==="
    # header. Guests that print no header (a tool, a demo) report the binary
    # as "?" — the file:line in the block is the actionable part regardless.
    #
    echo ""
    echo "*** FAIL: memory leaked at teardown. These blocks were still live"
    echo "    after atexit callbacks and the tier-1 registry sweep, so nothing"
    echo "    was ever going to free them:"
    awk -v marker="$TEST_LEAK_MARKER" '
        index($0, "=== ") && index($0, " Tests ===") {
            name = $0
            sub(/.*=== /, "", name); sub(/ Tests ===.*/, "", name)
            cur = name
        }
        index($0, marker) {
            on = 1; shown = 0; hidden = 0
            printf "      binary: %s\n", (cur == "" ? "?" : cur)
        }
        on {
            line = $0
            sub(/^.*\[WARN\][ \t]*mem: /, "", line)
            # Cap the per-block listing: one call site repeated 64 times
            # (a loop that leaks) buries every OTHER report under it.
            if (line ~ /^  \[/ && ++shown > MAXROWS) { hidden++; next }
            if (index(line, "=== end leak report ===") && hidden > 0) {
                printf "        (... %d more allocations)\n", hidden
            }
            print "        " line
        }
        /=== end leak report ===/ { on = 0 }
    ' MAXROWS=8 "$TEST_CLEAN_LOG"
    echo "    A leak in a test is a test bug; a leak in src/ is a library"
    echo "    defect. Do not widen this gate to make it green."
    return 1
}

# ---------------------------------------------------------------------------
# Debug-assertion gate.
#
# AXL_DEBUG_ASSERT logs a marker and CARRIES ON (include/axl/axl-debug.h), so a
# broken invariant never fails a test by itself: axl_memcpy's overlap assert
# fired on every AxlArray remove and AxlLog handler removal for a whole
# development round and no run noticed. This makes any marker a failure.
#
# A suite that fires some ON PURPOSE lists them in TEST_EXPECTED_ASSERTS as
# "function:count" pairs, matched EXACTLY -- so the expected markers double as
# the control that this grep can see one at all. RELEASE guests compile the
# asserts out; set TEST_EXPECTED_ASSERTS to empty there, or the count of 0
# reads as a blind gate.
# ---------------------------------------------------------------------------
TEST_ASSERT_MARKER='AXL_DEBUG_ASSERT FAILED:'

# Verdict on TEST_CLEAN_LOG. Returns 0 clean, 1 on an unexpected marker, a
# wrong expected count, or a log it could not read.
test_check_debug_asserts() {
    local total rc=0 grc=0 spec fn want got expected=0

    total=$(grep -acF "$TEST_ASSERT_MARKER" "$TEST_CLEAN_LOG") || grc=$?
    if (( grc > 1 )); then
        echo ""
        echo "FAIL: the debug-assert gate could not read the guest log"
        return 1
    fi
    for spec in ${TEST_EXPECTED_ASSERTS:-}; do
        fn=${spec%%:*}
        want=${spec##*:}
        got=$(grep -aF "$TEST_ASSERT_MARKER" "$TEST_CLEAN_LOG" \
              | grep -acF " ${fn}(): " || true)
        expected=$(( expected + got ))
        if [[ "$got" != "$want" ]]; then
            echo ""
            echo "FAIL: expected exactly $want intentional debug assert(s) from"
            echo "      ${fn}(), saw $got -- a count of 0 means this gate is blind"
            rc=1
        fi
    done
    if (( total > expected )); then
        echo ""
        echo "FAIL: $(( total - expected )) unexpected AXL_DEBUG_ASSERT(s) fired:"
        grep -aF "$TEST_ASSERT_MARKER" "$TEST_CLEAN_LOG" | sed 's/^/      /' \
            | head -20
        echo "    An assert that logs and continues is still a broken invariant."
        echo "    Fix the cause; do not add it to TEST_EXPECTED_ASSERTS."
        rc=1
    fi
    return "$rc"
}

# Count PASS/FAIL from cleaned serial log, print results, exit with status
test_count_results() {
    test_clean_log

    #
    # EVERY grep over TEST_CLEAN_LOG passes -a. The serial capture can carry a
    # stray NUL (firmware noise before our first output -- AARCH64 does it
    # reliably), and one NUL anywhere makes GNU grep call the whole file binary.
    # The consequence is split, which is what makes it nasty:
    #
    #   -c   still counts every match, so the ratchet numbers stay CORRECT
    #   -q   still exits 0 on a post-NUL match, so the assert gate still fires
    #   line output prints matches until the NUL, then "binary file matches"
    #
    # So the counts are right while the listings are silently truncated -- a
    # report that shows "19 group(s) SKIPPED" above a list of 2. That is how
    # this was found: the SKIP list is the only view of which groups are
    # arch-gated, and a truncated one hides balancer drift behind a number that
    # still looks healthy. test_check_leaks (grep -acF / grep -qa) already got
    # this right; the rest did not. Uniform -a, so no reader has to know which
    # greps happen to survive binary detection and which quietly do not.
    #
    local pass fail skip skipped_asserts ratchet_total
    pass=$(grep -ac '^PASS:' "$TEST_CLEAN_LOG" || true)
    fail=$(grep -ac '^FAIL:' "$TEST_CLEAN_LOG" || true)
    skip=$(grep -acE '^SKIP(\[[0-9]+\])?:' "$TEST_CLEAN_LOG" || true)

    #
    # Assertions DECLARED skipped by test_skip_n, i.e. "SKIP[n]:". These are
    # topology gates -- a device this QEMU image or this machine does not have
    # -- where both outcomes share one baseline, so the count has to be made up
    # or the ratchet drifts between images.
    #
    # It used to be made up with padding: ~170 test_check(true, "... SKIP
    # balance") calls, one per assertion the populated path would have run.
    # Those are indistinguishable from the assert-nothing anti-pattern the
    # project bans and they inflated the pass count with results nobody
    # produced. Now the count is declared and added here instead.
    #
    # Bare "SKIP:" (no [n]) is the build-configuration form, which gets its own
    # baseline file and so must NOT be added in.
    #
    skipped_asserts=$(sed -n 's/^SKIP\[\([0-9]\+\)\]:.*/\1/p' "$TEST_CLEAN_LOG" \
        | awk '{ t += $1 } END { print t + 0 }')
    ratchet_total=$((pass + skipped_asserts))

    grep -aE '^(PASS|FAIL|SKIP(\[[0-9]+\])?):' "$TEST_CLEAN_LOG" | while IFS= read -r line; do
        echo "  $line"
    done

    grep -aE '^=== Results:' "$TEST_CLEAN_LOG" || true

    #
    # Per-binary wall-clock timing (requires ts-prefixed log; see
    # test_run_foreground). Emits one line per binary with BEGIN→END
    # elapsed time, and flags any binary whose ____END marker never
    # printed — that's the one that stalled when a timeout bites.
    #
    if grep -q '=== .* Tests ===' "$TEST_LOG"; then
        echo ""
        echo "Per-binary timing:"
        # INCOMPLETE is NEUTRAL ON PURPOSE. It used to say STALLED, which is
        # a diagnosis, and the same one the block below got wrong: a binary
        # with no footer was as often the harness timeout landing on it. The
        # verdict is stated ONCE, below, where the exit status is known.
        awk -f "$TESTS_DIR/lib/unit-times.awk" "$TEST_LOG" | awk -F'\t' '
            $2 == "-" { printf "  %-16s   INCOMPLETE (no Results line seen)\n", $1; next }
            { printf "  %-16s %7.2fs\n", $1, $2 }'
    fi

    #
    # Stalled-binary detection. Each unit binary prints a
    # "=== <Suite> Tests ===" header and a "=== Results: ... ===" footer.
    # A header with no footer before the next header (or end of log) means
    # that binary hung or faulted mid-run — and in the combined suite a
    # single hang starves every binary after it (one shared QEMU boot, one
    # timeout). Surface the culprit loudly: a hung firmware/protocol test
    # otherwise shows up only as an opaque "fewer tests ran" ratchet drop.
    # (The outer "=== AXL ... Integration Tests (ARCH) ===" wrapper does not
    # match the per-binary "... Tests ===" anchor, so it is not flagged.)
    #
    local stalled
    stalled=$(awk '
        match($0, /=== .+ Tests ===/) {
            if (open != "") { print open }
            name = $0
            sub(/.*=== /, "", name)
            sub(/ Tests ===.*/, "", name)
            open = name
        }
        /=== Results:/ { open = "" }
        END { if (open != "") print open }
    ' "$TEST_CLEAN_LOG")

    if [[ -n "$stalled" ]]; then
        echo ""
        #
        # TWO CAUSES, ONE LOG SHAPE. A header with no footer means either the
        # binary died mid-run, or the harness's own `timeout` cut the log off
        # while that binary happened to be running. Only the exit status
        # captured in test_run_foreground separates them, and until it was
        # captured this printed the first diagnosis for both.
        #
        # Note what the truncated branch does NOT claim: 124 says the limit
        # fired, not why. A genuine hang reaches it too. What it can say --
        # and all it says -- is that the named binary was merely in flight,
        # so a slow box is as good an explanation as a defect.
        #
        if [[ "$TEST_RUN_KILLED_SIG" != "0" ]]; then
            echo "*** CUT SHORT: the guest was killed by signal $TEST_RUN_KILLED_SIG before it"
            echo "    finished, and the harness limit had NOT been reached (${TEST_RUN_LIMIT}s). On a"
            echo "    loaded box that is usually the OOM killer. The binary below was in"
            echo "    flight and tells you nothing about the cause; binaries after it"
            echo "    never ran:"
        elif [[ "$TEST_RUN_TIMED_OUT" == "1" ]]; then
            echo "*** TRUNCATED: the harness's own ${TEST_RUN_LIMIT}s timeout fired, so the log stops"
            echo "    mid-run. The binary below did NOT necessarily hang — it was merely"
            echo "    in flight when the kill landed, and a healthy run that outgrew the"
            echo "    limit, or a loaded box, looks identical from here. Binaries after"
            echo "    it never ran:"
        else
            # NOT "an infinite loop" any more. This branch is now reached
            # only when the guest ended ON ITS OWN inside the limit, and an
            # infinite loop is precisely what cannot do that -- it reaches
            # the limit and reads as TRUNCATED above. What is left is a
            # fault or a reset.
            echo "*** STALLED: the following test binary started but never"
            echo "    produced a Results footer, and the guest then ended on its own"
            echo "    inside the ${TEST_RUN_LIMIT:-}s limit — so it faulted or reset rather"
            echo "    than hanging; any binaries after it in the run were starved:"
        fi
        local _s
        while IFS= read -r _s; do echo "      - $_s"; done <<< "$stalled"
    fi

    # Calculate elapsed time
    local end_time elapsed_ms elapsed_s elapsed_frac
    end_time=$(date +%s%N)
    elapsed_ms=$(( (end_time - TEST_START_TIME) / 1000000 ))
    elapsed_s=$((elapsed_ms / 1000))
    elapsed_frac=$((elapsed_ms % 1000))

    echo ""
    if (( skip > 0 )); then
        printf "Results: %d passed, %d failed, %d group(s) SKIPPED (%d assertions) (%s) in %d.%03ds\n" \
            "$pass" "$fail" "$skip" "$skipped_asserts" "$TEST_ARCH" \
            "$elapsed_s" "$elapsed_frac"
    else
        printf "Results: %d passed, %d failed (%s) in %d.%03ds\n" \
            "$pass" "$fail" "$TEST_ARCH" "$elapsed_s" "$elapsed_frac"
    fi

    #
    # SKIPPED groups. A `#ifdef`-gated test block that compiles to nothing is
    # invisible in a pass count -- when TLS was optional, AxlTestJose and
    # AxlTestCrypto dropped ~190 assertions in the default build and still
    # reported "0 failed". That read as success once already, and new
    # assertions were called green having never executed. The TLS instance is
    # gone; the hazard is not, which is why skips are still announced.
    #
    # So skips are always announced. TEST_REQUIRE_TLS used to live here, turning
    # a TLS-gated skip into a failure; mbedTLS is unconditional now, so nothing
    # can skip for want of it and there is no configuration to escalate.
    #
    if (( skip > 0 )); then
        echo ""
        echo "*** $skip test GROUP(S) SKIPPED ($skipped_asserts assertions declared):"
        grep -aE '^SKIP(\[[0-9]+\])?:' "$TEST_CLEAN_LOG" | sed -E 's/^SKIP(\[[0-9]+\])?:/    /'
    fi

    # Leak verdict. Folded into the final exit (and into the baseline write
    # below) rather than exiting here, so a run that both leaks and drops its
    # count still reports both.
    local leak_rc=0 assert_rc=0
    test_check_leaks || leak_rc=1
    test_check_debug_asserts || assert_rc=1

    #
    # Wall-clock BUDGET. Distinct from test_run_foreground's timeout, which is
    # a hang detector -- a suite that creeps to just under it passes silently,
    # which is exactly how a slow regression hides. This is the ratchet's
    # time-shaped sibling: TEST_MAX_DURATION is the number that must not drift
    # up unnoticed. Named to match run-qemu.sh's --max-duration / MAX_DURATION
    # and AGT's AGT_MAX_DURATION rather than inventing a third spelling for one
    # concept across sibling repos. (test-axl.sh builds its own QEMU command
    # instead of shelling out to run-qemu.sh, so it cannot just pass the flag.)
    #
    # Deliberately generous. The host drifts ~10% run to run and the box is
    # shared, so a budget tight enough to catch a 5% regression would be
    # flaky, and a flaky gate is worse than none. Set it to roughly 2x the
    # observed time and treat a breach as "something got materially slower",
    # not "shave 3 seconds".
    #
    # FOLDED INTO THE FINAL EXIT, not returned here -- the same shape as the
    # leak verdict above, and for the same reason.
    #
    # This used to `return 1` on the spot, which skipped the ratchet block
    # below entirely: a slow run neither CHECKED its count nor RECORDED it.
    # Both halves were wrong. Four load-failed verify.sh runs left the
    # committed baseline four assertions short, and a genuine count
    # regression on a loaded box would have been reported as "took too long"
    # and nothing else.
    #
    # And unlike a leak, a breached budget does not make the count suspect:
    # every assertion still ran and still passed, and a slow box cannot
    # inflate a pass count. So the baseline write below is NOT gated on it.
    local budget_rc=0
    test_timing_breakdown "$elapsed_ms"
    if [[ -n "${TEST_MAX_DURATION:-}" && "${TEST_SKIP_RATCHET:-0}" != "1" ]]; then
        # WHOLE SECONDS, as the check always was: a run fails at BUDGET+1.000 s
        # and not before (comparing milliseconds would fail it up to 1 s sooner
        # on an idle box). The message carries milliseconds so "took 95s, over
        # the 95s budget" cannot be printed.
        if (( TEST_JUDGED_MS / 1000 > TEST_BUDGET_S )); then
            echo ""
            printf 'FAIL: guest took %d.%03ds, over the %ss budget (%ss x host contention %s, declared scale %s).\n' \
                "$((TEST_JUDGED_MS / 1000))" "$((TEST_JUDGED_MS % 1000))" "$TEST_BUDGET_S" \
                "$TEST_MAX_DURATION" "$TEST_CONTENTION" "${TEST_BUDGET_SCALE-1}"
            echo "  This is a wall-clock ratchet, not a hang timeout — something"
            echo "  got materially slower. Profile it, or raise TEST_MAX_DURATION"
            echo "  in the caller deliberately (and say why in the commit)."
            budget_rc=1
        fi
    fi

    #
    # Ratchet: fail if test count dropped from last known good run.
    # Auxiliary scripts (e.g. test-ipmi-qemu.sh which runs a single
    # test binary) opt out via TEST_SKIP_RATCHET=1 so they don't
    # clobber the canonical test-axl.sh baseline with their smaller
    # pass counts.
    #
    # The baseline is PER TLS CONFIGURATION, because the two configurations run
    # different numbers of tests: with AXL_TLS=1 the AxlTestJose and
    # AxlTestCrypto blocks compile in and the count jumps by ~160. Sharing one
    # file meant a single TLS run wrote the higher number and then every
    # ordinary non-TLS run failed the ratchet -- "expected at least 8789 but
    # only 8628 ran", which reads exactly like a regression and is not one.
    # That happened; the committed baseline had to be repaired by hand.
    #
    # Keyed off the run's own evidence (a TLS-gated SKIP means TLS is absent)
    # rather than off $AXL_TLS, so building with TLS and forgetting to export
    # the variable when invoking the suite cannot select the wrong baseline.
    #
    if [[ "${TEST_SKIP_RATCHET:-0}" != "1" ]]; then
        # ONE baseline. There were two, .last-pass-count and
        # .last-pass-count.tls, because AXL_TLS changed which sources compiled
        # and therefore how many tests existed -- sharing one file meant a
        # single TLS run raised it and every later non-TLS run failed the
        # ratchet with a message that blamed the tests for a stale baseline
        # from a different configuration.
        #
        # There is one configuration now, so the split is not merely
        # unnecessary but harmful: two baselines drift. The .tls file was
        # observed 104 assertions stale, because TLS runs happened less often.
        # TEST_BASELINE_FILE is the self-test seam; unset, it is the real one.
        local baseline_file="${TEST_BASELINE_FILE:-$TESTS_DIR/.last-pass-count}"
        if [[ -f "$baseline_file" ]]; then
            local expected
            expected=$(cat "$baseline_file")
            if [[ $ratchet_total -lt $expected ]]; then
                echo ""
                echo "FAIL: expected at least $expected tests but only $ratchet_total ran"
                echo "      ($pass passed + $skipped_asserts declared-skipped)"
                echo "      (baseline: $(basename "$baseline_file"))"
                if [[ -n "$stalled" && "$TEST_RUN_KILLED_SIG" != "0" ]]; then
                    echo "      Killed by signal $TEST_RUN_KILLED_SIG while running:" \
                         "$(echo "$stalled" | paste -sd, -)"
                    echo "      (so the count is short because the run was cut off from"
                    echo "       outside, not because that binary is broken)"
                elif [[ -n "$stalled" && "$TEST_RUN_TIMED_OUT" == "1" ]]; then
                    echo "      Cut short by the harness's own ${TEST_RUN_LIMIT}s timeout while running:" \
                         "$(echo "$stalled" | paste -sd, -)"
                    echo "      (so the count is short because the run did not finish, not"
                    echo "       necessarily because that binary is broken)"
                elif [[ -n "$stalled" ]]; then
                    echo "      Culprit (no Results footer): $(echo "$stalled" | paste -sd, -)"
                else
                    echo "      (a test binary may have crashed partway through)"
                fi
                exit 1
            fi
        fi
        # A leaking run is not a green run, so it does not get to raise the
        # bar either -- same reasoning as the $fail guard beside it.
        if [[ $fail -eq 0 && $pass -gt 0 && $leak_rc -eq 0 && $assert_rc -eq 0 ]]; then
            echo "$ratchet_total" > "$baseline_file"
        fi
    fi

    # One binary's share of the ratchet, for a run that sums them (the CTest
    # unit.<App> tests; lib/unit-ratchet.sh): "<count> green|red", written
    # HERE, once every verdict is known -- the sum judges every count, but only
    # an all-green run may raise the baseline, as the ratchet above writes only
    # on a green, leak-free, assert-clean run.
    if [[ -n "${TEST_COUNT_FILE:-}" ]]; then
        local _verdict=red
        [[ $fail -eq 0 && $pass -gt 0 && $leak_rc -eq 0 && $assert_rc -eq 0 ]] && _verdict=green
        mkdir -p "$(dirname "$TEST_COUNT_FILE")"
        # And the guest time, how the binary ran, and the host contention its
        # run measured: unit-ratchet.sh judges the one-boot budget over the
        # pooled per-binary guest times, each scaled as the one boot is (a
        # booted run's includes its own boot, so it is not judged). The
        # BINARIES' own run time (lib/unit-times.awk, the per-binary timing
        # above) -- not the guest window, which on a lease also holds that
        # lease's setup (the RAM disk, the copy), paid once by the one boot
        # and 49 times pooled. "-" when the log could not say (no stamps):
        # an unmeasured run is not judged, where 0 would pass as "fast".
        local _gms _mode=booted
        _test_pooled && _mode=pooled
        _gms=$(awk -v total=1 -f "$TESTS_DIR/lib/unit-times.awk" "$TEST_LOG" 2>/dev/null) || _gms=-
        [[ "$_gms" =~ ^[0-9]+$ ]] || _gms=-
        # The contention "-" when this run did not sample it: the fallback
        # 1.00 is the idle box's figure, and the ratchet must not judge a
        # loaded run by it.
        local _cont="$TEST_CONTENTION"
        [[ "$TEST_CONTENTION_MEASURED" == 1 ]] || _cont=-
        printf '%s %s %s %s %s\n' "$ratchet_total" "$_verdict" "$_gms" "$_mode" \
            "$_cont" > "$TEST_COUNT_FILE"
    fi

    if [[ $fail -eq 0 && $pass -gt 0 && $leak_rc -eq 0 && $assert_rc -eq 0 \
          && $budget_rc -eq 0 ]]; then
        exit 0
    else
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Host-side result helpers (for tests that probe a server FROM the host with
# curl, rather than parsing PASS/FAIL out of the guest serial log). These
# replace the pass()/fail()/PASS/FAIL boilerplate each such test re-defined.
# Use test_host_pass/_fail to tally, then test_host_summary at the end.
# ---------------------------------------------------------------------------

TEST_HOST_PASS=0
TEST_HOST_FAIL=0

# curl with its error message KEPT. Tests pass -s (no progress meter), and -s
# alone also silences curl's one-line error, which the call sites then threw
# away with 2>/dev/null as well -- so a client-side TLS refusal read as "000",
# "server path broken". It was a certificate OpenSSL 3 rejected (a
# non-minimal DER serial); nothing in the log could say so. -S restores the
# message and it is appended to TEST_CURL_ERR, which test_cleanup prints when
# the test fails. Before test_setup there is no file, and stderr is dropped.
test_curl() {
    curl -S "$@" 2>>"${TEST_CURL_ERR:-/dev/null}"
}

# On a failing exit, print each distinct curl error with its count. Silent on
# success, and when no curl failed (no file, or an empty one).
_test_report_curl_errors() {
    [[ "${1:-0}" -ne 0 && -n "${TEST_CURL_ERR:-}" && -s "$TEST_CURL_ERR" ]] || return 0
    echo "--- host curl errors during this test (count, message) ---"
    sort "$TEST_CURL_ERR" | uniq -c | sort -rn | head -10
}

test_host_pass() { echo "  PASS: $1"; TEST_HOST_PASS=$((TEST_HOST_PASS + 1)); }
test_host_fail() { echo "  FAIL: $1"; TEST_HOST_FAIL=$((TEST_HOST_FAIL + 1)); }

# Print the tally and return non-zero if any check failed (or none passed).
# $1: a label for the line (e.g. the suite name + arch).
test_host_summary() {
    echo ""
    printf "%s: %d passed, %d failed\n" "$1" "$TEST_HOST_PASS" "$TEST_HOST_FAIL"
    [[ $TEST_HOST_FAIL -eq 0 && $TEST_HOST_PASS -gt 0 ]]
}

# The module name pe-set-debug wrote into a PE image's .dbgdir: the 28-byte
# debug directory, then RSDS, a 16-byte GUID and a 4-byte age, then the
# NUL-terminated name (offset 52). A packed image's is `axl-pack-stub`; a raw
# image's is its file name. Prints ERR, never an empty string, when the file
# or the section cannot be read -- so a caller comparing names can tell "could
# not look" from a name.
test_pe_module() {
    # Read from the PE section table rather than `objdump -h`: the host
    # objdump does not recognise an AArch64 PE, so it printed ERR for every
    # aa64 image and a both-arch suite could not ask the question at all.
    local f="$1" pe nsec opt tbl i ent size off
    [[ -f "$f" ]] || { echo "ERR"; return; }
    pe=$(od -An -tu4 -j60 -N4 "$f" | tr -d ' ')
    [[ "$pe" =~ ^[0-9]+$ ]] \
        && [[ "$(dd if="$f" bs=1 skip="$pe" count=4 status=none | tr -d '\0')" == PE ]] \
        || { echo "ERR"; return; }
    nsec=$(od -An -tu2 -j$((pe + 6)) -N2 "$f" | tr -d ' ')
    opt=$(od -An -tu2 -j$((pe + 20)) -N2 "$f" | tr -d ' ')
    tbl=$((pe + 24 + opt))
    for ((i = 0; i < nsec; i++)); do
        ent=$((tbl + 40 * i))
        [[ "$(dd if="$f" bs=1 skip="$ent" count=8 status=none | tr -d '\0')" == .dbgdir ]] \
            || continue
        size=$(od -An -tu4 -j$((ent + 8)) -N4 "$f" | tr -d ' ')          # VirtualSize
        (( size > 0 )) || size=$(od -An -tu4 -j$((ent + 16)) -N4 "$f" | tr -d ' ')
        off=$(od -An -tu4 -j$((ent + 20)) -N4 "$f" | tr -d ' ')           # PointerToRawData
        (( size > 52 )) || { echo "ERR"; return; }
        dd if="$f" bs=1 skip=$(( off + 52 )) count=$(( size - 52 )) status=none | tr -d '\0'
        return
    done
    echo "ERR"
}

# Liveness watchdog: GET $1 and require HTTP $3 (default 200) within a bounded
# time. The concurrency wedges this harness targets manifest as a loop that
# stops dispatching, so a probe that times out (curl code 000) is the failure
# signal — re-run after every scenario to convert a silent hang into a named
# FAIL. $2 is the label. Honors CURL_OPTS if the caller set it.
#   test_liveness_probe "https://127.0.0.1:18450/api/version" "loop alive after WS close"
test_liveness_probe() {
    local url="$1" label="$2" want="${3:-200}"
    local opts=(-s --insecure -H "Connection: close" --max-time 10)
    # set -u safe: ${CURL_OPTS+x} is empty when unset, so the length check
    # (which would error under set -u on an unset array) only runs when set.
    [[ -n "${CURL_OPTS+x}" && "${#CURL_OPTS[@]}" -gt 0 ]] && opts=("${CURL_OPTS[@]}")
    local code
    code=$(test_curl "${opts[@]}" -o /dev/null -w "%{http_code}" "$url" || true)
    if [[ "$code" == "$want" ]]; then
        test_host_pass "liveness: $label (HTTP $code)"
    else
        test_host_fail "liveness: $label (got '$code', loop may be wedged)"
    fi
}

# Firmware ASSERTs. EDK2's DEBUG firmware prints an ASSERT (and then carries
# on or hangs) where AXL misuses a protocol -- cancelling a Transmit TcpDxe is
# still processing gives "ASSERT [TcpDxe] ...LinkedList.c(594)". Neither arch
# prints it on the console: OVMF (x64) sends DEBUG output to the isa-debugcon
# port, AAVMF (aa64) to its debug UART (qemu_fw_debug_uart), so it must be
# captured.
#
# test_capture_firmware_debug  -- call BEFORE test_build_qemu_cmd.
# test_check_firmware_asserts CHECK_FN MARKER
#   Runs CHECK_FN "label" cmd... for: no ASSERT on the serial log; a CONTROL
#   that the capture holds the firmware's DEBUG output (without it an empty
#   capture would pass the next check blind); and no ASSERT on the capture.
#   X64's CONTROL is MARKER, a driver name OVMF logs while loading (e.g.
#   TcpDxe). The AARCH64 firmware logs at ERROR level only -- no load lines
#   -- so its CONTROL is that the boot wrote to the debug UART at all. On a
#   pooled guest, whose boot came before the lease and whose run may well
#   print nothing, the CONTROL reads the guest's whole log (X64: MARKER in
#   it; AARCH64: not empty): the run's part is read from that same file. A
#   pooled X64 run has a second CONTROL on its own part: OVMF logs every
#   image load ("Loading driver at ..."), the run's app included -- so a
#   pooled X64 caller's run must launch an .efi (every current one does).
test_capture_firmware_debug() {
    TEST_FW_DEBUGCON=""
    case "$TEST_ARCH" in
        X64)     TEST_FW_DEBUGCON="$TEST_TMPDIR/debugcon.log" ;;
        AARCH64) TEST_FW_DEBUGCON="$TEST_TMPDIR/firmware-debug.log" ;;
    esac
    [[ -n "$TEST_FW_DEBUGCON" ]] && export TEST_QEMU_DEBUGCON="$TEST_FW_DEBUGCON"
    return 0
}

test_check_firmware_asserts() {
    local check_fn="$1" marker="$2"
    "$check_fn" "no firmware ASSERT on the serial log" \
        _test_no_assert_in "$TEST_LOG"
    [[ -n "${TEST_FW_DEBUGCON:-}" ]] || return 0
    if [[ "$TEST_ARCH" != AARCH64 ]]; then
        if _test_pooled; then
            "$check_fn" "CONTROL: the leased guest's debugcon log holds its boot's DEBUG output ($marker loaded)" \
                grep -aq -- "$marker" "${TEST_POOL_FW_SRC:-}"
            # The run's own part: DxeCore logs every image it loads, the
            # run's app included, so a stream that went blind (offset, path,
            # tail) fails here instead of passing the ASSERT check empty.
            "$check_fn" "CONTROL: the run's part of the debugcon log holds its own image loads" \
                grep -aq -- 'Loading driver at ' "$TEST_FW_DEBUGCON"
        else
            "$check_fn" "CONTROL: the debugcon capture holds the firmware's DEBUG output ($marker loaded)" \
                grep -aq -- "$marker" "$TEST_FW_DEBUGCON"
        fi
        "$check_fn" "no firmware ASSERT on the debugcon" \
            _test_no_assert_in "$TEST_FW_DEBUGCON"
        return 0
    fi
    if _test_pooled; then
        "$check_fn" "CONTROL: the leased guest's debug UART log holds its boot's DEBUG output" \
            test -s "${TEST_POOL_FW_SRC:-}"
    else
        "$check_fn" "CONTROL: the debug UART capture holds the boot's DEBUG output" \
            test -s "$TEST_FW_DEBUGCON"
    fi
    "$check_fn" "no firmware ASSERT on the debug UART" \
        _test_no_assert_in "$TEST_FW_DEBUGCON"
}

# True only when FILE was READ and holds no ASSERT. grep -c prints 0 and exits
# 1 for "read it, found none", and exits 2 for "could not read it" -- the same
# empty-looking answer to opposite questions, so the exit status decides.
_test_no_assert_in() {
    local n rc=0
    n=$(grep -ac 'ASSERT' "$1" 2>/dev/null) || rc=$?
    (( rc <= 1 )) && [[ "$n" == 0 ]]
}

# Fail if any AXL_DEBUG_ASSERT fired in the guest. The asserts log a distinct
# marker (see include/axl/axl-debug.h); its presence means a debug-build
# invariant was violated at its cause. Cleans the serial log first.
test_refute_debug_assert() {
    test_clean_log
    if grep -qaF "$TEST_ASSERT_MARKER" "$TEST_CLEAN_LOG"; then
        test_host_fail "no AXL_DEBUG_ASSERT fired"
        echo "    --- offending markers ---"
        grep -aF "$TEST_ASSERT_MARKER" "$TEST_CLEAN_LOG" | sed 's/^/    /'
    else
        test_host_pass "no AXL_DEBUG_ASSERT fired (invariants held)"
    fi
}
