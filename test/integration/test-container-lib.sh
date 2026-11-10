#!/bin/bash
# test-meta: arch=x64 needs=podman est=60 cpu=2 local-only=0
# test-container-lib.sh -- the container tests' two helpers, and the consumer
# test that uses both, against REAL podman.
#
# WHY THIS EXISTS (2026-11-09). A slow Ubuntu mirror turned
# test-consumer-install.sh into a 756 s CTest kill with no line naming the
# mirror, and every kill left a container "Up" that had to be removed by hand.
# Two helpers came out of that, and each claim they make is asserted here:
#
#   scripts/container-pkg.sh      a package install inside a container fails
#                                 FAST, with the marker line and the mirror
#                                 named -- on a dead network, on a mirror that
#                                 never answers (the wall clock), and on dnf;
#   test/integration/lib/podman-run.sh
#                                 a test killed by TERM, INT or even KILL
#                                 leaves no container of ITS OWN behind, and
#                                 never touches anyone else's.
#
# A STUB podman would have been cheaper, and would have proved nothing that
# matters: the leak came from how the real client and a PID-1 `sh` treat
# SIGTERM (the client forwards it, PID 1 ignores it, the client never exits),
# which a stub only restates. So the lib cases run a real container from a
# tiny fixture script, and two end-to-end cases run test-consumer-install.sh
# itself: one TERMs its process group mid-arm, the way run-one.sh's `timeout`
# does, and one runs it with `--network none` to see a mirror outage fail fast.
#
# Containers are found by NAME, axl-it-<pid of the owning process>-..., which
# is the lib's own contract; every container this file creates for its own
# purposes is removed by its EXIT trap.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail
cd "$PROJECT_DIR" || exit 1

if ! command -v podman >/dev/null 2>&1; then
    echo "SKIP: podman not available"
    exit 0
fi

# The end-to-end cases run test-consumer-install.sh, which SKIPs without a
# staged x64 toolchain; skipping here first, before any tally, keeps the count
# the same either way.
# shellcheck source=/dev/null
. "$PROJECT_DIR/scripts/axl-toolchains.conf"
if [[ ! -d "$AXL_X64_TOOLCHAIN_DIR" ]]; then
    echo "SKIP: x64 toolchain not installed: $AXL_X64_TOOLCHAIN_DIR"
    exit 0
fi

DEBIAN=docker.io/library/debian:stable-slim
FEDORA=docker.io/library/fedora:latest
PKG_SH="$PROJECT_DIR/scripts/container-pkg.sh"
LIB="$SCRIPT_DIR/lib/podman-run.sh"

# Images first, outside every timing: a pull's progress lines would land in
# the logs whose exact LINES are asserted below, and its time in the bounds.
for img in "$DEBIAN" "$FEDORA" docker.io/library/ubuntu:24.04; do
    podman image exists "$img" && continue
    if ! podman pull -q "$img" >/dev/null 2>&1; then
        echo "FAIL: could not pull $img -- the registry or the network, before any case ran"
        exit 1
    fi
done

TMP="$(mktemp -d -t axl-ctrlib.XXXXXXXX)"
FOREIGN="" # a container standing in for another run's, removed at exit
OWNERS=()  # every PID whose axl-it-<pid>-* containers this file may leave
cleanup() {
    local p
    for p in "${OWNERS[@]}"; do
        kill -KILL "$p" 2>/dev/null
        podman ps -a -q --filter "name=^axl-it-$p-" | xargs -r podman rm -f -t 0 >/dev/null 2>&1
    done
    [[ -n "$FOREIGN" ]] && podman rm -f -t 0 --ignore "$FOREIGN" >/dev/null 2>&1
    rm -rf "$TMP"
}
trap cleanup EXIT

echo "=== container lib ==="
echo ""

# owned_by <pid> -- the IDs of every container (any state) named for <pid>.
# The trailing dash matters: pid 123 must not match axl-it-1234-*.
owned_by() {
    podman ps -a -q --filter "name=^axl-it-$1-"
}

# wait_until <seconds> <command>... -- poll every 0.5 s; status 0 once the
# command succeeds, 1 if it never did.
wait_until() {
    local deadline=$(( SECONDS + $1 )); shift
    while (( SECONDS < deadline )); do
        "$@" && return 0
        sleep 0.5
    done
    return 1
}
has_owned()  { [[ -n "$(owned_by "$1")" ]]; }
none_owned() { [[ -z "$(owned_by "$1")" ]]; }

# ---------------------------------------------------------------------------
# container-pkg.sh
# ---------------------------------------------------------------------------

