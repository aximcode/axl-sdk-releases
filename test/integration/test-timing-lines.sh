#!/bin/bash
# test-meta: arch=none needs= est=25 local-only=0 guest=none
# guest=none: harness test: the TIMING lines every launcher writes, against
#   stub QEMU and stub pool runs.
# test-timing-lines.sh -- every guest run says where its wall time went, in
# the TIMING lines the "Wall-time accounting" block of scripts/axl-common.sh
# defines, and the lines reach the test's LOG however the test captures its
# output.
#
# A stub stands in for QEMU (named qemu-system-x86_64, so the harness's own
# pid lookups find it): it prints a firmware line, the Shell banner the boot
# reading keys on, then STUB-END, and lingers if asked. Pinned, with every
# seconds value normalised to S and everything else exact:
#
#   - common-test.sh, booted: two foreground runs and two background ones --
#     one stopped by test_guest_stop, one whose QEMU ended by itself well
#     before the test did (its end must come from the watcher, not from the
#     cleanup that noticed it); then the one summary line at exit.
#   - common-test.sh, killed mid-run: a SIGTERM during a foreground run still
#     prints the open run (rc=unfinished) and the summary.
#   - common-test.sh, no AXL_TEST_TIMING_LOG and no terminal: NO line in the
#     test's stdout or stderr. A caller capturing a harness snippet's output
#     with 2>&1 (test-duration-budget-selftest.sh reads its last line) must
#     never find a TIMING line there; only the log, or a terminal, gets one.
#   - common-test.sh, pooled: a foreground and a background run on a leased
#     guest (the agent's exec stubbed), and the lease's start kind and wait.
#   - scripts/run-qemu.sh, foreground and --background: one line per guest,
#     into the timing log and NOT into the caller's captured output (46 tests
#     capture run-qemu.sh's stdout+stderr in a variable and parse it).
#   - lib/run-one.sh: the runner's line ends the test's log, a line appended
#     by the test through AXL_TEST_TIMING_LOG survives between its own output
#     (the log is opened O_APPEND), and a retry's first attempt keeps its own.
#
# Host-only: no guest boots.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
set -uo pipefail
export TEST_SKIP_RATCHET=1

PASS=0 FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORK="$(mktemp -d -t axl-timing.XXXXXXXX)"
trap 'pkill -f "[q]emu-system-x86_64 .*$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT

mkdir -p "$WORK/stub"
STUB="$WORK/stub/qemu-system-x86_64"
cat > "$STUB" <<'EOF'
#!/bin/bash
echo "BdsDxe: starting Boot0001"
sleep "${STUB_BOOT:-0.3}"
echo "UEFI Interactive Shell v2.2"
echo "___AXL_APP_OUTPUT_BEGIN___"
sleep "${STUB_RUN:-0.3}"
echo "STUB-END"
sleep "${STUB_LINGER:-0}"
EOF
chmod +x "$STUB"
printf 'MZ' > "$WORK/probe.efi"

# norm: the TIMING-shaped lines of a file, every seconds value as S.
norm() { grep -aE '^(TIMING: |axlpool run: )' "$1" | sed -E 's/[0-9]+\.[0-9]{3}/S/g'; }
# field FILE LINE# KEY: KEY's value on the LINE#-th TIMING line.
field() { grep -a '^TIMING: ' "$1" | sed -n "${2}p" | tr ' ' '\n' | sed -n "s/^$3=//p"; }
# lt A B: A < B, as decimals.
lt() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a + 0 < b + 0) }'; }
# check_lines NAME FILE EXPECTED: the normalised lines are exactly EXPECTED.
check_lines() {
    local got; got=$(norm "$2")
    if [[ "$got" == "$3" ]]; then
        pass "$1"
    else
        fail "$1"
        diff <(printf '%s\n' "$got") <(printf '%s\n' "$3") | sed 's/^/      /'
    fi
}

