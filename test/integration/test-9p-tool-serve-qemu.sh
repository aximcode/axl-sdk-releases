#!/bin/bash
# test-meta: arch=both needs= est=140 local-only=0
# test-9p-tool-serve-qemu.sh -- `9p serve` as a RESIDENT driver. The guest
# Shell runs `9p.efi serve fs0:\9pexport --detach`, which deploys the
# embedded 9p-serve-dxe.efi; the driver brings the NIC up, exports the
# staged tree and listens. The host then drives it with p9-client.py -- the
# same wire client Phase 4 is gated by -- over a QEMU port forward.
#
# What this test owns that test-9p-server-qemu.sh does not: the LAUNCHER.
# That the opts crossed the LoadOptions boundary (root, port), that the
# driver stayed resident after the launching app exited, that `status` sees
# it, and that `serve-stop` actually tears the listener down rather than
# just unpublishing a marker. p9-client.py is run WITHOUT --ro-port, so only
# its functional suite runs (~2 s) -- the adversarial suite is Phase 4's
# gate and re-running it here would buy nothing for ~35 s.
#
# UNATTENDED SEQUENCE: the command sequence lives in startup.nsh and the
# host runs its clients inside `stall` windows sized to the work (see the
# *_WINDOW_US constants). A stall before a `reset -s` matters for
# correctness, not just timing: it keeps the guest ALIVE while a post-stop
# probe runs, so "the port no longer serves 9P" is evidence that the stop
# closed the listener rather than that QEMU went away underneath it.
#
# The run ends with serve's DOCUMENTED DEFAULT — no --detach, so the
# launcher supervises in the foreground until Ctrl-C. That one needs guest
# INPUT, which the host delivers as a real 0x03 on the serial console
# (test_enable_stdin / test_send_stdin, common-test.sh). Without it the
# default branch of the tool's headline verb would be exercised by nothing
# at all, while the README described its behavior as fact.
#
# That second serve runs on THE SAME PORT as the first, deliberately. "The
# port stops answering 9P" is satisfied by a listener that merely stopped
# accepting while its EFI_TCP4 child stayed bound -- and a stranded child
# fails the next Configure() on that port with EFI_INVALID_PARAMETER, so
# serve/serve-stop/serve is not repeatable within one boot. Only a
# same-port re-listen that answers a real client proves serve-stop RELEASED
# the port rather than just muting it. EFI_TCP4 has no SO_REUSEADDR.
#
# The export tree is staged from the HOST into the FAT image, so the guest
# needs no seeding step and the bytes are exact. It mirrors the tree
# p9-client.py's functional suite expects:
#   9pexport/hello.txt      "hello from 9p server\n"   (21 bytes)
#   9pexport/sub/inner.txt  "inner\n"                  (6 bytes)
#
# Opts out of the unit ratchet (TEST_SKIP_RATCHET=1).
#
# Usage: ./test/integration/test-9p-tool-serve-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=5640
# Guest-side windows, in microseconds for the Shell's `stall`. Each is a
# BUDGET for a measured piece of host work, sized from what that work
# actually takes (the run prints the functional suite's elapsed time against
# its window) plus room for a slow CI host -- not a hang mask. p9-client.py's
# own socket timeouts fail red long before any of these close, so widening
# them buys nothing but idle seconds, doubled across two arches.
SERVE_WINDOW_US=12000000    # host functional suite: 1 s X64 / 2 s AARCH64
# The post-stop probe is the one budget that is NOT sized to observed work:
# it is sized to STOPPED_PROBE_S, because a correctly released port answers
# the probe's SYN with nothing at all rather than with a refusal (see the
# probe below), so the probe runs to its own timeout. The guest window has
# to OUTLAST that, or the guest advances to the same-port re-serve while the
# probe is still connecting -- and the probe then lands on the NEW server,
# reporting success for a listener serve-stop was supposed to have closed
# AND consuming the foreground fixture's one-shot mutations (the retry loop
# below would then fail EEXIST forever). The margin is the point.
# The same pair covers the Ctrl-C teardown's identical probe at the end of
# the run, where the thing the window has to outlast is `reset -s` rather
# than a re-serve: the probe asserts the guest is still UP.
STOPPED_PROBE_S=8           # host-side hard bound on each not-serving probe
STOPPED_WINDOW_US=16000000  # guest window: must exceed STOPPED_PROBE_S
# The foreground SERVE itself needs no window: `9p serve` without --detach
# blocks the guest itself until the host's Ctrl-C arrives.

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all 9p 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/tools/9p.efi"