# pkg_case <log> <podman-run args>... -- run pkg_install in a container,
# printing nothing; the caller reads <log>. Bounded by podman's --timeout
# (conmon's, so the container really stops) and not by `timeout` alone: a
# TERM to the client is forwarded to the container's PID-1 sh, which ignores
# it, so `timeout 180 podman run` without a KILL waited forever when the wall
# clock under test was sabotaged.
pkg_case() {
    local log=$1; shift
    timeout -k 10 120 podman run --rm -q --timeout 60 "$@" > "$log" 2>&1
}

# A DEAD NETWORK: apt's update must fail by name. A plain `apt-get update`
# exits 0 here (every index "ignored"), which is how an outage used to read as
# "Unable to locate package curl" one step later -- so the step named in the
# marker is the assertion.
t0=$SECONDS
pkg_case "$TMP/apt-net.log" --network none "$DEBIAN" sh -c "$(cat "$PKG_SH")
pkg_install curl
echo NOT-REACHED"
rc=$?; dt=$(( SECONDS - t0 ))
check_line() {  # check_line <label> <log> <line-number> <expected>
    local got
    got=$(sed -n "${3}p" "$2")
    if [[ "$got" == "$4" ]]; then
        test_host_pass "$1"
    else
        test_host_fail "$1: line $3 is \"$got\", want \"$4\""
    fi
}
if [[ "$rc" -eq 1 ]]; then test_host_pass "apt, no network: the container exits 1"
else test_host_fail "apt, no network: exit $rc, want 1"; fi
check_line "apt, no network: the marker names the update step" "$TMP/apt-net.log" 1 \
    "AXL-PKG-FAIL: apt-get update failed (exit 100)"
check_line "apt, no network: the cause line names the image's mirrors" "$TMP/apt-net.log" 2 \
    "  likely cause: the package mirror or the network, not AXL (mirrors: http://deb.debian.org/debian http://deb.debian.org/debian-security)"
if grep -q "Temporary failure resolving 'deb.debian.org'" "$TMP/apt-net.log"; then
    test_host_pass "apt, no network: the log tail shows apt's own error"
else
    test_host_fail "apt, no network: the log tail does not show apt's error"
    sed 's/^/      /' "$TMP/apt-net.log"
fi
if (( dt < 60 )); then test_host_pass "apt, no network: fails fast (${dt}s)"
else test_host_fail "apt, no network: took ${dt}s"; fi

# A MIRROR THAT NEVER ANSWERS: the per-fetch bounds do not bound the total,
# so this is the wall clock's case. apt-get is replaced by a stub that hangs,
# and the wall is 3 s; the marker must say it timed out, inside the wall plus
# timeout's 10 s KILL grace plus container start.
t0=$SECONDS
pkg_case "$TMP/apt-wall.log" "$DEBIAN" sh -c "$(cat "$PKG_SH")
mkdir -p /tmp/stub
printf '#!/bin/sh\nexec sleep 600\n' > /tmp/stub/apt-get
chmod +x /tmp/stub/apt-get
PATH=/tmp/stub:\$PATH
AXL_PKG_WALL=3
pkg_install curl
echo NOT-REACHED"
rc=$?; dt=$(( SECONDS - t0 ))
check_line "apt, hung mirror: the marker says the wall clock fired" "$TMP/apt-wall.log" 1 \
    "AXL-PKG-FAIL: apt-get update failed (timed out after 3s)"
if (( rc == 1 && dt < 30 )); then test_host_pass "apt, hung mirror: exit 1 within the wall (${dt}s)"
else test_host_fail "apt, hung mirror: exit $rc after ${dt}s"; fi

# dnf. Exit 1 is the network failure; an option dnf did not accept would be
# exit 2 ("Option ... not found") -- so the status also proves the bounds in
# --setopt are real options, not silently-ignored text.
pkg_case "$TMP/dnf-net.log" --network none "$FEDORA" sh -c "$(cat "$PKG_SH")
pkg_install curl
echo NOT-REACHED"
rc=$?
check_line "dnf, no network: the marker names the step and exit 1" "$TMP/dnf-net.log" 1 \
    "AXL-PKG-FAIL: dnf install curl failed (exit 1)"
check_line "dnf, no network: the cause line names the mirror host" "$TMP/dnf-net.log" 2 \
    "  likely cause: the package mirror or the network, not AXL (mirrors: https://mirrors.fedoraproject.org)"
if [[ "$rc" -eq 1 ]]; then test_host_pass "dnf, no network: the container exits 1"
else test_host_fail "dnf, no network: exit $rc, want 1"; fi

