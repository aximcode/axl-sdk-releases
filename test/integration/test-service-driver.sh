#!/bin/bash
# test-meta: arch=x64 needs= est=7 local-only=0
# AxlService embedded-driver deployment integration test.
#
# Boots service_demo.efi (sdk/examples/service-demo.c, built via the
# AXL_SERVICE macro into a launcher app + service_demo-dxe.efi
# driver image). axl_service_main's `start --detach` verb serializes
# the foreground options through LoadOptions and calls
# axl_service_start_embedded; the driver image's AXL_SERVICE_DRIVER
# macro decodes LoadOptions and runs setup. The `stop` verb unloads
# via the protocol GUID; second `start --detach` proves a fresh
# load works after the prior unload.
#
# Asserts on:
#   - "service-demo: setup: ..." log line (driver setup ran)
#   - "service-demo: teardown" log line (stop fired the unload)
#   - "service-demo: stopped" line (axl_service_main stop verb)
#   - port=8080 verbose=0 name=demo (LoadOptions round-trip)
#
# Usage: ./test/integration/test-service-driver.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    service-demo 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$NATIVE_DIR/service_demo.efi"

# Launch → stop → relaunch round-trip — single AXL_SERVICE binary
# with the default verb tree from axl_service_main:
#   - `service_demo launch --detach` publishes the protocol GUID on
#     the driver image's handle and exits to the shell
#   - `service_demo stop` LocateHandleBuffers it from the GUID and
#     unloads (which fires AXL_SERVICE_DRIVER's unload stub: detach,
#     teardown, unregister, free)
#   - second `service_demo launch --detach` sees no protocol →
#     step-1 short-circuit doesn't fire → fresh LoadImage+StartImage
{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "service_demo.efi start --detach"
    echo "service_demo.efi stop"
    # Short-flag + CHOICE round-trip (exercises AxlConfigDesc.short_name
    # and AxlConfigDesc.choices passthrough in the axl_service_main
    # synthesizer): -p 9090 short flag, -v short flag, --name alpha
    # is a valid choice. Driver's setup log line should reflect all
    # three values post-LoadOptions decode.
    echo "service_demo.efi start -p 9090 -v --name alpha --detach"
    echo "service_demo.efi stop"
    echo "service_demo.efi start --detach"
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== AxlService Embedded-Driver Test ($TEST_ARCH) ==="

test_build_qemu_cmd
test_add_no_network
test_run_foreground 30

test_clean_log

# Echo the relevant lines for diagnosis. axl_service_main emits
# "service-demo: stopped" via axl_printf on the stop verb; the
# driver image emits "setup: ..." / "teardown" via axl_info under
# the "service-demo" log domain.
grep -E "service-demo:.*(setup:|teardown|stopped|running|start failed)" \
    "$TEST_CLEAN_LOG" | while IFS= read -r line; do
    echo "  $line"
done

setup_count=$(grep -c "service-demo:.*setup: port="     "$TEST_CLEAN_LOG" || true)
teardown_count=$(grep -c "service-demo:.*teardown"      "$TEST_CLEAN_LOG" || true)
stopped_count=$(grep -c "service-demo: stopped"         "$TEST_CLEAN_LOG" || true)
fails=$(grep -c "service-demo:.*start failed\|^FAIL:" "$TEST_CLEAN_LOG" || true)

# Cross-binary option round-trip: the driver image's setup log line
# carries g_opts post-LoadOptions-decode. Defaults (port=8080,
# verbose=false, name=demo) come through both via the descriptor and
# the foreground's serialize-into-LoadOptions step.
opts_ok=0
if grep -qE "service-demo:.*setup: port=8080 verbose=0 name=demo" \
        "$TEST_CLEAN_LOG"; then
    opts_ok=1
fi

# Short-flag + CHOICE round-trip: the second launch passed
# `-p 9090 -v --name alpha`, which exercises AxlConfigDesc.short_name
# and AxlConfigDesc.choices passthrough in axl_service_main's
# synthesizer. Driver's setup log should show all three values.
opts_short_ok=0
if grep -qE "service-demo:.*setup: port=9090 verbose=1 name=alpha" \
        "$TEST_CLEAN_LOG"; then
    opts_short_ok=1
fi

echo ""
echo "Counts: setup=$setup_count teardown=$teardown_count" \
     "stopped=$stopped_count FAIL=$fails" \
     "round_trip_ok=$opts_ok short_choice_ok=$opts_short_ok"

# Expected:
#   - >=3 driver setup log lines (three launch invocations)
#   - >=2 driver teardown log lines (each stop triggers UnloadImage)
#   - >=2 "service-demo: stopped" lines (axl_service_main's stop verb)
#   - opts_ok=1 (LoadOptions round-trip carried defaults to driver-side)
#   - opts_short_ok=1 (short flags + CHOICE option synthesized correctly)
#   - 0 FAIL
if [[ $fails -eq 0
      && $setup_count -ge 3
      && $teardown_count -ge 2
      && $stopped_count -ge 2
      && $opts_ok -eq 1
      && $opts_short_ok -eq 1 ]]; then
    echo "Service-driver test: OK (launch → stop → relaunch round-trip)"
    exit 0
else
    echo "Service-driver test: FAILED"
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi
