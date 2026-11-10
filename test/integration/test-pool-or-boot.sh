#!/bin/bash
# test-meta: arch=none needs= est=6 local-only=0
# test-pool-or-boot.sh -- lib/pool-or-boot.sh: run a command on a leased
# pool guest, and on a POOL miss (the wrapper's, or one the command's own
# harness met, both marked AXLPOOL-MISS:) run it booted instead -- said, the
# miss log kept. The test's OWN 77 is its SKIP, never a miss. With no pool
# socket it just runs booted. One copy: run-one.sh's integration tests and
# the pooled unit registration (unit.<App>) both use it
# (docs/AXL-Test-Guest-Pool-Design.md §6.7).
#
# A stub `axlpool` (a fake project's test/pool) stands in for the service;
# FAKE_POOL picks how it behaves. The command reports whether it ran pooled
# (AXL_POOL_AGENT set) or booted.
#
# Usage: ./test/integration/test-pool-or-boot.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
POB="$SCRIPT_DIR/lib/pool-or-boot.sh"
WORK="$(mktemp -d -t axl-pob.XXXXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}

proj="$WORK/proj"; mkdir -p "$proj/test/pool/axlpool"
: > "$proj/test/pool/axlpool/__init__.py"
printf '%s\n' \
    '"""Stub axlpool run for test-pool-or-boot.sh: FAKE_POOL = ok | miss | miss-empty | hang."""' \
    'import os' \
    'import subprocess' \
    'import sys' \
    'mode = os.environ.get("FAKE_POOL", "ok")' \
    'if os.environ.get("FAKE_ARGV"):' \
    '    open(os.environ["FAKE_ARGV"], "w").write(" ".join(sys.argv[1:sys.argv.index("--")]))' \
    'if sys.argv[1] == "run":' \
    '    if mode == "hang":' \
    '        import signal' \
    '        import time' \
    '        def on_term(signum, frame):' \
    '            time.sleep(1)' \
    '            open(os.environ["FAKE_TERM"], "a").write("released after SIGTERM\n")' \
    '            sys.exit(0)  # a clean release: the WRAPPER must still say 143' \
    '        signal.signal(signal.SIGTERM, on_term)' \
    '        open(os.environ["FAKE_TERM"], "a").write(f"leased {os.getpid()}\n")' \
    '        time.sleep(60)' \
    '        sys.exit(0)' \
    '    if mode == "miss-empty":' \
    '        print("AXLPOOL-MISS: ", file=sys.stderr)' \
    '        sys.exit(77)' \
    '    if mode == "miss":' \
    '        print("AXLPOOL-MISS: no guest: miss (no-slot): stub", file=sys.stderr)' \
    '        sys.exit(77)' \
    '    child = sys.argv[sys.argv.index("--") + 1:]' \
    '    sys.exit(subprocess.call(child, env=dict(os.environ, AXL_POOL_AGENT="127.0.0.1:1")))' \
    'sys.exit(2)' > "$proj/test/pool/axlpool/__main__.py"
cmd="$WORK/cmd.sh"
cat > "$cmd" <<'SH'
#!/bin/bash
[[ -n "${AXL_POOL_AGENT:-}" ]] && echo "MODE=pooled" || echo "MODE=booted"
[[ -n "${TEST_COUNT_FILE:-}" && ( -n "${AXL_POOL_AGENT:-}" || -z "${FAKE_BOOTED_NO_COUNT:-}" ) ]] \
    && echo "1 green 5 pooled 1.00" > "$TEST_COUNT_FILE"
[[ "${FAKE_RC:-0}" == harness-miss && -n "${AXL_POOL_AGENT:-}" ]] \
    && { echo "AXLPOOL-MISS: the agent could not run it" >&2; exit 77; }
[[ "${FAKE_RC:-0}" == harness-miss ]] && exit 0
[[ -n "${FAKE_SLEEP:-}" ]] && sleep "$FAKE_SLEEP"
exit "${FAKE_RC:-0}"
SH
chmod +x "$cmd"

pob() {   # pob [VAR=VALUE...] -> "rc=N | mode lines | miss lines"
    local out rc
    out=$(env "$@" "$POB" --class unit --arch X64 --tree "$proj" --log "$WORK/run.log" -- "$cmd" 2>&1); rc=$?
    printf 'rc=%s|%s|%s\n' "$rc" "$(grep -o 'MODE=[a-z]*' <<< "$out" | paste -sd,)" \
        "$(grep -c '^pool-or-boot: pool miss' <<< "$out")"
}

echo "=== lib/pool-or-boot.sh ==="
check "no pool socket: the command runs booted" "$(pob -u AXL_ONE_POOL_SOCK)" "rc=0|MODE=booted|0"
check "a serving pool: the command runs pooled, once" "$(pob AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=ok)" "rc=0|MODE=pooled|0"
check "...its status passes through" "$(pob AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=ok FAKE_RC=3)" "rc=3|MODE=pooled|0"
check "a pool miss: said, then the command runs BOOTED" "$(pob AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=miss)" "rc=0|MODE=booted|1"
check "...the miss's log is kept beside the run's, named as run-one.sh names one" \
    "$(grep -c '^AXLPOOL-MISS: ' "$WORK/run.pool-miss.log" 2>/dev/null)" "1"
