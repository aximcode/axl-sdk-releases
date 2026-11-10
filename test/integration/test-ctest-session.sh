#!/bin/bash
# test-meta: arch=none needs= est=3 local-only=0
# test-ctest-session.sh -- test/integration/lib/ctest-session.sh's waits, the
# library the CTest front ends (run-integration.sh, verify.sh) run ctest
# through (docs/AXL-Build-System-Design.md §10.8).
#
# cts_wait_any must return runs in the order they END, each with its own
# status: waiting in a fixed order timed verify's aa64 run (119 s) to x64's
# end (195 s). The runs here are background jobs of known length and status
# in CTS_PID, as cts_start leaves them; starting a real session is the front
# ends' self-tests' (test-runner-selftest.sh's signal cases).
#
# Usage: ./test/integration/test-ctest-session.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/ctest-session.sh
source "$SCRIPT_DIR/lib/ctest-session.sh"

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}

echo "=== cts_wait_any: runs come back in the order they END ==="
# Started slow-first, so a fixed-order wait returns slow first.
( sleep 2; exit 4 ) & CTS_PID[slow]=$!
( sleep 0.3; exit 3 ) & CTS_PID[fast]=$!
t0=$SECONDS
cts_wait_any; rc=$?
check "the run that ends first comes back first, with its own status" "$CTS_DONE $rc" "fast 3"
check "...without waiting for the slower run" "$(( SECONDS - t0 <= 1 ))" "1"
check "...and is no longer held" "${CTS_PID[fast]:-gone} ${CTS_PID[slow]:+held}" "gone held"
cts_wait_any; rc=$?
check "the slower run comes back next, with its own status" "$CTS_DONE $rc" "slow 4"
check "nothing is held once both are back" "${#CTS_PID[@]}" "0"
cts_wait_any; rc=$?
check "with nothing held it says so (127), never a clean 0" "${CTS_DONE:-none} $rc" "none 127"

echo "=== cts_wait_any: a run that ENDED before the wait is still returned ==="
# A job that exits and is followed by another `&` job leaves bash's job table:
# `wait -n` then says "no such job", waits for another listed pid, and the
# exited run's status is lost -- a ctest that failed fast while the other
# tree's run was starting vanished, and verify died on an unset RC under
# set -u, with no table. Run in a FRESH shell: the exact sequence that drops
# it (start, a foreground command, another start), no earlier waits.
lost() {   # lost -> "NAME RC" per cts_wait_any call
    bash -c 'source "$1"
             ( exit 5 ) & CTS_PID[early]=$!
             sleep 0.3
             ( sleep 1; exit 6 ) & CTS_PID[late]=$!
             for i in 1 2 3; do cts_wait_any; echo "${CTS_DONE:-none} $?"; done' _ "$SCRIPT_DIR/lib/ctest-session.sh" 2>&1
}
check "the run that had already ended is returned, with its status, then the other; then none" \
    "$(lost | paste -sd' ')" "early 5 late 6 none 127"

echo "=== cts_inherited: the run settings a front end never passes on ==="
got=$(env -i PATH="$PATH" HOME="$HOME" AXL_IT_LOGDIR=/x AXL_PREP_TESTS= AXL_ONE_POOL_SOCK=/s \
          AXL_UNIT_BUDGET_SCALE=9 AXL_TEST_CACHE=1 AXL_KEEP_ME=1 OTHER=1 \
          TEST_APPS_ONLY=AxlTestX TEST_SKIP_RATCHET=1 TEST_TIMEOUT_SCALE=3 TEST_MAX_DURATION=999 \
          bash -c 'source "$1"; cts_inherited' _ "$SCRIPT_DIR/lib/ctest-session.sh" | sort | paste -sd' ')
# TEST_APPS_ONLY / TEST_SKIP_RATCHET too: left in a shell, they turned a
# "full" unit job into a subset, or skipped its ratchet. TEST_TIMEOUT_SCALE
# is NOT scrubbed: it is a documented knob a caller may set on purpose.
# TEST_MAX_DURATION is: the unit budget is raised in test-axl.sh, in a
# commit that says why, and a shell's leftover would loosen it unannounced.
check "every AXL_IT_/PREP_/ONE_/UNIT_*, AXL_TEST_CACHE, TEST_APPS_ONLY, TEST_SKIP_RATCHET, TEST_MAX_DURATION; nothing else" "$got" \
    "AXL_IT_LOGDIR AXL_ONE_POOL_SOCK AXL_PREP_TESTS AXL_TEST_CACHE AXL_UNIT_BUDGET_SCALE TEST_APPS_ONLY TEST_MAX_DURATION TEST_SKIP_RATCHET"
got=$(env -i PATH="$PATH" HOME="$HOME" bash -c 'source "$1"; cts_inherited; echo "rc=$?"' _ "$SCRIPT_DIR/lib/ctest-session.sh")
check "none inherited: nothing printed, status 0" "$got" "rc=0"

