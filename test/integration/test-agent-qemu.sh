#!/bin/bash
# test-meta: arch=both needs= est=120 local-only=0 builds=agent,exit-status-selftest,shell-launcher guest=none
# guest=none: it tests the pool's own agent, on Shells booted -nonest and
#   nested: the guest the agent runs in is the subject.
# test-agent-qemu.sh -- tools/agent.c's protocol, driven over HTTP from the
# host by lib/agent-cases.py (every field asserted whole). Two guests:
#   nonest  the Shell booted -nonest, as a pool guest's is: every outcome is
#           definite, output and stderr are captured, refusals refuse
#   nested  the default Shell: the agent still runs lines and captures their
#           output, and says it cannot tell whether they ran (unknown)
# Each ends with POST /quit, and the agent's OWN leak verdict -- printed as
# it returns to the Shell -- must read clean: per-request leaks in a server
# that otherwise never exits are invisible to every other check.
#
# Usage: ./test/integration/test-agent-qemu.sh [--arch X64|AARCH64]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
export TEST_SKIP_RATCHET=1
# -nonest comes from the launcher; never let auto mode boot without it.
export AXL_SHELL_LAUNCHER=1

TEST_ARCH="X64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *) shift ;;
    esac
done
declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
NATIVE="${_NATIVE[$TEST_ARCH]:-x64}"

"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NATIVE" agent exit-status-selftest \
    shell-launcher 2>&1 | tail -1
PREFIX="$("$PROJECT_DIR/scripts/build-prefix.sh" --abs "$NATIVE")"
WORK="$(mktemp -d -t axl-agent.XXXXXXXX)"
QPIDS=()
cleanup() {
    local p
    for p in "${QPIDS[@]}"; do kill "$p" 2>/dev/null; done
    rm -rf "$WORK"
}
trap cleanup EXIT
# big.txt: UCS-2 with a BOM, an x then 524,300 U+00E9 (lib/agent-cases.py's
# BIG), for the 1 MiB truncation case.
python3 -c 'import sys; sys.stdout.buffer.write(b"\xff\xfe" + ("x" + "\u00e9" * 524300).encode("utf-16-le"))' \
    > "$WORK/big.txt"
# Scripts: one whose `exit /b 3` ends only itself, one that turns nonesting
# off and then poisons the Shell the agent runs in with an `exit`
# (lib/agent-cases.py runs it last).
printf 'echo before\r\nexit /b 3\r\necho after\r\n' > "$WORK/sexitb.nsh"
printf 'set nonesting False\r\nexit\r\necho after-exit\r\n' > "$WORK/snest.nsh"

PASS=0
FAIL=0

# run_mode <nonest|nested>: boot a guest running the agent, then the cases.
run_mode() {
    local mode="$1" log="$WORK/$1.run" nsh="$WORK/$1.nsh" opts=""
    [[ "$mode" == nonest ]] && opts="-nonest"
    printf 'fs0:\\agent.efi --port 8080\n' > "$nsh"
    AXL_SHELL_OPTIONS="$opts" timeout 60s "$PROJECT_DIR/scripts/run-qemu.sh" \
        --arch "$TEST_ARCH" --background --timeout 300 --net --hostfwd auto:8080 \
        --nsh "$nsh" --extra "$PREFIX/exit-status-selftest.efi" --extra "$WORK/big.txt" --extra "$WORK/sexitb.nsh" --extra "$WORK/snest.nsh" \
        "$PREFIX/tools/agent.efi" > "$log" 2>&1
    local qpid port serial
    qpid=$(grep -oP '^QEMU_PID=\K\d+' "$log")
    port=$(grep -oP '^HOSTFWD_8080=\K\d+' "$log")
    serial=$(grep -oP '^SERIAL_LOG=\K.*' "$log")
    [[ -n "$qpid" ]] && QPIDS+=("$qpid")
    if [[ -z "$qpid" || -z "$port" ]]; then
        echo "  FAIL: $mode: guest launched"
        sed 's/^/      /' "$log" | head -8
        FAIL=$((FAIL + 1))
        return
    fi
    local out
    out=$(python3 "$SCRIPT_DIR/lib/agent-cases.py" 127.0.0.1 "$port" "$TEST_ARCH" "$mode" "$serial")
    echo "$out" | grep -v '^agent-cases:'
    PASS=$((PASS + $(grep -c '^  PASS:' <<< "$out")))
    FAIL=$((FAIL + $(grep -c '^  FAIL:' <<< "$out")))
    # After /quit, the agent's own leak verdict, clean, and no report.
    # Children's verdicts go to their captured stderr, so the console holds
    # the agent's -- plus, in the -nonest guest, the launcher's: its cases end
    # by poisoning the Shell with an `exit`, so the Shell exits when the
    # agent returns and the launcher that started it returns too. An exact
    # count per mode, which also proves that Shell really exited.
    local i verdicts reports want_verdicts=1
    [[ "$mode" == nonest ]] && want_verdicts=2
    # Wait for every verdict this mode expects (the launcher's follows the
    # agent's), or for a report, before counting.
    for i in $(seq 1 75); do
        verdicts=$(tr -d '\r' < "$serial" | grep -acE '^[0-9:.]+ \[INFO\]  mem: no leaks detected$')
        reports=$(tr -d '\r' < "$serial" | grep -ac '=== AxlMem leak report:')
        (( verdicts >= want_verdicts || reports > 0 )) && break
        sleep 0.2
    done
    sleep 0.5   # a verdict beyond the expected count must have time to show
    verdicts=$(tr -d '\r' < "$serial" | grep -acE '^[0-9:.]+ \[INFO\]  mem: no leaks detected$')
    reports=$(tr -d '\r' < "$serial" | grep -ac '=== AxlMem leak report:')
    if [[ "$verdicts" == "$want_verdicts" && "$reports" == 0 ]]; then
        echo "  PASS: $mode: the agent quits with a clean leak verdict"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $mode: the agent quits with a clean leak verdict (verdicts=$verdicts, want $want_verdicts; reports=$reports)"
        tr -d '\r' < "$serial" | grep -a -A6 'AxlMem leak report' | head -8 | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
    # The ready line, exact: what a pool waits for on the console.
    local want="AXL-AGENT-READY port=8080 nonest=$([[ $mode == nonest ]] && echo 1 || echo 0)"
    if tr -d '\r' < "$serial" | grep -aqx "$want"; then
        echo "  PASS: $mode: console ready line"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $mode: console ready line (want '$want')"
        tr -d '\r' < "$serial" | grep -a 'AXL-AGENT\|agent:' | head -3 | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
    kill "$qpid" 2>/dev/null
}

echo "=== agent protocol ($TEST_ARCH) ==="
run_mode nonest
run_mode nested
echo "agent ($TEST_ARCH): $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
