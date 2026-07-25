#!/bin/bash
# test-meta: arch=both needs= est=15 local-only=0
# test-image-path-qemu.sh — axl_app_image_path() honours its documented NULL
# for synthetic load contexts, and sidecar discovery still works there.
#
# <axl/axl-app.h>: "NULL if the loaded-image protocol was unavailable or had
# no FILEPATH nodes (rare; only seen with synthetic load contexts that bypass
# the usual file-load path)."
#
# A buffer-loaded driver IS such a context — the firmware sets no FilePath,
# and AXL synthesizes a MemoryMapped(...)/FilePath device path after the load
# so the aa64 shell can render the handle. RED (before the fix): the driver
# decoded that synthetic node and reported a volume-less "\<name>" that named
# a file it was NOT loaded from (the launcher, via the default-name fallback)
# — non-NULL, and misleading to anything that then writes to <self>. The same
# synthetic FilePath also defeated the ParentHandle walk the sidecar anchor
# depends on, so `axl_resolve_data_file` found nothing at all.
#
# Staging puts the launcher + driver + sidecar in \app\ while the shell's cwd
# stays at \, so a bogus volume-less anchor cannot accidentally resolve.
#
#   RED   IMGPATH: self=\image-path-test.efi   /  sidecar=(null)
#   GREEN IMGPATH: self=(null)                 /  sidecar=...\app\imgpath-sidecar.txt
#
# The path-loaded run is the regression guard: an image that really was
# loaded from a file must still report that file.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-image-path-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"
NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"

make -C "$PROJECT_DIR" ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    image-path-test 2>&1 | tail -2

test_add_efi "$NATIVE_DIR/image-path-test.efi"   "app/image-path-test.efi"
test_add_efi "$NATIVE_DIR/image-path-driver.efi" "app/image-path-driver.efi"

mkdir -p "$TEST_STAGING/app"
echo "axl image-path sidecar marker" > "$TEST_STAGING/app/imgpath-sidecar.txt"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "app\\image-path-test.efi"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== axl_app_image_path synthetic-load contract ($TEST_ARCH) ==="

test_build_qemu_cmd
test_run_background

if ! test_wait_for "IMGPATH_DONE" 90; then
    echo "FAIL: fixture did not finish within 90s"
    test_clean_log; echo "--- Serial ---"; tail -50 "$TEST_CLEAN_LOG"
    exit 1
fi
sleep 1

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

test_clean_log

# The globs below all require a volume prefix (':') AND the \app\ directory:
# a bare "*image-path-driver.efi" would match the buggy volume-less
# "\image-path-driver.efi" just as happily as the correct
# "FS0:\app\image-path-driver.efi", which is a weaker guard than it looks.
# Assertion 1 is an exact string and carries the core contract.
mapfile -t SELFS    < <(grep -oE '^IMGPATH: self=.*$'    "$TEST_CLEAN_LOG" | sed 's/[[:space:]]*$//' || true)
mapfile -t SIDECARS < <(grep -oE '^IMGPATH: sidecar=.*$' "$TEST_CLEAN_LOG" | sed 's/[[:space:]]*$//' || true)

# One "self=" line per driver run (the launcher's own line is prefixed
# "launcher self=" so it does not land in this array).
[[ "${SELFS[0]:-}" == "IMGPATH: self=(null)" ]] \
    && pass "buffer-loaded driver reports NULL, as <axl/axl-app.h> documents" \
    || fail "buffer-loaded self ('${SELFS[0]:-<none>}')"

case "${SIDECARS[0]:-}" in
    *:*app*imgpath-sidecar.txt)
        pass "buffer-loaded driver still resolves its sidecar (anchored on the launcher)" ;;
    *)
        fail "buffer-loaded sidecar ('${SIDECARS[0]:-<none>}')" ;;
esac

case "${SELFS[1]:-}" in
    *:*app*image-path-driver.efi)
        pass "path-loaded driver reports the file it was loaded from" ;;
    *)
        fail "path-loaded self ('${SELFS[1]:-<none>}')" ;;
esac

case "${SIDECARS[1]:-}" in
    *:*app*imgpath-sidecar.txt)
        pass "path-loaded driver resolves its sidecar from its own directory" ;;
    *)
        fail "path-loaded sidecar ('${SIDECARS[1]:-<none>}')" ;;
esac

echo ""
printf "axl_app_image_path: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo ""; echo "--- Serial ---"; tail -60 "$TEST_CLEAN_LOG"
fi

[[ $FAIL -eq 0 && $PASS -eq 4 ]] && exit 0 || exit 1
