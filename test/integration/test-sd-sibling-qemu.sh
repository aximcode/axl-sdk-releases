#!/bin/bash
# test-meta: arch=both needs= est=38 local-only=0
# sd-sibling — divergence-matrix fixture for the sibling-locate hard-fail +
# default-search sibling-first reorder
# (docs/superpowers/specs/2026-07-04-shared-driver-sibling-locate-design.md).
#
# Three independent QEMU images, built and booted in sequence:
#
#   hardfail image: sd-sibling-probe.efi at app/probe.efi; the driver
#     staged ONLY at drivers/<arch>/sd-sibling-driver.efi (NOT beside the
#     probe). `probe.efi hardfail` calls axl_shared_driver_locate_sibling()
#     FIRST (cold path, nothing resident) — it must hard-fail
#     (SDSIB:sibling=NOTFOUND) because the driver isn't a sibling, while the
#     multi-path axl_shared_driver_locate() still resolves it via
#     /drivers/<arch>/ (SDSIB:multi=OK).
#
#   reorder image: probe at app/probe.efi; driver-A (tag A) staged BESIDE
#     the probe (app/sd-sibling-driver.efi); driver-B (tag B) staged at
#     drivers/<arch>/sd-sibling-driver.efi. `probe.efi reorder` calls the
#     multi-path axl_shared_driver_locate() and dispatches into whatever it
#     resolves — must be the SIBLING (tag A), proving the default search
#     prefers the co-staged driver over /drivers/<arch>/.
#
#   beside image: probe at app/probe.efi; driver-A (tag A) staged ONLY
#     beside the probe (app/sd-sibling-driver.efi) — no /drivers/ copy at
#     all. `probe.efi beside` calls axl_shared_driver_locate_sibling()
#     directly — must resolve (SDSIB:sibling=OK) and dispatch (SDSIB:tag=A).
#     Proves locate_sibling's POSITIVE path (the hardfail scenario only
#     proves the negative: hard-fail when NOT staged beside the launcher).
#
# One script drives THREE images/boots, so it calls test_setup three times.
# common-test.sh's single EXIT trap only ever sees the LAST test_setup's
# TEST_TMPDIR/TEST_LOG, so this script (a) captures each scenario's log
# content into variables before moving on, (b) explicitly rm -rf's each
# non-final scenario's TEST_TMPDIR itself (else it would leak, uncleaned,
# once the next test_setup reassigns the tracking variables), and (c)
# manages TEST_KEEP_LOG itself (writing ALL THREE scenarios into one file)
# instead of relying on the built-in single-shot copy, which would only
# capture the last scenario.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: ./test/integration/test-sd-sibling-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    sd-sibling 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
PROBE="$NATIVE_DIR/sd-sibling-probe.efi"
DRIVER_A="$NATIVE_DIR/sd-sibling-driver-a.efi"
DRIVER_B="$NATIVE_DIR/sd-sibling-driver-b.efi"

# Skip-and-warn if the fixture could not be built/staged on this box
# (matches the skip convention of the sd-ergo fixture).
if [[ ! -f "$PROBE" || ! -f "$DRIVER_A" || ! -f "$DRIVER_B" ]]; then
    echo "WARN: sd-sibling fixtures not built on this box; skipping."
    echo "sd-sibling test: SKIP"
    exit 0
fi

_arch_dir=$(arch_dir "$TEST_ARCH")

# See the header comment: we own TEST_KEEP_LOG across both scenarios.
_SIB_KEEP_LOG="${TEST_KEEP_LOG:-}"
unset TEST_KEEP_LOG

# ---------------------------------------------------------------------------
# Scenario 1: hardfail — driver staged ONLY at /drivers/<arch>/, never
# beside the probe.
# ---------------------------------------------------------------------------

test_setup
test_add_efi "$PROBE" "app/probe.efi"
test_add_efi "$DRIVER_B" "drivers/$_arch_dir/sd-sibling-driver.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo SIB_BEGIN"
    echo "app\\probe.efi hardfail"
    echo "echo SIB_DONE_MARKER"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== sd-sibling hardfail scenario ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 60

test_clean_log
HARDFAIL_RAW=$(cat "$TEST_LOG")
HARDFAIL_CLEAN=$(sed -n '/SIB_BEGIN/,/SIB_DONE_MARKER/p' "$TEST_CLEAN_LOG")

echo "--- serial log (SIB_BEGIN .. SIB_DONE_MARKER) ---"
echo "$HARDFAIL_CLEAN" | sed 's/^/  /'

