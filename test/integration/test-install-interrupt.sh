#!/bin/bash
# test-meta: arch=x64 needs= est=20 local-only=0
# test-install-interrupt.sh — a Ctrl-C during packaging/install.sh must end
# the script after its cleanup, not resume it.
#
# THE BUG. install.sh's trap used to be `trap cleanup EXIT INT TERM`, and
# cleanup() returns 0 rather than exiting. A trap handler that does not exit
# RESUMES the interrupted script at the point the signal landed, using
# whatever exit status the interrupted command actually finished with. Every
# window below is a foreground command install.sh runs synchronously and
# checks with `|| die ...` or `if ! ...`, so a signal that lands mid-command
# is delivered to that command too (a terminal Ctrl-C reaches the whole
# foreground PROCESS GROUP, not just the shell reading the keystroke) and the
# command dies from it -- which the resumed script then reads as an ordinary
# FAILURE of that command, not as "something interrupted me":
#
#   - during the SHA256 check (fetch_and_verify, `sha256sum -c ... || die
#     "SHA256 mismatch"`): a false integrity alarm over a download the
#     interrupt, not corruption, actually caused. The same shape gives a
#     false "could not download" / "could not extract" at the curl and tar
#     calls either side of it.
#   - during `axl-install-toolchain` under --toolchain (maybe_toolchain, `if
#     ! "$_it" "$TOOLCHAIN"`): absorbed as an ordinary toolchain failure
#     (TOOLCHAIN_FAILED=1), and main() carries on -- including into
#     maybe_prune, which with --prune --yes then runs the REAL prune anyway.
#   - during the prune's own `--dry-run` preview (maybe_prune, `"$_p"
#     --dry-run || true`): the `|| true` swallows the interrupted preview's
#     status, and execution resumes straight into the real deletion one line
#     later.
#
# THE FIX pins this test guards: install.sh's INT/TERM trap now runs cleanup
# itself and then re-raises with the signal's default action, so the shell
# dies BY the signal instead of resuming. See packaging/install.sh's
# die_by_signal for the POSIX /bin/sh mechanism (it cannot source
# scripts/axl-common.sh's bash-only axl_die_by_signal: `local`, `BASHPID`,
# `[[ ]]`).
#
# HOW EACH WINDOW IS LANDED RELIABLY. curl/tar/sha256sum finish in
# milliseconds against this test's tiny fixture, so racing a signal against
# the real window would be exactly the "hand-picked boundary proves too
# little" flakiness this tree avoids. Instead each windowed step gets a
# purpose-built stand-in that touches a marker file and then sleeps for a
# few controllable seconds before doing the real work -- the signal is sent
# only once the marker proves the target step has actually started, and a
# CONTROL run without any interrupt proves the stand-in still lets a normal
# install complete (so its absence under interrupt means something).
#
# THE GROUP SIGNAL. install.sh is always blocked on a synchronous foreground
# child (the real curl/sha256sum/tar, or this test's stand-in for one of
# them, or axl-install-toolchain/axl-prune.sh) at every window below.
# Signalling install.sh's PID ALONE would not reach that child, so the
# interrupt would sit pending until the child's own sleep finished on its
# own -- proving nothing about a signal that lands mid-step. `timeout CMD &`
# reliably starts CMD as the leader of its OWN new process group (measured:
# true whether or not the timeout ever fires), so `kill -INT -- -<pid>`
# against that PID reaches install.sh and its foreground child at once,
# exactly as a terminal Ctrl-C does. A plain `install.sh &` job, with job
# control off in a non-interactive script, would instead share THIS TEST's
# own process group -- and killing that would kill the test harness too.
#
# NOT DRIVEN: a separate window for "could not download" (the curl call
# immediately before the SHA256 check). It is the identical `cmd || die
# "msg"` shape as the checksum window below, on the same code path, a few
# lines apart -- the checksum and extraction windows already exercise that
# shape with a real interrupt twice, on two different foreground commands,
# so a third instance would be re-proving the same bash signal semantics
# rather than covering new code.
#
# Deliberately QEMU-free: drives install.sh only, no arch-specific bytes.
#
# Usage: ./test/integration/test-install-interrupt.sh

set -u
source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
# Every check below captures an rc it expects to sometimes be non-zero;
# errexit (inherited from common-test.sh) would abort before the tally.
set +e

