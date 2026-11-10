# shellcheck shell=bash
# webdav-fs-lib.sh -- the boot, the staging and the helpers of the
# axl_http_server_serve_fs end-to-end tests.
#
# Boots AxlTestNet.efi serve-davfs (an fs-backed WebDAV file server over a
# subtree of the boot volume: /dav read-write, /ro read-only, /nodel with
# DELETE refused; one staged read-only file, in davroot/lockcol) and
# drives the full verb set from the host with curl: PUT (streaming write
# via AxlFileWriter), GET (streaming read via AxlFileView), PROPFIND,
# MKCOL, MOVE, COPY, DELETE -- plus a traversal-escape rejection and a
# read-only 405. Proves the generic AxlWebDavOps glue + AxlFileWriter.
#
# THREE TESTS, ONE BOOT SEQUENCE. This was test-webdav-fs-qemu.sh, one boot
# running every case in ~160 s; one boot is the unit the runner's pool
# parallelises, so it was split by case number into three parts that set
# WEBDAV_PART and source this file:
#
#   test-webdav-fs-qemu.sh          1-12   the verbs, overwrite, auth
#   test-webdav-fs-put-qemu.sh      13-17  PUT atomicity, the slot, room
#   test-webdav-fs-framing-qemu.sh  17b-26 framing, live temps by name,
#                                          identity, case, encoding
#
# Sourcing it boots the guest and runs the no-status capture's control;
# the part runs its cases and ends with webdav_finish, whose 18a
# (request_id process-wide) and 18b (no PUT temp left on the volume) each
# part re-proves against its own boot. Every other check lives in exactly
# one part. A part that needs state an earlier case once left (precious.txt,
# /dav/sub, /dav/longdirectoryname) makes it itself, unchecked, before its
# first case. WEBDAV_STALE_TEMP1=1 stages ~AP00001.TMP, the stale temp case
# 13d deletes; 18b's control, ~AP00002.TMP, is always staged.

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
HOST_PORT2=$(test_port 1)   # the no-overwrite server (guest 8081)
HOST_PORT3=$(test_port 2)   # the case-sensitive volume (guest 8082)
HOST_PORT4=$(test_port 3)   # the PUT-in-place mounts (guest 8083)
GUEST_PORT=8080

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
test_add_efi "$(test_build_dir "$_native_arch")/AxlTestNet.efi"

cat << 'NSHEOF' | test_set_startup
@echo -off
fs0:
cd \
echo Connecting drivers...
connect -r
stall 1000000
echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000
echo Starting WebDAV file server...
AxlTestNet.efi serve-davfs
NSHEOF

# A collection that cannot be removed whole: the read-only file in it
# refuses deletion, which is how a replaced destination's leftover is made.
mkdir -p "$TEST_STAGING/davroot/lockcol"
printf 'locked\n' > "$TEST_STAGING/davroot/lockcol/ro.txt"
# A read-only file a PUT may not replace (403), and two files shaped like a
# PUT's temp but belonging to no live PUT -- a power loss's leftovers, which
# are ordinary files: ~AP00001.TMP is deleted by the test, ~AP00002.TMP is
# the control for the final on-disk check.
printf 'read only\n' > "$TEST_STAGING/davroot/rofile.txt"
if [[ "${WEBDAV_STALE_TEMP1:-0}" == 1 ]]; then
    printf 'stale\n' > "$TEST_STAGING/davroot/~AP00001.TMP"
fi
printf 'stale\n' > "$TEST_STAGING/davroot/~AP00002.TMP"
# A directory larger than the 256 entries PROPFIND once held on its stack.
mkdir -p "$TEST_STAGING/davroot/many"
for i in $(seq 1 300); do : > "$TEST_STAGING/davroot/many/f$i.txt"; done

test_build_image
if ! mattrib -i "$TEST_DISK" +r ::/davroot/lockcol/ro.txt \
    || ! mattrib -i "$TEST_DISK" +r ::/davroot/rofile.txt; then
    echo "FAIL: cannot set the read-only attribute on the staged files"
    exit 1
fi

echo "=== AxlNet serve_fs WebDAV Test: $WEBDAV_PART ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT" "$HOST_PORT2" 8081 "$HOST_PORT3" 8082 \
    "$HOST_PORT4" 8083
test_run_background

