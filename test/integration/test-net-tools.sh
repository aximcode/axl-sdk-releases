#!/bin/bash
# test-meta: arch=x64 needs= est=16 local-only=0
# tools/{fetch,netinfo} integration test — boots QEMU with user-mode
# networking, runs each tool against a host-side Python HTTP server
# (reused from test-http.sh), and validates the serial log output.
#
# Closes the test-tools.sh coverage gap for the network-using tools.
# rfbrowse already has its own end-to-end script (test-redfish.sh).
#
# Usage: ./test/integration/test-net-tools.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

# Auxiliary single-binary script; don't clobber test-axl.sh's baseline.
export TEST_SKIP_RATCHET=1

test_parse_args "$@"
test_setup

HOST_PORT=$(test_port 0)

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

make -C "$PROJECT_DIR" \
    ARCH="$_native_arch" ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} all tools 2>&1 | tail -3

NATIVE_DIR="$PROJECT_DIR/out/native-$_native_arch"
TOOLS_DIR="$NATIVE_DIR/tools"

test_add_efi "$TOOLS_DIR/fetch.efi"
test_add_efi "$TOOLS_DIR/netinfo.efi"

# Start a host-side HTTP server (same one test-http.sh uses) so fetch
# has a target the guest can reach via the QEMU slirp gateway 10.0.2.2.
python3 "$(dirname "$0")/host-server.py" "$HOST_PORT" &
HOST_SERVER_PID=$!
trap 'test_cleanup; [[ $HOST_SERVER_PID -gt 0 ]] && kill $HOST_SERVER_PID 2>/dev/null || true' EXIT
sleep 1
echo "  Host HTTP server PID: $HOST_SERVER_PID, port: $HOST_PORT"

cat << NSHEOF | test_set_startup
@echo -off
fs0:
cd \\

echo Connecting drivers...
connect -r
stall 1000000

echo Configuring network via DHCP...
ifconfig -s eth0 dhcp
stall 3000000

echo === TEST-NETINFO-LIST ===
netinfo.efi list

echo === TEST-NETINFO-NO-LOAD ===
netinfo.efi -v --no-load list

echo === TEST-NETINFO-VERBOSE ===
netinfo.efi -v list

echo === TEST-NETINFO-LIST-BUNDLE ===
netinfo.efi list-bundle

echo === TEST-NETINFO-DIAG ===
netinfo.efi diag

echo === TEST-FETCH-GET ===
fetch.efi http://10.0.2.2:${HOST_PORT}/hello

echo === TEST-FETCH-404 ===
fetch.efi http://10.0.2.2:${HOST_PORT}/no-such-thing

echo === TEST-FETCH-REDIRECT ===
fetch.efi http://10.0.2.2:${HOST_PORT}/redirect

echo === TEST-FETCH-SAVE ===
fetch.efi http://10.0.2.2:${HOST_PORT}/hello -o fetched.txt
type fetched.txt

echo === TEST-END ===
reset -s
NSHEOF

test_build_image

echo "=== AXL Network Tool Tests ($TEST_ARCH) ==="

# OVMF's built-in NIC drivers (e1000) are sufficient for the tools'
# axl_net_ensure_drivers() to find a NIC. Same setup as test-http.sh.
test_build_qemu_cmd
TEST_QEMU_CMD+=(
    -device e1000,netdev=net0
    -netdev "user,id=net0"
)
test_run_foreground 60

test_clean_log

# ---------------------------------------------------------------------------
# Result checking — scrape expected output per section
# ---------------------------------------------------------------------------

PASS=0
FAIL=0

check_section() {
    local section="$1"
    local pattern="$2"
    local desc="$3"
    local start_marker="=== ${section} ==="
    if ! grep -q "$start_marker" "$TEST_CLEAN_LOG"; then
        echo "  FAIL: $desc (section '$section' not found)"
        FAIL=$((FAIL + 1))
        return
    fi
    # Use our own TEST-... prefix as the section terminator. The
    # tools themselves emit ===-banner headers (e.g. netinfo prints
    # "=== Network Interfaces ===") so a bare /^=== / would close
    # the section right after the start marker.
    local extracted
    extracted=$(sed -n "/$start_marker/,/^=== TEST-/p" "$TEST_CLEAN_LOG")
    if echo "$extracted" | grep -qE -- "$pattern"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (pattern '$pattern' missing in '$section')"
        FAIL=$((FAIL + 1))
    fi
}