# A child test script: sources common-test.sh, runs BODY, its stub in place
# of QEMU.
child() {   # child NAME BODY
    {
        echo '#!/bin/bash'
        echo "source '$SCRIPT_DIR/common-test.sh'"
        echo 'TEST_ARCH=X64'
        echo 'test_setup'
        echo "$2"
    } > "$WORK/$1"
    chmod +x "$WORK/$1"
}
BOOTED='test_build_qemu_cmd; TEST_QEMU_CMD[0]='"'$STUB'"

echo "=== common-test.sh: booted runs ==="
child child-a.sh "$BOOTED
test_run_foreground 10 > /dev/null
STUB_RUN=0.1 test_run_foreground 10 > /dev/null
export STUB_LINGER=30
test_run_background
test_wait_for STUB-END 10 || exit 3
test_guest_stop
export STUB_LINGER=0
test_run_background
test_wait_for STUB-END 10 || exit 3
sleep 2
exit 0"
AXL_TEST_TIMING_LOG="$WORK/a.timing" "$WORK/child-a.sh" > "$WORK/a.out" 2>&1
rc=$?
[[ $rc -eq 0 ]] && pass "the booted child exits 0" || { fail "the booted child exits 0 (got $rc)"; tail -20 "$WORK/a.out" | sed 's/^/      /'; }
check_lines "booted: one line per run, then the summary" "$WORK/a.timing" \
"TIMING: guest kind=booted via=fg guest=S boot=S rc=0
TIMING: guest kind=booted via=fg guest=S boot=S rc=0
TIMING: guest kind=booted via=bg guest=S boot=S rc=-
TIMING: guest kind=booted via=bg guest=S boot=S rc=-
TIMING: test=child-a.sh total=S setup=S guest=S boot=S between=S post=S booted=4 pooled=0 start=- lease=-"
# The values, not just the shape. The stub boots in 0.3 s and runs 0.3 s.
g1=$(field "$WORK/a.timing" 1 guest) b1=$(field "$WORK/a.timing" 1 boot)
if lt 0.25 "$b1" && lt "$b1" 1.5 && lt 0.59 "$g1" && lt "$g1" 3; then
    pass "a foreground run: boot ~0.3 s inside a guest of ~0.6 s ($b1 of $g1)"
else
    fail "a foreground run: boot ~0.3 s inside a guest of ~0.6 s (got boot=$b1 guest=$g1)"
fi
g3=$(field "$WORK/a.timing" 3 guest)
if lt 0.59 "$g3" && lt "$g3" 5; then
    pass "a background run ends where test_guest_stop ended it, not at its 30 s linger ($g3)"
else
    fail "a background run ends where test_guest_stop ended it (guest=$g3)"
fi
g4=$(field "$WORK/a.timing" 4 guest)
if lt 0.59 "$g4" && lt "$g4" 1.6; then
    pass "a guest that ended by itself ends when it did, not when cleanup looked 2 s later ($g4)"
else
    fail "a guest that ended by itself ends when it did (guest=$g4; ~2.6 means the cleanup's clock)"
fi
sum=$(grep -a '^TIMING: guest' "$WORK/a.timing" | sed -E 's/.* guest=([0-9.]+) .*/\1/' | awk '{ s += $1 } END { printf "%.3f", s }')
tot_g=$(field "$WORK/a.timing" 5 guest) post=$(field "$WORK/a.timing" 5 post)
if awk -v a="$sum" -v b="$tot_g" 'BEGIN { d = a - b; exit !(d < 0.006 && d > -0.006) }'; then
    pass "the summary's guest is the sum of the runs ($tot_g)"
else
    fail "the summary's guest is the sum of the runs ($tot_g against $sum)"
fi
if lt 1.9 "$post"; then
    pass "post covers the time after the last guest ended ($post)"
else
    fail "post covers the time after the last guest ended (post=$post, want ~2)"