# Explicit cleanup NOW — the next test_setup call reassigns TEST_TMPDIR,
# and the script's single EXIT trap would otherwise never reclaim this one.
rm -rf "$TEST_TMPDIR"

# ---------------------------------------------------------------------------
# Scenario 2: reorder — driver-A beside the probe (sibling), driver-B at
# /drivers/<arch>/. The default multi-path search must prefer the sibling.
# ---------------------------------------------------------------------------

test_setup
test_add_efi "$PROBE" "app/probe.efi"
test_add_efi "$DRIVER_A" "app/sd-sibling-driver.efi"
test_add_efi "$DRIVER_B" "drivers/$_arch_dir/sd-sibling-driver.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo SIB_BEGIN"
    echo "app\\probe.efi reorder"
    echo "echo SIB_DONE_MARKER"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== sd-sibling reorder scenario ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 60

test_clean_log
REORDER_RAW=$(cat "$TEST_LOG")
REORDER_CLEAN=$(sed -n '/SIB_BEGIN/,/SIB_DONE_MARKER/p' "$TEST_CLEAN_LOG")

echo "--- serial log (SIB_BEGIN .. SIB_DONE_MARKER) ---"
echo "$REORDER_CLEAN" | sed 's/^/  /'

# Explicit cleanup NOW — the next test_setup call reassigns TEST_TMPDIR,
# and the script's single EXIT trap would otherwise never reclaim this one.
rm -rf "$TEST_TMPDIR"

# ---------------------------------------------------------------------------
# Scenario 3: beside — driver-A staged ONLY beside the probe; no /drivers/
# copy at all. Proves locate_sibling's POSITIVE (resolves + dispatches).
# ---------------------------------------------------------------------------

test_setup
test_add_efi "$PROBE" "app/probe.efi"
test_add_efi "$DRIVER_A" "app/sd-sibling-driver.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo SIB_BEGIN"
    echo "app\\probe.efi beside"
    echo "echo SIB_DONE_MARKER"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== sd-sibling beside scenario ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 60

test_clean_log
BESIDE_RAW=$(cat "$TEST_LOG")
BESIDE_CLEAN=$(sed -n '/SIB_BEGIN/,/SIB_DONE_MARKER/p' "$TEST_CLEAN_LOG")

echo "--- serial log (SIB_BEGIN .. SIB_DONE_MARKER) ---"
echo "$BESIDE_CLEAN" | sed 's/^/  /'

# ---------------------------------------------------------------------------
# TEST_KEEP_LOG — write all three scenarios' raw serial logs into ONE file.
# ---------------------------------------------------------------------------

if [[ -n "$_SIB_KEEP_LOG" ]]; then
    {
        echo "=== hardfail scenario raw serial log ==="
        echo "$HARDFAIL_RAW"
        echo ""
        echo "=== reorder scenario raw serial log ==="
        echo "$REORDER_RAW"
        echo ""
        echo "=== beside scenario raw serial log ==="
        echo "$BESIDE_RAW"
    } > "$_SIB_KEEP_LOG" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# Assertions
# ---------------------------------------------------------------------------

hardfail_sibling_notfound=$(echo "$HARDFAIL_CLEAN" | grep -c '^SDSIB:sibling=NOTFOUND$' || true)
hardfail_multi_ok=$(echo "$HARDFAIL_CLEAN" | grep -c '^SDSIB:multi=OK$' || true)
reorder_tag_a=$(echo "$REORDER_CLEAN" | grep -c '^SDSIB:tag=A$' || true)
beside_sibling_ok=$(echo "$BESIDE_CLEAN" | grep -c '^SDSIB:sibling=OK$' || true)
beside_tag_a=$(echo "$BESIDE_CLEAN" | grep -c '^SDSIB:tag=A$' || true)

echo ""
printf "Results: hardfail_sibling_notfound=%d hardfail_multi_ok=%d reorder_tag_a=%d beside_sibling_ok=%d beside_tag_a=%d\n" \
    "$hardfail_sibling_notfound" "$hardfail_multi_ok" "$reorder_tag_a" "$beside_sibling_ok" "$beside_tag_a"

if [[ "$hardfail_sibling_notfound" -ge 1 && "$hardfail_multi_ok" -ge 1 && "$reorder_tag_a" -ge 1 \
      && "$beside_sibling_ok" -ge 1 && "$beside_tag_a" -ge 1 ]]; then
    echo "sd-sibling test: OK"
    exit 0
else
    echo "sd-sibling test: FAIL"
    exit 1
fi