# ---------------------------------------------------------------------------
# podman-run.sh, from a fixture that owns one long-lived container
# ---------------------------------------------------------------------------
# FX_COMMON=1 sources common-test.sh first, as the real tests do: its
# TERM/INT handlers (the KILLED line, then death by the signal) must survive
# podman_owned_init and still reach the cleanup. Without it the lib installs
# axl_exit_on_signal, the configuration of a test that has no common-test.sh.
cat > "$TMP/fixture.sh" <<EOF
#!/bin/bash
[[ "\${FX_COMMON:-0}" == 1 ]] && source "$SCRIPT_DIR/common-test.sh"
source "$LIB"
podman_owned_init
trap 'podman_owned_cleanup' EXIT
podman_owned_run "$TMP/fixture-run.log" $DEBIAN sh -c 'echo up; exec sleep 300'
echo "fixture: the run returned \$?"
EOF

# start_fixture <FX_COMMON> -- in its own session and process group, with TERM
# and INT at their defaults: a background job of a non-interactive bash starts
# with INT IGNORED, and a signal ignored on entry cannot be trapped, which
# would make the INT case test bash instead of the lib. AXL_TEST_TIMEOUT is
# cleared so common-test.sh's KILLED line has one known shape. Sets FX, the
# fixture's PID (and PGID).
start_fixture() {
    FX_COMMON=$1 setsid env -u AXL_TEST_TIMEOUT --default-signal=INT,TERM \
        bash "$TMP/fixture.sh" > "$TMP/fixture.out" 2>&1 &
    FX=$!
    OWNERS+=("$FX")
}

# signal_case <label> <SIGNAL> <pid|group> <want-exit> <FX_COMMON> -- deliver
# SIGNAL to the fixture alone or to its whole process group; it must exit with
# <want-exit> and leave nothing named for it.
signal_case() {
    local label=$1 sig=$2 to=$3 want=$4 common=$5 rc target
    start_fixture "$common"
    if ! wait_until 60 has_owned "$FX"; then
        test_host_fail "$label: the fixture's container never came up"
        sed 's/^/      /' "$TMP/fixture.out"
        return
    fi
    # The FOREIGN container stands in for another run's (or the user's). Its
    # name shares the fixture's PID digits, so a cleanup matching by a sloppy
    # prefix would take it.
    FOREIGN="axl-it-${FX}0-foreign"
    podman run -d --rm --name "$FOREIGN" "$DEBIAN" sleep 300 >/dev/null
    target=$FX; [[ "$to" == group ]] && target="-$FX"
    kill "-$sig" -- "$target" 2>/dev/null
    if wait_until 20 bash -c "! kill -0 $FX 2>/dev/null"; then
        wait "$FX"; rc=$?
        if [[ "$rc" -eq "$want" ]]; then test_host_pass "$label: the test exits $want at once"
        else test_host_fail "$label: the test exited $rc, want $want"; fi
    else
        test_host_fail "$label: the test was still running 20 s after the signal"
    fi
    if none_owned "$FX"; then
        test_host_pass "$label: no container of this run survives"
    else
        test_host_fail "$label: left $(owned_by "$FX" | wc -l) container(s) behind"
    fi
    if [[ -n "$(podman ps -q --filter "name=^$FOREIGN\$")" ]]; then
        test_host_pass "$label: another run's container is untouched"
    else
        test_host_fail "$label: the cleanup removed a container it did not start"
    fi
    if [[ "$common" == 1 ]]; then
        if grep -qE "^KILLED: fixture\.sh received SIG$sig [0-9]+s in -- stopped from outside \(no runner time limit is known to this run\)\.$" "$TMP/fixture.out"; then
            test_host_pass "$label: common-test.sh's KILLED line still prints"
        else
            test_host_fail "$label: no KILLED line -- the lib replaced common-test.sh's handler"
            sed 's/^/      /' "$TMP/fixture.out"
        fi
    fi
    podman rm -f -t 0 --ignore "$FOREIGN" >/dev/null 2>&1; FOREIGN=""
}

# TERM to the test ALONE: the client never sees it, so only the trap -- run
# from `wait`, not deferred behind a foreground child -- can clean up.
signal_case "TERM to the test" TERM pid 143 0
# TERM to the whole group, which is what run-one.sh's `timeout` sends: the
# client gets it too, forwards it to PID 1, and PID 1 ignores it. Once with
# the lib's own handler, once under common-test.sh's.
signal_case "TERM to the process group" TERM group 143 0
signal_case "TERM to the group, under common-test.sh" TERM group 143 1
# INT, as Ctrl-C sends it to the foreground group -- both handlers.
signal_case "INT to the process group" INT group 130 0
signal_case "INT to the group, under common-test.sh" INT group 130 1

