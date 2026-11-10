#!/bin/bash
# AXL unit tests — boots QEMU and runs all test .efi applications.
#
# Usage: ./test/integration/test-axl.sh [--arch X64|AARCH64] [--log <path>]
#        ./test/integration/test-axl.sh --list-apps
#
# --log <path>   Save the raw QEMU serial log (full firmware boot,
#                Shell session, every test's PASS/FAIL lines) to the
#                given path. Equivalent to setting TEST_KEEP_LOG.
# --list-apps    Print the unit binaries this suite runs, one per line, and
#                exit. This is the list `verify.sh --apps=<csv>` validates
#                against, and it is published rather than copied: a second
#                copy of TEST_APPS in verify.sh would drift the first time a
#                binary was added, and a validator that accepts a name the
#                suite does not run refuses nothing at all.
# --check-inventory DIR
#                Only the inventory guard below (every built AxlTest*.efi in
#                DIR is run or deliberately skipped), booting nothing. The
#                CTest registration runs one binary per test (TEST_APPS_ONLY),
#                which bypasses the guard, so it runs this as its own test.

[[ -n "$DEBUG" ]] && set -x

source "$(dirname "$0")/common-test.sh"

LIST_APPS=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        --log)  TEST_KEEP_LOG="$2"; export TEST_KEEP_LOG; shift 2 ;;
        --list-apps) LIST_APPS=1; shift ;;
        --list-apps-skipped) LIST_APPS=2; shift ;;
        --budget) LIST_APPS=4; shift ;;
        --check-inventory) LIST_APPS=3; INVENTORY_DIR="${2:?--check-inventory needs a directory}"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64] [--log <path>]" \
                     "| --list-apps | --list-apps-skipped | --check-inventory DIR | --budget"; exit 1 ;;
    esac
done

# DECLARED BEFORE test_setup AND THE BUILD, so --list-apps can answer without
# either. verify.sh asks this question to validate --apps BEFORE it launches
# the arch jobs, and a build here would be a third build racing the two it is
# about to start -- the concurrent-build hazard axl-build.sh's per-tree lock
# exists for.
TEST_APPS=(AxlTestMem AxlTestString AxlTestIO AxlTestLog AxlTestData AxlTestUtil AxlTestLoop AxlTestSmbus AxlTestTask AxlTestNet AxlTestIpmi AxlTestPlatform AxlTestEvent AxlTestRuntime AxlTestXml AxlTestFsProvider AxlTestGfx AxlTestTruetype AxlTestPixmap AxlTestMath AxlTestInput AxlTestFileView AxlTestPieceTree AxlTestFind AxlTestDriver AxlTestCursor AxlTestCompositor AxlTestGfxRegion AxlTestCrypto AxlTestJose AxlTestNvme AxlTestAta AxlTestScsi AxlTestSmart AxlTestHii AxlTestAuth AxlTestFw AxlTestVterm AxlTest9p AxlTestJsonConformance AxlTestSsh AxlTestEd25519 AxlTestSshdBind AxlTestSshdConf AxlTestSshdSftp AxlTestPack AxlTestReplace AxlTestSshHostKey AxlTestSshHostKeyWeak)
# Tests deliberately NOT in the default run, each with a reason. The
# guard below treats anything here as accounted-for.
TEST_APPS_SKIP=(
    AxlTestCpuIdle   # ~3s perf workload; run only by test-cpu-idle.sh
    # Needs the external corpora mounted from deps/ (gitignored) and its case
    # count moves with what has been fetched, which the ratchet cannot accept.
    # Run by test-json-corpus-qemu.sh, from the bucket that opts out.
    AxlTestJsonCorpus
)

# GUEST time (setup is not graded; see test_timing_breakdown). Measured idle
# (load 2-4, contention x1.00) at 14,235 assertions: x64 87.2, 83.6, 86.1 s;
# aa64 73.1 s (it is not budget-checked under verify.sh, which skips the ratchet
# for it, but a standalone run is). 110 s is ~26% over the slowest x64 run, the
# margin the budget has always had (95 over a 75 s baseline). A breach means
# something got materially slower, not that the box was busy. Raise it
# deliberately, in a commit that says why. AxlTestLoop's driver idle-budget
# timing cases retry a failed attempt (about 8 s each, at most two retries),
# so on a busy box they can spend part of this margin without anything
# having got slower.
TEST_MAX_DURATION="${TEST_MAX_DURATION:-110}"
# --budget: that number, for the pooled per-binary runs' ratchet
# (lib/unit-ratchet.sh), which judges their sum against it -- one copy.
if [[ $LIST_APPS -eq 4 ]]; then
    echo "$TEST_MAX_DURATION"
    exit 0
