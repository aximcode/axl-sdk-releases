# shellcheck shell=bash
# linkdown-lib.sh -- a guest-visible link-DOWN virtio NIC, for the suites that
# need one (test-netload-qemu.sh, test-sshd-start-nonet-qemu.sh). Sourced after
# scripts/axl-common.sh (find_qemu). Never run on its own.
#
# A link-down virtio NIC was impossible in QEMU until two things were learned
# (see local/docs/handoff-qemu-linkdown-optionA.md):
#
#   (1) `romfile=` (empty) is LOAD-BEARING, not decoration. QEMU ships an iPXE
#       UNDI option ROM for virtio-net (1af4:1000). When present OVMF binds the
#       iPXE UNDI -- an NII3.1 SNP producer whose MediaPresent does NOT track
#       virtio link status (reports UP regardless). `romfile=` disables that ROM
#       so OVMF's native VirtioNetDxe, which DOES read virtio config.status into
#       SNP MediaPresent, is the SNP producer. Without `romfile=` a link-down NIC
#       still reads UP and every link-down test is a silent no-op.
#
#   (2) Two ways to actually put that native NIC's link down, applied
#       BELT-AND-SUSPENDERS so the tests run whether or not the QEMU patch is
#       present (CI / another dev runs stock QEMU):
#         a. `x-link-down=on` -- a virtio-net device property added by a LOCAL
#            QEMU patch. Realizes the NIC with virtio config.status LINK_UP
#            clear, so OVMF reads MediaPresent=FALSE at bind. Stock QEMU ABORTS
#            on an unknown property, so it is applied ONLY when probed-present.
#         b. QMP `set_link <netdev> off` -- works on STOCK QEMU. Driven early and
#            continuously (before OVMF binds the NIC) so the NIC is down by the
#            time VirtioNetDxe reads it; OVMF's MNP background poll also
#            refreshes MediaPresent live. Through axl_qmp (lib/qmp-lib.sh:
#            python3 with qemu.qmp).
#       If NEITHER is available (unpatched QEMU AND no qemu.qmp) a caller
#       SKIPS, loudly, rather than fail.
#
# Use: ld_setup ARCH DIR, then LD_XLD in a link-down NIC's -device string, a
# `-qmp unix:SOCK,server,nowait` on the QEMU line, and
# `pid=$(ld_inject SOCK NETDEV...)` ... `ld_inject_stop "$pid"` around the boot.

# shellcheck source=qmp-lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/qmp-lib.sh"

# ld_setup ARCH DIR -- probe both mechanisms; write the injector into DIR.
# Sets LD_HAVE_XLD, LD_HAVE_QMP, LD_XLD (`x-link-down=on,` or empty) and
# LD_AVAILABLE (1 when either mechanism is present).
ld_setup() {
    local qemu
    qemu=$(find_qemu "$1" 2>/dev/null || true)
    LD_HAVE_XLD=0
    if [[ -n "$qemu" ]] && "$qemu" -device virtio-net-pci,help 2>&1 | grep -q 'x-link-down'; then
        LD_HAVE_XLD=1
    fi
    LD_HAVE_QMP=0
    axl_qmp_available && LD_HAVE_QMP=1
    LD_XLD=""
    [[ "$LD_HAVE_XLD" -eq 1 ]] && LD_XLD="x-link-down=on,"
    LD_AVAILABLE=0
    [[ "$LD_HAVE_XLD" -eq 1 || "$LD_HAVE_QMP" -eq 1 ]] && LD_AVAILABLE=1
    LD_INJECTOR="$2/setlink-off.sh"
    # The injector: drive `set_link off` on the named netdev(s) until killed,
    # through axl_qmp (each call its own deadline-guarded session); errors
    # ignored so it never disturbs the run if QMP is momentarily unavailable.
    # A bounded loop (~300 s), NOT `while :`: ld_inject runs it from a `$(...)`,
    # so it is reparented to init if the test dies before ld_inject_stop --
    # an unbounded loop would then spin forever in CI.
    {
        printf '#!/bin/bash\n'
        printf 'source %q\n' "$_QMP_LIB_ROOT/test/integration/lib/qmp-lib.sh"
        cat <<'INJECTOR'
sock="$1"; shift; nets=("$@")
for _ in $(seq 1 120); do [[ -S "$sock" ]] && break; sleep 0.25; done
for _ in $(seq 1 300); do
  for n in "${nets[@]}"; do
    axl_qmp --timeout 3 "$sock" set_link "{\"name\":\"$n\",\"up\":false}" >/dev/null 2>&1 || true
  done
  sleep 1
done
INJECTOR
    } > "$LD_INJECTOR"
    chmod +x "$LD_INJECTOR"
}

# ld_inject SOCK NETDEV... -- echoes the injector's PID ("" without axl_qmp).
# The background child's fds MUST be redirected off the command-substitution
# pipe: an inherited stdout keeps `$(...)` blocked until the child closes it --
# i.e. hangs the whole test. </dev/null in too, so it never competes for the
# run's stdin.
ld_inject() {
    [[ "$LD_HAVE_QMP" -eq 1 ]] || { echo ""; return 0; }
    "$LD_INJECTOR" "$@" </dev/null >/dev/null 2>&1 & echo "$!"
}

# ld_inject_stop PID -- the injector and its children only; never `pkill -f`.
ld_inject_stop() {
    [[ -n "$1" ]] || return 0
    pkill -P "$1" 2>/dev/null || true
    kill "$1" 2>/dev/null || true
    wait "$1" 2>/dev/null || true
}