fi
if ! grep -q '^TIMING:' "$WORK/a.out"; then
    pass "with a timing log, the test's own output carries no TIMING line"
else
    fail "with a timing log, the test's own output carries no TIMING line"
fi

echo "=== common-test.sh: the watcher is stopped before its directory goes ==="
# A background guest that ends by itself just before the test does, its
# watcher caught writing the end stamp -- made deterministic by the test hook:
# the watcher ignores TERM and holds 3 s before the write. The cleanup must
# not remove the test's scratch tree while the watcher can still write into
# it (a file created mid-`rm -rf` leaves the tree behind): it stops the
# watcher and waits for it to EXIT first. The hook records any write that
# found its directory already gone -- the losing side of that race.
child child-r.sh "$BOOTED
export STUB_LINGER=0
test_run_background
test_wait_for STUB-END 10 || exit 3
sleep 0.6
exit 0"
: > "$WORK/late.txt"
AXL_TIMING_TEST_HOLD=3 AXL_TIMING_TEST_LATE="$WORK/late.txt" AXL_TEST_TIMING_LOG="$WORK/r.timing" \
    "$WORK/child-r.sh" > "$WORK/r.out" 2>&1
rc=$?
sleep 3.5   # past the hold: a watcher still alive would have written by now
if [[ $rc -eq 0 && ! -s "$WORK/late.txt" ]]; then
    pass "no watcher writes after the cleanup removed its directory"
else
    fail "no watcher writes after the cleanup removed its directory (exit $rc; late: $(cat "$WORK/late.txt"))"
fi
check_lines "...and the run still has its line" "$WORK/r.timing" \
"TIMING: guest kind=booted via=bg guest=S boot=S rc=-
TIMING: test=child-r.sh total=S setup=S guest=S boot=S between=S post=S booted=1 pooled=0 start=- lease=-"

echo "=== common-test.sh: killed mid-run ==="
child child-b.sh "$BOOTED
STUB_RUN=30 test_run_foreground 60 > /dev/null"
AXL_TEST_TIMING_LOG="$WORK/b.timing" "$WORK/child-b.sh" > "$WORK/b.out" 2>&1 &
bpid=$!
sleep 1.5
kill -TERM "$bpid"
rc=0
wait "$bpid" || rc=$?
[[ $rc -eq 143 ]] && pass "the killed child ends by SIGTERM (143)" || fail "the killed child ends by SIGTERM (got $rc)"
check_lines "killed: the open run as unfinished, then the summary" "$WORK/b.timing" \
"TIMING: guest kind=booted via=fg guest=S boot=S rc=unfinished
TIMING: test=child-b.sh total=S setup=S guest=S boot=S between=S post=S booted=1 pooled=0 start=- lease=-"
if ! pgrep -f "[q]emu-system-x86_64 .*$WORK" > /dev/null; then
    pass "no stub guest outlives the killed child"
else
    fail "a stub guest outlived the killed child"
fi

echo "=== common-test.sh: no timing log ==="
child child-c.sh "$BOOTED
test_run_foreground 10 > /dev/null"
env -u AXL_TEST_TIMING_LOG "$WORK/child-c.sh" > "$WORK/c.out" 2> "$WORK/c.err"
rc=$?
if [[ $rc -eq 0 ]] && ! grep -q 'TIMING:' "$WORK/c.out" "$WORK/c.err"; then
    pass "no timing log, no terminal: no TIMING line in the captured stdout or stderr"
else
    fail "no timing log, no terminal: no TIMING line in the captured stdout or stderr (exit $rc)"
    grep -h 'TIMING:' "$WORK/c.out" "$WORK/c.err" | sed 's/^/      /'
fi

