#!/bin/bash
# test-meta: arch=both needs=ssh est=110 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-policy-qemu.sh — AxlSsh's two LIVE policy setters, graded by a real
# ssh(1): the password-failure cap (D4, sshd's `MaxAuthTries`) and the
# pre-authentication banner (D9, sshd's `Banner`).
#
# WHY A FILE OF ITS OWN rather than more scenarios in test-ssh-auth-qemu.sh.
# That file "sits exactly on the shipped penalty threshold" — its own comment
# says so at length — and every scenario here either fails authentication
# repeatedly (the cap) or opens a connection per measurement cell (the
# banner). Added there, the first extra failing connection is REFUSED at
# accept and fails with a `kex_exchange_identification` transcript that has
# nothing to do with what it was testing. This file exempts the one address
# every forwarded connection arrives from, so penalties never enter into it,
# which is also why it can afford nineteen connections.
#
# WHAT ONLY A REAL PEER CAN SHOW, and therefore what is here rather than in
# AxlTestSsh:
#   cap-fixed   a connection is held to the cap it was ACCEPTED with. The
#               hook raises the cap from inside itself on the FIRST wrong
#               password, so the connection that triggered the change still
#               gets exactly the old cap's prompts. Nothing at unit level can
#               see this: it needs two connections and a change between them.
#   cap-live    ...and the NEXT connection gets the new cap.
#   banner-once one banner per connection, before the first prompt, whatever
#               user name or method is used.
#   banner-oom  a set_banner that fails to allocate leaves the PREVIOUS
#               banner in force — checked as the exact bytes a client
#               receives, not as a return code.
#   banner-live a set_banner from inside a password hook does not give the
#               current connection a second banner, and does reach the next.
#   render      how ssh(1) renders the bytes we send: LF, CRLF, a lone CR,
#               TAB, ESC, 2- and 3-byte UTF-8 and no final newline, under two
#               locales and four log levels. Captured to a FILE and compared
#               with `od -c`, never read off a terminal. The two sentences in
#               axl_ssh_server_set_banner()'s docstring about rendering and
#               suppression are written from this table, so it is a
#               measurement that is re-run on every suite pass rather than a
#               claim made once.
#
# NO KEY MATERIAL OR PASSWORD IS COMMITTED. Both are generated per run into
# $TEST_TMPDIR and only the public half of the key reaches the guest.
#
# Usage: ./test/integration/test-ssh-policy-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/ssh-guest-lib.sh"

test_parse_args "$@"

if ! command -v ssh >/dev/null 2>&1 || ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "SKIP: no ssh(1)/ssh-keygen on the host — OpenSSH is this test's oracle"
    exit 77
fi
if ! command -v od >/dev/null 2>&1; then
    echo "SKIP: no od(1) on the host — the banner is compared as BYTES"
    exit 77
fi

test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=2222
# The fixture quits its loop at this deadline and prints SERVER: DONE, which
# the teardown below waits for and grades -- a loop that stopped dispatching
# never fires its timer. Nineteen connections (the warm-up, eight render
# cells, three failing-login log-level cells, two cap, one OOM, one
# survivor, one live-set, one after, one bad-banner) at roughly a second
# each is the work; the rest is margin, and every second of it is paid on
# every run. Measured end to end at sixteen connections: 72-78 s on x64 and
# 75-82 s on aa64, the guest idling out the tail of this deadline, so three
# more fit inside it with room. Every one of them arrives from the exempt
# address, so the failing ones accrue no penalty however many there are.
SERVER_DEADLINE_MS=60000
GRACE_SEC=40

# The cap the server starts with, and the one a "setcap" hook installs from
# inside itself. BOTH are strictly below NumberOfPasswordPrompts below, so in
# either scenario the SERVER is what stops the client. A client limit equal to
# the server's cannot be falsified: with the default 3 against a cap of 3, a
# server with NO cap at all still shows exactly 3 prompts.
CAP_FIRST=2
CAP_SECOND=5
PROMPTS=7

# QEMU's user-mode gateway: the host as the guest sees it, and the one source
# every connection below arrives from.
HOST_AS_SEEN=10.0.2.2

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"

"$PROJECT_DIR/scripts/axl-build.sh" \
    ARCH="$_native_arch" all ssh-server-selftest 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/ssh-server-selftest.efi"

ssh-keygen -q -t ed25519 -N '' -C 'axl-policy' -f "$TEST_TMPDIR/good" </dev/null
{
    echo "# generated per run by $(basename "$0") -- never committed"
    cat "$TEST_TMPDIR/good.pub"
} > "$TEST_STAGING/authorized_keys"

rand_hex() { od -An -tx1 -N8 /dev/urandom | tr -d ' \n'; }
PW_CAP=$(rand_hex)
PW_BANNER=$(rand_hex)
PW_OOM=$(rand_hex)
PW_BADBAN=$(rand_hex)
{
    echo "capuser:${PW_CAP}:setcap"
    echo "banneruser:${PW_BANNER}:banner"
    echo "oomuser:${PW_OOM}:oom"
    echo "badbanuser:${PW_BADBAN}:badban"
} > "$TEST_STAGING/credentials"

# THE MEASUREMENT BANNER, written byte by byte with printf so what the guest
# sends is exactly what this file says. One banner carries every case, because
# `od -c` of the capture shows each one separately anyway and eight banners
# would be eight times the connections.
#
#   LF          an ordinary line break
#   CRLF        the break RFC 4252 section 5.4 actually names
#   lone CR     a break with no LF after it
#   TAB, ESC    the two control characters an operator is most likely to have
#               in a file by accident (a pasted colour code, an indented
#               notice)
#   2-byte UTF-8 (U+00E9) and 3-byte (U+20AC), which is where a C-locale
#               client differs from a UTF-8 one
#   no final newline    what a textarea in a settings page submits
printf 'AXL1-LF\nAXL2-CRLF\r\nAXL3-CR\rAFTER\nAXL4-TAB\there\nAXL5-ESC\033[31mRED\033[0m\nAXL6-U2\xc3\xa9-U3\xe2\x82\xac\nAXL7-noeol' \
    > "$TEST_STAGING/banner.txt"
# The banner a "banner" password hook installs from inside itself. Plain
# ASCII with a trailing LF: this one is compared for PRESENCE and ABSENCE
# across connections, and the rendering questions are the first banner's job.
printf 'AXLNEW-live banner\n' > "$TEST_STAGING/banner2.txt"

# The startup commands live in test-ssh-policy-qemu.nsh; it names the staged
# files relatively (booted: fs0:\, pooled: the lease's share directory).
test_use_nsh "$GUEST_PORT" "$SERVER_DEADLINE_MS" "$GRACE_SEC" "$HOST_AS_SEEN" "$CAP_FIRST" "$CAP_SECOND"

test_build_image

echo "=== AxlSsh policy setters (D4 cap + D9 banner) vs $(ssh -V 2>&1) ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background
trap 'test_cleanup' EXIT

echo "  QEMU PID: $TEST_QEMU_PID, host :$HOST_PORT -> guest :$GUEST_PORT"

if ! test_wait_boot "ssh: listening" 90; then
    echo "FAIL: SSH server did not start listening within ${TEST_WAITED_S} seconds"
    test_clean_log
    echo "--- Serial log ---"; tail -30 "$TEST_CLEAN_LOG"
    exit 1
fi
echo "  Server is listening"
# When the deadline clock effectively started, so the teardown waits for what
# the deadline has LEFT rather than for a fixed guess -- sized too short it
# gives up on a healthy deadline, sized from the deadline itself it reports a
# real wedge a whole deadline late.
LISTEN_T=$SECONDS

# common-test.sh installs `set -euo pipefail`, so a bare `grep -q` followed by
# `check "$?"` never reaches check. The grep goes INSIDE the condition.
pass=0; fail=0
check() {
    local name="$1"; shift
    if "$@"; then
        echo "PASS: $name"; pass=$((pass+1))
    else
        echo "FAIL: $name"; fail=$((fail+1))
    fi
}

cat > "$TEST_TMPDIR/askpass.sh" <<'ASKPASS'
#!/bin/sh
echo x >> "$ASKPASS_COUNT"
printf '%s\n' "$ASKPASS_PASSWORD"
ASKPASS
chmod +x "$TEST_TMPDIR/askpass.sh"

# A password login. NOT BatchMode (that disables password prompting outright),
# and NumberOfPasswordPrompts well above every cap this file sets, so the
# server is always what ends the conversation.
run_ssh_pw() {
    local log="$1" user="$2" pass="$3"
    shift 3
    : > "$log.count"
    SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force \
    ASKPASS_PASSWORD="$pass" ASKPASS_COUNT="$log.count" \
    ssh -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=20 \
        -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o PreferredAuthentications=password \
        -o NumberOfPasswordPrompts="$PROMPTS" \
        "$@" "$user@127.0.0.1" true > "$log" 2>&1 < /dev/null || true
}
prompts() { wc -l < "$1.count" | tr -d ' '; }

