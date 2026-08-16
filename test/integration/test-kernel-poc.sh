#!/bin/bash
# test-meta: arch=x64 needs= est=7 local-only=0
# axl-kernel POC integration test — boots AxlKernelPoc.efi in QEMU,
# verifies that ping-pong + stress tests pass.
#
# Usage: ./test/integration/test-kernel-poc.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    all kernel-poc 2>&1 | tail -3

TEST_BUILD_DIR="$(test_build_dir)"
test_add_efi "$TEST_BUILD_DIR/AxlKernelPoc.efi"

cat << 'NSHEOF' | test_set_startup
@echo -off
fs0:
cd \

echo Running AxlKernelPoc...
AxlKernelPoc.efi

reset -s
NSHEOF

test_build_image

echo "=== axl-kernel POC Integration Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 60

test_clean_log

PASS=0
FAIL=0

check() {
    local name="$1"
    local pattern="$2"
    if grep -q "$pattern" "$TEST_CLEAN_LOG"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (expected: $pattern)"
        FAIL=$((FAIL + 1))
    fi
}

check "kernel-started"     "AxlKernelPoc: starting"
check "pingpong-pass"      "^PASS: pingpong"
check "stress-pass"        "^PASS: stress"
check "all-tests-passed"   "AxlKernelPoc: all tests passed"
check "clean-exit"         "AxlKernelPoc: kernel exited rc=0"

echo ""
printf "axl-kernel POC: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "--- Serial log (tail) ---"
    tail -40 "$TEST_CLEAN_LOG"
    exit 1
fi

exit 0