echo "=== common-test.sh: pooled runs ==="
mkdir -p "$WORK/lease"
: > "$WORK/pool-serial.log"
: > "$WORK/firmware-debug.log"   # the guest's firmware DEBUG log, as a pooled guest has
printf 'echo hi\n' > "$WORK/child-d.nsh"
child child-d.sh '_test_pool_exec() { case "$2" in axl-pool-run.nsh) sleep 0.4 ;; esac; return 0; }
test_use_nsh
test_build_image
test_run_foreground 10 > /dev/null
test_run_background
sleep 0.6
test_guest_stop'
# The child runs twice on its lease, so its second run restores the guest
# first (common-test.sh): a stub answers the restore.
printf '#!/bin/bash\n[[ "$1" == restore ]] && exit 0\nexit 2\n' > "$WORK/axlpool-stub"
chmod +x "$WORK/axlpool-stub"
AXL_TEST_TIMING_LOG="$WORK/d.timing" AXL_POOL_AGENT=127.0.0.1:1 AXL_POOL_ARCH=X64 \
    AXL_POOL_CLASS=standard AXL_POOL_CLI="$WORK/axlpool-stub" \
    AXL_POOL_GUEST_DIR='fs1:\lease-1' AXL_POOL_HOST_DIR="$WORK/lease" \
    AXL_POOL_SERIAL="$WORK/pool-serial.log" AXL_POOL_SERIAL_OFFSET=0 \
    AXL_POOL_START=restored AXL_POOL_START_MS=380 AXL_POOL_LEASE_MS=412 \
    "$WORK/child-d.sh" > "$WORK/d.out" 2>&1
rc=$?
[[ $rc -eq 0 ]] && pass "the pooled child exits 0" || { fail "the pooled child exits 0 (got $rc)"; tail -20 "$WORK/d.out" | sed 's/^/      /'; }
check_lines "pooled: one line per run, the lease in the summary" "$WORK/d.timing" \
"TIMING: guest kind=pooled via=fg guest=S boot=- rc=0
TIMING: guest kind=pooled via=bg guest=S boot=- rc=-
TIMING: test=child-d.sh total=S setup=S guest=S boot=S between=S post=S booted=0 pooled=2 start=restored lease=412"
g1=$(field "$WORK/d.timing" 1 guest)
if lt 0.39 "$g1" && lt "$g1" 3; then pass "a pooled run covers its exec ($g1)"; else fail "a pooled run covers its exec (guest=$g1)"; fi

echo "=== run-qemu.sh ==="
RQ=(env QEMU_DIR="$WORK/stub" "$PROJECT_DIR/scripts/run-qemu.sh" --no-cpu-warn --timeout 20)
out=$(AXL_TEST_TIMING_LOG="$WORK/e.timing" "${RQ[@]}" "$WORK/probe.efi" 2>&1)
check_lines "run-qemu.sh foreground: one line" "$WORK/e.timing" \
"TIMING: run-qemu mode=fg guest=S boot=S rc=0"
if [[ "$out" != *TIMING* ]]; then
    pass "...and nothing in the caller's captured stdout+stderr"
else
    fail "...and nothing in the caller's captured stdout+stderr"
fi
b=$(field "$WORK/e.timing" 1 boot)
if lt 0.29 "$b" && lt "$b" 3; then pass "run-qemu.sh: the boot reading is the banner's (~0.3 s, got $b)"; else fail "run-qemu.sh: the boot reading (got $b)"; fi
AXL_TEST_TIMING_LOG="$WORK/f.timing" "${RQ[@]}" --background "$WORK/probe.efi" > "$WORK/f.out" 2>&1
for _ in $(seq 1 50); do [[ -s "$WORK/f.timing" ]] && break; sleep 0.2; done
check_lines "run-qemu.sh --background: one line, when the guest ends" "$WORK/f.timing" \
"TIMING: run-qemu mode=bg guest=S boot=S rc=0"
# The QEMU_PID= --background prints is still the guest's `timeout` wrapper,
# and killing it still ends the guest: the boot watcher must not be a child
# of the watchdog subshell, where `pgrep -P | head -1` would pick it instead
# (it did: test-ssh-demo-script.sh then found its guest's port still open).
STUB_LINGER=30 AXL_TEST_TIMING_LOG="$WORK/g.timing" "${RQ[@]}" --background "$WORK/probe.efi" > "$WORK/g.out" 2>&1
qp=$(sed -n 's/^QEMU_PID=//p' "$WORK/g.out")
qcomm=$(cat "/proc/${qp:-0}/comm" 2>/dev/null)
kill "${qp:-0}" 2>/dev/null
for _ in $(seq 1 20); do pgrep -f "[q]emu-system-x86_64 .*$WORK" > /dev/null || break; sleep 0.1; done
if [[ "$qcomm" == timeout ]] && ! pgrep -f "[q]emu-system-x86_64 .*$WORK" > /dev/null; then
    pass "run-qemu.sh --background: QEMU_PID is the timeout wrapper, and killing it ends the guest"