# THE KNOWN-HOSTS FILE IS WARMED BY THE FIRST render_cell BELOW, and that is
# not housekeeping. At the default log level ssh(1) prints
# `Warning: Permanently added '[127.0.0.1]:P'` ahead of everything else, and
# P moves every run -- so a capture taken with an empty known_hosts cannot be
# compared byte for byte against anything. Once the file is warm the banner is
# the FIRST thing on stderr and `head -c` is an exact comparison rather than
# a search.
KNOWN="$TEST_TMPDIR/known_hosts"
: > "$KNOWN"
WARM_LOG="$TEST_TMPDIR/render-warm.log"

# ---------------------------------------------------------------------------
# M1 — how ssh(1) renders the bytes we send, and which options suppress it
# ---------------------------------------------------------------------------
#
# stderr to a FILE, compared with `cmp`. Reading a terminal would measure the
# terminal: a lone CR moves a cursor rather than leaving a byte, and that is
# one of the cases.
#
# THE TWO EXPECTED FILES ARE THE MEASUREMENT, written down. OpenSSH 9.9p1
# passes TAB, LF and CR through unchanged and renders every OTHER control
# byte as a four-character octal escape; non-ASCII passes through only when
# the client's locale is UTF-8, and is escaped the same way under LC_ALL=C.
# So the same banner has two correct renderings and neither equals the bytes
# the server sent. They are built with the same printf the banner is, so the
# difference between each pair of lines IS the transformation being pinned.
printf 'AXL1-LF\nAXL2-CRLF\r\nAXL3-CR\rAFTER\nAXL4-TAB\there\nAXL5-ESC\\033[31mRED\\033[0m\nAXL6-U2\xc3\xa9-U3\xe2\x82\xac\nAXL7-noeol' \
    > "$TEST_TMPDIR/expect-utf8"
printf 'AXL1-LF\nAXL2-CRLF\r\nAXL3-CR\rAFTER\nAXL4-TAB\there\nAXL5-ESC\\033[31mRED\\033[0m\nAXL6-U2\\303\\251-U3\\342\\202\\254\nAXL7-noeol' \
    > "$TEST_TMPDIR/expect-c"

render_cell() {
    local name="$1" locale="$2"; shift 2
    env LC_ALL="$locale" ssh -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile="$KNOWN" \
        -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey \
        -i "$TEST_TMPDIR/good" \
        "$@" root@127.0.0.1 true > /dev/null 2> "$TEST_TMPDIR/render-$name.log" \
        || true
}

# The banner is the WHOLE of what a default-level run prints before anything
# else, so its length is where the comparison stops. `head -c` past the end of
# a short file yields a short file, which cmp then reports as differing --
# a truncated banner cannot pass this.
render_prefix_is() {
    local log="$TEST_TMPDIR/render-$1.log" expect="$TEST_TMPDIR/$2"
    local n
    n=$(wc -c < "$expect")
    if head -c "$n" "$log" | cmp -s - "$expect"; then
        return 0
    fi
    echo "      expected:"; od -c "$expect" | sed 's/^/        /'
    echo "      got (first $n bytes of $log):"
    head -c "$n" "$log" | od -c | sed 's/^/        /'
    return 1
}
# A NEGATIVE that an absent banner also satisfies, so it is worth saying what
# anchors it: the two exact-byte checks above run against the SAME server in
# the SAME boot, so "nothing on stderr" here can only be ssh(1) suppressing a
# banner this run has already proved is being sent. Sabotaging the send site
# fails those two and leaves these four passing, which is exactly the shape
# this note exists to stop anyone reading as coverage.
# `! -s` alone is TRUE for a file that does not exist, so a renamed cell
# would turn four assertions into four silent no-ops. The `-f` is what makes
# "empty" mean "ssh wrote nothing" rather than "ssh was never run".
render_empty()  { test -f "$TEST_TMPDIR/render-$1.log" \
                  && ! test -s "$TEST_TMPDIR/render-$1.log"; }
render_once()   { test "$(grep -acF 'AXL1-LF' "$TEST_TMPDIR/render-$1.log")" = 1; }

# The warm-up run, through the same helper so there is one publickey
# invocation in this file and not three. It is also the first assertion: a
# publickey login, which never sends a password, is sent the banner.
render_cell warm C.UTF-8
banner_seen() { grep -qaF "$2" "$1"; }
check "banner: a publickey login, which sends no password, is sent it" \
    banner_seen "$WARM_LOG" 'AXL1-LF'