echo "=== cts_take_down: an interrupt cleans up only the runs this script owns ==="
# What the cleanup fixture would have removed, removed here -- the temp base
# (its disks fill /dev/shm) and the tree's owner record -- but ONLY for a run
# whose owner record names its own ctest: a run that lost the race to another
# must not remove that run's base, which sits at the same path. A FOREIGN
# base -- the caller's own directory, as the registration's tmp-setup says
# (--foreign) -- is never removed. Both front ends call this.
W=$(mktemp -d -t axl-cts.XXXXXX); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/bin" "$W/mine" "$W/theirs"
# No exec: the process must stay NAMED ctest, which is how cts_start finds it.
printf '#!/bin/bash\nsleep 30\n' > "$W/bin/ctest"; chmod +x "$W/bin/ctest"
argv=("$W/bin/ctest"); noenv=()
cts_start mine "$W/mine.out" noenv argv
cts_start theirs "$W/theirs.out" noenv argv
cts_ctest mine > "$W/mine/.run-owner"
echo 999999 > "$W/theirs/.run-owner"          # another run's record
mkdir -p "$W/mine/base/disk" "$W/theirs/base/disk"
cts_own mine "$W/mine/.run-owner" "$W/mine/base" 0
cts_own theirs "$W/theirs/.run-owner" "$W/theirs/base" 0
m=$(cts_ctest mine); t=$(cts_ctest theirs)
check "control: both fake ctest runs were found" "$([[ -n $m && -n $t ]] && echo yes)" "yes"
cts_take_down
sleep 0.3
check "every run's ctest is down, owned or not" \
    "$(kill -0 "$m" 2>/dev/null && echo up || echo down) $(kill -0 "$t" 2>/dev/null && echo up || echo down)" "down down"
check "the owned run's base and owner record are removed" \
    "$([[ -e $W/mine/base ]] && echo base) $([[ -e $W/mine/.run-owner ]] && echo rec)" " "
check "another run's base and record are left alone" \
    "$([[ -e $W/theirs/base ]] && echo base) $([[ -e $W/theirs/.run-owner ]] && echo rec)" "base rec"
check "nothing is held afterwards" "${#CTS_PID[@]}" "0"
mkdir -p "$W/caller/base"
cts_start own2 "$W/own2.out" noenv argv
cts_ctest own2 > "$W/caller/.run-owner"
cts_own own2 "$W/caller/.run-owner" "$W/caller/base" 1
cts_take_down
check "a FOREIGN base (the caller's) is never removed; the owner record is" \
    "$([[ -e $W/caller/base ]] && echo base) $([[ -e $W/caller/.run-owner ]] && echo rec)" "base "

echo "=== cts_tree_base: the base, the owner record and FOREIGN, from the registration ==="
# Read from the tree's own tmp-setup command (a stub ctest prints one), so a
# front end never decides foreignness from an environment variable it may
# not even map (verify.sh does not map AXL_QEMU_TMPDIR; the runner does).
mkdir -p "$W/fakectest"
cat > "$W/fakectest/ctest" <<'FAKE'
#!/bin/bash
echo "1: Test command: /usr/bin/bash \"/src/lib/ctest-tmpbase.sh\" \"setup\" \"/dev/shm/axl-ct-x\" \"/t/integration-logs/.run-owner\" $FAKE_MODE"
FAKE
chmod +x "$W/fakectest/ctest"
noenv=()
FAKE_MODE='' PATH="$W/fakectest:$PATH" cts_tree_base /t noenv
check "an owned base: base, record, FOREIGN=0" "$CTS_BASE $CTS_OWNER_REG $CTS_BASE_FOREIGN" \
    "/dev/shm/axl-ct-x /t/integration-logs/.run-owner 0"
FAKE_MODE='"--foreign"' PATH="$W/fakectest:$PATH" cts_tree_base /t noenv
check "a caller's base: FOREIGN=1" "$CTS_BASE $CTS_BASE_FOREIGN" "/dev/shm/axl-ct-x 1"

echo "=== cts_reap_after: a normal run reaps only if ITS setup ran ==="
got=$(bash -c 'source "$1"; reap_pool() { echo "reaped $1"; }
    printf "<testcase name=\"integration.tmp-setup\" classname=\"x\" status=\"run\"/>\n" > "$2/ran.xml"
    printf "<testcase name=\"integration.tmp-setup\" classname=\"x\" status=\"fail\"/>\n" > "$2/refused.xml"
    cts_reap_after "$2/ran.xml" /b1; cts_reap_after "$2/refused.xml" /b2; cts_reap_after "$2/none.xml" /b3
    echo done' _ "$SCRIPT_DIR/lib/ctest-session.sh" "$W")
check "reaped after its own setup ran; not after a refused setup or no report" "$(paste -sd' ' <<< "$got")" "reaped /b1 done"

echo ""
echo "ctest-session: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