else
    fail "run-qemu.sh --background: QEMU_PID is the timeout wrapper (got '$qcomm'), and killing it ends the guest"
    pkill -f "[q]emu-system-x86_64 .*$WORK" 2>/dev/null
fi
for _ in $(seq 1 20); do [[ -s "$WORK/g.timing" ]] && break; sleep 0.1; done
if [[ "$(norm "$WORK/g.timing" | sed -E 's/ rc=[0-9]+$/ rc=N/')" == "TIMING: run-qemu mode=bg guest=S boot=S rc=N" ]]; then
    pass "...and its line still comes, when the kill ends the guest"
else
    fail "...and its line still comes, when the kill ends the guest ($(norm "$WORK/g.timing"))"
fi
out=$(env -u AXL_TEST_TIMING_LOG "${RQ[@]}" "$WORK/probe.efi" 2>&1)
if [[ "$out" != *TIMING* ]]; then
    pass "run-qemu.sh with no timing log and no terminal prints no TIMING line"
else
    fail "run-qemu.sh with no timing log and no terminal prints no TIMING line"
fi

echo "=== lib/run-one.sh ==="
cat > "$WORK/fake-ok.sh" <<'EOF'
#!/bin/bash
echo one
echo "middle" >> "$AXL_TEST_TIMING_LOG"
echo two
EOF
printf '#!/bin/bash\necho flaky\nexit 1\n' > "$WORK/fake-bad.sh"
chmod +x "$WORK/fake-ok.sh" "$WORK/fake-bad.sh"
AXL_ONE_LOGDIR="$WORK/logs" "$SCRIPT_DIR/lib/run-one.sh" "$WORK/fake-ok.sh" > /dev/null 2>&1
got=$(sed -E 's/[0-9]+\.[0-9]{3}/S/g' "$WORK/logs/fake-ok.sh.log")
want="one
middle
two
TIMING: runner test=fake-ok.sh attempt=1 via=boot wall=S cpu=S rc=0"
if [[ "$got" == "$want" ]]; then
    pass "run-one: the test's output, its appended line in place, the runner's line last"
else
    fail "run-one: the log"
    diff <(printf '%s\n' "$got") <(printf '%s\n' "$want") | sed 's/^/      /'
fi
AXL_ONE_LOGDIR="$WORK/logs" "$SCRIPT_DIR/lib/run-one.sh" --retry "$WORK/fake-bad.sh" > /dev/null 2>&1
check_lines "run-one --retry: the first attempt's log keeps its line" "$WORK/logs/fake-bad.sh.attempt1.log" \
"TIMING: runner test=fake-bad.sh attempt=1 via=boot wall=S cpu=S rc=1"
check_lines "run-one --retry: the second attempt's log has its own" "$WORK/logs/fake-bad.sh.log" \
"TIMING: runner test=fake-bad.sh attempt=2 via=boot wall=S cpu=S rc=1"

