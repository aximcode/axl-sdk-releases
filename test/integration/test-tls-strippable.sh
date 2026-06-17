#!/bin/bash
# test-tls-strippable.sh — mbedTLS must be strippable for HTTP consumers that
# never reference TLS.
#
# Building libaxl with AXL_TLS=1 must NOT pull mbedTLS (~280 KB) into a
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

echo "=== TLS strippability test (AXL_TLS=1) ==="

# Build (AXL_TLS=1 throughout — no toggle): the full lib + the plain-HTTP
# fixture + an https-capable consumer (fetch calls axl_tls_init for https).
if ! make ARCH="$ARCH" AXL_TLS=1 all http-plain-selftest \
        "out/native-$ARCH/tools/fetch.efi" > /tmp/tls-strip-build.log 2>&1; then
    echo "FAIL: build failed"; tail -20 /tmp/tls-strip-build.log; exit 1
fi

PLAIN="out/native-$ARCH/http-plain-selftest.efi"
FETCH="out/native-$ARCH/tools/fetch.efi"

mbedtls_strings() { strings "$1" 2>/dev/null | grep -c -i mbedtls; }

# 1. The plain-HTTP-only client must contain NO mbedTLS.
n=$(mbedtls_strings "$PLAIN")
[[ "$n" -eq 0 ]] \
    && pass "plain-HTTP client links NO mbedTLS under AXL_TLS=1" \
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

echo ""
printf "tls-strippable: %d passed, %d failed\n" "$PASS" "$FAIL"
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
