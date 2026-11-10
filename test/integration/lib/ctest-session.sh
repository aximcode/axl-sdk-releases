#!/bin/bash
# ctest-session.sh -- run ctest in a session of its own, and take it down.
#
# The CTest front ends (run-integration.sh, verify.sh; slice 5a step 6) each
# start one or more ctest runs and must be able to stop every test those runs
# started -- and the guests those tests booted -- on an interrupt. ctest does
# not pass a SIGTERM on to its tests, and run-one runs each test under GNU
# timeout, which moves it into a process group of its own, so neither a
# signal to ctest nor a group kill reaches them. A SESSION does: everything
# ctest starts stays in its session unless it calls setsid itself, and the
# guests that do are reaped by path (reap_pool).
#
# Sourced. Each run is NAMED, so a caller can hold several at once:
#   cts_start NAME OUTFILE ENV_ARRAY ARGV_ARRAY
#       env "${ENV_ARRAY[@]}" setsid --fork --wait "${ARGV_ARRAY[@]}" > OUTFILE &
#       ENV_ARRAY holds `env` arguments (-u VAR and VAR=value); ARGV_ARRAY is
#       the ctest command. setsid --fork --wait ALWAYS forks and waits, so the
#       background pid is the waiter and the ctest is its child, found by
#       NAME: without --fork setsid forks only when its caller leads a process
#       group, and a backgrounded shell function adds a subshell level -- the
#       pid in hand was sometimes ctest and sometimes not.
#   cts_ctest NAME     the ctest's pid (its session's id), or nothing yet
#   cts_wait NAME      wait for the run; its ctest's exit status
#   cts_wait_any       wait for whichever run ends first: its name in
#                      CTS_DONE, its status returned (127: none held)
#   cts_inherited      the AXL_IT_/PREP_/ONE_/UNIT_* and AXL_TEST_CACHE this
#                      shell holds, which a front end must not pass on
#   cts_kill NAME      take the run down: its ctest's session, then the waiter
#   cts_kill_all       every run still held
#   cts_own NAME OWNER BASE FOREIGN   what run NAME's tree would clean up
#   cts_reap_after JUNIT BASE   after a run: reap leftover guests if its setup ran
#   cts_take_down      an interrupt: kill every run, then clean up (reap,
#                      base, owner record) only those this script owns
#   reap_pool BASE     guests (qemu-system*, virtiofsd) whose argv names BASE
#
# CTS_WHO names the caller in warnings (default "ctest-session").

declare -gA CTS_PID=() CTS_SID=() CTS_OWNER_FILE=() CTS_TMPBASE=() CTS_FOREIGN=()

_cts_resolve() {   # NAME -> CTS_SID[NAME], while the waiter is alive
    local name="$1" p="${CTS_PID[$1]:-}"
    [[ -n "$p" && -z "${CTS_SID[$name]:-}" ]] || return 0
    CTS_SID[$name]=$(pgrep -P "$p" -x ctest 2>/dev/null | head -1)
}

cts_start() {
    local name="$1" out="$2" i
    local -n _cts_env="$3" _cts_argv="$4"
    env ${_cts_env[@]+"${_cts_env[@]}"} setsid --fork --wait "${_cts_argv[@]}" > "$out" 2>&1 &
    CTS_PID[$name]=$!
    CTS_SID[$name]=""
    for ((i = 0; i < 100; i++)); do
        _cts_resolve "$name"
        [[ -n "${CTS_SID[$name]}" ]] && return 0
        kill -0 "${CTS_PID[$name]}" 2>/dev/null || return 0   # already finished
        sleep 0.05
    done
    echo "${CTS_WHO:-ctest-session}: warning: could not find the ctest of run '$name'" \
         "(under setsid pid ${CTS_PID[$name]}); an interrupt resolves it then" >&2
}

cts_ctest() { printf '%s' "${CTS_SID[$1]:-}"; }

cts_wait() {
    local name="$1" rc
    wait "${CTS_PID[$name]}"; rc=$?
    unset 'CTS_PID[$name]' 'CTS_SID[$name]'
    return "$rc"
}