# netinfo list: prints an interface table with MAC + LINK columns.
# We don't assert IPv4 — netinfo doesn't run DHCP itself; the EDK
# Shell's `ifconfig dhcp` configures a separate interface that
# netinfo's enumerator may not report addresses for. The columns
# being present at all proves the tool ran and found NIC handles.
check_section "TEST-NETINFO-LIST" "52:54:00:12:34:56" "netinfo list prints QEMU NIC MAC"
check_section "TEST-NETINFO-LIST" "MTU"               "netinfo list prints interface table"

# --no-load: list still works (firmware already provides e1000) and
# the snapshot label switches to the "firmware-provided, --no-load"
# variant — only emitted on the --no-load path. Its presence proves
# ensure_drivers was skipped without needing an absence-of-line check.
check_section "TEST-NETINFO-NO-LOAD" "52:54:00:12:34:56" "--no-load list prints NIC MAC"
check_section "TEST-NETINFO-NO-LOAD" "firmware-provided, --no-load" "--no-load: snapshot label confirms skip"

# -v list: shows pre/post NIC Drivers snapshots and the enriched
# per-NIC info (device path text + PCI BDF). The e1000 lives at
# 0:3.0 in QEMU's default Q35/i440FX layout.
check_section "TEST-NETINFO-VERBOSE" "NIC Drivers \(before driver-load\)"  "verbose: pre-load snapshot header"
check_section "TEST-NETINFO-VERBOSE" "NIC Drivers \(after driver-load\)"   "verbose: post-load snapshot header"
check_section "TEST-NETINFO-VERBOSE" "PciRoot"                              "verbose: device path text rendered"

# list-bundle: no drivers/<arch>/ on the test image, so the verb
# should print the "no drivers staged" line. Asserting the literal
# tells us the verb ran (vs. parser rejecting it).
check_section "TEST-NETINFO-LIST-BUNDLE" "no drivers staged"  "list-bundle reports empty bundle"

# diag: composite report. Spot-check that each section header lands
# in the output — proves all sub-routines ran without aborting.
check_section "TEST-NETINFO-DIAG" "=== Firmware ==="                 "diag: firmware section"
check_section "TEST-NETINFO-DIAG" "=== Mounted Volumes ==="          "diag: volumes section"
check_section "TEST-NETINFO-DIAG" "=== PCI Network Controllers"      "diag: PCI nic section"
check_section "TEST-NETINFO-DIAG" "=== ensure_drivers status ==="    "diag: ensure status section"

# fetch GET /hello returns the JSON body our host-server.py serves.
check_section "TEST-FETCH-GET" "hello from host"     "fetch GET prints response body"

# fetch 404: status code surfaces in the > line and exit code is non-zero.
# Just verify the next test marker appears (proves fetch returned, didn't hang).
check_section "TEST-FETCH-404" "TEST-FETCH-REDIRECT" "fetch 404 returned (next marker reached)"

# fetch redirect: host-server.py replies 302 to /hello, which fetch's
# redirect-following logic chases and prints the body.
check_section "TEST-FETCH-REDIRECT" "hello from host" "fetch follows 302 to /hello"

# fetch -o: the save path reported its OUTCOME BACKWARDS. AXL_OK is 0, so the
# `if (!axl_file_set_contents(...))` guard ran the failure branch on every
# successful write -- fetch announced "write 'x' failed" and still exited 0 for
# a file it had just saved correctly. Assert both halves: the success line, and
# that the failure line is absent for a save that plainly worked (the `type`
# of the file in the same section proves the bytes are really there).
check_section "TEST-FETCH-SAVE" "Saved [0-9]+ bytes to fetched.txt" \
    "fetch -o reports the save it actually performed"
check_section "TEST-FETCH-SAVE" "hello from host" \
    "fetch -o wrote the body to the file"
if sed -n "/=== TEST-FETCH-SAVE ===/,/^=== TEST-/p" "$TEST_CLEAN_LOG" \
        | grep -q "write 'fetched.txt' failed"; then
    echo "  FAIL: fetch -o must not report a failure for a save that worked"
    FAIL=$((FAIL + 1))
else
    echo "  PASS: fetch -o must not report a failure for a save that worked"
    PASS=$((PASS + 1))
fi

echo ""
printf "Results: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "--- Serial log tail ---"
    tail -60 "$TEST_CLEAN_LOG"
    exit 1
fi
exit 0
