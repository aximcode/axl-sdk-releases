#!/bin/bash
# test-meta: arch=x64 needs= est=0 local-only=0
# test-tls-strippable.sh — mbedTLS must be strippable for HTTP consumers that
# never reference TLS.
#
# libaxl always contains mbedTLS, and it must NOT be pulled into a
# consumer that links the HTTP client but only ever speaks plain http:// and
# never references axl_tls_*. The client's TLS path is reachable only through
# an ops indirection (src/net/axl-http-client-tls.h) populated by
# axl_tls_init(), so a plain-HTTP consumer lets `ld --gc-sections` drop the TLS
# module + mbedTLS, while https clients (which call axl_tls_init) and
# axl_http_server_use_tls still pull it in.
#
# This is a build/link test — no QEMU. It checks the produced binaries.
#
# Usage: ./test/integration/test-tls-strippable.sh

set -uo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJECT_DIR"

ARCH=x64
PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== TLS strippability test ==="

# Build: the full lib + the plain-HTTP
# fixture + an https-capable consumer (fetch calls axl_tls_init for https).
if ! make ARCH="$ARCH" all http-plain-selftest \
        "$("$PROJECT_DIR/scripts/build-prefix.sh" "$ARCH")/tools/fetch.efi" > /tmp/tls-strip-build.log 2>&1; then
    echo "FAIL: build failed"; tail -20 /tmp/tls-strip-build.log; exit 1
fi

PLAIN="$("$PROJECT_DIR/scripts/build-prefix.sh" "$ARCH")/http-plain-selftest.efi"
FETCH="$("$PROJECT_DIR/scripts/build-prefix.sh" "$ARCH")/tools/fetch.efi"

# NOTE ON THE MARGIN, since the numbers moved a lot and look alarming.
# Images are objcopy'd with --strip-all, so the COFF symbol table is gone and
# most `mbedtls` hits used to be SYMBOL NAMES. Measured on the same fetch.efi:
# 452 matches unstripped, 3 stripped. Assertion 2 below still holds -- the 3
# are genuine .rodata strings -- but it is now a thin margin rather than a
# comfortable one, so read a future failure there as "mbedTLS stopped linking"
# only after checking it is not "mbedTLS stopped carrying literals".
mbedtls_strings() { strings "$1" 2>/dev/null | grep -c -i mbedtls; }

# 1. The plain-HTTP-only client must contain NO mbedTLS.
n=$(mbedtls_strings "$PLAIN")
[[ "$n" -eq 0 ]] \
    && pass "plain-HTTP client links NO mbedTLS" \
    || fail "plain-HTTP client carries $n mbedTLS strings (strip regression)"

# 2. An https-capable consumer (references axl_tls_init) DOES pull mbedTLS —
#    TLS still links when the consumer actually uses it.
n=$(mbedtls_strings "$FETCH")
[[ "$n" -gt 0 ]] \
    && pass "https-capable client (fetch) still links mbedTLS ($n strings)" \
    || fail "fetch lost mbedTLS — https client would not work"

# 3. The plain client must be substantially smaller (mbedTLS is ~280 KB).
ps=$(stat -c%s "$PLAIN"); fs=$(stat -c%s "$FETCH")
[[ "$ps" -lt "$fs" ]] \
    && pass "plain client ${ps} B < https client ${fs} B (mbedTLS stripped, $((fs - ps)) B lighter)" \
    || fail "plain client ${ps} B not smaller than https client ${fs} B"

# 4/5. THE INVARIANT AN UNCONDITIONAL mbedTLS BUILD RESTS ON: an app that
# touches nothing network-related must carry no mbedTLS at all. Assertions 1-3
# prove it for an HTTP consumer; this proves it for the most basic app there
# is, which is the shape that matters once AXL_TLS stops existing and every
# build is a TLS build.
#
# Checked on the UNSTRIPPED .so with nm, not on the .efi with strings: symbol
# names are exact where the 3-literal margin noted above is thin.
#
# Both directions are asserted, because a count of zero has two causes -- "nm
# found no mbedTLS" and "nm never ran". Assertion 5 is the control: the same
# method, on an image that MUST contain mbedTLS. Without it, a broken nm
# invocation reports assertion 4 as a pass forever.
if ! make ARCH="$ARCH" hello >> /tmp/tls-strip-build.log 2>&1; then
    echo "FAIL: hello build failed"; tail -20 /tmp/tls-strip-build.log; exit 1
fi
PREFIX_DIR="$("$PROJECT_DIR/scripts/build-prefix.sh" "$ARCH")"
HELLO_SO="$PREFIX_DIR/hello.so"
FETCH_SO="$PREFIX_DIR/tools/fetch.so"

# Print the count, or "ERR" when nm could not run / the input is missing --
# never a bare 0 that cannot be told apart from a clean result.
mbedtls_syms() {
    local img="$1" out
    [[ -f "$img" ]] || { echo "ERR"; return; }
    out=$(nm "$img" 2>/dev/null) || { echo "ERR"; return; }
    printf '%s\n' "$out" | grep -c -i mbedtls
}

n=$(mbedtls_syms "$HELLO_SO")
if [[ "$n" == "ERR" ]]; then
    fail "could not read symbols from $HELLO_SO (nm failed or file absent)"
elif [[ "$n" -eq 0 ]]; then
    pass "plain hello links NO mbedTLS"
else
    fail "plain hello carries $n mbedTLS symbols — TLS is no longer strippable"
fi

n=$(mbedtls_syms "$FETCH_SO")
if [[ "$n" == "ERR" ]]; then
    fail "control unusable: could not read symbols from $FETCH_SO"
elif [[ "$n" -gt 0 ]]; then
    pass "control: the same nm check DOES see mbedTLS in fetch ($n symbols)"
else
    fail "control failed: nm saw no mbedTLS in fetch, so assertion 4 proves nothing"
fi

echo ""
printf "tls-strippable: %d passed, %d failed\n" "$PASS" "$FAIL"
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
