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

# The build tree for this run's arch — ASKED OF MAKE, never composed here.
#
# 71 scripts used to spell out "$PROJECT_DIR/out/native-$arch" themselves,
# which is correct only while PREFIX depends on nothing but ARCH. It already
# depends on BUILD, and folding AXL_TLS into it — so a TLS run stops wiping the
# non-TLS objects and vice versa — would have silently pointed every one of
# those scripts at a tree nothing had built. The Makefile owns the answer;
# this asks it. `make -s print-prefix` honours ARCH, BUILD and AXL_TLS
# together, so this stays right through any future split.
#
# Memoized per arch: 71 scripts times several calls times a make fork is
# measurable, and the answer cannot change inside one run.
# $1 (optional) names the arch when it is NOT this run's TEST_ARCH — a script
# that deliberately builds the other one, or that has already mapped TEST_ARCH
# to a Makefile arch of its own. Accepts either spelling (X64/x64,
# AARCH64/aa64) because the scripts use both.
test_build_prefix() {
    local arch="${1:-${TEST_ARCH:-X64}}"
    case "$arch" in
        AARCH64|aarch64|aa64) arch=aa64 ;;
        *)                    arch=x64  ;;
    esac
    # Key the memo on the TLS state too, not just the arch: a caller may ask
    # for the TLS prefix (AXL_TLS=1 test_build_prefix x64) in a run whose
    # default is non-TLS, and an arch-only key would hand back the cached
    # non-TLS answer -- the exact class of bug this helper exists to prevent.
    local var="_TEST_BUILD_PREFIX_${arch}_${AXL_TLS:+tls}"
    if [[ -z "${!var:-}" ]]; then
        # Delegates rather than re-asking make itself: scripts/build-prefix.sh
        # is the one definition, since 51 integration scripts source nothing
        # and can only call it as a command. This adds the memo on top.
        printf -v "$var" '%s' "$("$PROJECT_DIR/scripts/build-prefix.sh" "$arch")"
    fi
    printf '%s\n' "${!var}"
}

# Absolute form. The relative one above is what a make TARGET wants (targets
# are relative to PROJECT_DIR); this one is what a file test or a QEMU
# staging path wants. Both come from the same answer.
test_build_dir() {
    printf '%s/%s\n' "$PROJECT_DIR" "$(test_build_prefix "$@")"
}

# The STAGED SDK -- a different question from test_build_dir, with different
# inputs, which is why it is a separate helper rather than a mode of that one.
#
#   test_build_dir   out/native-<arch>[-release][-tls]   objects and images,
#                    varies with ARCH x BUILD x AXL_TLS
#   test_sdk_dir     out/{bin,lib,include,share}         what install.sh
#                    produces and a consumer consumes; varies with nothing
#
# Both live under out/ and both were called "the prefix", which is how
# AXL-Distribution-Design.md's P2 (separate the two) came to look like the same
# 149-file sweep as the CMake port's slice 3. Measured, the real overlap is
# seven files; P2's own surface is the handful of callers that ask this.
#
# No memo: scripts/sdk-prefix.sh forks no `make` -- it reads one environment
# variable -- so the cost test_build_prefix memoizes away does not exist here.
test_sdk_dir() {
    "$PROJECT_DIR/scripts/sdk-prefix.sh" --abs
}

# AXL_TLS: warn only when a toggle is actually imminent.
#
# run-integration.sh exports AXL_TLS=1 for the whole suite. A test run BY HAND
# with it unset toggles the flag, and the Makefile's state-change rule wipes
# every .o, libaxl.a and .efi under the prefix. The failure that produces is
# badly misleading: the link goes looking for crt0/reloc objects the wipe just
# removed and the test reports "driver build failed", which reads like a code
# error rather than a flag toggle. (Cost an hour to diagnose once.)
#
# Two things this deliberately is NOT:
#
#   - not `export AXL_TLS="${AXL_TLS:-1}"`. test-axl.sh sources this file too,
#     and defaulting the flag changes which sources the unit suite builds --
#     measured, 8820 -> 8964 tests. Silently moving the number the whole
#     project ratchets on is not a convenience default's job. (test-axl.sh:28
#     runs its own make with no AXL_TLS, which is why CI -- despite building
#     AXL_TLS=1 -- also lands on 8820.)
#
#   - not an unconditional warning. test-axl.sh runs constantly with the flag
#     unset and that is CORRECT there, so warning every time would be noise
#     advising a change that moves the count.
#
# The precise condition is "the recorded state says on and we are about to
# make it off", which the Makefile already tracks in .axl-tls-state.
if [[ -z "${AXL_TLS:-}" ]]; then
    _axl_tls_arch=x64
    [[ "${1:-}" == *AARCH64* || "${*:-}" == *AARCH64* ]] && _axl_tls_arch=aa64
    _axl_tls_state="$(test_build_dir "$_axl_tls_arch")/build/.axl-tls-state"
    if [[ -r "$_axl_tls_state" && "$(cat "$_axl_tls_state" 2>/dev/null)" == "on" ]]; then
        echo "note: the build tree was last built with AXL_TLS=1 and this run has" >&2
        echo "      it unset, so make will WIPE and rebuild it (watch for 'cannot" >&2
        echo "      find axl-crt0-*.o' if that races). Prefer AXL_TLS=1 when running" >&2
        echo "      a single integration test by hand -- that is what the suite uses." >&2
    fi
    unset _axl_tls_arch _axl_tls_state