# TWO identical trees, one per serve phase. p9-client.py's functional suite
# MUTATES what it walks (it Tlcreates wtest.txt and Tmkdirs newdir), so a
# second run against the first run's tree fails on EEXIST -- a fixture
# collision that would look exactly like a broken foreground serve.
for _root in 9pexport 9pexport-fg; do
    mkdir -p "$TEST_STAGING/$_root/sub"
    printf 'hello from 9p server\n' > "$TEST_STAGING/$_root/hello.txt"
    printf 'inner\n'                > "$TEST_STAGING/$_root/sub/inner.txt"
done

# Interpolated heredoc (unquoted) so $GUEST_PORT lands in the startup script.
cat << NSHEOF | test_set_startup
@echo -off
fs0:
cd \\

echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo TOOL-SERVE
9p.efi serve fs0:\\9pexport --port ${GUEST_PORT} --detach
echo TOOL-STATUS-1
9p.efi status
echo TOOL-STATUS-1-DONE
echo TOOL-SERVE-READY
stall ${SERVE_WINDOW_US}
9p.efi serve-stop
9p.efi status
echo TOOL-STOPPED
stall ${STOPPED_WINDOW_US}
echo TOOL-FG-SERVE
9p.efi serve fs0:\\9pexport-fg --port ${GUEST_PORT}
echo TOOL-FG-EXITED
9p.efi status
echo TOOL-FG-DONE
stall ${STOPPED_WINDOW_US}
reset -s
NSHEOF

test_build_image

echo "=== 9p serve resident driver ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
# The foreground-serve phase at the end needs to deliver a serial Ctrl-C.
test_enable_stdin
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID, host :$HOST_PORT -> guest :$GUEST_PORT"

if ! test_wait_for "TOOL-SERVE-READY" 120; then
    echo "FAIL: '9p serve' did not reach the ready marker within 120 seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi

CLIENT_OUT="$TEST_TMPDIR/p9-client-tool.out"
CLIENT_RC=0
_client_start=$SECONDS
timeout 90 python3 "$(dirname "$0")/p9-client.py" 127.0.0.1 "$HOST_PORT" \
    > "$CLIENT_OUT" 2>&1 || CLIENT_RC=$?
echo "  functional suite took $((SECONDS - _client_start))s of the" \
     "$((SERVE_WINDOW_US / 1000000))s serve window"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log

assert_guest_line() {
    local log="${2:-$TEST_CLEAN_LOG}"
    if grep -Fxq "$1" "$log"; then
        pass "guest: $1"
    else
        fail "guest: $1"
    fi
}

assert_client_line() {
    local out="${2:-$CLIENT_OUT}"
    if grep -Fxq "$1" "$out"; then
        pass "client: $1"
    else
        fail "client: $1  [$(grep -F "DIAG [$1]" "$out" | head -1)]"
    fi
}

# assert_port_not_serving <label> <transcript-file>
#
# Probe HOST_PORT and assert that NO 9P session completes on it, with the
# guest still up -- both teardown phases below make the same claim, so they
# make it the same way.
#
# HARD-BOUNDED, and deliberately NOT expecting a refusal. Once the teardown
# actually releases the port there is no EFI_TCP4 instance configured on it,
# so the guest answers the probe's SYN with silence rather than a RST and
# slirp keeps retrying until this bound fires. (While the port was STRANDED
# -- the bug this suite now covers -- the still-bound child produced an
# instant refusal, which is why the pre-fix probe returned in under a
# second; whether a given teardown leaves the IP4 receive path up enough to
# RST varies, so pinning either outcome would be pinning an accident.) Both
# outcomes prove the same thing. What must NOT happen is a COMPLETED 9P
# session -- that is the only way p9-client.py exits 0, so the assertion
# still fails loudly if the port is really still serving.
#
# Guest-still-up is part of the claim, not a side condition: a port that
# does not serve 9P proves the teardown closed the listener only while the
# VM (and its hostfwd) is still there to not-serve it.
assert_port_not_serving() {
    local label="$1" out="$2"
    local rc=0 alive=0 why t0=$SECONDS
    timeout "$STOPPED_PROBE_S" python3 "$(dirname "$0")/p9-client.py" \
        127.0.0.1 "$HOST_PORT" > "$out" 2>&1 || rc=$?
    kill -0 "$TEST_QEMU_PID" 2>/dev/null && alive=1
    if grep -q "CLIENT ERROR" "$out"; then
        why="connection refused"
    elif [[ $rc -eq 124 ]]; then
        why="no response within ${STOPPED_PROBE_S}s"
    else
        why="unexpected: rc=$rc, $(tail -2 "$out" | tr '\n' ' ')"
    fi
    # Printed for the same reason the functional suite's elapsed time is: the
    # guest-side stall covering this probe is a budget, and a budget nobody
    # measures drifts back into 4x headroom.
    echo "  $label probe took $((SECONDS - t0))s (${STOPPED_PROBE_S}s bound)"
    if [[ $alive -eq 1 && $rc -ne 0 && $why != unexpected:* ]]; then
        pass "$label the port no longer serves 9P ($why)"
    else
        fail "$label rc=$rc guest_alive=$alive [$why] (want a bounded non-completion with the guest still up)"
    fi
}