# cts_wait_any -- wait for whichever started run ends first; its name in
# CTS_DONE, its status returned (127: none held). Waiting in a FIXED order
# times every later run to the earlier one's end: verify reported aa64's
# 119 s run as 195 s, x64's.
# A run that has ALREADY ended is collected first, by a plain `wait PID`:
# once another job is started after it exits, bash drops it from the job
# table, `wait -n` says "no such job" and waits for a different pid, and
# that run's status is lost (test-ctest-session.sh reproduces it). A plain
# wait still returns it. (wait -n -p: bash 5.1.)
cts_wait_any() {
    local n done_pid rc
    local -a pids=()
    CTS_DONE=""
    for n in "${!CTS_PID[@]}"; do
        if ! kill -0 "${CTS_PID[$n]}" 2> /dev/null; then
            wait "${CTS_PID[$n]}"; rc=$?
            CTS_DONE="$n"
            unset 'CTS_PID[$n]' 'CTS_SID[$n]'
            return "$rc"
        fi
        pids+=("${CTS_PID[$n]}")
    done
    (( ${#pids[@]} )) || return 127
    wait -n -p done_pid "${pids[@]}"; rc=$?
    for n in "${!CTS_PID[@]}"; do
        [[ "${CTS_PID[$n]}" == "${done_pid:-}" ]] && CTS_DONE="$n"
    done
    [[ -n "$CTS_DONE" ]] && unset 'CTS_PID[$CTS_DONE]' 'CTS_SID[$CTS_DONE]'
    return "$rc"
}

# cts_own NAME OWNER_FILE BASE FOREIGN -- what run NAME's tree would clean
# up: its owner record (lib/ctest-tmpbase.sh) and its temp base, unless the
# base is FOREIGN (1: the caller's own directory, CTS_BASE_FOREIGN from
# cts_tree_base). cts_take_down acts on it only while the record names this
# run's ctest. Call it BEFORE cts_start: a signal right after the start must
# already know what to clean.
cts_own() { CTS_OWNER_FILE[$1]="$2"; CTS_TMPBASE[$1]="$3"; CTS_FOREIGN[$1]="${4:?cts_own: FOREIGN 0|1}"; }

# cts_take_down -- an interrupt's cleanup, for every run still held:
#   1. which runs this script OWNS -- the owner record names its ctest --
#      asked BEFORE the kill (cleanup would remove the record);
#   2. every run's session killed (cts_kill_all);
#   3. for an owned run only, AFTER the kill (a guest started meanwhile is
#      then reaped too): its guests reaped, and what the cleanup fixture
#      would have removed, removed -- the temp base (its disks fill
#      /dev/shm; never a FOREIGN one) and the owner record.
# A run that lost the race to another must not touch that run's base: it
# sits at the same path.
cts_take_down() {
    local n o
    local -a owned=()
    for n in "${!CTS_PID[@]}"; do
        _cts_resolve "$n"
        o=$(cts_ctest "$n")
        [[ -n "$o" && -n "${CTS_OWNER_FILE[$n]:-}" ]] || continue
        [[ "$(cat "${CTS_OWNER_FILE[$n]}" 2>/dev/null)" == "$o" ]] && owned+=("$n")
    done
    cts_kill_all
    for n in ${owned[@]+"${owned[@]}"}; do
        reap_pool "${CTS_TMPBASE[$n]:-}"
        if [[ "${CTS_FOREIGN[$n]:-1}" == 0 && -n "${CTS_TMPBASE[$n]:-}" ]]; then
            rm -rf "${CTS_TMPBASE[$n]:?}"
        fi
        rm -f "${CTS_OWNER_FILE[$n]}"
    done
    return 0
}

# cts_reap_after JUNIT BASE -- after a run: reap a guest that outlived its
# test, only if THIS run's own setup fixture ran (its JUnit file says
# integration.tmp-setup status="run") -- another run's base sits at the same
# path, and a run whose setup was refused owns nothing.
cts_reap_after() {
    grep -q '<testcase name="integration.tmp-setup"[^>]*status="run"' "$1" 2>/dev/null || return 0
    reap_pool "$2"
}

# cts_inherited -- the run settings this shell holds that a front end must
# NOT pass on, one name per line: an outer run's, or the caller's shell's,
# would override a flag (AXL_TEST_CACHE re-enables the cache under
# --no-cache, AXL_IT_POOL turns the pool on), the prep, or a unit run's
# scales. ONE list for both front ends.
cts_inherited() {
    local v
    for v in "${!AXL_IT_@}" "${!AXL_PREP_@}" "${!AXL_ONE_@}" "${!AXL_UNIT_@}"; do
        printf '%s\n' "$v"
    done
    # And three the unit harness reads: left in a shell, TEST_APPS_ONLY turns
    # a "full" unit job into a subset, TEST_SKIP_RATCHET skips its ratchet,
    # and TEST_MAX_DURATION loosens its budget unannounced (it is raised in
    # test-axl.sh, in a commit that says why).
    for v in AXL_TEST_CACHE TEST_APPS_ONLY TEST_SKIP_RATCHET TEST_MAX_DURATION; do
        [[ -n "${!v+set}" ]] && echo "$v"
    done
    return 0
}

cts_kill() {
    local name="$1" sid i
    [[ -n "${CTS_PID[$name]:-}" ]] || return 0
    # A ctest not yet found is resolved NOW, while its waiter lives: once the
    # waiter is gone the ctest is reparented and -P cannot find it -- and it
    # would run on alone in its own session.
    _cts_resolve "$name"
    sid="${CTS_SID[$name]:-}"
    kill -TERM "${CTS_PID[$name]}" 2>/dev/null
    if [[ -n "$sid" ]]; then
        pkill -TERM -s "$sid" 2>/dev/null
        for ((i = 0; i < 40; i++)); do pgrep -s "$sid" >/dev/null 2>&1 || break; sleep 0.25; done
        pkill -KILL -s "$sid" 2>/dev/null
    fi
    unset 'CTS_PID[$name]' 'CTS_SID[$name]'
}

cts_kill_all() {
    local name
    for name in "${!CTS_PID[@]}"; do cts_kill "$name"; done
}

# THE GUESTS, reaped BEFORE their disks are removed and never after. A guest
# that outlives its disk keeps it open as a DELETED inode: the directory is
# gone, so every directory-based check reports clean while tmpfs cannot
# reclaim the pages. 365 MB accumulated that way from five orphans of a
# single interrupted run.
#
# For anything the session kill missed (a guest that called setsid itself).
# Two filters, and BOTH matter: the base on the command line (every guest
# carries it via `-drive file=$TMPDIR/...`), and the process actually being a
# guest -- `pgrep -f` alone matches any process whose command line merely
# CONTAINS the path (an editor, a grep, an agent shell), and killing one of
# those would be far worse than the leak.
_guest_pids() {
    local base="$1" pid comm
    for pid in $(pgrep -f -- "$base" 2>/dev/null); do
        comm=$(cat "/proc/$pid/comm" 2>/dev/null) || continue
        case "$comm" in
            qemu-system*|virtiofsd) echo "$pid" ;;
        esac
    done
}

reap_pool() {
    local base="$1" i pids
    [[ -n "$base" ]] || return 0
    pids=$(_guest_pids "$base"); [[ -n "$pids" ]] || return 0
    # shellcheck disable=SC2086
    kill -TERM $pids 2>/dev/null
    # Bounded wait, then escalate: without it, a removal right after can
    # still beat a slow guest and recreate the very leak this prevents.
    for ((i = 0; i < 40; i++)); do
        pids=$(_guest_pids "$base"); [[ -n "$pids" ]] || return 0
        sleep 0.25
    done
    # shellcheck disable=SC2086
    kill -KILL $pids 2>/dev/null
    sleep 0.2
}

# ---------------------------------------------------------------------------
# THE TREE a front end runs ctest on.
#
#   cts_tree_current TREE PRESET REPO LOGFILE
#       Configure TREE (cmake --preset PRESET, from REPO) when it has no
#       registration, or one OLDER than its template -- CMake reconfigures on
#       a build, never on a bare ctest, so after a pull it would run
#       yesterday's. Says so on stdout; non-zero, named on stderr, on failure.
#   cts_tree_busy OWNER_FILE
#       Print the pid of a LIVE ctest run recorded there (status 0), else
#       nothing (status 1). One run per tree: ask it before anything -- a
#       reconfigure included -- touches the tree.
#   cts_tree_base TREE ENV_ARRAY
#       Read the temp base and the owner record from the tree's own setup
#       fixture (ctest -N -V of integration.tmp-setup, in ENV_ARRAY's
#       environment) into CTS_BASE and CTS_OWNER_REG, and CTS_BASE_FOREIGN
#       (1: the caller's own base, passed --foreign); status 1 if unreadable.
# _cts_tree_ok TREE REPO -- configured by the pinned cmake, with a CTest
# registration no older than its template.
_cts_tree_ok() {
    ! axl_cmake_tree_foreign "$1" && [[ -f "$1/CTestTestfile.cmake" \
        && ! "$2/cmake/AxlIntegrationTests.cmake.in" -nt "$1/axl-integration-ctest.cmake" ]]
}
cts_tree_current() {
    local tree="$1" preset="$2" repo="$3" log="$4" fresh=() lockfd="" rc=0
    # shellcheck source=../../../scripts/axl-cmake-pin.sh
    source "$repo/scripts/axl-cmake-pin.sh"
    _cts_tree_ok "$tree" "$repo" && return 0
    # Any configure here runs under the tree's build lock (axl-build.sh's,
    # which a build there holds), and the question is asked AGAIN once it is
    # held: a build that was running may have just configured it.
    if command -v flock > /dev/null 2>&1; then
        # The lock lives BESIDE the tree (build/<preset>.axl-build.lock), and a
        # clean checkout has no build/ yet -- CI's release suite gets here
        # first, from its unit step. cmake would create the tree anyway.
        mkdir -p "$tree" 2>/dev/null
        if ! { exec {lockfd}> "$tree.axl-build.lock"; } 2>/dev/null || ! flock "$lockfd"; then
            echo "${CTS_WHO:-ctest-session}: cannot lock $tree.axl-build.lock to configure $tree" >&2
            return 1
        fi
    fi
    if _cts_tree_ok "$tree" "$repo"; then
        [[ -n "$lockfd" ]] && exec {lockfd}>&-
        return 0
    fi
    if axl_cmake_tree_foreign "$tree"; then
        # Its registration came from ANOTHER cmake (the box's, before the pin):
        # current or not, it is redone by the pinned one, from scratch. The old
        # build.ninja and registration go first, so a --fresh that fails
        # leaves a tree that reads as unconfigured, never as current.
        echo "${CTS_WHO:-ctest-session}: $(axl_cmake_foreign_reason "$tree")"
        rm -f "$tree/build.ninja" "$tree/CTestTestfile.cmake" "$tree/axl-integration-ctest.cmake"
        fresh=(--fresh)
    else
        echo "${CTS_WHO:-ctest-session}: configuring $tree (cmake --preset $preset)..."
    fi
    (cd "$repo" && cmake "${fresh[@]}" --preset "$preset") > "$log" 2>&1 || rc=$?
    [[ -n "$lockfd" ]] && exec {lockfd}>&-
    if (( rc != 0 )); then
        echo "${CTS_WHO:-ctest-session}: cannot configure $tree -- see $log" >&2
        return 1
    fi
    # configure_file leaves an UNCHANGED output as it was, mtime included, so
    # without this the copy stays "older than its template" and every later
    # run would configure again.
    touch "$tree/axl-integration-ctest.cmake"
}

cts_tree_busy() {
    local o
    o=$(cat "$1" 2>/dev/null) || return 1
    [[ "$o" =~ ^[0-9]+$ ]] && kill -0 "$o" 2>/dev/null || return 1
    printf '%s' "$o"
}

cts_tree_base() {
    local tree="$1" tb
    local -n _ctb_env="$2"
    tb=$(env ${_ctb_env[@]+"${_ctb_env[@]}"} ctest --test-dir "$tree" -N -V \
             -R '^integration\.tmp-setup$' 2>/dev/null | grep -m1 'Test command:')
    CTS_BASE=$(sed -nE 's/.*"setup" "([^"]+)" "([^"]+)".*/\1/p' <<< "$tb")
    CTS_OWNER_REG=$(sed -nE 's/.*"setup" "([^"]+)" "([^"]+)".*/\2/p' <<< "$tb")
    CTS_BASE_RAW="$tb"
    # FOREIGN: the registration hands tmp-setup --foreign exactly when the
    # base is the caller's own directory -- the one fact that decides whether
    # an interrupt may remove it, read where it is decided.
    CTS_BASE_FOREIGN=0
    [[ "$tb" == *'"--foreign"'* ]] && CTS_BASE_FOREIGN=1
    [[ -n "$CTS_BASE" && -n "$CTS_OWNER_REG" ]]
}
