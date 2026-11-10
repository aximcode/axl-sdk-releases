#!/bin/bash
# test-meta: arch=none needs= est=10 cpu=4 local-only=0 guest=none
# guest=none: harness test: run-qemu.sh and common-test.sh building the
#   standard machine, compared against stub QEMU.
# test-standard-guest.sh -- run-qemu.sh and common-test.sh build ONE standard
# guest, judged by the guest-pool census's own definition of a guest shape.
#
# WHY. The census (docs/AXL-Test-Guest-Pool-Design.md §7) found the suite's
# four commonest guests were one machine built four ways by the two launchers,
# and a resident pool can serve a shape only if every launcher agrees on it.
# These are the assertions that keep them agreeing:
#
#   - `run-qemu.sh --bridges` (no network) and common-test.sh with
#     test_add_no_network are ONE census class, on both arches. The census
#     classifier itself decides (lib/pool-census.py), so "same shape" means
#     exactly what the pool will mean by it.
#   - The CONTROL: a plain run-qemu.sh guest is a DIFFERENT class -- the
#     classifier can tell the guests apart, so its "one class" above is a
#     verdict and not a classifier that merges everything.
#   - Every standard guest carries the root port at 00:02.0, xHCI at 00:03.0
#     and exactly ONE virtio-rng, behind the root port (AARCH64 once carried
#     two). Pinned slots: tests match BDFs in exact strings, and unpinned they
#     differed per arch and moved whenever a device ahead of them did.
#   - A plain run-qemu.sh guest is the bare machine it always was (no root
#     port; AARCH64's one RNG bare on the root bus): outside consumers
#     (softbmc's PCI goldens) pin its layout.
#   - A common-test.sh guest whose test chose no network gets the standard NIC
#     stated, never QEMU's default one -- read from what test_run_foreground
#     and test_run_background ACTUALLY LAUNCH (a stub stands in for QEMU and
#     records its argv), not from a helper this test calls itself.
#
# Host-only: run-qemu.sh assembles under QEMU_DRYRUN, and common-test.sh's
# launchers start a stub; no guest boots.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
# shellcheck source=/dev/null
source "$SCRIPT_DIR/common-test.sh"
set +e
set -uo pipefail
export TEST_SKIP_RATCHET=1

echo "=== One standard guest from both launchers ==="
echo ""

WORK="$(mktemp -d -t axl-stdguest.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT
EFI="$WORK/probe.efi"
printf 'MZ' > "$EFI"   # staged, never run
# The stub QEMU: records the argv it was started with, one token per line.
STUB="$WORK/qemu-stub"
cat > "$STUB" <<'STUBEOF'
#!/bin/bash
printf '%s\n' "$@" > "$STUB_OUT"
STUBEOF
chmod +x "$STUB"

# rq_argv <arch> [run-qemu args...]: the run-qemu.sh argv, one token per line.
rq_argv() {
    local arch="$1"; shift
    QEMU_DRYRUN=1 "$PROJECT_DIR/scripts/run-qemu.sh" --arch "$arch" "$@" "$EFI" 2>/dev/null \
        | sed -n 's/^QEMU_DRYRUN: //p'
}

# ct_argv <arch> <net: none|default> <foreground|background>: the argv
# common-test.sh's launcher actually starts, one token per line (the real
# QEMU path first, where the stub stood). A subshell, so each run gets fresh harness state.
ct_argv() (
    TEST_ARCH="$1"
    test_setup >/dev/null 2>&1 || exit 1
    test_build_qemu_cmd
    [[ "$2" == none ]] && test_add_no_network
    TEST_QEMU_CMD[0]="$STUB"
    export STUB_OUT="$TEST_TMPDIR/argv"
    if [[ "$3" == background ]]; then
        test_run_background >/dev/null 2>&1
        wait "$TEST_QEMU_PID" 2>/dev/null
    else
        test_run_foreground 10 >/dev/null 2>&1
    fi
    printf '%s\n' "$TEST_QEMU_BIN"   # the executable the stub stood in for
    cat "$STUB_OUT"
    cpu_monitor_finish >/dev/null 2>&1
    rm -rf "$TEST_TMPDIR"
)

# record <name> <argv file>: a census launch record, as lib/profile.sh writes.
record() {
    local -a argv
    mapfile -t argv < "$2"
    printf 'launch|%s|probe||%s\n' "$1" "$(axl_census_argv "${argv[@]}")"
}

# classes <records...>: how many classes the census sees.
classes() {
    local f="$WORK/prof.$RANDOM"
    printf '%s\n' "$@" > "$f"
    python3 "$SCRIPT_DIR/lib/pool-census.py" "$f" \
        | sed -n 's/^=== .* \([0-9][0-9]*\) classes,.*/\1/p'
}

# count <pattern> <argv file>: tokens matching an extended regex.
count() { grep -cE "$1" "$2"; }