for loc in C.UTF-8 C; do
    render_cell "default-$loc" "$loc"
    render_cell "quiet-$loc"   "$loc" -q
    render_cell "error-$loc"   "$loc" -o LogLevel=ERROR
    render_cell "verbose-$loc" "$loc" -v
done

check "render: LC_ALL=C.UTF-8, default level -- exact bytes (TAB/LF/CR kept, ESC octal-escaped, UTF-8 raw)" \
    render_prefix_is default-C.UTF-8 expect-utf8
check "render: LC_ALL=C, default level -- exact bytes (the same, with UTF-8 octal-escaped too)" \
    render_prefix_is default-C expect-c
check "render: -q suppresses the banner entirely (LC_ALL=C.UTF-8)" \
    render_empty quiet-C.UTF-8
check "render: -q suppresses the banner entirely (LC_ALL=C)" \
    render_empty quiet-C
check "render: LogLevel=ERROR suppresses the banner entirely (LC_ALL=C.UTF-8)" \
    render_empty error-C.UTF-8
check "render: LogLevel=ERROR suppresses the banner entirely (LC_ALL=C)" \
    render_empty error-C
check "render: -v prints the banner exactly once, not twice (LC_ALL=C.UTF-8)" \
    render_once verbose-C.UTF-8
check "render: -v prints the banner exactly once, not twice (LC_ALL=C)" \
    render_once verbose-C

# WHICH LOG LEVELS SUPPRESS IT, asked with a login that HAS something to
# print. The four cells above use a SUCCEEDING publickey login, so at `-q`
# and at `LogLevel=ERROR` they capture nothing -- and "nothing" there cannot
# tell "ssh withheld the banner" from "ssh had nothing to say at all". A
# docstring written from them overstated the result, which is the
# "empty is not an answer unless you know the question was asked" trap.
#
# `nokey` is the one user the fixture's publickey hook DENIES, so these three
# cells fail authentication and ssh(1) has an ERROR-level line of its own to
# print. Same server, same boot, same login: only the log level differs, and
# the default-level cell is the control that proves a failing login is sent
# the banner at all.
render_fail_cell() {
    local name="$1"; shift
    env LC_ALL=C.UTF-8 ssh -p "$HOST_PORT" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile="$KNOWN" \
        -o BatchMode=yes -o ConnectTimeout=20 -o IdentitiesOnly=yes \
        -o PreferredAuthentications=publickey \
        -i "$TEST_TMPDIR/good" \
        "$@" nokey@127.0.0.1 true > /dev/null 2> "$TEST_TMPDIR/render-$name.log" \
        || true
}
render_fail_cell deffail
render_fail_cell errfail   -o LogLevel=ERROR
render_fail_cell quietfail -q

has_banner()  { grep -qaF 'AXL1-LF' "$TEST_TMPDIR/render-$1.log"; }
no_banner()   { ! grep -qaF 'AXL1-LF' "$TEST_TMPDIR/render-$1.log"; }
nonempty()    { test -s "$TEST_TMPDIR/render-$1.log"; }

check "loglevel CONTROL: a FAILING login at the default level is sent the banner" \
    has_banner deffail
check "loglevel: -o LogLevel=ERROR still prints ssh's own error output" \
    nonempty errfail
check "loglevel: ...and withholds ONLY the banner, which is the weaker claim" \
    no_banner errfail
check "loglevel: -q on a FAILING login prints nothing at all" \
    render_empty quietfail

# ---------------------------------------------------------------------------
# D4 — the cap a connection is held to is the one it was ACCEPTED with
# ---------------------------------------------------------------------------
#
# capuser's password is CORRECT and its hook refuses it anyway, which is what
# lets one connection spend a whole cap without this file holding a list of
# wrong passwords. The hook also raises the cap to $CAP_SECOND, once, on the
# first request it ever sees -- so connection A triggers the change and must
# still get only $CAP_FIRST prompts, and connection B must get $CAP_SECOND.
CAP_A="$TEST_TMPDIR/cap-a.log"
CAP_B="$TEST_TMPDIR/cap-b.log"
run_ssh_pw "$CAP_A" capuser "$PW_CAP"
run_ssh_pw "$CAP_B" capuser "$PW_CAP"

check "cap-fixed: the connection that RAISED the cap still stops at the old one ($CAP_FIRST prompts)" \
    test "$(prompts "$CAP_A")" = "$CAP_FIRST"
