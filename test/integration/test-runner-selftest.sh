#!/bin/bash
# test-runner-selftest.sh — host-only checks for run-integration.sh using stub
# "tests" (a pass, a fail, a hang). No QEMU. Not a real integration test;
# excluded from discovery by name (*-selftest.sh).
set -uo pipefail
cd "$(dirname "$0")"

stub=$(mktemp -d); trap 'rm -rf "$stub"' EXIT
printf '#!/bin/bash\nexit 0\n'   > "$stub/test-pass-qemu.sh"
printf '#!/bin/bash\nexit 1\n'   > "$stub/test-fail-qemu.sh"
printf '#!/bin/bash\nsleep 30\n' > "$stub/test-hang-qemu.sh"
# Exit 77 is the automake convention for "skipped". A test that cannot run --
# an absent external corpus, a missing host tool -- must NOT be counted as a
# pass: 39 of the 176 integration tests can exit 0 having tested nothing, and
# the suite reported them among "170 passed".
printf '#!/bin/bash\necho "SKIP: nothing to test"\nexit 77\n' > "$stub/test-skip-qemu.sh"
# Reports whether the runner enabled caching for it. AXL_TEST_CACHE is the only
# thing a test can observe about that decision, so it is what the assertions
# below read -- rather than inspecting the runner's internals.
printf '#!/bin/bash\necho "CACHEENV=${AXL_TEST_CACHE:-unset}"\nexit 0\n' > "$stub/test-cacheprobe-qemu.sh"
chmod +x "$stub"/*.sh

fail=0
need() { grep -q "$1" <<<"$2" || { echo "  FAIL: expected /$1/ in output"; fail=1; }; }

# --- serial (-j1 explicitly; the no-j default now auto-sizes to nproc-2) ---
out=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh -j1 --timeout 2 2>&1); rc=$?
need "test-pass-qemu.sh PASS" "$out"
need "test-fail-qemu.sh FAIL" "$out"
need "test-hang-qemu.sh TIMEOUT" "$out"
# A skip is its own verdict, and must be neither PASS nor FAIL.
need "test-skip-qemu.sh SKIP" "$out"
grep -qE 'test-skip-qemu\.sh (PASS|FAIL)' <<<"$out" \
    && { echo "  FAIL: an exit-77 test was scored PASS/FAIL, not SKIP"; fail=1; }
# ...and the totals must say so, or a suite that skipped half of itself still
# reads as a clean run.
need "skipped" "$out"
[[ $rc -ne 0 ]] || { echo "  FAIL: runner exit 0 despite a failing test"; fail=1; }
[[ $fail -eq 0 ]] && echo "  PASS: serial verdicts + non-zero exit"

# --- caching is ON BY DEFAULT, and OFF for the runs that must be authoritative
#
# The cache skips a test whose inputs are byte-identical to its last green run
# (lib/test-cache.sh). It is the default because an opt-in flag does not get
# typed -- measured the hard way: it was built and then not used once in the
# session that built it. What must NOT be cached is the run that certifies a
# tree: --no-cache for a pre-push/release gate, and --ci, because CI is the
# backstop and a backstop that skips is not one.
# A test's stdout goes to its per-test log, not the runner's, so the probe is
# read from there. The runner prints the log directory on its last line.
cacheenv() {   # $1.. = runner args; echoes the probe's observation
    local out logdir
    out=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh "$@" 2>&1)
    logdir=$(grep -oE '^logs: .*' <<<"$out" | tail -1 | cut -d' ' -f2)
    [[ -n "$logdir" ]] && grep -h -oE 'CACHEENV=[^ ]*' \
        "$logdir/test-cacheprobe-qemu.sh.log" 2>/dev/null | tail -1
}
[[ "$(cacheenv -j1 --timeout 2)" == CACHEENV=/* ]] \
    || { echo "  FAIL: caching is not on by default"; fail=1; }
[[ "$(cacheenv -j1 --timeout 2 --no-cache)" == "CACHEENV=unset" ]] \
    || { echo "  FAIL: --no-cache did not disable the cache"; fail=1; }
[[ "$(cacheenv -j1 --timeout 2 --ci)" == "CACHEENV=unset" ]] \
    || { echo "  FAIL: --ci did not disable the cache — CI would skip tests"; fail=1; }
outc=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh -j1 --timeout 2 2>&1)
# The PARTIAL banner must fire only when something was actually skipped, or it
# is printed on every run and stops carrying information.
grep -q 'PARTIAL -- --cache' <<<"$outc" \
    && { echo "  FAIL: PARTIAL printed with 0 cached tests"; fail=1; }
[[ $fail -eq 0 ]] && echo "  PASS: cache on by default; off for --no-cache and --ci"

# --- parallel (-j4): same verdicts as serial, still non-zero exit ---
outp=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh -j4 --timeout 2 2>&1); rcp=$?
need "test-pass-qemu.sh PASS" "$outp"
need "test-fail-qemu.sh FAIL" "$outp"
need "test-hang-qemu.sh TIMEOUT" "$outp"
need "test-skip-qemu.sh SKIP" "$outp"
[[ $rcp -ne 0 ]] || { echo "  FAIL: -j4 exit 0 despite a failing test"; fail=1; }
[[ $fail -eq 0 ]] && echo "  PASS: -j4 verdicts + non-zero exit"

# --- retry-once: a test that fails on attempt 1 but passes on attempt 2 is
#     reported PASS (the parallel pool's transient-contention mitigation). ---
rstub=$(mktemp -d)
printf '#!/bin/bash\nm=%s/flaky.marker\n[[ -e "$m" ]] && exit 0\ntouch "$m"; exit 1\n' "$rstub" \
    > "$rstub/test-flaky-qemu.sh"
chmod +x "$rstub"/*.sh
rout=$(RUN_INTEGRATION_DIR="$rstub" ./run-integration.sh -j1 --timeout 5 2>&1); rrc=$?
rm -rf "$rstub"
need "test-flaky-qemu.sh PASS" "$rout"
need "(retry" "$rout"
[[ $rrc -eq 0 ]] || { echo "  FAIL: retry-recovered flaky test should exit 0"; fail=1; }
[[ $fail -eq 0 ]] && echo "  PASS: transient failure retried -> PASS, exit 0"

# --- concurrent workers get distinct host-port bases ---
# The runner no longer assigns TEST_PORT_BASE (a formula only keeps ONE
# invocation self-consistent — see run_one). It must leave the variable
# unset so each test claims its own base through common-test.sh, and those
# claims must not collide across workers running at the same time. The stubs
# source common-test.sh for real and hold their claim while the others run,
# so the overlap is genuine rather than sequential reuse of one port.
bstub=$(mktemp -d); bout=$(mktemp -d)
for n in a b c; do
    printf '#!/bin/bash\nsource "%s/common-test.sh"\necho "$TEST_PORT_BASE" > "%s/$$.base"\nsleep 2\nexit 0\n' \
        "$PWD" "$bout" > "$bstub/test-base-$n-qemu.sh"
done
chmod +x "$bstub"/*.sh
RUN_INTEGRATION_DIR="$bstub" ./run-integration.sh -j4 --timeout 20 >/dev/null 2>&1
nbases=$(cat "$bout"/*.base 2>/dev/null | wc -l)
ndistinct=$(cat "$bout"/*.base 2>/dev/null | sort -u | wc -l)
inrange=$(awk '$1 >= 18000 && $1 <= 19999' "$bout"/*.base 2>/dev/null | wc -l)
rm -rf "$bstub" "$bout"
if [[ "$nbases" -eq 3 && "$ndistinct" -eq 3 ]]; then
    echo "  PASS: 3 concurrent workers claimed 3 distinct port bases"
else
    echo "  FAIL: expected 3 distinct port bases, got $ndistinct of $nbases"; fail=1
fi
if [[ "$inrange" -eq 3 ]]; then
    echo "  PASS: runner left TEST_PORT_BASE to the allocator (all in 18000-19999)"
else
    echo "  FAIL: $inrange/3 bases came from the allocator range"; fail=1
fi

# --- --shard i/K partitions the set: union == full, no overlap ---
a=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --shard 0/2 --list | sort)
b=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --shard 1/2 --list | sort)
allset=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --list | sort)
union=$(printf '%s\n%s\n' "$a" "$b" | grep -v '^$' | sort -u)
overlap=$(comm -12 <(printf '%s\n' "$a" | grep -v '^$') <(printf '%s\n' "$b" | grep -v '^$'))
if [[ "$union" == "$allset" ]]; then echo "  PASS: shards cover the full set"; else echo "  FAIL: shard union != full set"; fail=1; fi
if [[ -z "$overlap" ]]; then echo "  PASS: shards do not overlap"; else echo "  FAIL: shard overlap: $overlap"; fail=1; fi

# --- a signalled runner must reap the guest pool BEFORE removing the tree ---
# The runner exports one shared AXL_QEMU_TMPDIR and every worker launches a
# guest underneath it. If the signal trap rm -rf's that tree without first
# killing the guests, each survivor keeps its disk open as a DELETED inode:
# the directory is gone, so every directory-based check says "clean", while
# tmpfs cannot reclaim the pages. 365 MB accumulated that way before anyone
# noticed. df sees it; du cannot -- which is why the assertion below is a df
# delta and not a check that the directory went away.
sig_check() {
    local signal="$1" sstub sout base held delta_kb rpid rc=0
    sstub=$(mktemp -d); sout=$(mktemp -d)
    # Two stand-ins, because the runner reaps by two different mechanisms and
    # a test that only covered one would let the other rot:
    #
    #   A: an ordinary descendant of the worker -- caught by the PID walk.
    #   B: orphaned to init and named qemu-system-*, so it passes the
    #      reaper's comm filter. This is the shape the real orphans had
    #      (PPID 1), and only reap_pool can get it.
    #
    # B is launched as `( setsid ... & )`, not a bare `setsid ... &`. setsid
    # changes the SESSION, not the parent: on its own the guest stays a child
    # of the stub and the PID walk reaches it anyway, so the assertion would
    # pass without reap_pool existing at all. The throwaway subshell exits
    # immediately, which is what actually reparents it to init. Sabotage
    # caught this -- the first version passed with reap_pool deleted.
    #
    # `tail -f` is the body of B: it holds the disk open AND carries the path
    # in argv, which are the two properties the reaper depends on. A shell
    # script could not be used -- /proc/PID/comm would read `bash`.
    cp "$(command -v tail)" "$sstub/qemu-system-selftest"
    cat > "$sstub/test-guest-qemu.sh" <<STUB
#!/bin/bash
echo "\$AXL_QEMU_TMPDIR" > "$sout/base"
d="\$AXL_QEMU_TMPDIR/axl-qemu.\$\$"
mkdir -p "\$d"
dd if=/dev/zero of="\$d/a.img" bs=1M count=32 status=none
dd if=/dev/zero of="\$d/b.img" bs=1M count=32 status=none
bash -c 'exec 9< "\$1/a.img"; sleep 300' _ "\$d" &
( setsid "$sstub/qemu-system-selftest" -f "\$d/b.img" >/dev/null 2>&1 & )
echo ready > "$sout/ready.\$\$"
sleep 300
STUB
    chmod +x "$sstub"/*.sh "$sstub/qemu-system-selftest"

    local before_kb; before_kb=$(df -k --output=used /dev/shm | tail -1)
    RUN_INTEGRATION_DIR="$sstub" ./run-integration.sh -j1 --timeout 120 >/dev/null 2>&1 &
    rpid=$!
    # Wait for the stand-in guest to be holding its disk open.
    for _ in $(seq 1 100); do
        compgen -G "$sout/ready.*" >/dev/null 2>&1 && break
        sleep 0.2
    done
    base=$(cat "$sout/base" 2>/dev/null)

    # Signal only the runner -- that is the `timeout` case, and the one that
    # produced the observed orphans. (A terminal Ctrl-C hits the whole process
    # group, so the workers die of their own accord and the bug is masked.)
    kill -"$signal" "$rpid" 2>/dev/null
    # Bounded wait, then escalate the way `timeout --kill-after` does. The
    # handler exits on its own now, so this normally completes in one pass;
    # it exists so a regression that leaves the runner spinning fails the
    # assertion instead of hanging the suite.
    local exited=0
    for _ in $(seq 1 40); do
        kill -0 "$rpid" 2>/dev/null || { exited=1; break; }
        sleep 0.25
    done
    if [[ "$exited" -eq 0 ]]; then
        kill -9 "$rpid" 2>/dev/null
    fi
    wait "$rpid" 2>/dev/null
    sleep 1

    # 1. No survivor may still reference the run's tree.
    held=$(pgrep -af -- "$base" 2>/dev/null | grep -cv 'run-integration' || true)
    # 2. The tree itself must be gone.
    local tree_gone=1; [[ -n "$base" && -e "$base" ]] && tree_gone=0
    # 3. tmpfs must actually be reclaimed -- the deleted-inode check. This is
    #    the one a directory-existence assertion cannot make.
    local after_kb; after_kb=$(df -k --output=used /dev/shm | tail -1)
    delta_kb=$(( after_kb - before_kb ))

    if [[ "$held" -ne 0 ]]; then
        echo "  FAIL: SIG$signal left $held process(es) holding $base"
        pgrep -af -- "$base" 2>/dev/null | grep -v 'run-integration' | sed 's/^/         /'
        rc=1
    fi
    [[ "$tree_gone" -eq 1 ]] || { echo "  FAIL: SIG$signal left the tree $base behind"; rc=1; }
    if [[ "$delta_kb" -gt 8192 ]]; then
        echo "  FAIL: SIG$signal leaked ${delta_kb} KB of /dev/shm (deleted inodes)"
        rc=1
    fi
    # Never leave our own strays behind, pass or fail. Two handles are needed:
    # the guest stand-ins carry the /dev/shm base, the stub test scripts carry
    # the stub dir.
    [[ -n "$base" ]] && { pkill -f -- "$base" 2>/dev/null; sleep 0.5; rm -rf "$base"; }
    pkill -f -- "$sstub" 2>/dev/null
    sleep 0.3
    rm -rf "$sstub" "$sout"
    if [[ "$rc" -eq 0 ]]; then
        echo "  PASS: SIG$signal reaps the pool, removes the tree, reclaims tmpfs"
    else
        fail=1
    fi
}
# --- --only: an explicit subset, and a LOUD refusal for a name that is not there
#
# WHY EXPLICIT AND NOT COMPUTED. §12.5 of AXL-CI-Release-Speed-Design.md is the
# argument: `8af4e530` touched src/log/ and the Makefile, any relevance map
# picks "the logging tests", and the real blast radius was every image in the
# tree failing at link. So the runner gains the ability to run a SUBSET and no
# ability whatever to decide which -- the caller names them, and the caller
# that matters (ci-plan.sh) is itself fixture-tested and sabotage-verified.
echo "-- --only --"
o=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --only=test-pass-qemu.sh --list 2>&1)
[[ "$(echo "$o" | grep -c .)" == "1" ]] && grep -q 'test-pass-qemu.sh' <<<"$o"     || { echo "  FAIL: --only=<name> did not select exactly that test: $o"; fail=1; }

# The `.sh` is optional, because a caller naming tests is a human or a script
# quoting a test-meta name, and both spellings are what people write.
o=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --only=test-pass-qemu --list 2>&1)
grep -q 'test-pass-qemu.sh' <<<"$o"     || { echo "  FAIL: --only without .sh did not match: $o"; fail=1; }

o=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --only=test-pass-qemu.sh,test-skip-qemu.sh --list 2>&1)
[[ "$(echo "$o" | grep -c .)" == "2" ]]     || { echo "  FAIL: --only with two names did not select two: $o"; fail=1; }

# THE ONE THAT MATTERS. A typo must not select nothing and report success --
# "the tool ran and found nothing" and "you asked for something that is not
# there" are the same empty set and opposite facts, and a CI caller passing a
# renamed test would silently verify NOTHING while going green.
o=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --only=test-nope-qemu.sh --list 2>&1); rc=$?
[[ $rc -ne 0 ]] && grep -qi 'test-nope-qemu' <<<"$o"     || { echo "  FAIL: --only with an unknown name did not fail loudly (rc=$rc): $o"; fail=1; }

# A filtered run is PARTIAL and must say so, like --shard and --ci do.
o=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh -j1 --timeout 5 --only=test-pass-qemu.sh 2>&1)
grep -q 'PARTIAL' <<<"$o"     || { echo "  FAIL: an --only run did not announce itself as PARTIAL: $o"; fail=1; }

# ...and it must NOT write the release stamp. A subset did not test the tree,
# and the stamp is what lets a release skip its gate: this is the same defect
# --ci once had, one flag later.
# THE CONTROL COMES FIRST, and it has to be a stub set that can actually go
# green: $stub holds a failing test and a hanging one, so NOTHING stamps on it
# and "the --only run did not stamp" would pass for a reason that has nothing
# to do with --only. That is the shape this file exists to catch elsewhere.
stampdir=$(mktemp -d)
gstub=$(mktemp -d)
printf '#!/bin/bash\nexit 0\n' > "$gstub/test-green-a-qemu.sh"
printf '#!/bin/bash\nexit 0\n' > "$gstub/test-green-b-qemu.sh"
chmod +x "$gstub"/*.sh
# --no-cache on BOTH: only an uncached run stamps at all, so without it the
# control fails for that reason and proves nothing about --only.
AXL_STAMP_FILE="$stampdir/control" RUN_INTEGRATION_DIR="$gstub" ./run-integration.sh -j1 --timeout 5 --no-cache >/dev/null 2>&1
[[ -f "$stampdir/control" ]] || { echo "  FAIL: control -- an unfiltered green run did NOT stamp, so the next check proves nothing"; fail=1; }
# ...and now the same green stubs, filtered. A subset did not test the tree,
# and the stamp is what lets a release skip its gate: this is the defect --ci
# once had, one flag later.
AXL_STAMP_FILE="$stampdir/only" RUN_INTEGRATION_DIR="$gstub" ./run-integration.sh -j1 --timeout 5 --no-cache --only=test-green-a-qemu.sh >/dev/null 2>&1
[[ ! -f "$stampdir/only" ]] || { echo "  FAIL: an --only run wrote the release stamp"; fail=1; }
rm -rf "$stampdir" "$gstub"

# SIGTERM only, and that is not a gap. SIGTERM is the delivery that actually
# produced the orphans (a `timeout` wrapper expiring, which signals just the
# runner), and run-integration.sh routes both signals through one _signal_exit
# body, so this covers the SIGINT path's logic too.
#
# SIGINT genuinely cannot be driven from here: bash starts a background job of
# a non-interactive shell with SIGINT ignored (/proc/PID/status SigIgn bit 2),
# and a signal ignored on entry cannot be trapped -- so `kill -INT` at the
# runner is swallowed and the assertion would be measuring the harness, not
# the runner. A terminal Ctrl-C reaches the whole foreground process group
# instead, where the trap does fire.
sig_check TERM
# No HUP/PIPE cases: bash runs the EXIT trap for untrapped fatal signals too,
# so they exercise the same body as TERM and cannot fail independently of it.
# Verified by sabotage -- deleting per-signal HUP/PIPE handlers changed nothing.

[[ $fail -eq 0 ]] && echo "runner selftest: OK" || { echo "runner selftest: FAILED"; exit 1; }
