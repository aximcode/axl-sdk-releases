#!/bin/bash
# test-meta: arch=both needs=openssl est=90 local-only=0 guest=standard
# test-tls-rekey-qemu.sh -- replacing the certificate of a LIVE HTTPS server
# (axl_tls_server_set_cert, which every axl_http_server_use_tls calls).
#
# THE DEFECTS. The server certificate and key were ONE global pair that
# set_cert freed and re-parsed in place:
#   - a certificate or key that did not parse left the server with neither,
#     still attached, so every later handshake failed -- a bad upload of a
#     certificate took HTTPS down until the next good one;
#   - anything still using the old pair (a handshake in flight, which keeps
#     a pointer to it) read freed memory.
# Now each set_cert builds a new credential (config + certificate + key) and
# swaps it in only once it is complete; every connection holds a reference
# to the credential it was accepted under, released when it is freed.
#
# AxlTestNet.efi serve-tls-rekey serves POST /rekey (a new self-signed
# certificate, CN AXL-Rekey-N), /rekey-bad-cert and /rekey-bad-key, and GET
# /heap (the live AxlMem allocation count, in X-Heap-Count). The certificate
# a new connection gets is read with openssl s_client.
#
# Usage: ./test/integration/test-tls-rekey-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)
GUEST_PORT=8443

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
TEST_BUILD_DIR="$(test_build_dir)"
"$PROJECT_DIR/scripts/axl-build.sh" \
    ARCH="$_native_arch" all tests 2>&1 | tail -3

test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

# The startup commands live in test-tls-rekey-qemu.nsh
test_use_nsh

test_build_image

echo "=== HTTPS certificate replacement ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_port_forward "$HOST_PORT" "$GUEST_PORT"
test_run_background
trap 'test_cleanup; true' EXIT

if ! test_wait_boot "READY" 120; then
    echo "FAIL: HTTPS server did not start within ${TEST_WAITED_S} seconds"
    test_clean_log
    tail -20 "$TEST_CLEAN_LOG"
    exit 1
fi
sleep 2

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

BASE_URL="https://127.0.0.1:${HOST_PORT}"
CURL_OPTS=(-s --insecure -H "Connection: close" --max-time 20)

# subject -- the certificate subject a NEW connection is served, e.g.
# "subject=O=AximCode,CN=AXL-Test"; empty when the handshake fails.
subject() {
    { timeout 20 openssl s_client -connect "127.0.0.1:${HOST_PORT}" </dev/null 2>/dev/null \
          || true; } \
        | { openssl x509 -noout -subject -nameopt RFC2253 2>/dev/null || true; }
}
# post PATH -- a POST's body
post() { curl "${CURL_OPTS[@]}" -X POST "${BASE_URL}$1" 2>/dev/null || true; }
# code PATH -- a GET's status
code() { curl "${CURL_OPTS[@]}" -o /dev/null -w '%{http_code}' "${BASE_URL}$1" 2>/dev/null || true; }
# heap -- the live allocation count, after a settle: a closed connection's
# state (and the credential it held) goes when its close completes.
heap() {
    sleep 4
    { curl "${CURL_OPTS[@]}" -o /dev/null -D - "${BASE_URL}/heap" 2>/dev/null || true; } \
        | tr -d '\r' | sed -n 's/^[Xx]-[Hh]eap-[Cc]ount: \([0-9]*\)$/\1/p'
}
# serving WHAT CN -- a new connection gets CN, and a request over HTTPS works
serving() {
    local got
    got=$(subject)
    if [[ "$got" == "subject=O=AximCode,CN=$2" && "$(code /plain)" == 200 ]]; then
        pass "$1: new connections get CN=$2 and are served"
    else
        fail "$1: expected CN=$2 and /plain 200, got '$got' and $(code /plain)"
    fi
}

serving "at start" AXL-Test

echo ""
echo "  --- a certificate or key that does not parse changes nothing ---"
got=$(post /rekey-bad-cert)
[[ "$got" == '{"rc":-1}' ]] && pass "a junk certificate is refused (AXL_ERR)" \
    || fail "a junk certificate: '$got'"