echo ""
echo "  --- launcher + residency ---"

assert_guest_line "9p: serving fs0:\\9pexport on port ${GUEST_PORT}"

# Both `status` lines, sliced to the FIRST status call (the one right after
# `serve`) so neither can be satisfied by the post-serve-stop call further
# down the log.
#
# "9p-mount: stopped" is not decoration: the two resident services are
# distinguished only by the GUID axl_service_* derives from
# AxlService::name, so a collision between "9p-serve" and "9p-mount" would
# make `serve-stop` unload the mount (and vice versa). Asserting only
# "9p-serve: running" cannot see that -- a collision prints BOTH as running
# and passes. Nothing in this suite deploys the mount, so it must read
# stopped while the export reads running. test-9p-tool-qemu.sh carries the
# mirror of this pair.
STATUS1_LOG="$TEST_TMPDIR/clean-status1.log"
test_slice_log TOOL-STATUS-1 TOOL-STATUS-1-DONE "$STATUS1_LOG"

assert_guest_line "9p-serve: running" "$STATUS1_LOG"
assert_guest_line "9p-mount: stopped" "$STATUS1_LOG"

echo ""
echo "  --- the resident export answers a real 9P client ---"

assert_client_line "VERSION msize=8192 version=9P2000.L"
assert_client_line "ATTACH root isdir=1"
assert_client_line "GETATTR hello.txt size=21"
assert_client_line "READ hello.txt = hello from 9p server"
assert_client_line "WALK sub/inner.txt read = ok"

[[ $CLIENT_RC -eq 0 ]] \
    && pass "p9-client functional suite exited 0" \
    || fail "p9-client functional suite exited $CLIENT_RC ($(tail -2 "$CLIENT_OUT" | tr '\n' ' '))"

# --- serve-stop must actually close the listener ------------------------
echo ""
echo "  --- serve-stop ---"

if ! test_wait_for "TOOL-STOPPED" 90; then
    fail "the guest never acknowledged serve-stop"
else
    test_clean_log

    # By this point the cleaned log holds BOTH `status` invocations (the
    # one right after `serve`, and the one right after `serve-stop`) - a
    # plain grep of the whole log can't prove which call printed "9p-serve:
    # stopped". Slice to the tail AFTER the TOOL-STATUS-1-DONE marker
    # (emitted right after the FIRST status call) so this assertion can
    # only match the SECOND status call's output.
    STATUS2_LOG="$TEST_TMPDIR/clean-after-status1.log"
    test_slice_log TOOL-STATUS-1-DONE "" "$STATUS2_LOG"

    assert_guest_line "9p-serve: stopped" "$STATUS2_LOG"

    assert_port_not_serving "after serve-stop" \
        "$TEST_TMPDIR/p9-client-afterstop.out"
fi

# --- RE-SERVE ON THE SAME PORT, and serve's DEFAULT (Ctrl-C) -------------
#
# Two claims share one phase, because the second serve is the only honest
# way to make the first one:
#
# 1. serve-stop RELEASED the port. The probe above only shows it stopped
#    answering, which a listener whose EFI_TCP4 child is still bound also
#    does. A second `9p serve` on the SAME port, answering a real client,
#    is what separates "released" from "muted" -- and EFI_TCP4 has no
#    SO_REUSEADDR, so a stranded child fails the re-listen's Configure()
#    with EFI_INVALID_PARAMETER and serve_setup reports failure.
# 2. Everything above passes --detach. The DOCUMENTED DEFAULT is the other
#    branch: axl_service_supervise blocks the launcher on axl_loop_run until
#    a break, then stops the driver. src/9p/README.md states that as plain
#    fact, so it needs a test rather than an argument -- and it is correct
#    only because axl_loop_next_event's `event_count == 0 && blocking` path
#    falls through to the poll-timer + break-event wait (src/loop/axl-loop.c).
#    The launcher's default loop has NO sources of its own, so that
#    fall-through is what carries a whole verb's default behavior.
#
# The Ctrl-C is a real 0x03 on the guest's serial ConIn (test_send_stdin),
# not a simulation: that is the byte axl-loop.c screens for.
echo ""
echo "  --- re-serve on the same port, without --detach (the default) ---"

if ! test_wait_for "TOOL-FG-SERVE" 60; then
    fail "the guest never reached the foreground-serve phase"
else
    # The driver has to redeploy and bring the NIC back up, so retry rather
    # than guess a settling time. Each attempt is p9-client.py's own
    # functional suite; the FIRST one that exits 0 is the proof.
    FG_OUT="$TEST_TMPDIR/p9-client-foreground.out"
    FG_RC=1
    _fg_start=$SECONDS
    for _i in $(seq 1 15); do
        FG_RC=0
        timeout 30 python3 "$(dirname "$0")/p9-client.py" 127.0.0.1 "$HOST_PORT" \
            > "$FG_OUT" 2>&1 || FG_RC=$?
        [[ $FG_RC -eq 0 ]] && break
        sleep 1
    done
    echo "  foreground export answered after $((SECONDS - _fg_start))s"

    [[ $FG_RC -eq 0 ]] \
        && pass "serve without --detach supervises a LIVE export" \
        || fail "foreground serve never answered (rc=$FG_RC: $(tail -2 "$FG_OUT" | tr '\n' ' '))"

    # The launcher only prints this AFTER axl_service_start_embedded returned
    # AXL_OK, which requires serve_setup's axl_9p_server_listen to have bound
    # the port. On a stranded child the launcher prints "9p: could not start
    # the serve driver" instead, and this line is simply absent -- so it is
    # the direct assertion that the SAME port was re-bindable. Sliced to the
    # tail after TOOL-FG-SERVE so the first serve's line cannot satisfy it
    # (different root, but the slice makes that structural rather than a
    # coincidence of the fixture names).
    test_clean_log
    FG_LOG="$TEST_TMPDIR/clean-after-fg-serve.log"
    test_slice_log TOOL-FG-SERVE "" "$FG_LOG"
    assert_guest_line "9p: serving fs0:\\9pexport-fg on port ${GUEST_PORT}" \
                      "$FG_LOG"

    # And real 9P replies off the re-bound port, not merely a zero exit: an
    # empty or truncated transcript with rc=0 is the "passes for the wrong
    # reason" shape these exact-line assertions exist to refuse.
    assert_client_line "VERSION msize=8192 version=9P2000.L" "$FG_OUT"
    assert_client_line "READ hello.txt = hello from 9p server" "$FG_OUT"

    # A real serial Ctrl-C. AXL's universal interrupt notice is printed from
    # _axl_cleanup on the way out (src/runtime/axl-runtime.c), so this line
    # is evidence the LAUNCHER exited through the break path -- not merely
    # that the shell script moved on, which a script the Shell aborts on
    # Ctrl-C would also produce.
    test_send_stdin '\003'

    if test_wait_for "Interrupted (Ctrl-C)" 60; then
        pass "a serial Ctrl-C ends the foreground supervise loop"
    else
        fail "Ctrl-C did not end the foreground serve (no interrupt notice)"
    fi

    # And the payload claim: supervise's axl_service_stop actually ran, so
    # the export is gone. Exactly the serve-stop probe, guest liveness
    # included for the same reason -- the guest's closing stall is sized to
    # outlast it so `reset -s` cannot race the liveness check.
    assert_port_not_serving "after Ctrl-C" \
        "$TEST_TMPDIR/p9-client-fg-afterbreak.out"
fi

# --- no leaked loop source across EITHER teardown ------------------------
#
# The cause, asserted separately from the symptom. axl_loop_free names any
# caller-owned event source still registered when the loop is torn down;
# the 9p driver's unload sequence hits that path, so a listener whose close
# only DEFERRED leaves the accept source behind and this ERROR is printed.
# Both teardowns in this run (serve-stop and the Ctrl-C stop) go through it.
#
# Deliberately a SUBSTRING grep, unlike every presence assertion above: this
# is an ABSENCE check, where a broader pattern is the stronger one -- the id
# in the real line varies per boot and the message wraps. A whole-line match
# would quietly stop matching the moment the wording shifted, which is the
# exact failure mode "never substring" exists to prevent for presence.
echo ""
echo "  --- teardown left no registered loop source ---"

test_clean_log
if grep -q "caller-owned event source" "$TEST_CLEAN_LOG"; then
    fail "a teardown leaked a loop source: $(grep -m1 "caller-owned event source" "$TEST_CLEAN_LOG")"
else
    pass "no 'caller-owned event source still active' ERROR in the serial log"
fi

echo ""
printf "9p serve resident driver: %d passed, %d failed (%s)\n" \
    "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- client output ---"; tail -30 "$CLIENT_OUT"
    echo "--- Serial log ---"; tail -60 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 18 ]] && exit 0 || exit 1