fi

if [[ $LIST_APPS -eq 1 ]]; then
    printf '%s\n' "${TEST_APPS[@]}"
    exit 0
fi
# The deliberately-excluded list, published for the same reason: a validator
# that calls AxlTestCpuIdle "unknown" names the wrong cause and hides the
# reason recorded two lines above it.
if [[ $LIST_APPS -eq 2 ]]; then
    printf '%s\n' "${TEST_APPS_SKIP[@]}"
    exit 0
fi

# The inventory guard: every built AxlTest*.efi must be either in TEST_APPS
# (run) or TEST_APPS_SKIP (explicitly excluded). A newly added test that is
# wired into the build but forgotten here would otherwise build but never run
# -- silently dropping coverage. Make that a hard error instead. A directory
# with NO unit binaries is an error too: a guard that saw nothing passed
# nothing.
check_inventory() {   # check_inventory DIR
    local _efi _name _seen=0
    for _efi in "$1"/AxlTest*.efi; do
        [ -e "$_efi" ] || continue
        _seen=$((_seen + 1))
        _name="$(basename "$_efi" .efi)"
        case " ${TEST_APPS[*]} ${TEST_APPS_SKIP[*]} " in
            *" $_name "*) ;;
            *)
                echo "ERROR: $_name.efi is built but is in neither TEST_APPS" \
                     "nor TEST_APPS_SKIP." >&2
                echo "       Add it to one of those lists in" \
                     "test/integration/test-axl.sh." >&2
                return 1
                ;;
        esac
    done
    if (( _seen == 0 )); then
        echo "ERROR: no AxlTest*.efi in $1 -- the inventory saw nothing to check." >&2
        return 1
    fi
    return 0
}
if [[ $LIST_APPS -eq 3 ]]; then
    check_inventory "$INVENTORY_DIR" && echo "inventory: clean ($INVENTORY_DIR)"
    exit $?
fi

test_setup

# Map test arch to the build's arch (axl-build.sh ARCH=x64|aa64)
declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

"$PROJECT_DIR/scripts/axl-build.sh" \
    ARCH="$_native_arch" all tests 2>&1 | tail -3

NATIVE_DIR="$(test_build_dir)"

# TEST_APPS_ONLY="AxlTestGfx AxlTestTruetype" RUNS a subset: a debug override,
# and how the CTest registration runs one binary per test (unit.<App>).
#
# EVERY binary is still STAGED, only the selected ones are run: the volume a
# subset sees is the one-boot run's. Binaries read each other off it --
# AxlTestUtil's image tests load AxlTestRuntime.efi -- so a volume holding only
# the selected binary dropped 13 of AxlTestUtil's assertions to a 6-count SKIP,
# and the per-binary ratchet summed 14901 against the one-boot run's 14908.
RUN_APPS=("${TEST_APPS[@]}")
if [ -n "${TEST_APPS_ONLY:-}" ]; then
    read -r -a RUN_APPS <<< "$TEST_APPS_ONLY"
else
    check_inventory "$NATIVE_DIR" || exit 1
fi

for app in "${TEST_APPS[@]}"; do
    test_add_efi "$NATIVE_DIR/$app.efi"
done
# ...and a selected binary outside TEST_APPS (a deliberately-skipped one run
# by hand), which the loop above does not stage.
for app in "${RUN_APPS[@]}"; do
    [[ " ${TEST_APPS[*]} " == *" $app "* ]] || test_add_efi "$NATIVE_DIR/$app.efi"
done

