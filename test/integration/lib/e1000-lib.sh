# shellcheck shell=bash
# e1000-lib.sh -- a NIC driver the firmware lacks, for the suites that sweep
# or embed one (test-sshd-sweep-qemu.sh, test-sshd-rtk-qemu.sh): iPXE's e1000
# driver, cut from QEMU's efi-e1000.rom, for a ROM-less QEMU e1000. x64 only:
# QEMU ships no aarch64 e1000 driver. Sourced after common-test.sh's
# test_setup (find_qemu, $NAT, $TEST_TMPDIR). Never run on its own.

# e1000_driver -- the driver into $TEST_TMPDIR/e1000.efi, named by E1000; or
# E1000 "" and WHY_NO_E1000 saying why not (return 1).
E1000=""
WHY_NO_E1000=""
e1000_driver() {
    local rom="" r qemu kind
    if [[ "$NAT" != x64 ]]; then
        WHY_NO_E1000="QEMU ships no aarch64 e1000 driver"
        return 1
    fi
    qemu=$(find_qemu X64 2>/dev/null || true)
    for r in ${qemu:+"$(dirname "$qemu")/../share/qemu/efi-e1000.rom"} \
             /usr/share/qemu/efi-e1000.rom /usr/share/ipxe/qemu/efi-e1000.rom; do
        [[ -f "$r" ]] && { rom="$r"; break; }
    done
    if [[ -z "$rom" ]]; then
        WHY_NO_E1000="no efi-e1000.rom beside QEMU or in /usr/share"
        return 1
    fi
    if ! kind=$(python3 -I "$(dirname "$0")/lib/efi-rom-image.py" "$rom" 8664 \
                "$TEST_TMPDIR/e1000.payload"); then
        WHY_NO_E1000="cannot cut the EFI image from $rom"
        return 1
    fi
    if [[ "$kind" == raw ]]; then
        cp "$TEST_TMPDIR/e1000.payload" "$TEST_TMPDIR/e1000.efi"
    elif ! command -v TianoCompress >/dev/null 2>&1; then
        WHY_NO_E1000="no TianoCompress to expand $rom's driver"
        return 1
    elif ! TianoCompress -d --uefi -o "$TEST_TMPDIR/e1000.efi" \
              "$TEST_TMPDIR/e1000.payload" >/dev/null 2>&1; then
        WHY_NO_E1000="TianoCompress could not expand $rom's driver"
        return 1
    fi
    E1000="$TEST_TMPDIR/e1000.efi"
}

# e1000_required_in_ci -- under CI, an x64 run without the fixture FAILS: the
# lease it exists for must run there.
e1000_required_in_ci() {
    if [[ "$NAT" == x64 && -z "$E1000" && ( "${CI:-}" == true || "${GITHUB_ACTIONS:-}" == true ) ]]; then
        echo "FAIL: the e1000 driver fixture: $WHY_NO_E1000 (CI must run the lease)"
        exit 1
    fi
}