echo "  QEMU PID: $TEST_QEMU_PID  (host $HOST_PORT -> $GUEST_PORT)"
if ! test_wait_boot "READY" 60; then
    echo "FAIL: server did not start within ${TEST_WAITED_S} seconds"
    test_clean_log
    tail -20 "$TEST_CLEAN_LOG"
    exit 1
fi
echo "  Server ready"
# READY is printed once every server listens; the first answer is the proof.
for ((i = 0; i < 50; i++)); do
    [[ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:${HOST_PORT}/fs-ls" \
          2>/dev/null)" == 200 ]] && break
    sleep 0.2
done

BASE="http://127.0.0.1:${HOST_PORT}"
C=(-s -H "Connection: close" --max-time 10)
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

# code <method> <path> [extra curl args...]
code() {
    local m="$1" path="$2"; shift 2
    status_of "$m $path" curl "${C[@]}" -o /dev/null -w '%{http_code}' -X "$m" "$@" "${BASE}${path}"
}
# F11: one run in ~30 once saw a request get no HTTP status at all (curl
# 000) and it did not recur in 2,100 looped requests, idle or under full
# CPU load. So a recurrence must explain itself: status_of runs a curl
# that prints a status, and when there is none it prints, to stderr,
# curl's EXIT CODE (7 refused, 28 timed out, 52 empty reply, 56 reset --
# each a different fault), the host's TCP state for the server port, and
# the guest log's tail (a rejected client is "no free connection slots",
# at debug level). It still answers 000 on stdout, so callers are unchanged.
# status_of <what> <curl command...>
status_of() {
    local what="$1" out rc
    shift
    out=$("$@" 2>/dev/null) && rc=0 || rc=$?
    if [[ "$rc" == 0 && "$out" =~ ^[1-5][0-9][0-9]$ ]]; then
        echo "$out"
        return
    fi
    {
        echo "  DIAG: '$what' got no HTTP status: curl exit $rc, wrote '$out', at $(date +%T.%N)"
        ss -tan "( sport = :${HOST_PORT} or dport = :${HOST_PORT} )" 2>/dev/null \
            | sed 's/^/  DIAG tcp: /' | head -20
        test_clean_log
        tail -30 "$TEST_CLEAN_LOG" | sed 's/^/  DIAG guest: /'
    } >&2
    echo 000
}
# The capture's control: a request to a port nothing listens on must come
# back 000 WITH its diagnosis (curl exit 7), or the capture cannot see.
s0=$(status_of "control" curl -s -o /dev/null -w '%{http_code}' --max-time 3 \
     "http://127.0.0.1:1/" 2>"$TEST_TMPDIR/diag-control.txt")
[[ "$s0" == 000 ]] && grep -q "^  DIAG: 'control' got no HTTP status: curl exit 7," "$TEST_TMPDIR/diag-control.txt" \
    && pass "the no-status capture sees a refused connection (000, curl exit 7)" \
    || fail "no-status capture control (got '$s0', diag '$(head -1 "$TEST_TMPDIR/diag-control.txt")')"
# body <path>
body() { curl "${C[@]}" "${BASE}$1" 2>/dev/null || true; }
in2xx() { [[ "$1" == 2* ]]; }