# Stage the curated PCI vendor/device database next to the EFIs so
# AxlTestPlatform's pci-ids loader tests find it via the standard
# axl_resolve_data_file companion-path lookup. Without this, tests
# would only exercise the "no database loaded" code path.
PCI_IDS_FILE="$PROJECT_DIR/share/pci-ids.json5"
if [[ -f "$PCI_IDS_FILE" ]]; then
    mkdir -p "$TEST_STAGING"
    cp "$PCI_IDS_FILE" "$TEST_STAGING/pci-ids.json5"
fi
JEDEC_FILE="$PROJECT_DIR/share/jedec.json5"
if [[ -f "$JEDEC_FILE" ]]; then
    mkdir -p "$TEST_STAGING"
    cp "$JEDEC_FILE" "$TEST_STAGING/jedec.json5"
fi
USB_IDS_FILE="$PROJECT_DIR/share/usb-ids.json5"
if [[ -f "$USB_IDS_FILE" ]]; then
    mkdir -p "$TEST_STAGING"
    cp "$USB_IDS_FILE" "$TEST_STAGING/usb-ids.json5"
fi

# Real-firmware ACPI captures for the AML namespace walker. These are
# LOCAL-ONLY by policy -- .gitignore keeps hardware captures out of the
# repo (see test/fixtures/real-hw/README.md), so CI and fresh clones
# have neither, and the tests that use them SKIP with balanced counts.
# Staged flat next to the EFIs, matching the sidecar convention above.
#
# Only the DSDTs are staged: the walker's per-table counts are what the
# tests assert, and hauling 27 SSDTs in would cost ~2.8 MB for coverage
# the DSDTs already provide.
for fixture_arch in client server; do
    dsdt="$PROJECT_DIR/test/fixtures/real-hw/$fixture_arch/acpi/dsdt.dat"
    if [[ -f "$dsdt" ]]; then
        mkdir -p "$TEST_STAGING"
        cp "$dsdt" "$TEST_STAGING/fixture-$fixture_arch-dsdt.dat"
        test_record_input "$dsdt"
    fi
done
# The server's 26 _SUN objects live in SSDT5, not its DSDT -- and _SUN
# is the object the slot correlation leans on hardest where SMBIOS
# publishes no bus address. 8 KB, so it comes along.
server_ssdt5="$PROJECT_DIR/test/fixtures/real-hw/server/acpi/ssdt5.dat"
if [[ -f "$server_ssdt5" ]]; then
    mkdir -p "$TEST_STAGING"
    cp "$server_ssdt5" "$TEST_STAGING/fixture-server-ssdt5.dat"
    test_record_input "$server_ssdt5"
fi

# Test-only class overlay fixture. Carries the "[overlay]" marker
# entry that test_pci_class_db_singleton_overrides loads via the
# explicit-override path — the production sidecar above stays clean
# (zero overrides) so deployed lspci output isn't polluted.
PCI_CLASS_TEST_FILE="$PROJECT_DIR/test/data/pci-class-test.json5"
if [[ -f "$PCI_CLASS_TEST_FILE" ]]; then
    mkdir -p "$TEST_STAGING"
    cp "$PCI_CLASS_TEST_FILE" "$TEST_STAGING/pci-class-test.json5"
fi

# Stage a deliberately-malformed fixture so the load -1 vs -2 test
# can prove parse failures distinguish from missing-file failures.
mkdir -p "$TEST_STAGING"
cat > "$TEST_STAGING/pci-ids-malformed.json5" <<'EOF'
@@@ deliberately invalid — no token here is valid JSON5 so parse fails
unambiguously even with JSON5's permissive grammar (no unquoted-identifier
key escape, no value-position bareword that could partial-parse).
EOF

# Files with the FAT read-only attribute (set on the image below): what
# AxlTestSshdSftp's SFTP REMOVE/RENAME/OPEN-for-write refusals act on, and
# what AxlTestIO's delete-of-a-read-only-file (AXL_ERR) acts on. No
# AXL call sets the attribute, and mcopy does not carry a Unix mode over.
printf 'locked\n' > "$TEST_STAGING/sftp-ro.txt"
printf 'locked\n' > "$TEST_STAGING/axl-ro-del.txt"