serving "after the junk certificate" AXL-Test
got=$(post /rekey-bad-key)
[[ "$got" == '{"rc":-1}' ]] && pass "a junk key is refused (AXL_ERR)" \
    || fail "a junk key: '$got'"
serving "after the junk key" AXL-Test
# Both parse, but the key is not the certificate's: the likeliest real
# upload mistake, and every handshake would fail with it.
got=$(post /rekey-mismatch)
[[ "$got" == '{"rc":-1}' ]] && pass "a key that is not the certificate's is refused (AXL_ERR)" \
    || fail "a mismatched key: '$got'"
serving "after the mismatched key" AXL-Test

echo ""
echo "  --- a connection outlives the credential it was accepted under ---"
# ONE keep-alive connection: the POST replaces the certificate, the GET
# that follows rides the same TLS session (num_connects 0 says so) -- under
# the credential that has just been replaced.
same=$(curl -s --insecure --max-time 20 -o /dev/null -w '%{http_code} %{num_connects}\n' \
            -X POST "${BASE_URL}/rekey" \
            --next -s --insecure --max-time 20 -o /dev/null \
            -w '%{http_code} %{num_connects}\n' "${BASE_URL}/plain" 2>/dev/null || true)
if [[ "$same" == $'200 1\n200 0' ]]; then
    pass "a rekey and a GET on ONE connection both answer 200"
else
    fail "rekey then GET on one connection: $(tr '\n' '|' <<< "$same")"
fi
serving "after the rekey" AXL-Rekey-1
# A 200 there cannot show the reference: the old config is zeroized when
# freed, and the session still answers. The heap can. On ONE connection,
# POST then read the count while that connection is still open; read it
# again from a fresh connection once the first has closed. The CONTROL does
# the same with a POST that replaces nothing, so whatever else differs
# between a reused connection and a fresh one cancels out: what remains is
# the replaced credential, alive exactly as long as its connection.
# held_minus_released ROUTE -- that difference for one POST route
held_minus_released() {
    local held released
    sleep 4
    held=$({ curl -s --insecure --max-time 20 -o /dev/null -X POST "${BASE_URL}$1" \
                 --next -s --insecure --max-time 20 -o /dev/null -D - "${BASE_URL}/heap" \
                 2>/dev/null || true; } \
           | tr -d '\r' | sed -n 's/^[Xx]-[Hh]eap-[Cc]ount: \([0-9]*\)$/\1/p')
    released=$(heap)
    [[ -n "$held" && -n "$released" ]] && echo $((held - released))
}
control=$(held_minus_released /noop)
rekeyed=$(held_minus_released /rekey)
if [[ -n "$control" && -n "$rekeyed" && "$rekeyed" -gt "$control" ]]; then
    pass "the replaced credential lives while its connection does ($rekeyed allocations held, control $control)"
else
    fail "the replaced credential is not held by its connection (rekey '$rekeyed', control '$control')"
fi

echo ""
echo "  --- a replaced credential is freed ---"
N=10
post /rekey > /dev/null   # first-use allocations: AXL-Rekey-3
before=$(heap)
bad=0
for _ in $(seq 1 "$N"); do
    [[ "$(post /rekey)" == '{"rc":0}' ]] || bad=$((bad + 1))
done
after=$(heap)
if [[ "$bad" -ne 0 ]]; then
    fail "$bad of $N rekeys did not answer {\"rc\":0}"
elif [[ -z "$before" || -z "$after" ]]; then
    fail "could not read /heap (before='$before' after='$after')"
elif [[ "$after" -eq "$before" ]]; then
    pass "$N rekeys: live allocations $before -> $after"
else
    fail "$N rekeys changed the heap by $((after - before)) allocations ($before -> $after)"
fi
serving "after $N more rekeys" "AXL-Rekey-$((N + 3))"

echo ""
echo "tls-rekey ($TEST_ARCH): $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 && $PASS -gt 0 ]] && exit 0 || exit 1