# KILL cannot be trapped, which is what --timeout is for: conmon enforces it,
# not the client. The control first: one second after the KILL the container
# is still up, so what removes it later is the cap, not the kill.
export AXL_PODMAN_CAP=8
start_fixture 0
unset AXL_PODMAN_CAP
if wait_until 60 has_owned "$FX"; then
    kill -KILL "$FX"
    sleep 1
    if has_owned "$FX"; then
        test_host_pass "KILL control: the container outlives its killed client"
    else
        test_host_fail "KILL control: the container was already gone -- the cap below proves nothing"
    fi
    if wait_until 30 none_owned "$FX"; then
        test_host_pass "KILL: the cap stops and removes the orphan"
    else
        test_host_fail "KILL: the orphan was still there 30 s after an 8 s cap"
    fi
else
    test_host_fail "KILL: the fixture's container never came up"
fi

# ---------------------------------------------------------------------------
# test-consumer-install.sh itself
# ---------------------------------------------------------------------------
CONSUMER_TEST="$SCRIPT_DIR/test-consumer-install.sh"

# TERM mid-arm, to the group, exactly as a test timeout delivers it. The arm
# must still be RUNNING when the TERM lands, or a healthy arm finishing on its
# own (~15 s) would clean up through --rm and prove nothing -- that is how a
# first reproduction of the leak came up empty. So the first arm's apt-get is
# a stub that hangs, mounted over /usr/local/sbin (ahead of /usr/bin on the
# image's PATH): the install stalls the way it did against the dead mirror,
# deterministically. The control asserts the container is running when the
# TERM is sent.
mkdir -p "$TMP/hang"
printf '#!/bin/sh\nexec sleep 600\n' > "$TMP/hang/apt-get"
chmod +x "$TMP/hang/apt-get"
AXL_PODMAN_EXTRA_ARGS="-v $TMP/hang/apt-get:/usr/local/sbin/apt-get:ro" \
    setsid env --default-signal=INT,TERM bash "$CONSUMER_TEST" > "$TMP/consumer-term.out" 2>&1 &
CT=$!
OWNERS+=("$CT")
if wait_until 180 has_owned "$CT"; then
    sleep 3
    if [[ -n "$(podman ps -q --filter "name=^axl-it-$CT-" --filter status=running)" ]]; then
        test_host_pass "consumer test control: the arm is still running when the TERM is sent"
    else
        test_host_fail "consumer test control: no running arm at the TERM -- the cases below prove nothing"
    fi
    kill -TERM -- "-$CT"
    if wait_until 30 bash -c "! kill -0 $CT 2>/dev/null"; then
        test_host_pass "consumer test: exits within 30 s of a group TERM mid-arm"
    else
        test_host_fail "consumer test: still running 30 s after a group TERM"
    fi
    if none_owned "$CT"; then
        test_host_pass "consumer test: no container survives the TERM"
    else
        test_host_fail "consumer test: left $(owned_by "$CT" | wc -l) container(s) behind"
    fi
else
    test_host_fail "consumer test: no container named axl-it-$CT-* came up in 180 s"
    tail -5 "$TMP/consumer-term.out" | sed 's/^/      /'
fi


# A MIRROR OUTAGE, made by running every container with no network. It must
# FAIL (never skip), name the cause in the first arm, and not spend the
# remaining arms rediscovering it.
t0=$SECONDS
AXL_PODMAN_EXTRA_ARGS="--network none" timeout 400 bash "$CONSUMER_TEST" > "$TMP/consumer-net.out" 2>&1
rc=$?; dt=$(( SECONDS - t0 ))
if [[ "$rc" -eq 1 ]]; then test_host_pass "outage: the consumer test FAILS (exit 1)"
else test_host_fail "outage: the consumer test exited $rc, want 1"; fi
want_line() {  # want_line <label> <file> <exact line>
    if grep -qxF -- "$3" "$2"; then
        test_host_pass "$1"
    else
        test_host_fail "$1: no line \"$3\""
    fi
}
want_line "outage: the first arm names the failed step" "$TMP/consumer-net.out" \
    "    AXL-PKG-FAIL: apt-get update failed (exit 100)"
for arm in "fedora(bash)" "ubuntu(host gcc)" "ubuntu(both)"; do
    want_line "outage: $arm is not run, and says why" "$TMP/consumer-net.out" \
        "  FAIL: $arm: NOT RUN -- a package install already failed in debian(dash) (AXL-PKG-FAIL above)"
done
if grep -q '^SKIP' "$TMP/consumer-net.out"; then
    test_host_fail "outage: the consumer test SKIPPED -- a skip would certify an install nobody ran"
else
    test_host_pass "outage: nothing is skipped"
fi
if (( dt < 150 )); then test_host_pass "outage: fails fast (${dt}s)"
else test_host_fail "outage: took ${dt}s"; fi
if (( TEST_HOST_FAIL > 0 )); then
    echo "  --- consumer test output (outage run) ---"
    tail -30 "$TMP/consumer-net.out" | sed 's/^/      /'
fi

test_host_summary "container-lib"