for arch in X64 AARCH64; do
    rq_argv "$arch" --bridges > "$WORK/rq-bridges.$arch"
    rq_argv "$arch" > "$WORK/rq-plain.$arch"
    ct_argv "$arch" none foreground > "$WORK/ct-none.$arch"
    ct_argv "$arch" default foreground > "$WORK/ct-default.$arch"
    ct_argv "$arch" default background > "$WORK/ct-default-bg.$arch"
    for f in rq-bridges rq-plain ct-none ct-default ct-default-bg; do
        if [[ ! -s "$WORK/$f.$arch" ]]; then
            echo "SKIP: could not assemble the $f guest for $arch (QEMU or firmware missing?)"
            exit 77
        fi
    done

    n=$(classes "$(record a "$WORK/rq-bridges.$arch")" "$(record b "$WORK/ct-none.$arch")")
    if [[ "$n" == 1 ]]; then
        test_host_pass "$arch: run-qemu.sh --bridges and common-test.sh are one census class"
    else
        test_host_fail "$arch: run-qemu.sh --bridges and common-test.sh are one census class (got ${n:-none})"
        diff <(sort "$WORK/rq-bridges.$arch") <(sort "$WORK/ct-none.$arch") | head -10 | sed 's/^/      /'
    fi

    n=$(classes "$(record a "$WORK/rq-plain.$arch")" "$(record b "$WORK/ct-none.$arch")")
    if [[ "$n" == 2 ]]; then
        test_host_pass "$arch: control -- a guest without USB is a different class"
    else
        test_host_fail "$arch: control -- a guest without USB is a different class (got ${n:-none})"
    fi

    for f in rq-bridges ct-none ct-default ct-default-bg; do
        rng=$(count '^virtio-rng-pci' "$WORK/$f.$arch")
        behind=$(count '^virtio-rng-pci,bus=axl_rp0$' "$WORK/$f.$arch")
        port=$(count '^pcie-root-port,id=axl_rp0,bus=pcie.0,addr=0x2,' "$WORK/$f.$arch")
        xhci=$(count '^qemu-xhci,id=axl_usb0,bus=pcie.0,addr=0x3$' "$WORK/$f.$arch")
        if [[ "$rng" == 1 && "$behind" == 1 && "$port" == 1 && "$xhci" == 1 ]]; then
            test_host_pass "$arch $f: root port at 00:02.0, xHCI at 00:03.0, one virtio-rng behind the port"
        else
            test_host_fail "$arch $f: root port at 00:02.0, xHCI at 00:03.0, one virtio-rng behind the port (ports=$port xhci=$xhci rngs=$rng behind=$behind)"
        fi
    done

    want_rng=0; [[ "$arch" == AARCH64 ]] && want_rng=1
    rng=$(count '^virtio-rng-pci' "$WORK/rq-plain.$arch")
    bare=$(count '^virtio-rng-pci$' "$WORK/rq-plain.$arch")
    port=$(count '^pcie-root-port' "$WORK/rq-plain.$arch")
    usb=$(count '^(qemu-xhci|usb-)' "$WORK/rq-plain.$arch")
    if [[ "$rng" == "$want_rng" && "$bare" == "$want_rng" && "$port" == 0 && "$usb" == 0 ]]; then
        test_host_pass "$arch rq-plain: the bare machine, unchanged ($want_rng bare RNG, no root port, no USB)"
    else
        test_host_fail "$arch rq-plain: the bare machine, unchanged (rngs=$rng bare=$bare ports=$port usb=$usb, want $want_rng/$want_rng/0/0)"
    fi

    nic=$(TEST_ARCH=$arch; TEST_NIC_MODEL=; _test_nic_device)
    for f in ct-default ct-default-bg; do
        if [[ $(count '^user,id=net0$' "$WORK/$f.$arch") == 1 \
              && $(count "^${nic},netdev=net0\$" "$WORK/$f.$arch") == 1 ]]; then
            test_host_pass "$arch $f: no network chosen launches the standard NIC ($nic), stated"
        else
            test_host_fail "$arch $f: no network chosen launches the standard NIC ($nic), stated"
            grep -nE 'net' "$WORK/$f.$arch" | sed 's/^/      /'
        fi
    done
    if [[ $(count '^-netdev$' "$WORK/ct-none.$arch") == 0 ]]; then
        test_host_pass "$arch: test_add_no_network still means no NIC"
    else
        test_host_fail "$arch: test_add_no_network still means no NIC"
    fi
done

# A hand-built TEST_QEMU_CMD chose its machine whole: the launcher must not
# append the standard NIC to it (`-M none` has no PCI bus, so QEMU would die at
# startup and turn a "the guest is gone" check into a pass for the wrong reason).
hand=$(
    test_setup >/dev/null 2>&1 || exit 1
    TEST_QEMU_CMD=("$STUB" -M none -nodefaults)
    export STUB_OUT="$TEST_TMPDIR/argv"
    test_run_background >/dev/null 2>&1
    wait "$TEST_QEMU_PID" 2>/dev/null
    cat "$STUB_OUT"
    cpu_monitor_finish >/dev/null 2>&1
    rm -rf "$TEST_TMPDIR"
)
if [[ "$hand" == *"-M"* ]] && ! grep -qE '^-(net|netdev|nic)$' <<< "$hand"; then
    test_host_pass "a hand-built TEST_QEMU_CMD launches with no NIC added"
else
    test_host_fail "a hand-built TEST_QEMU_CMD launches with no NIC added"
    printf '%s\n' "$hand" | sed 's/^/      /' | head -8
fi

echo ""
test_host_summary "standard-guest"
