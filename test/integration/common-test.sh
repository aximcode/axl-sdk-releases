#!/bin/bash
# Shared test infrastructure for UdkLib QEMU-based tests.
# Sources common.sh for QEMU/firmware discovery, provides lightweight
# disk creation (FAT32, no GPT, no sudo) and QEMU run helpers.
#
# Usage in test scripts:
#   source "$(dirname "$0")/common-test.sh"
#   test_parse_args "$@"
#   test_setup
#   test_add_efi "$BUILD_DIR/MyApp.efi"
#   test_set_startup "MyApp.efi"
#   test_build_image
#   test_run_foreground 20    # timeout in seconds
#   test_count_results        # parse PASS/FAIL from serial log

set -euo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$TESTS_DIR")")"
source "$PROJECT_DIR/scripts/axl-common.sh"

# State variables (set by helpers)
TEST_ARCH="X64"
TEST_TMPDIR=""
TEST_STAGING=""
TEST_DISK=""
TEST_LOG=""
TEST_CLEAN_LOG=""
TEST_NVRAM=""
TEST_BOOT_NAME=""
TEST_QEMU_PID=0
TEST_ECHO_PID=""
TEST_ECHO_PORT=""
TEST_UDP_ECHO_PID=""

# ---------------------------------------------------------------------------
# Arg parsing
# ---------------------------------------------------------------------------

test_parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --arch) TEST_ARCH="$2"; shift 2 ;;
            *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
        esac
    done
}

# ---------------------------------------------------------------------------
# Setup / teardown
# ---------------------------------------------------------------------------

test_setup() {
    TEST_START_TIME=$(date +%s%N)
    TEST_TMPDIR=$(mktemp -d)
    TEST_STAGING="$TEST_TMPDIR/staging"
    TEST_DISK="$TEST_TMPDIR/test.img"
    TEST_LOG="$TEST_TMPDIR/serial.log"
    TEST_CLEAN_LOG="$TEST_TMPDIR/clean.log"
    TEST_NVRAM="$TEST_TMPDIR/vars.fd"
    TEST_BOOT_NAME=$(boot_efi_name "$TEST_ARCH")

    # Resolve QEMU and firmware. find_qemu's `export QEMU_DIR` happens
    # in the $() subshell, so it doesn't reach our parent shell —
    # re-derive it here so find_firmware below can locate QEMU-bundled
    # firmware ($QEMU_DIR/../share/qemu/edk2-*-code.fd) when applicable.
    TEST_QEMU_BIN=$(find_qemu "$TEST_ARCH") || { echo "QEMU not found for $TEST_ARCH"; exit 1; }
    QEMU_DIR="$(dirname "$TEST_QEMU_BIN")"
    export QEMU_DIR
    find_firmware "$TEST_ARCH" || { echo "Firmware not found for $TEST_ARCH"; exit 1; }

    # Copy NVRAM template
    cp "$FW_VARS" "$TEST_NVRAM"

    # Cleanup trap
    trap 'test_cleanup' EXIT
}

test_cleanup() {
    if [[ $TEST_QEMU_PID -gt 0 ]]; then
        kill "$TEST_QEMU_PID" 2>/dev/null || true
        wait "$TEST_QEMU_PID" 2>/dev/null || true
    fi
    if [[ -n "${TEST_ECHO_PID:-}" ]] && kill -0 "$TEST_ECHO_PID" 2>/dev/null; then
        kill "$TEST_ECHO_PID" 2>/dev/null || true
        wait "$TEST_ECHO_PID" 2>/dev/null || true
    fi
    if [[ -n "${TEST_UDP_ECHO_PID:-}" ]] && kill -0 "$TEST_UDP_ECHO_PID" 2>/dev/null; then
        kill "$TEST_UDP_ECHO_PID" 2>/dev/null || true
        wait "$TEST_UDP_ECHO_PID" 2>/dev/null || true
    fi
    #
    # TEST_KEEP_LOG=<path> preserves the raw serial log outside the
    # tmpdir so post-mortem inspection is possible after the
    # auto-cleanup runs. Useful when hunting stalls.
    #
    if [[ -n "${TEST_KEEP_LOG:-}" && -f "$TEST_LOG" ]]; then
        cp -f "$TEST_LOG" "$TEST_KEEP_LOG" 2>/dev/null || true
    fi
    [[ -n "$TEST_TMPDIR" ]] && rm -rf "$TEST_TMPDIR"
}