INSTALLER="$PROJECT_DIR/packaging/install.sh"
[[ -f "$INSTALLER" ]] || { echo "FAIL: no $INSTALLER"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# shellcheck source=/dev/null
source "$(dirname "$0")/release-fixture.sh"

check() {   # check <rc> <message> [log-to-show-on-failure]
    if [[ "$1" -eq 0 ]]; then
        test_host_pass "$2"
    else
        test_host_fail "$2"
        [[ -n "${3:-}" && -f "${3:-}" ]] && sed -n '1,20p' "$3"
    fi
    return 0
}

# --- fixture -----------------------------------------------------------------
# A minimal SDK release. axl-install-toolchain and axl-prune.sh are STUBS
# (bare shell, not the real scripts): what is under test is install.sh's OWN
# signal handling around calling them, not their own behaviour, and the real
# ones have no controllable delay to land a signal inside deterministically.
make_stub_sdk() {   # make_stub_sdk <dir> <version>
    local d="$1" v="$2"
    mkdir -p "$d/bin" "$d/libexec/axl" "$d/share/axl"
    echo "$v" > "$d/share/axl/version"
    cat > "$d/bin/axl-install-toolchain" <<'TOOLCHAIN'
#!/bin/sh
touch "$AXL_TEST_MARK/toolchain-started" 2>/dev/null
sleep "${AXL_TEST_SLEEP:-3}"
touch "$AXL_TEST_MARK/toolchain-finished" 2>/dev/null
exit 0
TOOLCHAIN
    chmod +x "$d/bin/axl-install-toolchain"
    cat > "$d/libexec/axl/axl-prune.sh" <<'PRUNE'
#!/bin/sh
if [ "${1:-}" = "--dry-run" ]; then
    touch "$AXL_TEST_MARK/prune-dryrun-started" 2>/dev/null
    sleep "${AXL_TEST_SLEEP:-3}"
    echo "would remove: nothing (fixture)"
    exit 0
fi
touch "$AXL_TEST_MARK/PRUNED" 2>/dev/null
exit 0
PRUNE
    chmod +x "$d/libexec/axl/axl-prune.sh"
}

V="9.5.1"
REL="$WORK/rel"
publish "$REL" "axl-sdk-linux-$V-x86_64.tar.gz" "axl-sdk-$V" "$V" make_stub_sdk
seal "$REL" "$V"

# Slow stand-ins for sha256sum and tar, each in its OWN directory so a
# scenario puts only the one it needs ahead of the real tools on PATH.
REAL_SHA256SUM="$(command -v sha256sum)"
mkdir -p "$WORK/stub-sha"
cat > "$WORK/stub-sha/sha256sum" <<EOF
#!/bin/sh
touch "\$AXL_TEST_MARK/sha256sum-started" 2>/dev/null
sleep "\${AXL_TEST_SLEEP:-3}"
exec "$REAL_SHA256SUM" "\$@"
EOF
chmod +x "$WORK/stub-sha/sha256sum"

REAL_TAR="$(command -v tar)"
mkdir -p "$WORK/stub-tar"
cat > "$WORK/stub-tar/tar" <<EOF
#!/bin/sh
touch "\$AXL_TEST_MARK/tar-started" 2>/dev/null
sleep "\${AXL_TEST_SLEEP:-3}"
exec "$REAL_TAR" "\$@"
EOF
chmod +x "$WORK/stub-tar/tar"

HOME_DIR="$WORK/home"; mkdir -p "$HOME_DIR"

# wait_for_mark <file> -- up to 10s. Returns 1 (never found) if the target
# step was never reached, which every caller below treats as a control
# failure: nothing after it means anything without this.
wait_for_mark() {
    for _ in $(seq 1 100); do [[ -e "$1" ]] && return 0; sleep 0.1; done
    return 1
}

# run_bg <log> <mark-dir> <extra-path> <args...> -- launches install.sh
# under `timeout`, in THIS shell (never inside a `$(...)` subshell, whose job
# table this script could not later `wait` on). Leaves the child's pid in
# RUN_PID.
run_bg() {
    local log="$1" mark="$2" extra="$3"; shift 3
    mkdir -p "$mark"
    AXL_TEST_MARK="$mark" AXL_TEST_SLEEP=3 PATH="${extra:+$extra:}$PATH" \
        HOME="$HOME_DIR" timeout 30 sh "$INSTALLER" --yes "$@" \
        > "$log" 2>&1 &
    RUN_PID=$!
}

# A Ctrl-C's group signal, against the process group `timeout` created for
# RUN_PID (see the file header for why a bare PID does not do this).
group_int() { kill -INT -- "-$1" 2>/dev/null; }

echo "=== install.sh interrupt safety ==="

# --- 0. control: the slow stand-ins do not themselves break a real install --
CTL_LOG="$WORK/ctl.log"; CTL_MARK="$WORK/mark-ctl"
run_bg "$CTL_LOG" "$CTL_MARK" "$WORK/stub-sha:$WORK/stub-tar" \
    --version "$V" --base-url "file://$REL" \
    --prefix "$WORK/ctl-prefix" --bin-dir "$WORK/ctl-bin"
wait "$RUN_PID"; ctlrc=$?
[[ "$ctlrc" -eq 0 ]]
check $? "control: an uninterrupted run through both slow stand-ins still installs (rc=$ctlrc)" "$CTL_LOG"
[[ "$(cat "$WORK/ctl-prefix/axl-sdk-$V/share/axl/version" 2>/dev/null)" == "$V" ]]
check $? "control: ...and the version it installed is really $V"

# --- 1. a Ctrl-C during the SHA256 checksum ----------------------------------
LOG1="$WORK/interrupt-checksum.log"; MARK1="$WORK/mark-checksum"
run_bg "$LOG1" "$MARK1" "$WORK/stub-sha" \
    --version "$V" --base-url "file://$REL" \
    --prefix "$WORK/p1" --bin-dir "$WORK/b1"
wait_for_mark "$MARK1/sha256sum-started"
check $? "1: control: reached the checksum step before the interrupt"
group_int "$RUN_PID"
wait "$RUN_PID"; rc1=$?
[[ "$rc1" -eq 130 ]]
check $? "1: a Ctrl-C during the checksum step ends install.sh by SIGINT (rc=$rc1, want 130)" "$LOG1"
! grep -qF "SHA256 mismatch" "$LOG1"
check $? "1: ...and does not print a false SHA256 mismatch" "$LOG1"
! grep -qF "could not download" "$LOG1"
check $? "1: ...nor a false could-not-download" "$LOG1"
! grep -qF "could not extract" "$LOG1"
check $? "1: ...nor a false could-not-extract" "$LOG1"

# --- 2. a Ctrl-C during extraction -------------------------------------------
LOG2="$WORK/interrupt-extract.log"; MARK2="$WORK/mark-extract"
run_bg "$LOG2" "$MARK2" "$WORK/stub-tar" \
    --version "$V" --base-url "file://$REL" \
    --prefix "$WORK/p2" --bin-dir "$WORK/b2"
wait_for_mark "$MARK2/tar-started"
check $? "2: control: reached extraction before the interrupt"
group_int "$RUN_PID"
wait "$RUN_PID"; rc2=$?
[[ "$rc2" -eq 130 ]]
check $? "2: a Ctrl-C during extraction ends install.sh by SIGINT (rc=$rc2, want 130)" "$LOG2"
! grep -qF "could not extract" "$LOG2"
check $? "2: ...and does not print a false could-not-extract" "$LOG2"
! grep -qF "SHA256 mismatch" "$LOG2"
check $? "2: ...nor a false SHA256 mismatch" "$LOG2"

# --- 3. --prune --toolchain: a Ctrl-C during the toolchain step -------------
# must not let the real prune run anyway (the specific path task 1 names:
# TOOLCHAIN_FAILED absorbed, then maybe_prune reached on the resume).
LOG3="$WORK/interrupt-toolchain.log"; MARK3="$WORK/mark-toolchain"
run_bg "$LOG3" "$MARK3" "" \
    --version "$V" --base-url "file://$REL" \
    --prefix "$WORK/p3" --bin-dir "$WORK/b3" --prune --toolchain x64
wait_for_mark "$MARK3/toolchain-started"
check $? "3: control: reached the toolchain step before the interrupt"
group_int "$RUN_PID"
wait "$RUN_PID"; rc3=$?
[[ "$rc3" -eq 130 ]]
check $? "3: a Ctrl-C during the toolchain step ends install.sh by SIGINT (rc=$rc3, want 130)" "$LOG3"
[[ ! -e "$MARK3/toolchain-finished" ]]
check $? "3: control: the toolchain step was genuinely cut off, not merely slow"
[[ ! -e "$MARK3/PRUNED" ]]
check $? "3: ...and the interrupt did not let the real prune run anyway"

# --- 4. --prune: a Ctrl-C during the prune's own --dry-run preview ----------
# must not resume straight into the real deletion one line later.
LOG4="$WORK/interrupt-prune-dryrun.log"; MARK4="$WORK/mark-prune"
run_bg "$LOG4" "$MARK4" "" \
    --version "$V" --base-url "file://$REL" \
    --prefix "$WORK/p4" --bin-dir "$WORK/b4" --prune
wait_for_mark "$MARK4/prune-dryrun-started"
check $? "4: control: reached the prune dry-run preview before the interrupt"
group_int "$RUN_PID"
wait "$RUN_PID"; rc4=$?
[[ "$rc4" -eq 130 ]]
check $? "4: a Ctrl-C during the prune dry-run preview ends install.sh by SIGINT (rc=$rc4, want 130)" "$LOG4"
[[ ! -e "$MARK4/PRUNED" ]]
check $? "4: ...and does not resume straight into the real prune"

test_host_summary "install.sh interrupt safety ($TEST_ARCH)"