put() { curl "${C[@]}" -o /dev/null -T - "${BASE}$1" <<< "$2" 2>/dev/null || true; }
# putc <path> <body> [curl args...]: PUT <body> plus a newline; the status.
putc() {
    local p="$1" d="$2"
    shift 2
    status_of "PUT $p" curl "${C[@]}" -o /dev/null -w '%{http_code}' "$@" -T - "${BASE}${p}" <<< "$d"
}
# raw_put_hold <port> <path> <content-length> <bytes>: send a PUT's headers
# and <bytes> of its body on fd 4, and keep the connection open.
# The connection is fd 4, or fd $RAW_FD for a second PUT held at once.
raw_put_hold() {
    local fd=${RAW_FD:-4}
    eval "exec ${fd}<>/dev/tcp/127.0.0.1/$1"
    printf 'PUT %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %s\r\n\r\n' "$2" "$3" >&"$fd"
    head -c "$4" /dev/zero >&"$fd"
}
# raw_put_finish <bytes>: send the rest of the held body; print the status line.
raw_put_finish() {
    local fd=${RAW_FD:-4}
    head -c "$1" /dev/zero >&"$fd"
    timeout 10 head -1 <&"$fd" | tr -d '\r' || true
    eval "exec ${fd}>&-"
}
raw_put_drop() { local fd=${RAW_FD:-4}; eval "exec ${fd}>&-"; }
# bodyz <path> [base]: the body with any NUL bytes dropped (a zero-filled
# partial file reads as '' rather than tripping the shell's NUL warning).
bodyz() { curl "${C[@]}" "${2:-$BASE}$1" 2>/dev/null | tr -d '\0' || true; }
# fs_ls [small | d=<dir>]: the served directory (or one below it) as the
# guest's filesystem lists it -- the only listing that shows a live temp.
fs_ls() { curl "${C[@]}" "${BASE}/fs-ls${1:+?$1}" 2>/dev/null | tr -d '\r' || true; }
# b64 <algo> <text>: RFC 9530's :base64: digest of <text> plus a newline.
b64() { printf '%s\n' "$2" | openssl dgst "-$1" -binary | base64 -w0; }
TEMP_RE='^~AP[0-9A-F]{5}\.TMP$'
# put_temps N [small | d=<dir>]: wait until the guest lists exactly N live
# temps (the two staged stale ones aside) -- a held PUT has begun storing
# its body, or a dropped one has removed its temp -- for at most
# TEMP_WAIT_S. A wait, not a check: each case asserts what it needs of the
# temp. Polled WITHOUT Connection: close, so curl closes first and no poll
# leaves the guest a TIME_WAIT close.
TEMP_WAIT_S=10
put_temps() {
    local want="$1" i n
    for ((i = 0; i < TEMP_WAIT_S * 10; i++)); do
        n=$(curl -s --max-time 5 "${BASE}/fs-ls${2:+?$2}" 2>/dev/null | tr -d '\r' \
            | grep -E "$TEMP_RE" | grep -cvxE '~AP0000[12]\.TMP' || true)
        [[ "$n" == "$want" ]] && return 0
        sleep 0.1
    done
    return 1
}

# The servers past 8080: 8081 (no-overwrite, /small, /full, /near) and 8082
# (the case-sensitive volume, /vol).
BASE2="http://127.0.0.1:${HOST_PORT2}"
BASE3="http://127.0.0.1:${HOST_PORT3}"

# webdav_finish -- 18a and 18b against this boot, the summary, the exit status.
webdav_finish() {
    # 18a) request_id is one process-wide sequence, not one per server: three
    #      requests alternating between the two servers get rising ids.
    i1=$(curl "${C[@]}" "${BASE}/req-id" 2>/dev/null || true)
    i2=$(curl "${C[@]}" "${BASE2}/req-id" 2>/dev/null || true)
    i3=$(curl "${C[@]}" "${BASE}/req-id" 2>/dev/null || true)
    [[ "$i1" =~ ^[0-9]+$ && "$i2" =~ ^[0-9]+$ && "$i3" =~ ^[0-9]+$ && "$i1" -gt 0 \
       && "$i2" -gt "$i1" && "$i3" -gt "$i2" ]] \
        && pass "request_id rises across both servers ($i1 < $i2 < $i3)" \
        || fail "request_id not process-wide (8080 '$i1', 8081 '$i2', 8080 '$i3')"

    # 18b) No PUT left a temp on the volume. Read the disk itself once the guest
    #     is down. The staged ~AP00002.TMP is the control -- the listing must see
    #     it, or it sees nothing.
    kill "$TEST_QEMU_PID" 2>/dev/null || true
    wait "$TEST_QEMU_PID" 2>/dev/null || true
    TEST_QEMU_PID=0
    left=$(mdir -i "$TEST_DISK" -/ -b ::/davroot 2>/dev/null | grep -E '/~AP[0-9A-F]{5}\.TMP$' || true)
    [[ "$left" == "::/davroot/~AP00002.TMP" ]] \
        && pass "no PUT temp left on the volume (only the staged stale one)" \
        || fail "PUT temps on the volume: '$(tr '\n' ' ' <<< "$left")' (want only ::/davroot/~AP00002.TMP)"

    echo ""
    echo "  serve_fs WebDAV $WEBDAV_PART: $PASS passed, $FAIL failed ($TEST_ARCH)"
    [[ "$FAIL" -eq 0 ]] || exit 1
    exit 0
}