# POOLED (docs/AXL-Test-Guest-Pool-Design.md §6.7): on a leased guest of the
# `unit` class the machine is already running -- its NIC configured, and the
# echo and sink peers AxlTestNet connects to provided by the class itself
# (slirp `guestfwd ... cmd:`, spawned per connection; test/pool/classes.toml).
# So no network of this test's own, and no startup.nsh: a generated
# test-axl.nsh runs through the agent from the lease's share.
#
# The binaries do NOT run from the share. It is virtio-fs, and four of them
# need a FAT volume with them at its root, as booted: AxlTestIO and
# AxlTestSshdSftp the FAT read-only attribute (a delete refused), AxlTestUtil
# its binary at the volume's root, AxlTestDriver FAT's names (8.3 aliases,
# trailing-dot trimming). So the script makes a FAT RAM disk in the guest
# (mkrd, the firmware's own FAT driver), copies the staged files onto it, sets
# the read-only attribute where the image build used mattrib, and runs every
# binary from its root -- re-entered before each one: a binary may leave the
# Shell elsewhere (AxlTestUtil's volume and directory cases do), and the next
# one's relative name would not resolve. The disk lives in guest RAM, so the
# snapshot restore after the lease discards it: every lease starts clean.
# The UDP echo is the pool service's (see below).
if _test_pooled; then
    mkdir -p "$TEST_STAGING"
    _mkrd="$NATIVE_DIR/mkrd.efi"
    [[ -f "$_mkrd" ]] || _mkrd="$PROJECT_DIR/build/${_native_arch}-debug/tools/mkrd.efi"
    cp "$_mkrd" "$TEST_STAGING/mkrd.efi" || {
        echo "FAIL: no mkrd.efi to make the pooled run's FAT RAM disk"; exit 1; }
    # The guest runs it: an mkrd change must miss the key (the `tools` goal
    # is pre-built, so no builds= names it).
    test_record_input "$_mkrd"
    _pvol="${AXL_POOL_GUEST_DIR%%:*}:"; _pdir="\\${AXL_POOL_GUEST_DIR#*:\\}"
    _rd=AXLUNIT
    # Not a pipe: a pipeline's last stage is a subshell, and the variables
    # test_use_nsh_text sets (TEST_NSH_LINE) would not reach this one.
    test_use_nsh_text test-axl < <(
        # 64 MB: the staged files are ~10 MB per arch, and AxlTestSshdSftp's
        # grow cases need room past SFTP_GROW_MAX (24 MB failed them).
        printf '%s\ncd %s\nmkrd.efi %s -s 64\n' "$_pvol" "$_pdir" "$_rd"
        # AxlTestDriver.efi FIRST: its self-load case finds its own 8.3 alias
        # as AXLTES~n -- the sequential tails mtools gives the booted image.
        # The firmware's FAT driver, writing them here, switches to another
        # short-name scheme once enough AxlTest* names collide; copied first,
        # the image still gets a low sequential tail on either.
        printf 'cp -q %s%s\\AxlTestDriver.efi %s:\\\n' "$_pvol" "$_pdir" "$_rd"
        printf 'cp -q %s%s\\* %s:\\\n' "$_pvol" "$_pdir" "$_rd"
        for ro in sftp-ro.txt axl-ro-del.txt; do printf 'attrib +r %s:\\%s\n' "$_rd" "$ro"; done
        # The RAM disk becomes fs0: -- the binaries name their fixtures and
        # themselves by fs0:\ (axl-ro-del.txt, sftp-ro.txt, self copies), as
        # booted, where fs0: is their disk. The pool's own fs0 gives up the
        # name for the lease, as fs21 (the agent finds its home again by marker); the
        # snapshot restore after the lease puts the map back.
        # The pool's own disk keeps an fsN of its own (fs21) BEFORE giving up
        # fs0: with no fsN at all, axl_volume_enumerate names it by its handle
        # index -- fs0 -- and two volumes would both read as fs0.
        # And the RAM disk gives up mkrd's FS20 once it is FS0: the Shell
        # reports a volume by its first fsN, so with both it would still read
        # as fs20. (mkrd takes the lowest free FS<n> from FS20 up: FS20 on a
        # fresh leased guest. `map -d` wants the name WITH its colon --
        # without, it says "Cannot find mapped device" and may do nothing.)
        # Its label alias goes too: a volume whose first name is not an fsN is
        # spelled by that name by axl_app_boot_path but as fs0 by
        # axl_driver_locate -- one file, two spellings (booted, FS0 is the
        # disk's only name). The binaries reach it as fs0: from here.
        printf 'map FS21 FS0:\nmap -d FS0:\nmap FS0 %s:\nmap -d FS20:\nmap -d %s:\n' "$_rd" "$_rd"
        for app in "${RUN_APPS[@]}"; do
            printf 'fs0:\ncd \\\n%s.efi\n' "$app"
        done)
    # No way back to the share at the end: on AARCH64 the share IS fs0 before
    # the remap (it enumerates first), so its name now means the RAM disk and
    # a closing `fs0:` + `cd <lease>` fails -- read as a script the agent could
    # not run. Nothing needs it: the agent finds its home by marker, and the
    # snapshot restore after the lease resets the guest.
    # What the guest is told to do, in the log: its own commands print nothing
    # the serial log would show when one of them (mkrd, cp, attrib) fails.
    echo "pooled: $TEST_NSH, run from $AXL_POOL_GUEST_DIR:"
    sed 's/^/  | /' "$TEST_STAGING/$TEST_NSH"
    test_build_image
    # The UDP echo (10.0.2.2:35555, the host's loopback) is the POOL
    # SERVICE's, served for its whole life (axlpool serve --udp-echo-port): an
    # echo this run started could be killed by its cleanup while a concurrent
    # run's AxlTestNet (the other arch, say) still used it.