# cpu= is the CPU (user + system) of everything the test's process reaped --
# QEMU included -- over that attempt. It must SEE the work: a child doing a
# FIXED amount of work (~0.4 s of CPU here) reads it, a child sleeping 1 s
# reads near 0 (the control). Fixed work, not a wall-clock spin: under a
# loaded suite a `timeout 1` spinner got 0.12 s of CPU and failed.
SPIN='i=0; while [ $i -lt 200000 ]; do i=$((i+1)); done'
printf '#!/bin/bash\nbash -c %q\n' "$SPIN" > "$WORK/fake-spin.sh"
# ORPHANED work: the shape of `run-qemu.sh --background`, whose watchdog
# outlives the launcher that started it and is reparented away from the test.
# The test waits for it to finish (a flag file); its CPU must still count.
cat > "$WORK/fake-orphan.sh" <<EOF
#!/bin/bash
( ( bash -c '$SPIN'; : > "$WORK/orphan.done" ) & )
for _ in \$(seq 200); do [[ -e "$WORK/orphan.done" ]] && break; sleep 0.05; done
sleep 0.2
EOF
printf '#!/bin/bash\nsleep 1\n' > "$WORK/fake-sleep.sh"
chmod +x "$WORK/fake-spin.sh" "$WORK/fake-sleep.sh" "$WORK/fake-orphan.sh"
AXL_ONE_LOGDIR="$WORK/logs" "$SCRIPT_DIR/lib/run-one.sh" "$WORK/fake-spin.sh" > /dev/null 2>&1
AXL_ONE_LOGDIR="$WORK/logs" "$SCRIPT_DIR/lib/run-one.sh" "$WORK/fake-sleep.sh" > /dev/null 2>&1
AXL_ONE_LOGDIR="$WORK/logs" "$SCRIPT_DIR/lib/run-one.sh" "$WORK/fake-orphan.sh" > /dev/null 2>&1
orph=$(field "$WORK/logs/fake-orphan.sh.log" 1 cpu)
spin=$(field "$WORK/logs/fake-spin.sh.log" 1 cpu); slp=$(field "$WORK/logs/fake-sleep.sh.log" 1 cpu)
if [[ -n "$spin" ]] && lt 0.15 "$spin"; then
    pass "run-one: cpu= sees a child's fixed CPU work ($spin)"
else
    fail "run-one: cpu= for a child's fixed CPU work is [$spin], want > 0.15"
fi
if [[ -n "$orph" ]] && lt 0.15 "$orph"; then
    pass "run-one: cpu= counts work orphaned by its launcher, as a --background guest is ($orph)"
else
    fail "run-one: cpu= for work orphaned by its launcher is [$orph], want > 0.15"
fi
if [[ -n "$slp" ]] && lt "$slp" 0.1; then
    pass "run-one: cpu= for a child sleeping 1 s stays near 0 ($slp)"
else
    fail "run-one: cpu= for a child sleeping 1 s is [$slp], want < 0.1"
fi

# The subreaper is python3, which COERCES a C/unset locale to LC_CTYPE=C.UTF-8
# in its own environment (PEP 538) -- and exec would hand that to every test.
# A run-one.sh started with no locale must give its test none.
printf '#!/bin/bash\necho "ctype=${LC_CTYPE-unset} coerce=${PYTHONCOERCECLOCALE-unset}"\n' > "$WORK/fake-env.sh"
chmod +x "$WORK/fake-env.sh"
env -i PATH="$PATH" HOME="$HOME" AXL_ONE_LOGDIR="$WORK/logs" \
    "$SCRIPT_DIR/lib/run-one.sh" "$WORK/fake-env.sh" > /dev/null 2>&1
got=$(grep -m1 '^ctype=' "$WORK/logs/fake-env.sh.log")
if [[ "$got" == "ctype=unset coerce=unset" ]]; then
    pass "run-one: the subreaper leaves a test's locale as it found it (no LC_CTYPE coerced in)"
else
    fail "run-one: a test started with no locale saw [$got], want [ctype=unset coerce=unset]"
fi

echo ""
echo "timing-lines: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