# ---------------------------------------------------------------------------
# Staging helpers
# ---------------------------------------------------------------------------

# Add an EFI binary to the image root (or a subdirectory)
test_add_efi() {
    local src="$1"
    local dest="${2:-$(basename "$1")}"

    if [[ ! -f "$src" ]]; then
        echo "Not found: $src"
        # Point at the right make target based on the missing artifact
        # so the next command actually builds the missing thing.
        local hint
        case "$src" in
            */tools/*.efi)  hint="make tools${TEST_ARCH:+ ARCH=${TEST_ARCH,,}}" ;;
            */AxlTest*.efi) hint="make tests${TEST_ARCH:+ ARCH=${TEST_ARCH,,}}" ;;
            *.efi)          hint="make all${TEST_ARCH:+ ARCH=${TEST_ARCH,,}}" ;;
            *)              hint="make all tools tests${TEST_ARCH:+ ARCH=${TEST_ARCH,,}}" ;;
        esac
        echo "Build first: $hint"
        exit 1
    fi

    mkdir -p "$TEST_STAGING/$(dirname "$dest")"
    cp "$src" "$TEST_STAGING/$dest"
}

# Add NIC drivers for the current arch
test_add_drivers() {
    local driver_dir="$PROJECT_DIR/build/staging/drivers/$(arch_dir "$TEST_ARCH")"

    if [[ -d "$driver_dir" ]]; then
        mkdir -p "$TEST_STAGING/drivers"
        cp "$driver_dir"/*.efi "$TEST_STAGING/drivers/" 2>/dev/null || true
    fi
}

# Write startup.nsh content
test_set_startup() {
    cat > "$TEST_STAGING/startup.nsh"
}

# ---------------------------------------------------------------------------
# Image creation
# ---------------------------------------------------------------------------

test_build_image() {
    # Stage Shell.efi as boot binary if available. This prevents PXE
    # boot delays when networking is enabled and ensures the NIC option
    # ROM loads the network stack drivers.
    local shell_efi
    shell_efi=$(find_shell_efi "$TEST_ARCH") || true
    if [[ -n "$shell_efi" && -f "$shell_efi" ]]; then
        mkdir -p "$TEST_STAGING/EFI/BOOT"
        cp "$shell_efi" "$TEST_STAGING/EFI/BOOT/$TEST_BOOT_NAME"
    fi

    if [[ -n "${MKIMAGE_DIR:-}" && -f "$MKIMAGE_DIR/mkimage.py" ]]; then
        "$MKIMAGE_DIR/mkimage.py" \
            --source "$TEST_STAGING" \
            --target "$TEST_DISK" \
            --label TEST 2>/dev/null
    else
        # Fallback: create FAT32 image with standard tools.
        # mcopy -s -p recurses and preserves attributes; passing
        # individual top-level entries lets it auto-create the
        # destination directory tree (per-file `mmd ::/EFI/BOOT`
        # doesn't work because mmd refuses to create intermediate
        # parents and the per-file loop hits set-e on the first
        # unwritable nested file).
        local size_kb
        size_kb=$(du -sk "$TEST_STAGING" | cut -f1)
        size_kb=$(( (size_kb + 4096) / 1024 * 1024 ))  # round up to nearest MB
        [[ $size_kb -lt 40960 ]] && size_kb=40960       # minimum 40MB
        dd if=/dev/zero of="$TEST_DISK" bs=1K count="$size_kb" 2>/dev/null
        mkfs.vfat -F 32 -n TEST "$TEST_DISK" >/dev/null 2>&1
        # Recursive copy of every top-level entry under STAGING.
        # `find -maxdepth 1` to enumerate top-level files and dirs,
        # then mcopy -s for each (recursive). Run within STAGING so
        # the destination paths come out at the FAT root.
        (
            cd "$TEST_STAGING" || exit 1
            for entry in $(find . -maxdepth 1 -mindepth 1 -printf '%P\n'); do
                mcopy -s -i "$TEST_DISK" "$entry" "::/" 2>/dev/null
            done
        )
    fi
}

# ---------------------------------------------------------------------------
# QEMU command construction
# ---------------------------------------------------------------------------

# Build base QEMU command array. Caller can append extra args.
# Sets TEST_QEMU_CMD array.
test_build_qemu_cmd() {
    mapfile -d '' -t TEST_QEMU_CMD < <(
        build_qemu_base_cmd "$TEST_ARCH" "$TEST_QEMU_BIN" 512M "$TEST_NVRAM"
    )
    TEST_QEMU_CMD+=(
        -drive "format=raw,file=$TEST_DISK"
        -nographic
        -no-reboot
    )
    # Inject a small PCI bridge tree so the AxlPci tree-walker tests
    # have non-trivial topology to exercise. Stock q35 + aa64 virt are
    # otherwise flat (no PCI-PCI bridges), and shipping bridge code
    # without test coverage is how the aa64 cap-walk infinite-loop
    # (commit 8b90954) made it past CI in the first place.
    #
    # Layout: one PCIe root port (a PCI-PCI bridge per the spec) on
    # the root PCIe bus, with a virtio-rng device behind it. The
    # exact BDF/secondary-bus is QEMU/firmware-assigned; tests that
    # care must locate the bridge by class (0x060400) rather than by
    # hard-coded address.
    #
    # Cross-arch: pcie.0 is the root PCIe bus on both q35 and aa64
    # virt; virtio-rng-pci is supported by both QEMU machines.
    # chassis=1 is required for pcie-root-port; slot is left unset
    # so QEMU auto-assigns and avoids collision with the q35 mch
    # at 00:00.0.
    TEST_QEMU_CMD+=(
        -device "pcie-root-port,id=axl_rp0,bus=pcie.0,chassis=1"
        -device "virtio-rng-pci,bus=axl_rp0"
    )
    # USB controller + leaf devices so AxlUsb tests have real
    # EFI_USB_IO_PROTOCOL handles to enumerate. qemu-xhci is
    # supported on both q35 and aa64-virt; usb-mouse is class-
    # compliant HID and gets enumerated by OVMF / AAVMF without an
    # extra driver bundle. usb-hub adds a downstream USB bus tier
    # so the AxlUsb tree walker (Phase F) can prove non-zero hub
    # depth against a real port chain; usb-tablet attaches behind
    # it via port=hub_port.subport notation (QEMU's hierarchical
    # USB addressing). Total: 3 EFI_USB_IO_PROTOCOL handles
    # (mouse direct, hub direct, tablet behind hub).
    TEST_QEMU_CMD+=(
        -device "qemu-xhci,id=axl_usb0"
        -device "usb-mouse,bus=axl_usb0.0,port=1"
        -device "usb-hub,bus=axl_usb0.0,port=2"
        -device "usb-tablet,bus=axl_usb0.0,port=2.1"
    )
    # Debug hooks — opt-in via env vars so the normal test path is
    # untouched. TEST_QEMU_GDB=PORT exposes the QEMU GDB stub for
    # interactive debugging; TEST_QEMU_DEBUGCON=FILE captures OVMF
    # DEBUG output (port 0x402) which scripts/gdb-syms.py needs to
    # recover module load addresses.
    if [[ -n "${TEST_QEMU_GDB:-}" ]]; then
        # Strip -enable-kvm / -cpu host (mirroring run-qemu.sh): KVM
        # is incompatible with single-stepping early-boot instructions,
        # so the GDB stub falls back to TCG.
        local _filtered=()
        local _skip=0
        for _arg in "${TEST_QEMU_CMD[@]}"; do
            if [[ $_skip -gt 0 ]]; then _skip=$((_skip-1)); continue; fi
            case "$_arg" in
                -enable-kvm) ;;          # drop
                -cpu)        _skip=1 ;;  # drop with its value
                *)           _filtered+=("$_arg") ;;
            esac
        done
        TEST_QEMU_CMD=("${_filtered[@]}")
        TEST_QEMU_CMD+=(-gdb "tcp::${TEST_QEMU_GDB}")
        log_info "QEMU GDB stub on tcp::${TEST_QEMU_GDB} (KVM disabled, TCG)"
    fi
    if [[ -n "${TEST_QEMU_DEBUGCON:-}" ]]; then
        TEST_QEMU_CMD+=(
            -debugcon "file:${TEST_QEMU_DEBUGCON}"
            -global "isa-debugcon.iobase=0x402"
        )
        log_info "OVMF debugcon → ${TEST_QEMU_DEBUGCON}"
    fi
}

# NIC device appropriate for the architecture
_test_nic_device() {
    if [[ "$TEST_ARCH" == "AARCH64" ]]; then
        echo "virtio-net-pci"
    else
        echo "e1000"
    fi
}

# Add port forwarding: test_add_port_forward HOST_PORT GUEST_PORT
test_add_port_forward() {
    TEST_QEMU_CMD+=(
        -device "$(_test_nic_device),netdev=net0"
        -netdev "user,id=net0,hostfwd=tcp::${1}-:${2}"
    )
}

# Add networking (QEMU user-mode, no port forwarding)
test_add_network() {
    TEST_QEMU_CMD+=(
        -device "$(_test_nic_device),netdev=net0"
        -netdev "user,id=net0"
    )
}

# Slirp subnet IP that the unit-test runner reserves as the "echo
# target." Tests connect to this IP (any port) and slirp's guestfwd
# rules redirect the byte stream to a host-side stream-echo server.
# Picked inside slirp's default 10.0.2.0/24 subnet but distinct from
# the guest's own DHCP'd 10.0.2.15.
#
# Slirp does NOT intercept connections to the guest's own IP — those
# packets bypass slirp's TCP routing — so a non-self IP is required.
TEST_ECHO_HOST="10.0.2.100"

# UDP echo port. Slirp's `guestfwd` is TCP-only (no UDP form), but
# UDP datagrams sent from the guest to 10.0.2.2 (the slirp gateway)
# are delivered to host loopback natively by slirp. We pin a fixed
# port so the guest-side test code can target it directly without
# needing to plumb a runtime-allocated port number into the guest.
TEST_UDP_ECHO_PORT=35555

# Add networking with a stream-echo backstop reachable at
# $TEST_ECHO_HOST:<any port>. Tests can do
# `axl_tcp_connect("10.0.2.100", port, ...)` and get a working remote
# peer that echoes every byte back, without any real intra-guest
# networking.
#
# This helper:
#   1. Starts a host-side TCP echo server (test/integration/echo-stream.py)
#      on 127.0.0.1:<TEST_ECHO_PORT> as a background process. The PID
#      is captured in TEST_ECHO_PID so the cleanup trap kills it on
#      exit.
#   2. Installs `guestfwd` rules that intercept guest connections to
#      $TEST_ECHO_HOST:<port> at the slirp layer and forward them as
#      TCP to 127.0.0.1:<TEST_ECHO_PORT>. Both directions of byte
#      traffic flow transparently.
#
# Usage: test_add_network_with_echo <port1> [port2] [port3] ...
#
# Note: the `guestfwd cmd:` (spawn a process per connection) form
# was removed in QEMU 8.0 for security reasons, so a host-side
# listener is required for the redirect target.
test_add_network_with_echo() {
    local echo_script
    echo_script="$(dirname "${BASH_SOURCE[0]}")/echo-stream.py"

    # Pick a free host port for the echo server.
    if [[ -z "${TEST_ECHO_PORT:-}" ]]; then
        TEST_ECHO_PORT=$(python3 -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()')
        export TEST_ECHO_PORT
    fi

    # Start the TCP echo server in the background and stash its PID
    # for test_cleanup. Wait briefly for the listener to come up.
    python3 "$echo_script" --port "$TEST_ECHO_PORT" \
        > "$TEST_TMPDIR/echo-stream.out" 2>&1 &
    TEST_ECHO_PID=$!

    local i
    for i in $(seq 1 50); do
        if (echo > /dev/tcp/127.0.0.1/"$TEST_ECHO_PORT") 2>/dev/null; then
            break
        fi
        sleep 0.05
    done

    # Start the UDP echo on the pinned port. No readiness probe — UDP
    # is connectionless; if bind failed we'd see test failures, not
    # hangs.
    python3 "$echo_script" --udp --port "$TEST_UDP_ECHO_PORT" \
        > "$TEST_TMPDIR/echo-udp.out" 2>&1 &
    TEST_UDP_ECHO_PID=$!

    # Build the netdev string — one guestfwd rule per requested port.
    local netdev="user,id=net0"
    local port
    for port in "$@"; do
        netdev+=",guestfwd=tcp:${TEST_ECHO_HOST}:${port}-tcp:127.0.0.1:${TEST_ECHO_PORT}"
    done
    TEST_QEMU_CMD+=(
        -device "$(_test_nic_device),netdev=net0"
        -netdev "$netdev"
    )
}

# Add -net none (no networking)
test_add_no_network() {
    TEST_QEMU_CMD+=(-net none)
}

# Attach QEMU's IPMI BMC simulator via the ISA KCS interface at the
# canonical x86 ports 0xCA2 (data) / 0xCA3 (cmd). Only supported on
# x86 (isa-ipmi-kcs is ISA). Lets AxlIpmi's KCS transport exercise
# the real FSM end-to-end against the simulator, which replies with
# CC=0xC7 for malformed wire framing — catches KCS protocol bugs
# that mock-callback unit tests cannot see.
test_add_ipmi_bmc_sim_kcs() {
    if [[ "$TEST_ARCH" != "X64" ]]; then
        log_warning "test_add_ipmi_bmc_sim_kcs: ISA KCS is x86-only; skipping on $TEST_ARCH"
        return
    fi
    TEST_QEMU_CMD+=(
        -device "ipmi-bmc-sim,id=bmc0"
        -device "isa-ipmi-kcs,bmc=bmc0,ioport=0xca2"
    )
}

# Attach QEMU's IPMI BMC simulator via SMBus. Requires a guest-side
# EFI_I2C_MASTER_PROTOCOL provider (sdk/examples/smbus-hc-shim.c)
# because stock OVMF doesn't publish one. x86-only: the ICH9 SMBus
# controller is on q35, and there's no equivalent on the aa64 virt
# machine.
#
# ipmi-bmc-sim defaults slave_addr=0x20 (the IPMI-standard 8-bit BMC
# address), which QEMU writes verbatim into SMBIOS Type 38's
# I2CSlaveAddress byte. AxlSmbus shifts that right by 1 to get the
# 7-bit SMBus address (0x10) and hands it to the shim. For the
# request to land, smbus-ipmi's `address` must equal that 7-bit
# form. QEMU's smbus-ipmi defaults address=0 (SMBus general-call) —
# useless — so we pin it to 0x10 explicitly. A mismatch there would
# manifest as an SSIF session that opens but times out on every
# command; SmbusHcShim's boot-time SMBIOS Type 38 dump surfaces it.
test_add_ipmi_bmc_sim_ssif() {
    if [[ "$TEST_ARCH" != "X64" ]]; then
        log_warning "test_add_ipmi_bmc_sim_ssif: ICH9 SMBus is x86-only; skipping on $TEST_ARCH"
        return
    fi
    TEST_QEMU_CMD+=(
        -device "ipmi-bmc-sim,id=bmc0"
        -device "smbus-ipmi,bmc=bmc0,address=0x10"
    )
}

# Attach an SPD EEPROM image to the platform SMBus at <address> (default
# 0x50). Requires the locally-patched QEMU build (scripts/qemu-patches/
# 0001-smbus-eeprom-add-memdev-link.patch) which adds a memdev= link
# property to smbus-eeprom; stock 10.x rejects the memdev= argument.
# The blob must be at least SMBUS_EEPROM_SIZE bytes (256) — the device
# copies the first 256 bytes into init_data at realize.
#
# Usage: test_add_smbus_eeprom <blob-path> [address]
test_add_smbus_eeprom() {
    local blob="$1"
    local addr="${2:-0x50}"
    if [[ "$TEST_ARCH" != "X64" ]]; then
        log_warning "test_add_smbus_eeprom: SMBus EEPROM injection is x86-only; skipping on $TEST_ARCH"
        return
    fi
    if [[ ! -f "$blob" ]]; then
        log_error "test_add_smbus_eeprom: blob '$blob' not found"
        return 1
    fi
    local id="spd_${addr//0x/}"
    # memory-backend-file rounds `size=` up to the host page; the blob
    # must be at least one page or QEMU rejects "backing store too
    # small". gen-spd.py pads to 4096 bytes for this reason. The
    # smbus-eeprom device only consumes the first 256 bytes; padding
    # is harmless.
    TEST_QEMU_CMD+=(
        -object "memory-backend-file,id=$id,mem-path=$blob,size=4096,share=off,readonly=on"
        -device "smbus-eeprom,address=$addr,memdev=$id"
    )
}

# ---------------------------------------------------------------------------
# QEMU execution
# ---------------------------------------------------------------------------

# Run QEMU in foreground with timeout. Serial output goes to TEST_LOG.
#
# If `ts` (moreutils) is available every output line is prefixed with
# the elapsed seconds since QEMU start, which lets test_count_results
# compute per-binary wall-clock times and pinpoint the binary that
# owned any stall. Falls back to untimestamped output if `ts` isn't
# installed.
test_run_foreground() {
    local timeout_sec="${1:-20}"
    #
    # AArch64 QEMU under TCG boots noticeably slower than x86_64 and
    # the per-suite QEMU startup cost dominates. Every extra test
    # binary added to the aa64 sweep widens the gap. +60 over the
    # x86_64 budget covers the current 10-binary matrix with
    # headroom; bump if we add more.
    #
    [[ "$TEST_ARCH" == "AARCH64" ]] && timeout_sec=$((timeout_sec + 60))

    if command -v ts &>/dev/null; then
        timeout "$timeout_sec" "${TEST_QEMU_CMD[@]}" 2>&1 \
            | ts -s '[%.s]' > "$TEST_LOG" || true
    else
        timeout "$timeout_sec" "${TEST_QEMU_CMD[@]}" > "$TEST_LOG" 2>&1 || true
    fi
}

# Run QEMU in background. Sets TEST_QEMU_PID.
test_run_background() {
    "${TEST_QEMU_CMD[@]}" > "$TEST_LOG" 2>&1 &
    TEST_QEMU_PID=$!
}

# Wait for a string to appear in the serial log (for background QEMU).
# Returns 0 on success, 1 on timeout.
test_wait_for() {
    local pattern="$1"
    local timeout_sec="${2:-60}"

    for i in $(seq 1 "$timeout_sec"); do
        if grep -q "$pattern" "$TEST_LOG" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

# ---------------------------------------------------------------------------
# Serial log processing
# ---------------------------------------------------------------------------

# Strip ANSI escape codes AND the `ts` per-line timestamp prefix
# ("[SSS.sss] ") from the serial log so downstream PASS/FAIL grep
# anchored with ^ still matches. The raw TEST_LOG keeps the
# timestamps for per-binary timing parsing.
test_clean_log() {
    sed -E 's/\x1b\[[0-9;]*[a-zA-Z]//g; s/^\[[0-9]+\.[0-9]+\] //' "$TEST_LOG" \
        | tr -d '\r' > "$TEST_CLEAN_LOG"
}

# Count PASS/FAIL from cleaned serial log, print results, exit with status
test_count_results() {
    test_clean_log

    local pass fail
    pass=$(grep -c '^PASS:' "$TEST_CLEAN_LOG" || true)
    fail=$(grep -c '^FAIL:' "$TEST_CLEAN_LOG" || true)

    grep -E '^(PASS|FAIL):' "$TEST_CLEAN_LOG" | while IFS= read -r line; do
        echo "  $line"
    done

    grep -E '^=== Results:' "$TEST_CLEAN_LOG" || true

    #
    # Per-binary wall-clock timing (requires ts-prefixed log; see
    # test_run_foreground). Emits one line per binary with BEGIN→END
    # elapsed time, and flags any binary whose ____END marker never
    # printed — that's the one that stalled when a timeout bites.
    #
    if grep -q '=== .* Tests ===' "$TEST_LOG"; then
        echo ""
        echo "Per-binary timing:"
        awk '
            {
                # Strip ANSI escapes so the === Tests === regex matches.
                gsub(/\x1b\[[0-9;]*[a-zA-Z]/, "")
            }
            /^\[[0-9.]+\].*=== .+ Tests ===/ {
                t = $1; gsub(/[\[\]]/, "", t)
                match($0, /=== .+ Tests ===/)
                name = substr($0, RSTART + 4, RLENGTH - 13)
                begin_t[name] = t + 0.0
                order[++n] = name
            }
            /^\[[0-9.]+\].*=== Results:/ {
                t = $1; gsub(/[\[\]]/, "", t)
                # Pair with most recent suite lacking an end timestamp.
                for (i = n; i >= 1; i--) {
                    name = order[i]
                    if (!(name in end_t)) {
                        end_t[name] = t + 0.0
                        break
                    }
                }
            }
            END {
                for (i = 1; i <= n; i++) {
                    name = order[i]
                    if (name in end_t) {
                        printf "  %-16s %7.2fs\n", name, end_t[name] - begin_t[name]
                    } else {
                        printf "  %-16s   STALLED (no Results line seen)\n", name
                    }
                }
            }
        ' "$TEST_LOG"
    fi

    # Calculate elapsed time
    local end_time elapsed_ms elapsed_s elapsed_frac
    end_time=$(date +%s%N)
    elapsed_ms=$(( (end_time - TEST_START_TIME) / 1000000 ))
    elapsed_s=$((elapsed_ms / 1000))
    elapsed_frac=$((elapsed_ms % 1000))

    echo ""
    printf "Results: %d passed, %d failed (%s) in %d.%03ds\n" \
        "$pass" "$fail" "$TEST_ARCH" "$elapsed_s" "$elapsed_frac"

    #
    # Ratchet: fail if test count dropped from last known good run.
    # Auxiliary scripts (e.g. test-ipmi-qemu.sh which runs a single
    # test binary) opt out via TEST_SKIP_RATCHET=1 so they don't
    # clobber the canonical test-axl.sh baseline with their smaller
    # pass counts.
    #
    if [[ "${TEST_SKIP_RATCHET:-0}" != "1" ]]; then
        local baseline_file="$TESTS_DIR/.last-pass-count"
        if [[ -f "$baseline_file" ]]; then
            local expected
            expected=$(cat "$baseline_file")
            if [[ $pass -lt $expected ]]; then
                echo ""
                echo "FAIL: expected at least $expected tests but only $pass ran"
                echo "      (a test binary may have crashed partway through)"
                exit 1
            fi
        fi
        if [[ $fail -eq 0 && $pass -gt 0 ]]; then
            echo "$pass" > "$baseline_file"
        fi
    fi

    if [[ $fail -eq 0 && $pass -gt 0 ]]; then
        exit 0
    else
        exit 1
    fi
}