check "a run with no miss leaves no miss log (a stale one is cleared)" \
    "$(pob AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=ok >/dev/null; ls "$WORK/run.pool-miss.log" 2>/dev/null | grep -c .)" "0"
echo "1 green 5 pooled 1.00" > "$WORK/count"
pob AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=ok FAKE_RC=harness-miss TEST_COUNT_FILE="$WORK/count" FAKE_BOOTED_NO_COUNT=1 > /dev/null
check "a miss clears the pooled attempt's count: a booted rerun that writes none leaves NONE" \
    "$(ls "$WORK/count" 2>/dev/null | grep -c .)" "0"
check "a miss whose marker gives NO reason is still a miss, not the test's SKIP" \
    "$(pob AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=miss-empty)" "rc=0|MODE=booted|1"
check "a miss the command's own harness met: rerun booted too" \
    "$(pob AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=ok FAKE_RC=harness-miss)" "rc=0|MODE=pooled,MODE=booted|1"
check "the command's OWN 77 is its SKIP: no rerun" \
    "$(pob AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=ok FAKE_RC=77)" "rc=77|MODE=pooled|0"
out=$("$POB" --class unit 2>&1); rc=$?
check "usage errors are refused (2), not run" "$rc $(grep -c 'usage' <<< "$out")" "2 1"
out=$(timeout 5 "$POB" --class unit --arch X64 --tree "$proj" --log "$WORK/run.log" --log 2>&1); rc=$?
check "a trailing option with no value: refused (2), not a hang" "$rc $(grep -c 'usage' <<< "$out")" "2 1"
out=$(timeout 5 "$POB" --class unit --arch X64 --tree "$proj" --log "$WORK/run.log" --bogus x -- "$cmd" 2>&1); rc=$?
check "an unknown option: refused (2), not run as the command" "$rc $(grep -c 'usage' <<< "$out")" "2 1"

# --timeout N: the whole run is bounded -- pooled (the attempt, wrapper and
# all, as run-one.sh bounds its own) or booted -- and the limit's 124 is the
# status. The quiet slot (scripts/qloop-verdict.sh) runs under one.
pobt() {   # pobt LIMIT [VAR=VALUE...] -> "rc=N | mode lines | took<5"
    local lim="$1" out rc t0; shift
    t0=$SECONDS
    out=$(env "$@" "$POB" --class unit --arch X64 --tree "$proj" --log "$WORK/run.log" \
              --timeout "$lim" -- "$cmd" 2>&1); rc=$?
    printf 'rc=%s|%s|%s\n' "$rc" "$(grep -o 'MODE=[a-z]*' <<< "$out" | paste -sd,)" \
        "$(( SECONDS - t0 < 5 ))"
}
check "--timeout: a pooled run past it is cut off, 124" \
    "$(pobt 1 AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=ok FAKE_SLEEP=30)" "rc=124|MODE=pooled|1"
check "--timeout: a booted run past it is cut off, 124" \
    "$(pobt 1 -u AXL_ONE_POOL_SOCK FAKE_SLEEP=30)" "rc=124|MODE=booted|1"
check "--timeout: a run inside it is untouched" \
    "$(pobt 20 AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=ok FAKE_RC=3)" "rc=3|MODE=pooled|1"
# AXL_IT_POOL_AGENT=tree (the CI presets): the lease runs the TREE's own
# agent build, so a CI run exercises the commit's agent, not the service's.
pob AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=ok FAKE_ARGV="$WORK/argv" AXL_IT_POOL_AGENT=tree > /dev/null
check "AXL_IT_POOL_AGENT=tree leases with --agent tree" "$(grep -o -- '--agent [a-z]*' "$WORK/argv")" "--agent tree"
pob -u AXL_IT_POOL_AGENT AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=ok FAKE_ARGV="$WORK/argv" > /dev/null
check "...and unset, the service's agent (no --agent)" "$(grep -c -- '--agent' "$WORK/argv")" "0"
# AXL_POOL_CODE_ID (the service's code identity, asked once per run by the
# CTest registration): every lease is held to it, so a service restarted
# with other code mid-run misses instead of running the test.
pob AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=ok FAKE_ARGV="$WORK/argv" AXL_POOL_CODE_ID=c0de > /dev/null
check "AXL_POOL_CODE_ID leases with --expect-code" "$(grep -o -- '--expect-code [a-z0-9]*' "$WORK/argv")" "--expect-code c0de"
pob -u AXL_POOL_CODE_ID AXL_ONE_POOL_SOCK=/stub.sock FAKE_POOL=ok FAKE_ARGV="$WORK/argv" > /dev/null
check "...and unset, no --expect-code" "$(grep -c -- '--expect-code' "$WORK/argv")" "0"
out=$("$POB" --class unit --arch X64 --tree "$proj" --log "$WORK/run.log" --timeout 0 -- "$cmd" 2>&1); rc=$?
check "--timeout 0 (GNU timeout: NO limit) is refused (2), not run unbounded" \
    "$rc $(grep -c 'usage' <<< "$out")" "2 1"

# A SIGTERM to the wrapper reaches the lease client AT ONCE. ctest's
# TIMEOUT_SIGNAL_NAME signals the test's own process -- this script, or
# run-one.sh -- and nothing else. A bash with no trap dies of it and leaves
# the client holding its lease, orphaned; a bash with a trap but a
# FOREGROUND child defers the trap until the child ends, and ctest KILLs the
# tree first. Either way the client's own TERM handling (forward to its test,
# release) never ran. The stub client (FAKE_POOL=hang) writes `leased <pid>`,
# then `released after SIGTERM` if a TERM reaches it -- a second later, as a
# release takes time: a wrapper that exits before its client has finished
# leaves that client outside the tree ctest's KILL reaches.
term_run() {   # term_run CMD... -> "exited=0|1 took<5=0|1 rc=N"; $WORK/term, $WORK/term.out
    local pid i t0 exited=0 cp took rc
    : > "$WORK/term"; : > "$WORK/term.out"
    FAKE_POOL=hang FAKE_TERM="$WORK/term" "$@" > "$WORK/term.out" 2>&1 &
    pid=$!
    for ((i = 0; i < 100; i++)); do grep -q '^leased ' "$WORK/term" && break; sleep 0.1; done
    t0=$SECONDS
    kill -TERM "$pid" 2>/dev/null
    for ((i = 0; i < 100; i++)); do kill -0 "$pid" 2>/dev/null || { exited=1; break; }; sleep 0.1; done
    took=$(( SECONDS - t0 < 5 ))
    (( exited )) || kill -KILL "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null; rc=$?
    # 143: the status says a TERM stopped it, whatever the client returned.
    printf 'exited=%s took<5=%s rc=%s\n' "$exited" "$took" "$rc"
    # A client the TERM never reached (the defect this pins) is still
    # sleeping: end it, by its PID, once it is shown to be the stub's.
    cp=$(sed -n 's/^leased \([0-9]*\)$/\1/p' "$WORK/term")
    if [[ -n "$cp" && "$(readlink "/proc/$cp/cwd" 2>/dev/null)" == "$proj/test/pool" ]]; then
        kill -KILL "$cp" 2>/dev/null
    fi
    return 0
}
leased_released() { sed 's/^leased [0-9]*$/leased/' "$WORK/term" | paste -sd,; }
check "SIGTERM to pool-or-boot.sh while pooled: it exits promptly" \
    "$(term_run env AXL_ONE_POOL_SOCK=/stub.sock "$POB" --class unit --arch X64 --tree "$proj" \
        --log "$WORK/run.log" -- "$cmd")" "exited=1 took<5=1 rc=143"
check "...having passed the TERM to the lease client, which released" \
    "$(leased_released)" "leased,released after SIGTERM"
check "...and runs nothing booted after it (a TERM is not a pool miss)" \
    "$(grep -c -e 'MODE=' -e 'pool miss' "$WORK/term.out")" "0"
check "...the same under --timeout (the quiet slot's form)" \
    "$(term_run env AXL_ONE_POOL_SOCK=/stub.sock "$POB" --class unit --arch X64 --tree "$proj" \
        --log "$WORK/run.log" --timeout 300 -- "$cmd") $(leased_released)" \
    "exited=1 took<5=1 rc=143 leased,released after SIGTERM"

# run-one.sh's pooled attempt is the same pool_attempt, sourced: a TERM to
# run-one reaches the client too, ends the run with no booted rerun and no
# retry, and says it was stopped rather than claiming a verdict.
T="$WORK/tests"; RL="$WORK/rologs"; mkdir -p "$T" "$RL"
printf '#!/bin/bash\n# test-meta: arch=both needs= est=1 local-only=0 guest=standard\necho "MODE=ran"\n' \
    > "$T/test-hang-qemu.sh"
check "SIGTERM to run-one.sh while pooled: it exits promptly" \
    "$(term_run env -u AXL_TEST_CACHE AXL_ONE_LOGDIR="$RL" AXL_ONE_POOL_SOCK=/stub.sock \
        AXL_ONE_PROJECT_DIR="$proj" AXL_TEST_TIMEOUT=60 bash "$SCRIPT_DIR/lib/run-one.sh" --retry "$T/test-hang-qemu.sh")" \
    "exited=1 took<5=1 rc=143"
check "...having passed the TERM to the lease client, which released" \
    "$(leased_released)" "leased,released after SIGTERM"
check "...with no booted rerun and no retry" \
    "$(grep -c 'MODE=ran' "$RL/test-hang-qemu.sh.log"; ls "$RL" | grep -c 'attempt1')" "$(printf '0\n0')"
check "...and its line says it was stopped, not how the test did" \
    "$(grep -cxE '  test-hang-qemu\.sh FAIL [0-9]+s \(stopped by SIGTERM from outside -- not its own 60s limit, and not a verdict\)' "$WORK/term.out")" "1"

echo ""
echo "pool-or-boot: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