fi

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
TEST_CPU_SPIKE=0

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
    # Build the per-test scratch (a ~40 MB QEMU image + pflash vars) on a tmpfs
    # when available — under the parallel pool, concurrent root-fs image builds
    # are an I/O bottleneck that starves booting guests. When run-integration.sh
    # set AXL_QEMU_TMPDIR, nest under it so the whole run's scratch is cleaned
    # wholesale even if a SIGTERM skips our own trap below; else prefer /dev/shm.
    _tt_base="${AXL_QEMU_TMPDIR:-}"
    if [[ -z "$_tt_base" && -d /dev/shm && -w /dev/shm ]]; then _tt_base=/dev/shm; fi
    if [[ -n "$_tt_base" ]]; then
        TEST_TMPDIR=$(mktemp -d -p "$_tt_base" axl-ctest.XXXXXXXX)
    else
        TEST_TMPDIR=$(mktemp -d)
    fi
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

    # CPU-spike policy — shared with run-qemu.sh via cpu_policy_init (axl-common.sh),
    # which picks the KVM/TCG-aware threshold (0.5), warm-up and warn carve-out (see
    # its comment for the measured rationale). TEST_CPU_* env vars override per run.
    CPU_WARN="${TEST_CPU_WARN:-}"
    CPU_THRESHOLD="${TEST_CPU_THRESHOLD:-}"
    cpu_policy_init "$TEST_ARCH"
    CPU_WARMUP="${TEST_CPU_WARMUP:-$CPU_WARMUP}"
    CPU_SUSTAIN="${TEST_CPU_SUSTAIN:-$CPU_SUSTAIN}"

    # Cleanup trap. Include INT/TERM, not just EXIT: a `timeout` wrapper's
    # SIGTERM would otherwise bypass EXIT and leak the (40 MB) scratch dir.
    trap 'test_cleanup' EXIT INT TERM
}

test_cleanup() {
    if [[ $TEST_QEMU_PID -gt 0 ]]; then
        kill "$TEST_QEMU_PID" 2>/dev/null || true
        wait "$TEST_QEMU_PID" 2>/dev/null || true
    fi
    # Summarize the CPU sampler for background runs, where QEMU only dies
    # here. Foreground runs already summarized in test_run_foreground; the
    # call is a no-op once the monitor globals are cleared.
    test_cpu_check || true
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
        # Boot the Shell via the AXL launcher ("-delay 0", skips the 5 s startup
        # countdown); see stage_boot_shell in axl-common.sh.
        stage_boot_shell "$TEST_STAGING" "$TEST_ARCH" "$TEST_BOOT_NAME" "$shell_efi"
    fi

    # Shared with run-qemu.sh — see qemu_stage_disk in axl-common.sh.
    qemu_stage_disk "$TEST_STAGING" "$TEST_DISK" TEST || exit 1
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
        # TCG fallback for single-stepping — shared with run-qemu.sh.
        qemu_strip_kvm TEST_QEMU_CMD
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

# Host-port allocation. A test derives each host port it needs as
# `test_port <slot>` (slot 0, 1, 2, ...). The same value drives both the
# host-side server and the number baked into the guest's startup.nsh, so the
# two always agree within a single test run — which is why each slot is
# resolved ONCE and cached for the life of the test.
#
# Two modes:
#
#   - TEST_PORT_BASE set explicitly  -> `TEST_PORT_BASE + slot`, verbatim.
#     A caller that pins the base has a reason (reproducing a capture,
#     matching a firewall rule) and owns the consequences.
#
#   - TEST_PORT_BASE unset (the norm) -> the base is claimed HERE, at source
#     time, as a contiguous run of TEST_PORT_SLOTS ports that are verified
#     free at that moment and held for the test's lifetime. This is what
#     makes two INDEPENDENT suite runs safe: an arithmetic base only keeps
#     one invocation self-consistent, and every invocation starts from the
#     same constants.
#
# The claim is taken here, in the test's own shell, and NOT inside
# test_port. Call sites are all `HOST_PORT=$(test_port 0)`, and a command
# substitution runs in a subshell that exits immediately — a claim made
# there would be released the instant it was granted. Claiming at source
# time keeps every port held for as long as the test runs, and test_port
# stays the pure arithmetic every call site already expects.
TEST_PORT_SLOTS="${TEST_PORT_SLOTS:-4}"
if [[ -z "${TEST_PORT_BASE:-}" ]]; then
    axl_alloc_host_port TEST_PORT_BASE "$TEST_PORT_SLOTS" || exit 1
fi
test_port() {
    echo $(( TEST_PORT_BASE + ${1:-0} ))
}

# Add port forwarding: test_add_port_forward HOST GUEST [HOST GUEST ...]
#
# Every pair lands on the SAME netdev — call this once with all of them. A
# second call would emit a second `-netdev id=net0` and QEMU would reject the
# duplicate id, so a test needing two forwards passes four arguments rather
# than calling twice.
test_add_port_forward() {
    local fwd=""
    while [[ $# -ge 2 ]]; do
        fwd+=",hostfwd=tcp::${1}-:${2}"
        shift 2
    done
    TEST_QEMU_CMD+=(
        -device "$(_test_nic_device),netdev=net0"
        -netdev "user,id=net0${fwd}"
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

    # Pick a free host port for the echo server. This used to bind port 0,
    # read the kernel's choice and close again — which leaves the port
    # unclaimed for the whole gap before echo-stream.py binds it, and draws
    # from the ephemeral range where an outbound connection can take it.
    # The allocator claims it and holds the claim until this shell exits.
    if [[ -z "${TEST_ECHO_PORT:-}" ]]; then
        axl_alloc_host_port TEST_ECHO_PORT || return 1
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

    # Backgrounded so the CPU sampler can attach to QEMU while it runs. The
    # pipeline and the `|| true` (a non-zero guest exit is not a harness
    # failure) are preserved exactly.
    if command -v ts &>/dev/null; then
        ( timeout "$timeout_sec" "${TEST_QEMU_CMD[@]}" 2>&1 \
            | ts -s '[%.s]' > "$TEST_LOG" ) &
    else
        ( timeout "$timeout_sec" "${TEST_QEMU_CMD[@]}" > "$TEST_LOG" 2>&1 ) &
    fi
    local _wrapper=$!
    cpu_monitor_start "$(_test_qemu_pid)" "$TEST_TMPDIR"
    wait "$_wrapper" 2>/dev/null || true
    test_cpu_check || true
    axl_report_hostfwd_failure "$TEST_LOG" "$(basename "$0")" || true

    # A firmware DXE-core pool/heap ASSERT (e.g. a double-FreePool) is NEVER
    # acceptable — the guest's allocator state is corrupt. DEBUG firmware prints
    # "ASSERT [DxeCore] ... Pool.c(...)"; RELEASE just spins at 100% CPU. Fail
    # the whole test here rather than let it be scored on other criteria: the
    # driver start-failure double-free shipped precisely because a run that
    # printed this line was still judged on later greps. Global, so every
    # foreground QEMU test is covered. The EXIT trap still runs cleanup.
    if grep -qaE 'ASSERT \[DxeCore\]' "$TEST_LOG" 2>/dev/null; then
        echo "*** FAIL: firmware DXE-core ASSERT (pool/heap corruption) in" \
             "$(basename "$0"):" >&2
        grep -aE 'ASSERT \[DxeCore\]' "$TEST_LOG" | head -3 | sed 's/^/    /' >&2
        exit 1
    fi
}

# Resolve THIS test's QEMU pid. Matched by the test's own disk image path,
# which is unique per TEST_TMPDIR — `pgrep -P` cannot be used through the
# `timeout`+`ts` pipeline, and the comm check keeps the `timeout` wrapper
# (whose argv also contains the path) from being mistaken for QEMU.
_test_qemu_pid() {
    local i pid comm
    for i in 1 2 3 4 5; do
        for pid in $(pgrep -f "$TEST_DISK" 2>/dev/null); do
            comm=$(cat "/proc/$pid/comm" 2>/dev/null || true)
            if [[ "$comm" == qemu-system* ]]; then
                printf '%s' "$pid"
                return 0
            fi
        done
        sleep 0.2
    done
    printf ''
}

# Summarize the CPU sampler. WARNS but does NOT fail the test by default.
#
# The integration pool runs ~25 VMs concurrently and the 1.5-core threshold is
# one that firmware boot legitimately brushes (measured: a large cluster of
# short-lived guests peaks 1.4-1.65 cores, all of it inside the warm-up
# window). Failing on that would make the suite flaky for a signal that is
# advisory, so the default here is a WARN line plus TEST_CPU_SPIKE=1 — unlike
# run-qemu.sh, which keeps failing with CPU_SPIKE_EXIT. Set
# TEST_CPU_SPIKE_FAIL=1 to opt a specific test into failing.
test_cpu_check() {
    local rc=0
    cpu_monitor_finish || rc=$?
    [[ $rc -eq 0 ]] && return 0
    TEST_CPU_SPIKE=1
    if [[ "${TEST_CPU_SPIKE_FAIL:-0}" == "1" ]]; then
        return "$rc"
    fi
    return 0
}

# Suppress the CPU advisory for a suite where the spike is STRUCTURAL.
#
# The sampler cannot tell AXL's CPU use from the firmware's, and for a suite
# whose cost is dominated by firmware boot the warning fires on every single
# run (observed 6/6 on x64: peak ~1.05-1.10 cores, sustained 4.6-5.8s against
# a 2s threshold). A warning that always fires is not a signal, it is noise
# that teaches people to skim past the summary -- so such a suite opts out and
# uses TEST_MAX_DURATION instead, which measures something actionable.
#
# This does NOT weaken the sampler elsewhere: run-qemu.sh still fails on a
# spike (CPU_SPIKE_EXIT), test-cpu-spike-qemu.sh still covers the machinery,
# and any test can still opt into failing with TEST_CPU_SPIKE_FAIL=1.
test_cpu_advisory_off() {
    CPU_WARN=false
}

# Run QEMU in background. Sets TEST_QEMU_PID.
#
# QEMU's own stderr lands in TEST_LOG alongside the guest serial stream, so a
# refused -netdev is visible here — but only if someone looks. Nothing did:
# the test went straight to test_wait_for and reported "server did not start
# within 60s" a minute later. Check the moment QEMU has had a chance to fail.
test_run_background() {
    if [[ -n "${TEST_STDIN_FD:-}" ]]; then
        "${TEST_QEMU_CMD[@]}" > "$TEST_LOG" 2>&1 <&"$TEST_STDIN_FD" &
    else
        "${TEST_QEMU_CMD[@]}" > "$TEST_LOG" 2>&1 &
    fi
    TEST_QEMU_PID=$!
    local i
    for i in $(seq 1 20); do
        kill -0 "$TEST_QEMU_PID" 2>/dev/null || break   # died: log is final
        grep -qa "Could not set up host forwarding rule" "$TEST_LOG" 2>/dev/null && break
        sleep 0.1
    done
    if ! axl_report_hostfwd_failure "$TEST_LOG" "$(basename "$0")"; then
        return 1
    fi
    # TEST_QEMU_PID is QEMU itself here (no timeout wrapper), so the sampler
    # can attach directly. test_cleanup summarizes once QEMU is killed.
    cpu_monitor_start "$TEST_QEMU_PID" "$TEST_TMPDIR"
}

# ---------------------------------------------------------------------------
# Guest console INPUT (background runs)
# ---------------------------------------------------------------------------
#
# The harness is otherwise write-once: startup.nsh drives the guest and the
# host only reads serial. That is enough for anything scriptable, but not for
# the class of behavior a key press IS — a foreground server ended by Ctrl-C,
# say, whose whole contract is what happens when a byte arrives on ConIn.
#
# test_enable_stdin routes QEMU's stdin from a FIFO this script holds open;
# `-nographic` multiplexes the guest serial on stdio, so a byte written here
# is delivered to the guest as a keystroke. QEMU's stdio chardev reserves
# Ctrl-A (0x01) as its own escape prefix and passes everything else straight
# through, so 0x03 arrives at ConIn as a real serial Ctrl-C.
#
# Call BEFORE test_run_background; the FIFO is opened read-WRITE on this end
# so QEMU never sees EOF between writes (a write-only open would close after
# each printf and QEMU would stop watching stdin).
test_enable_stdin() {
    local fifo="$TEST_TMPDIR/qemu-stdin.fifo"
    rm -f "$fifo"
    mkfifo "$fifo"
    exec {TEST_STDIN_FD}<>"$fifo"
}

# test_send_stdin <printf-format>
#
# Write raw bytes to the guest console; the argument goes through
# `printf '%b'`, so escapes like '\003' work. No-op-safe only after
# test_enable_stdin — calling it without one is a harness bug, and the
# `set -u` bare expansion below is what makes that loud instead of silent.
test_send_stdin() {
    printf '%b' "$1" >&"$TEST_STDIN_FD"
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
    # Timed out. Before the caller reports "the server never came up", say so
    # if the guest never had a network to come up on.
    axl_report_hostfwd_failure "$TEST_LOG" "$(basename "$0")" || true
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

# test_slice_log <start-marker> <end-marker> <outfile>
#
# Extract the lines strictly between two exact-match `echo` markers in
# TEST_CLEAN_LOG into <outfile>. Pass "" as <end-marker> to slice from
# <start-marker> to the end of the log instead of to a second marker.
#
# For binding an assertion to the ONE command that must have produced a
# line, when the same line (a repeated `status` label, say) can legally
# appear more than once across a run — a whole-log grep would also be
# satisfied by an earlier/later occurrence that proves nothing. Neither
# marker line itself is included in the slice.
#
# A marker that is not in the log is a HARD error, not an empty slice:
# renaming an `echo` marker in a startup.nsh without renaming it here would
# otherwise silently turn every assertion bound to that slice into a no-op —
# an absence check over zero lines passes, and a presence check fails with a
# message that blames the tool rather than the harness. Returns 1 (fatal
# under the `set -e` this file installs) after naming the missing marker.
test_slice_log() {
    local start="$1" end="$2" outfile="$3"
    local m
    for m in "$start" "$end"; do
        [[ -z "$m" ]] && continue
        if ! grep -Fxq "$m" "$TEST_CLEAN_LOG"; then
            echo "  FAIL: test_slice_log: marker '$m' is not in the serial log" >&2
            echo "        (the slice would be empty and every assertion over it vacuous)" >&2
            return 1
        fi
    done
    if [[ -z "$end" ]]; then
        awk -v s="$start" '$0 == s { on = 1; next } on' \
            "$TEST_CLEAN_LOG" > "$outfile"
    else
        awk -v s="$start" -v e="$end" \
            '$0 == e { on = 0 } on; $0 == s { on = 1 }' \
            "$TEST_CLEAN_LOG" > "$outfile"
    fi
}

# ---------------------------------------------------------------------------
# Memory-leak gate
# ---------------------------------------------------------------------------
#
# Every AXL image prints a leak verdict from its teardown path (_axl_cleanup,
# or the minimal CRT0) under AXL_MEM_DEBUG: either "mem: no leaks detected" or
# a report listing every block still live AFTER atexit callbacks and the tier-1
# registry sweep have run. Nothing frees anything after that point, so a report
# there is a leak, full stop.
#
# Until this gate existed nothing read those lines. Two leaked streams sat in
# test/unit/axl-test-io.c until a human happened to read the code, and the
# measurement that produced this function found five more the same way.
#
# The two anchors below are load-bearing and are pinned on the guest side:
#
#   TEST_LEAK_MARKER      The teardown report. axl_mem_dump_leaks() called by a
#                         RUNNING program prints "=== AxlMem leak report (live
#                         allocations): ..." instead — live blocks are not
#                         leaked blocks, and a diagnostic dump must not fail a
#                         run. That infix is asserted exactly by test_leak_dump
#                         (test/unit/axl-test-mem.c), so collapsing the two
#                         spellings back together breaks a unit test rather
#                         than silently unarming this grep.
#
#   TEST_LEAK_OK_MARKER   The clean verdict. Used as a POSITIVE CONTROL: a
#                         guest that reached teardown printed one marker or the
#                         other, so an ABSENT verdict means this gate saw
#                         nothing rather than nothing being wrong. Every cause
#                         is reported as a failure (the message enumerates
#                         them). A gate that silently no-ops is worse than no
#                         gate; that is the whole point of the exercise.
#
# The control is applied PER BINARY when the log carries "=== Results:"
# footers, and whole-log otherwise. Per-binary is what catches the interesting
# case: AxlTestLog used to silence its console mid-run and never restore it, so
# that one binary's verdict never reached the serial log and a leak in it would
# have been invisible while 38 siblings kept a whole-log control green.
#
# TEST_SKIP_LEAK_GATE=1 opts a suite out (RELEASE guests, or a scenario whose
# whole subject is a crash before teardown). Say why at the call site.
TEST_LEAK_MARKER='=== AxlMem leak report:'
TEST_LEAK_OK_MARKER='mem: no leaks detected'

# Verdict on TEST_CLEAN_LOG. Returns 0 clean, 1 on leaks or a blind gate.
# Caller must have run test_clean_log.
test_check_leaks() {
    if [[ "${TEST_SKIP_LEAK_GATE:-0}" == "1" ]]; then
        return 0
    fi

    local n_leak n_ok rc=0
    n_leak=$(grep -acF "$TEST_LEAK_MARKER" "$TEST_CLEAN_LOG" || true)
    n_ok=$(grep -acF "$TEST_LEAK_OK_MARKER" "$TEST_CLEAN_LOG" || true)

    # --- positive control -------------------------------------------------
    #
    # Keyed on the "=== Results:" FOOTER, not the "=== NAME Tests ===" header:
    # a footer is what says a binary ran to completion, and the verdict is the
    # next thing it prints. Keying on the header instead would let a binary
    # that forgot to print one merge into its predecessor's window, so the
    # predecessor's verdict would vouch for both -- precisely the silent blind
    # spot this control exists to remove. (Two binaries were in that state
    # until the headers went into axl-test-vterm.c / axl-test-9p.c.) The
    # header is used only to NAME the offender; a nameless one still counts.
    local silent
    if grep -qa '=== Results:' "$TEST_CLEAN_LOG"; then
        silent=$(awk -v leak="$TEST_LEAK_MARKER" -v ok="$TEST_LEAK_OK_MARKER" '
            match($0, /=== .+ Tests ===/) {
                name = $0
                sub(/.*=== /, "", name); sub(/ Tests ===.*/, "", name)
                cur = name
            }
            /=== Results:/ {
                if (pending) { print label }
                n++
                label = (cur == "" ? sprintf("(binary #%d, no \"=== NAME Tests ===\" header)", n) : cur)
                cur = ""; pending = 1
                next
            }
            # Only AFTER a footer: the teardown verdict is printed once main
            # has returned, so a mid-run axl_mem_dump_leaks() is not one.
            pending && (index($0, leak) || index($0, ok)) { pending = 0 }
            END { if (pending) print label }
        ' "$TEST_CLEAN_LOG")
    elif (( n_leak == 0 && n_ok == 0 )); then
        silent="(the whole run)"
    fi

    if [[ -n "${silent:-}" ]]; then
        echo ""
        echo "FAIL: the leak gate saw NO memory verdict from:"
        local _b
        while IFS= read -r _b; do echo "      - $_b"; done <<< "$silent"
        echo "  An AXL_APP image prints one at teardown under AXL_MEM_DEBUG."
        echo "  Absent, the guest is one of:"
        echo "    - a RELEASE build (the Makefile defines AXL_MEM_DEBUG only"
        echo "      for BUILD=DEBUG);"
        echo "    - an image that silenced the console and never restored it"
        echo "      (axl_log_set_console_enabled), or raised the log level"
        echo "      above INFO so the clean verdict is filtered;"
        echo "    - an AXL_DRIVER, which has no _axl_cleanup and so never"
        echo "      emits a teardown verdict at all;"
        echo "    - a fault before teardown;"
        echo "    - a wording change in src/mem/axl-mem.c that this grep"
        echo "      anchors on."
        echo "  Fix it or set TEST_SKIP_LEAK_GATE=1 with a reason — do NOT"
        echo "  leave the gate blind."
        rc=1
    fi

    # --- the verdict itself ----------------------------------------------
    if (( n_leak == 0 )); then
        return "$rc"
    fi

    #
    # Attribute each report to the test binary that was running, the same way
    # the stalled-binary detector does: the most recent "=== NAME Tests ==="
    # header. Guests that print no header (a tool, a demo) report the binary
    # as "?" — the file:line in the block is the actionable part regardless.
    #
    echo ""
    echo "*** FAIL: memory leaked at teardown. These blocks were still live"
    echo "    after atexit callbacks and the tier-1 registry sweep, so nothing"
    echo "    was ever going to free them:"
    awk -v marker="$TEST_LEAK_MARKER" '
        index($0, "=== ") && index($0, " Tests ===") {
            name = $0
            sub(/.*=== /, "", name); sub(/ Tests ===.*/, "", name)
            cur = name
        }
        index($0, marker) {
            on = 1; shown = 0; hidden = 0
            printf "      binary: %s\n", (cur == "" ? "?" : cur)
        }
        on {
            line = $0
            sub(/^.*\[WARN\][ \t]*mem: /, "", line)
            # Cap the per-block listing: one call site repeated 64 times
            # (a loop that leaks) buries every OTHER report under it.
            if (line ~ /^  \[/ && ++shown > MAXROWS) { hidden++; next }
            if (index(line, "=== end leak report ===") && hidden > 0) {
                printf "        (... %d more allocations)\n", hidden
            }
            print "        " line
        }
        /=== end leak report ===/ { on = 0 }
    ' MAXROWS=8 "$TEST_CLEAN_LOG"
    echo "    A leak in a test is a test bug; a leak in src/ is a library"
    echo "    defect. Do not widen this gate to make it green."
    return 1
}

# Count PASS/FAIL from cleaned serial log, print results, exit with status
test_count_results() {
    test_clean_log

    #
    # EVERY grep over TEST_CLEAN_LOG passes -a. The serial capture can carry a
    # stray NUL (firmware noise before our first output -- AARCH64 does it
    # reliably), and one NUL anywhere makes GNU grep call the whole file binary.
    # The consequence is split, which is what makes it nasty:
    #
    #   -c   still counts every match, so the ratchet numbers stay CORRECT
    #   -q   still exits 0 on a post-NUL match, so the assert gate still fires
    #   line output prints matches until the NUL, then "binary file matches"
    #
    # So the counts are right while the listings are silently truncated -- a
    # report that shows "19 group(s) SKIPPED" above a list of 2. That is how
    # this was found: the SKIP list is the only view of which groups are
    # arch-gated, and a truncated one hides balancer drift behind a number that
    # still looks healthy. test_check_leaks (grep -acF / grep -qa) already got
    # this right; the rest did not. Uniform -a, so no reader has to know which
    # greps happen to survive binary detection and which quietly do not.
    #
    local pass fail skip tls_skips skipped_asserts ratchet_total
    pass=$(grep -ac '^PASS:' "$TEST_CLEAN_LOG" || true)
    fail=$(grep -ac '^FAIL:' "$TEST_CLEAN_LOG" || true)
    skip=$(grep -acE '^SKIP(\[[0-9]+\])?:' "$TEST_CLEAN_LOG" || true)
    tls_skips=$(grep -ac '^SKIP:.*AXL_TLS' "$TEST_CLEAN_LOG" || true)

    #
    # Assertions DECLARED skipped by test_skip_n, i.e. "SKIP[n]:". These are
    # topology gates -- a device this QEMU image or this machine does not have
    # -- where both outcomes share one baseline, so the count has to be made up
    # or the ratchet drifts between images.
    #
    # It used to be made up with padding: ~170 test_check(true, "... SKIP
    # balance") calls, one per assertion the populated path would have run.
    # Those are indistinguishable from the assert-nothing anti-pattern the
    # project bans and they inflated the pass count with results nobody
    # produced. Now the count is declared and added here instead.
    #
    # Bare "SKIP:" (no [n]) is the build-configuration form, which gets its own
    # baseline file and so must NOT be added in.
    #
    skipped_asserts=$(sed -n 's/^SKIP\[\([0-9]\+\)\]:.*/\1/p' "$TEST_CLEAN_LOG" \
        | awk '{ t += $1 } END { print t + 0 }')
    ratchet_total=$((pass + skipped_asserts))

    grep -aE '^(PASS|FAIL|SKIP(\[[0-9]+\])?):' "$TEST_CLEAN_LOG" | while IFS= read -r line; do
        echo "  $line"
    done

    grep -aE '^=== Results:' "$TEST_CLEAN_LOG" || true

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

    #
    # Stalled-binary detection. Each unit binary prints a
    # "=== <Suite> Tests ===" header and a "=== Results: ... ===" footer.
    # A header with no footer before the next header (or end of log) means
    # that binary hung or faulted mid-run — and in the combined suite a
    # single hang starves every binary after it (one shared QEMU boot, one
    # timeout). Surface the culprit loudly: a hung firmware/protocol test
    # otherwise shows up only as an opaque "fewer tests ran" ratchet drop.
    # (The outer "=== AXL ... Integration Tests (ARCH) ===" wrapper does not
    # match the per-binary "... Tests ===" anchor, so it is not flagged.)
    #
    local stalled
    stalled=$(awk '
        match($0, /=== .+ Tests ===/) {
            if (open != "") { print open }
            name = $0
            sub(/.*=== /, "", name)
            sub(/ Tests ===.*/, "", name)
            open = name
        }
        /=== Results:/ { open = "" }
        END { if (open != "") print open }
    ' "$TEST_CLEAN_LOG")

    if [[ -n "$stalled" ]]; then
        echo ""
        echo "*** STALLED: the following test binary started but never"
        echo "    produced a Results footer — it hung or faulted (likely an"
        echo "    infinite loop or a firmware call that wedged); any binaries"
        echo "    after it in the run were starved:"
        local _s
        while IFS= read -r _s; do echo "      - $_s"; done <<< "$stalled"
    fi

    # Calculate elapsed time
    local end_time elapsed_ms elapsed_s elapsed_frac
    end_time=$(date +%s%N)
    elapsed_ms=$(( (end_time - TEST_START_TIME) / 1000000 ))
    elapsed_s=$((elapsed_ms / 1000))
    elapsed_frac=$((elapsed_ms % 1000))

    echo ""
    if (( skip > 0 )); then
        printf "Results: %d passed, %d failed, %d group(s) SKIPPED (%d assertions) (%s) in %d.%03ds\n" \
            "$pass" "$fail" "$skip" "$skipped_asserts" "$TEST_ARCH" \
            "$elapsed_s" "$elapsed_frac"
    else
        printf "Results: %d passed, %d failed (%s) in %d.%03ds\n" \
            "$pass" "$fail" "$TEST_ARCH" "$elapsed_s" "$elapsed_frac"
    fi

    #
    # SKIPPED groups. A `#ifdef`-gated test block that compiles to nothing is
    # invisible in a pass count -- the default build has no TLS, so AxlTestJose
    # and AxlTestCrypto drop ~190 assertions and still report "0 failed". That
    # read as success once already and new assertions were called green having
    # never executed.
    #
    # So skips are always announced, and TEST_REQUIRE_TLS=1 turns a TLS-gated
    # skip into a failure. Not the default: a build with no mbedtls submodule is
    # a supported, everyday configuration, and a suite that went red for it
    # would just teach everyone to ignore red. The completeness gates before a
    # commit or a release are what should set it.
    #
    if (( skip > 0 )); then
        echo ""
        echo "*** $skip test GROUP(S) SKIPPED ($skipped_asserts assertions declared):"
        grep -aE '^SKIP(\[[0-9]+\])?:' "$TEST_CLEAN_LOG" | sed -E 's/^SKIP(\[[0-9]+\])?:/    /'
        if (( tls_skips > 0 )); then
            if [[ "${TEST_REQUIRE_TLS:-0}" == "1" ]]; then
                echo ""
                echo "FAIL: TEST_REQUIRE_TLS=1 but $tls_skips group(s) need AXL_TLS=1."
                echo "      Rebuild with TLS and re-run:"
                echo "        git submodule update --init --depth 1 deps/mbedtls"
                echo "        AXL_TLS=1 make ARCH=${_native_arch:-x64} all tests"
                echo "        AXL_TLS=1 $0"
                return 1
            fi
            echo ""
            echo "    ^ build with AXL_TLS=1 to run these; set TEST_REQUIRE_TLS=1"
            echo "      to make skipping them a failure."
        fi
    fi

    # Leak verdict. Folded into the final exit (and into the baseline write
    # below) rather than exiting here, so a run that both leaks and drops its
    # count still reports both.
    local leak_rc=0
    test_check_leaks || leak_rc=1

    #
    # Wall-clock BUDGET. Distinct from test_run_foreground's timeout, which is
    # a hang detector -- a suite that creeps to just under it passes silently,
    # which is exactly how a slow regression hides. This is the ratchet's
    # time-shaped sibling: TEST_MAX_DURATION is the number that must not drift
    # up unnoticed. Named to match run-qemu.sh's --max-duration / MAX_DURATION
    # and AGT's AGT_MAX_DURATION rather than inventing a third spelling for one
    # concept across sibling repos. (test-axl.sh builds its own QEMU command
    # instead of shelling out to run-qemu.sh, so it cannot just pass the flag.)
    #
    # Deliberately generous. The host drifts ~10% run to run and the box is
    # shared, so a budget tight enough to catch a 5% regression would be
    # flaky, and a flaky gate is worse than none. Set it to roughly 2x the
    # observed time and treat a breach as "something got materially slower",
    # not "shave 3 seconds".
    #
    if [[ -n "${TEST_MAX_DURATION:-}" && "${TEST_SKIP_RATCHET:-0}" != "1" ]]; then
        if (( elapsed_s > TEST_MAX_DURATION )); then
            echo ""
            echo "FAIL: suite took ${elapsed_s}s, over the ${TEST_MAX_DURATION}s budget."
            echo "  This is a wall-clock ratchet, not a hang timeout — something"
            echo "  got materially slower. Profile it, or raise TEST_MAX_DURATION"
            echo "  in the caller deliberately (and say why in the commit)."
            return 1
        fi
    fi

    #
    # Ratchet: fail if test count dropped from last known good run.
    # Auxiliary scripts (e.g. test-ipmi-qemu.sh which runs a single
    # test binary) opt out via TEST_SKIP_RATCHET=1 so they don't
    # clobber the canonical test-axl.sh baseline with their smaller
    # pass counts.
    #
    # The baseline is PER TLS CONFIGURATION, because the two configurations run
    # different numbers of tests: with AXL_TLS=1 the AxlTestJose and
    # AxlTestCrypto blocks compile in and the count jumps by ~160. Sharing one
    # file meant a single TLS run wrote the higher number and then every
    # ordinary non-TLS run failed the ratchet -- "expected at least 8789 but
    # only 8628 ran", which reads exactly like a regression and is not one.
    # That happened; the committed baseline had to be repaired by hand.
    #
    # Keyed off the run's own evidence (a TLS-gated SKIP means TLS is absent)
    # rather than off $AXL_TLS, so building with TLS and forgetting to export
    # the variable when invoking the suite cannot select the wrong baseline.
    #
    if [[ "${TEST_SKIP_RATCHET:-0}" != "1" ]]; then
        # One baseline PER BUILD CONFIGURATION. AXL_TLS changes which sources
        # are compiled and therefore how many tests exist -- measured, 8820
        # without it and 8964 with. Sharing one file across both is a trap
        # with teeth, because the baseline is rewritten after every green run
        # (below): a single AXL_TLS=1 run silently raises it to 8964, and the
        # next ordinary run then fails with "expected at least 8964 tests but
        # only 8820 ran" -- a message that blames the tests for what is
        # actually a stale baseline from a different configuration. That cost
        # real time to diagnose once; the suffix makes it unrepresentable.
        #
        # The default (no AXL_TLS) keeps the unsuffixed name so the committed
        # baseline stays valid and CI is unaffected.
        local baseline_file="$TESTS_DIR/.last-pass-count"
        if (( tls_skips == 0 )); then
            baseline_file="$TESTS_DIR/.last-pass-count.tls"
        fi
        if [[ -f "$baseline_file" ]]; then
            local expected
            expected=$(cat "$baseline_file")
            if [[ $ratchet_total -lt $expected ]]; then
                echo ""
                echo "FAIL: expected at least $expected tests but only $ratchet_total ran"
                echo "      ($pass passed + $skipped_asserts declared-skipped)"
                echo "      (baseline: $(basename "$baseline_file"))"
                if [[ -n "$stalled" ]]; then
                    echo "      Culprit (no Results footer): $(echo "$stalled" | paste -sd, -)"
                else
                    echo "      (a test binary may have crashed partway through)"
                fi
                exit 1
            fi
        fi
        # A leaking run is not a green run, so it does not get to raise the
        # bar either -- same reasoning as the $fail guard beside it.
        if [[ $fail -eq 0 && $pass -gt 0 && $leak_rc -eq 0 ]]; then
            echo "$ratchet_total" > "$baseline_file"
        fi
    fi

    if [[ $fail -eq 0 && $pass -gt 0 && $leak_rc -eq 0 ]]; then
        exit 0
    else
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Host-side result helpers (for tests that probe a server FROM the host with
# curl, rather than parsing PASS/FAIL out of the guest serial log). These
# replace the pass()/fail()/PASS/FAIL boilerplate each such test re-defined.
# Use test_host_pass/_fail to tally, then test_host_summary at the end.
# ---------------------------------------------------------------------------

TEST_HOST_PASS=0
TEST_HOST_FAIL=0

test_host_pass() { echo "  PASS: $1"; TEST_HOST_PASS=$((TEST_HOST_PASS + 1)); }
test_host_fail() { echo "  FAIL: $1"; TEST_HOST_FAIL=$((TEST_HOST_FAIL + 1)); }

# Print the tally and return non-zero if any check failed (or none passed).
# $1: a label for the line (e.g. the suite name + arch).
test_host_summary() {
    echo ""
    printf "%s: %d passed, %d failed\n" "$1" "$TEST_HOST_PASS" "$TEST_HOST_FAIL"
    [[ $TEST_HOST_FAIL -eq 0 && $TEST_HOST_PASS -gt 0 ]]
}

# Liveness watchdog: GET $1 and require HTTP $3 (default 200) within a bounded
# time. The concurrency wedges this harness targets manifest as a loop that
# stops dispatching, so a probe that times out (curl code 000) is the failure
# signal — re-run after every scenario to convert a silent hang into a named
# FAIL. $2 is the label. Honors CURL_OPTS if the caller set it.
#   test_liveness_probe "https://127.0.0.1:18450/api/version" "loop alive after WS close"
test_liveness_probe() {
    local url="$1" label="$2" want="${3:-200}"
    local opts=(-s --insecure -H "Connection: close" --max-time 10)
    # set -u safe: ${CURL_OPTS+x} is empty when unset, so the length check
    # (which would error under set -u on an unset array) only runs when set.
    [[ -n "${CURL_OPTS+x}" && "${#CURL_OPTS[@]}" -gt 0 ]] && opts=("${CURL_OPTS[@]}")
    local code
    code=$(curl "${opts[@]}" -o /dev/null -w "%{http_code}" "$url" 2>/dev/null || true)
    if [[ "$code" == "$want" ]]; then
        test_host_pass "liveness: $label (HTTP $code)"
    else
        test_host_fail "liveness: $label (got '$code', loop may be wedged)"
    fi
}

# Fail if any AXL_DEBUG_ASSERT fired in the guest. The asserts log a distinct
# marker (see include/axl/axl-debug.h); its presence means a debug-build
# invariant was violated at its cause. Cleans the serial log first.
test_refute_debug_assert() {
    test_clean_log
    if grep -qa 'AXL_DEBUG_ASSERT FAILED' "$TEST_CLEAN_LOG"; then
        test_host_fail "no AXL_DEBUG_ASSERT fired"
        echo "    --- offending markers ---"
        grep -a 'AXL_DEBUG_ASSERT FAILED' "$TEST_CLEAN_LOG" | sed 's/^/    /'
    else
        test_host_pass "no AXL_DEBUG_ASSERT fired (invariants held)"
    fi
}
