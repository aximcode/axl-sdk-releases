#!/bin/bash
# test-meta: arch=x64 needs= est=5 local-only=1
# test-axl-cc-hosted-headers.sh — a consumer that directly #include's hosted
# libc headers (<string.h>, <stdlib.h>, <stdio.h>, ...) must build via the
# STAGED SDK for every staged arch that RESOLVES TO THE BARE-METAL TOOLCHAIN,
# and must resolve them from that toolchain — never from host glibc.
#
# That qualifier is load-bearing since AXL_TOOLCHAIN=auto landed
# (docs/AXL-Host-Toolchain-Design.md §3): the headers come from the
# toolchain's newlib, and where x64 resolves to `host` instead there is no
# newlib and no C library at all (§5.1), so the guarantee is conditional on
# the toolchain being installed rather than unconditional. Such an arch SKIPS
# by name below. Failing it would tell a consumer whose install is working
# exactly as designed that it is broken.
#
# The guarantee is the same one this test was written for; only the mechanism
# changed. It used to be met by SHIMS: install.sh staged include/compat/ and
# axl-cc put it on -isystem, because a ported app using <string.h> failed on
# aa64 with `fatal error: string.h: No such file or directory` (the Linux
# cross-gcc has only freestanding headers) while x64 "passed" by silently
# borrowing /usr/include — exactly what a freestanding UEFI SDK must never do.
#
# It is now met properly: axl-cc compiles C with the bare-metal cross on both
# arches, whose newlib supplies the genuine headers, and the shims are gone
# (docs/AXL-Libc-Substrate-Design.md §4.1b). So the assertions moved from "the
# shim is staged and on -isystem" to what actually matters and is now provable:
# the header resolves INSIDE the toolchain and NOT under /usr/include.
#
# Requires a staged SDK (scripts/install.sh); point AXL_STAGE_DIR at it, or
# default to wherever sdk-prefix.sh says it is. Exits 2 if it is absent.

source "$(dirname "$0")/common-test.sh"
test_setup
# The body probes a real compile-failure path (aa64 before the fix) and reads
# each rc, so disable errexit.
set +e

STAGE="${AXL_STAGE_DIR:-$(test_sdk_dir)}"
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

# 1. The shims are GONE from the staged SDK. Staging them again would put a
#    `typedef void FILE` back in front of the toolchain's real <stdio.h>.
[[ ! -e "$SDK_INC/compat" ]]
check "$?" "install.sh no longer stages compat/ shims"

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
    # Ask axl-cc which toolchain it resolved rather than re-deriving it here:
    # it owns the rule, and a second copy of it is the drift that would make
    # this test skip on the wrong machines.
    #
    # The probe's stderr is KEPT and its status read. "The probe could not
    # run" and "the probe ran and said axl" are the same empty string and
    # opposite facts: swallowing stderr would turn a broken axl-cc into a
    # silent decision to build anyway, with the reason invisible. An
    # unanswerable probe is a FAILURE here, not a skip and not a build --
    # this test cannot know whether its premise holds.
    variant="$("$AXL_CC" --arch "$arch" --print-toolchain 2>"$WORK/probe-$arch.err")"
    probe_rc=$?
    if [[ "$probe_rc" -ne 0 || -z "$variant" ]]; then
        check 1 "$arch: axl-cc could not report its resolved toolchain (rc=$probe_rc)"
        sed 's/^/      /' "$WORK/probe-$arch.err" | head -5
        continue
    fi
    if [[ "$variant" == "host" ]]; then
        echo "SKIP: $arch resolved to the host compiler; hosted libc headers"
        echo "      do not exist there by design (no newlib, no libc at all)."
        echo "      Install the bare-metal toolchain to run this:"
        echo "        axl toolchain install $arch"
        continue
    fi
    built_any=1

    "$AXL_CC" --arch "$arch" --type app -c "$WORK/consumer.c" \
        -o "$WORK/consumer-$arch.o" >"$WORK/$arch.log" 2>&1
    rc=$?
    check "$rc" "hosted-header consumer compiles for $arch via staged axl-cc (rc=$rc)"

    # Hermeticity, asserted on the RESOLVED header rather than on a flag:
    # -H makes gcc print the full path of every header it opens, so this reads
    # where <string.h> actually came from. It must be inside the bare-metal
    # toolchain and must NOT be host glibc — the silent borrow this test exists
    # to prevent, and the one thing a compile that merely SUCCEEDS cannot rule
    # out (on x64 it succeeds either way).
    "$AXL_CC" --arch "$arch" --type app -c "$WORK/consumer.c" \
        -o "$WORK/consumer-$arch.o" -H >"$WORK/$arch.vlog" 2>&1

    grep -qE '^\.+ +/.*/(string\.h)$' "$WORK/$arch.vlog"
    check "$?" "$arch: -H reported a resolved <string.h>"

    ! grep -qE '^\.+ +/usr/include/.*\.h$' "$WORK/$arch.vlog"
    check "$?" "$arch: no header resolved under /usr/include (no host-glibc borrow)"

    resolved=$(grep -oE '^\.+ +/[^ ]*/string\.h$' "$WORK/$arch.vlog" | head -1 | awk '{print $NF}')
    case "$arch" in
        x64)  want="x86_64-elf" ;;
        aa64) want="aarch64-none-elf" ;;
    esac
    [[ "$resolved" == *"$want"* ]]
    check "$?" "$arch: <string.h> came from the $want toolchain ($resolved)"
done

if [[ "$built_any" -eq 0 ]]; then
    echo "SKIP: no staged arch resolves to the bare-metal toolchain under $STAGE/lib/axl"
fi

echo "--- results ---"
echo "axl-cc hosted headers: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
