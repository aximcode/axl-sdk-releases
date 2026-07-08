#!/bin/bash
# test-meta: arch=x64 needs= est=20 local-only=1
# test-axl-cc-hosted-headers.sh — a consumer that directly #include's the
# compat/ hosted-libc shims (<string.h>, <stdlib.h>, <stdio.h>, ...) must
# build via the STAGED SDK for EVERY staged arch, and resolve those shims
# from the SDK — never from host glibc.
#
# Regression guard for a long-standing gap: install.sh did not stage
# include/compat/ and axl-cc did not add it to -isystem. A ported app that
# uses <string.h> et al. then failed on aa64 with
#   fatal error: string.h: No such file or directory
# (the aa64 cross-gcc has only freestanding headers, no glibc), while x64
# only "passed" by silently borrowing /usr/include (host glibc) — exactly
# what a freestanding UEFI SDK must never do. axl-utils never hit it: it
# includes no hosted-libc header directly, and no public header pulls one in.
#
# Requires a staged SDK (scripts/install.sh); point AXL_STAGE_DIR at it, or
# default to out/. Per staged arch (lib/axl/<arch>), compiles a shim-header
# consumer with the staged axl-cc and asserts (a) it builds and (b) the shim
# resolves from the SDK's compat dir. Exits 2 if the staged SDK is absent.

source "$(dirname "$0")/common-test.sh"
test_setup
# The body probes a real compile-failure path (aa64 before the fix) and reads
# each rc, so disable errexit.
set +e

STAGE="${AXL_STAGE_DIR:-$PROJECT_DIR/out}"
AXL_CC="$STAGE/bin/axl-cc"
SDK_INC="$STAGE/include/axl-sdk"
if [[ ! -x "$AXL_CC" || ! -d "$SDK_INC" ]]; then
    echo "ERROR: staged SDK missing at $STAGE — run 'scripts/install.sh' first" >&2
    exit 2
fi

pass=0
fail=0
check() {  # check <ok:0/1> <msg>
    if [[ "$1" == "0" ]]; then echo "PASS: $2"; pass=$((pass+1))
    else echo "FAIL: $2"; fail=$((fail+1)); fi
}

# 1. install.sh staged the compat shims under the SDK include root.
[[ -f "$SDK_INC/compat/string.h" ]]
check "$?" "install.sh stages compat/ shims (compat/string.h present)"

WORK="$TEST_TMPDIR/hosted"
mkdir -p "$WORK"
# A consumer that leans on the hosted-libc shims directly, like a ported app.
cat > "$WORK/consumer.c" <<'EOF'
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
int consume(const char *s, void *dst) {
    size_t n = strlen(s);
    memcpy(dst, s, n);
    return (int)n;
}
EOF

built_any=0
for arch in x64 aa64; do
    if [[ ! -d "$STAGE/lib/axl/$arch" ]]; then
        echo "SKIP: $arch not staged (lib/axl/$arch absent)"
        continue
    fi
    built_any=1

    "$AXL_CC" --arch "$arch" --type app -c "$WORK/consumer.c" \
        -o "$WORK/consumer-$arch.o" >"$WORK/$arch.log" 2>&1
    rc=$?
    check "$rc" "hosted-header consumer compiles for $arch via staged axl-cc (rc=$rc)"

    # Hermeticity: the shim must resolve from the SDK's compat dir, proving
    # axl-cc added the -isystem (not a silent host-glibc borrow).
    "$AXL_CC" --arch "$arch" --type app -c "$WORK/consumer.c" \
        -o "$WORK/consumer-$arch.o" -Wp,-v >"$WORK/$arch.vlog" 2>&1
    grep -q "axl-sdk/compat" "$WORK/$arch.vlog"
    check "$?" "$arch: compat shims on the include search path (-isystem present)"
done

if [[ "$built_any" -eq 0 ]]; then
    echo "SKIP: no arch staged under $STAGE/lib/axl"
fi

echo "--- results ---"
echo "axl-cc hosted headers: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
