#!/bin/bash
# test-meta: arch=x64 needs= est=25 local-only=0
# test-tcp-multi-transmit-qemu.sh — the AXL-Tcp-Queue-Design §7 spike, made
# repeatable.
#
# THE QUESTION. AXL queues sends above EFI_TCP4 because struct AxlTcp holds a
# single EFI_TCP4_IO_TOKEN, so only one send can be with the firmware at a
# time. §2 justifies keeping that limit on the grounds that the spec does not
# GUARANTEE the completion order of multiple outstanding Transmit tokens —
# while EFI_TCP4.Transmit is itself specified as "Queue outgoing data into the
# transmit queue". §7 records that the misbehaviour was never measured, only
# feared, and that the one-token limit is ours to remove if the firmware is
# in fact well behaved.
#
# WHAT THIS MEASURES. AxlTestNet.efi tcp-multi-tx submits FOUR concurrent raw
# EFI_TCP4.Transmit tokens on one connection (no AxlTcp in the path — that
# would measure our queue, not the firmware) and reports:
#   MTX-SUBMIT:<i>:<status>        what Transmit returned for each token
#   MTX-OUTSTANDING:<n>            tokens STILL un-retired once all four are
#                                  submitted — without it the run cannot tell
#                                  "the firmware queued four" from "it finished
#                                  each before the next arrived", and only n>=2
#                                  makes this a concurrency measurement at all
#   MTX-DONE:<order>:<i>:<status>  completions, in the order they signalled
#   MTX-RESULT:...                 the summary, including inorder=yes|no
# The host recorder reports the remaining half — the order the BYTES arrived
# (MTX-WIRE), which the events cannot show.
#
# WHAT IT ASSERTS. Only the invariants that hold whatever the answer is: the
# connection came up, every token was submitted, and all MTX_TOKENS x 64 KB
# reached the peer uncorrupted. The measurement itself (accepted / inorder / wire
# order) is REPORTED, not asserted: pinning firmware behaviour we do not
# control would make this a flake rather than a gate. The conclusion belongs
# in the design doc, and this script is how it gets re-derived.
#
# Usage: ./test/integration/test-tcp-multi-transmit-qemu.sh [--arch X64]

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

# Outbound networking needs a working NIC link. Under AARCH64/TCG the virt
# machine brings up no usable NIC, so DHCP never gets a lease and any outbound
# connect fails regardless of what is being measured — the same reason every
# other outbound-net test here is X64-only.
if [[ "$TEST_ARCH" == "AARCH64" ]]; then
    echo "SKIP: multi-transmit spike is X64-only (AARCH64/TCG has no NIC link)"
    exit 0
fi

REC_HOST_PORT=$(test_port 0)
REC_OUT="$TEST_TMPDIR/mtx-wire.txt"

# THE CONTROL. MTX_GAP_MS=400 makes the guest wait between submits, so each
# token retires before the next is handed over and MTX-OUTSTANDING MUST fall
# below 4. Run it that way once whenever this spike's conclusion is being
# relied on: a key number that cannot move is not a measurement, and "4
# outstanding" would read identically if the firmware only ever wrote Status
# from inside our own Poll. Default 0 — the measurement itself.
MTX_GAP_MS="${MTX_GAP_MS:-0}"

# The second control, for the ORDER measurement. MTX_ORDER=reverse submits
# 3,2,1,0, so a firmware that retires in submission order makes MTX-DONE
# print 3,2,1,0 and inorder flip to no. Run it once whenever inorder=yes is
# being relied on: the FIRST version of the collector could not print
# anything but ascending, because it polled the slots in index order, and
# said inorder=yes for that reason alone.
MTX_ORDER="${MTX_ORDER:-forward}"

# Concurrency, and the probe for the firmware's queue DEPTH. The spec sanctions
# EFI_NOT_READY ("the completion token could not be queued because the transmit
# queue is full"); OVMF has never returned it here, measured to 32 x 64 KB =
# 2 MB. The assertions below are DERIVED from this rather than hardcoded, so
# raising it stays a run rather than an edit-and-rebuild.
MTX_TOKENS="${MTX_TOKENS:-4}"
MTX_EXPECT_BYTES=$(( MTX_TOKENS * 65536 ))

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tests 2>&1 | tail -3

TEST_BUILD_DIR="$PROJECT_DIR/out/native-$_native_arch"
test_add_efi "$TEST_BUILD_DIR/AxlTestNet.efi"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo ""
    echo "echo Connecting drivers..."
    echo "connect -r"
    echo "stall 1000000"
    echo ""
    echo "echo Configuring network via DHCP..."
    echo "ifconfig -s eth0 dhcp"
    echo "stall 3000000"
    echo ""
    echo "echo Running concurrent-Transmit spike..."
    echo "AxlTestNet.efi tcp-multi-tx 10.0.2.2 ${REC_HOST_PORT} ${MTX_GAP_MS} ${MTX_ORDER} ${MTX_TOKENS}"
    echo ""
    echo "reset -s"
} | test_set_startup

test_build_image

echo "=== Concurrent EFI_TCP4.Transmit spike ($TEST_ARCH) ==="

REC_PID=0
python3 "$(dirname "$0")/tcp-multi-tx-server.py" "$REC_HOST_PORT" "$REC_OUT" &
REC_PID=$!

# EXIT INT TERM, not EXIT alone: test_setup installed all three, and a bare
# `trap ... EXIT` REPLACES them -- so a `timeout` SIGTERM from
# run-integration.sh would skip test_cleanup entirely, leaking the scratch
# dir and orphaning the recorder.
trap 'test_cleanup; [[ $REC_PID -gt 0 ]] && kill $REC_PID 2>/dev/null || true' EXIT INT TERM

sleep 1
echo "  recorder PID: $REC_PID, port: $REC_HOST_PORT"

# SLIRP user-mode networking: outbound TCP to 10.0.2.2 reaches the host.
# test_add_network rather than a hand-rolled -device/-netdev pair: it picks
# the NIC model for the arch instead of hardcoding the x64 one.
test_build_qemu_cmd
test_add_network
test_run_foreground 90

test_clean_log

# The recorder exits on EOF; give it a moment to write its summary.
wait "$REC_PID" 2>/dev/null || true
REC_PID=0

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

check "connected"      "MTX-CONNECTED"
check "all-submitted"  "MTX-RESULT:submitted=${MTX_TOKENS}"

# MTX_TOKENS x 65536 bytes. The byte count is the corruption/loss check and
# holds whatever order the firmware chose.
WIRE="$(cat "$REC_OUT" 2>/dev/null || echo "MTX-WIRE:missing")"
if [[ "$WIRE" == *"bytes=${MTX_EXPECT_BYTES}"* ]]; then
    echo "  PASS: all-bytes-arrived"
    PASS=$((PASS + 1))
else
    echo "  FAIL: all-bytes-arrived (recorder said: $WIRE)"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "--- MEASUREMENT (this is what the spike is for) ---"
# `|| true`: under set -euo pipefail an unmatched grep would abort the
# script HERE -- killing the summary and the serial-log dump below in
# exactly the case they exist for (the guest printed nothing).
grep -E "MTX-(SUBMIT|OUTSTANDING|DONE|RESULT)" "$TEST_CLEAN_LOG" | sed 's/^/  /' || true
echo "  $WIRE"
echo ""

printf "multi-transmit spike: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "--- Serial log ---"
    cat "$TEST_CLEAN_LOG"
    exit 1
fi

exit 0