check "cap-live: the next connection gets the new cap ($CAP_SECOND prompts)" \
    test "$(prompts "$CAP_B")" = "$CAP_SECOND"

# ...and both were ended by the SERVER, not by the client running out of
# prompts. Without this, a client that gave up early would look like a cap.
# Delivered since P5 (the linger), so the DISCONNECT's line is pinned whole
# (CAP_DISC_RE, lib/ssh-guest-lib.sh).
cap_a_ended() { grep -qaE "$CAP_DISC_RE" "$CAP_A"; }
cap_b_ended() { grep -qaE "$CAP_DISC_RE" "$CAP_B"; }
check "cap-fixed: the client saw the SERVER end it, with $PROMPTS prompts still allowed" \
    cap_a_ended
check "cap-live: the client saw the SERVER end it" cap_b_ended

# ONE banner for the whole of connection A, across $CAP_FIRST password
# requests -- "once per connection", measured where more than one request
# happens.
check "banner-once: one banner across a connection's $CAP_FIRST password requests" \
    test "$(grep -acF 'AXL1-LF' "$CAP_A")" = 1

test_clean_log
guest_lines() { grep -acF "$1" "$TEST_CLEAN_LOG" || true; }
check "cap-live: the guest set the new cap from inside a hook, once (SERVER: CAP-SET 0 $CAP_SECOND)" \
    test "$(guest_lines "SERVER: CAP-SET 0 $CAP_SECOND")" = 1

# ---------------------------------------------------------------------------
# D9 — a failed set leaves the previous banner in force, a live set reaches
#      the NEXT connection and not the current one
# ---------------------------------------------------------------------------
#
# oomuser's hook rigs the next allocation to fail and calls set_banner with
# the REPLACEMENT text. AXL_NO_RESOURCES is the return; the assertion that
# matters is the one after it -- the connection that follows still receives
# the ORIGINAL banner, as exact bytes.
OOM_LOG="$TEST_TMPDIR/oom.log"
run_ssh_pw "$OOM_LOG" oomuser "$PW_OOM"
test_clean_log
# -8 is AXL_NO_RESOURCES (include/axl/axl-macros.h). The number rather than
# the name because the fixture prints what it got, exactly as the hostile
# half pins SERVER: FREE-DONE -5; a substring check would accept any failure
# and this has to be the OOM one.
check "banner-oom: the guest reported AXL_NO_RESOURCES (-8) from set_banner, once" \
    test "$(guest_lines 'SERVER: BANNER-OOM -8')" = 1

render_cell survivor C.UTF-8
check "banner-oom: the NEXT connection still gets the PREVIOUS banner, byte for byte" \
    render_prefix_is survivor expect-utf8

# The live set, from inside a password hook. This connection's own banner
# decision was taken before its hook ran, so it must see the OLD text and
# never the new one.
LIVE_LOG="$TEST_TMPDIR/live.log"
run_ssh_pw "$LIVE_LOG" banneruser "$PW_BANNER"
test_clean_log
check "banner-live: the guest set a new banner from inside a hook, once (SERVER: BANNER-SET 0)" \
    test "$(guest_lines 'SERVER: BANNER-SET 0')" = 1

live_saw_old() { grep -qaF 'AXL1-LF' "$LIVE_LOG"; }
live_no_new()  { ! grep -qaF 'AXLNEW' "$LIVE_LOG"; }
check "banner-live: the connection that set it saw the OLD banner" live_saw_old
check "banner-live: ...and was NOT sent a second one" live_no_new

render_cell after C.UTF-8
printf 'AXLNEW-live banner\n' > "$TEST_TMPDIR/expect-new"
check "banner-live: the next connection gets the new banner, byte for byte" \
    render_prefix_is after expect-new
after_no_old() { ! grep -qaF 'AXL1-LF' "$TEST_TMPDIR/render-after.log"; }
check "banner-live: ...and not the old one as well" after_no_old

# ---------------------------------------------------------------------------
# axl_ssh_auth_complete: a banner it refuses leaves the request PENDING
# ---------------------------------------------------------------------------
#
# The refusal is reachable BY A PEER: the user name a hook is handed is the
# peer's bytes and is never checked for UTF-8, so an application that builds
# a banner from it gets AXL_INVALID for a name the peer chose. What the
# contract owes that application is a second chance at the same request, and
# only a live request can show it. The hook completes once with an overlong
# sequence and once with no banner at all; the login succeeding is the
# evidence, not the return codes alone.
BADBAN_LOG="$TEST_TMPDIR/badban.log"
# -v ONLY here, and only for the success line: ssh(1) prints
# `Authenticated to ... using "password".` at debug1, not at its default
# level, so without it the assertion below greps for a line this client was
# never going to write. (The same regex as test-ssh-auth-qemu.sh's
# SUCCESS_PW_RE, pinned there from a real 9.9p1 transcript; an OpenSSH bump
# moves both.)
run_ssh_pw "$BADBAN_LOG" badbanuser "$PW_BADBAN" -v
test_clean_log
# -4 is AXL_INVALID, 0 is AXL_OK (include/axl/axl-macros.h).
check "auth_complete: an invalid-UTF-8 banner is refused, then the SAME request completes (SERVER: BAD-BANNER -4 0)" \
    test "$(guest_lines 'SERVER: BAD-BANNER -4 0')" = 1
badban_authenticated() { grep -qa 'Authenticated to .* using "password"' "$BADBAN_LOG"; }
check "auth_complete: ...and the login really succeeded, so the request was still pending" \
    badban_authenticated

# ---------------------------------------------------------------------------
# Teardown: the loop reached its own deadline, and nothing leaked
# ---------------------------------------------------------------------------
#
# Waited for rather than skipped, unlike test-ssh-auth-qemu.sh, because the
# banner is the first thing in this server that OWNS heap across a
# replacement: set_banner frees the old copy and axl_ssh_server_free frees the
# last one. A leak there is invisible to every assertion above, and the
# fixture's teardown verdict is the only thing that can see it.
#
# THIS SUITE DOES NOT SEND THE EARLY-FINISH SIGNAL (lib/ssh-hostile-lib.sh,
# THE EARLY FINISH), deliberately: every hostile-lib suite now ends on the
# finish timer it arms, so this is the one suite whose boot proves, on every
# suite run, that the fixture's deadline -- the ceiling those runs fall back
# to, and a one-shot armed at boot that outlives its scenarios -- still quits
# the loop on its own. (SSH_HOSTILE_ABUSE=0 proves the same on demand.) It is the cheapest place for that: a fixed 60 s
# deadline, most of it spent on the scenarios.
done_wait=$(( SERVER_DEADLINE_MS / 1000 - (SECONDS - LISTEN_T) + 20 ))
if [[ "$done_wait" -lt 20 ]]; then done_wait=20; fi
test_wait_for "SERVER: DONE" "$done_wait" || true
test_clean_log
if [[ "$(grep -cxF 'SERVER: DONE' "$TEST_CLEAN_LOG" || true)" == 1 \
      && "$(grep -cxF 'SERVER: DEADLINE' "$TEST_CLEAN_LOG" || true)" == 1 ]]; then
    echo "PASS: the event loop reached its deadline (never wedged)"
    pass=$((pass+1))
else
    echo "FAIL: the event loop never reached its deadline — it is wedged"
    fail=$((fail+1))
fi

test_wait_for 'mem: no leaks detected\|=== AxlMem leak report:' 15 || true
test_clean_log
n_leak=$(grep -acF '=== AxlMem leak report:' "$TEST_CLEAN_LOG" || true)
n_ok=$(grep -acF 'mem: no leaks detected' "$TEST_CLEAN_LOG" || true)
check "teardown: the fixture leaked nothing (${n_leak:-0} leak reports)" \
    test "${n_leak:-0}" = 0
check "teardown: exactly one leak verdict, clean (${n_ok:-0})" \
    test "${n_ok:-0}" = 1
check "teardown: the heap recorded no corruption (SERVER: MEM-CORRUPTION 0)" \
    test "$(guest_lines 'SERVER: MEM-CORRUPTION 0')" = 1

ssh_check_no_reentry teardown

if [[ "$fail" -ne 0 ]]; then
    for f in "$WARM_LOG" "$CAP_A" "$CAP_B" "$OOM_LOG" "$LIVE_LOG" \
             "$BADBAN_LOG"; do
        [[ -e "$f" ]] || continue
        echo "--- $(basename "$f") (tail) ---"
        tail -20 "$f" | sed 's/^/      /'
    done
    test_clean_log
    echo "--- guest serial log (tail) ---"
    tail -30 "$TEST_CLEAN_LOG" | sed 's/^/      /'
fi

echo
echo "ssh policy: $pass passed, $fail failed ($TEST_ARCH)"
[[ "$fail" -eq 0 ]]