else
    # Startup script: init network, then run each test app.
    #
    # Per-binary timing is extracted host-side from the `=== NAME Tests ===`
    # header and the `=== Results: N passed, M failed ===` footer that
    # each test emits via axl-test.h. No shell-level markers — an earlier
    # revision emitted `echo ____BEGIN/____END` here, but those extra
    # UEFI Shell echoes deterministically crashed AxlTestNet on aa64 with
    # an ArmCpuDxe data-abort (translation fault) during the socket UDP
    # async test's teardown path.
    #
    {
        echo "@echo -off"
        echo "fs0:"
        echo "cd \\"
        echo ""
        echo "connect -r"
        echo "stall 1000000"
        echo "ifconfig -s eth0 dhcp"
        echo "stall 3000000"
        echo ""
        for app in "${RUN_APPS[@]}"; do
            echo "$app.efi"
        done
        echo "reset -s"
    } | test_set_startup

    test_build_image
    for ro in sftp-ro.txt axl-ro-del.txt; do
        if ! mattrib -i "$TEST_DISK" +r "::/$ro"; then
            echo "FAIL: cannot set the read-only attribute on $ro"
            exit 1
        fi
    done
fi

echo "=== AXL Integration Tests ($TEST_ARCH) ==="

# The unit suite's CPU profile is dominated by firmware boot, so the sampler's
# advisory fired on every run and told us nothing actionable. Measure wall
# clock instead -- same trade run-qemu.sh makes for its duration gate.
test_cpu_advisory_off

test_build_qemu_cmd
# AxlTestNet does several "self-connect" tests against the guest's
# own DHCP IP. Slirp doesn't loopback to the guest, so without help
# every connect waits the full 10 s timeout and SKIPs. Wire the
# stream-echo guestfwd onto each port the suite uses, so the
# client-side connect+send+recv path runs against a real remote. Port 9997
# is a SINK (reads, never replies): AxlTestNet's linger tests need a peer
# whose reply cannot complete the receive under examination.
_test_pooled || test_add_network_with_echo 9994 9996 9998 9999 sink:9997
test_run_foreground 120
# AxlTestUtil's test_debug_assert fires exactly two on purpose (a false
# AXL_DEBUG_ASSERT and a false _MSG); they are also the debug-assert gate's
# control. Under BUILD=RELEASE both compile out.
TEST_EXPECTED_ASSERTS=""
_build="${BUILD:-DEBUG}"   # compared case-insensitively (BUILD=release)
if [[ " ${RUN_APPS[*]} " == *" AxlTestUtil "* && "${_build^^}" != RELEASE ]]; then
    TEST_EXPECTED_ASSERTS="test_debug_assert:2"
fi
test_count_results
