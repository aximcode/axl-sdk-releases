#!/bin/bash
# test-meta: arch=x64 needs= est=1 local-only=0 guest=none
# guest=none: harness test: run-qemu.sh's argument parsing (--help,
#   refusals, QEMU_DRYRUN); no guest boots.
# test-run-qemu-flags.sh -- argument-parsing tests for run-qemu.sh.
#
# These run on the host (no QEMU), so they're cheap and live outside
# the QEMU integration matrix. They cover the bits of run-qemu.sh
# that don't need a guest:
#   - bash -n syntax pass
#   - --help exits 0 and advertises the flags we care about
#   - --interactive rejects --background and --screenshot
#   - missing EFI file is reported
#   - --monitor-socket / --qmp-socket: --background only, unix sockets in CMD

set -euo pipefail

# The runner case below names THIS test. Should the runner ever fail to refuse,
# it would run us again, and we would call it again -- forever.
if [[ -n "${AXL_FLAGS_TEST_NESTED:-}" ]]; then
    echo "test-run-qemu-flags: nested under its own runner check; not recursing"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"

PASS=0
FAIL=0

check() {
    local name="$1" expected_rc="$2" expected_pat="$3"
    shift 3
    local out rc=0
    out=$("$@" 2>&1) || rc=$?
    if [[ "$rc" != "$expected_rc" ]]; then
        echo "FAIL: $name -- expected rc=$expected_rc, got $rc"
        echo "  output: $out"
        FAIL=$((FAIL + 1))
        return
    fi
    if [[ -n "$expected_pat" ]] && ! grep -qE "$expected_pat" <<< "$out"; then
        echo "FAIL: $name -- output did not match /$expected_pat/"
        echo "  output: $out"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "PASS: $name"
    PASS=$((PASS + 1))
}

# --- syntax ---------------------------------------------------------------
if bash -n "$RUN_QEMU"; then
    echo "PASS: bash -n"
    PASS=$((PASS + 1))
else
    echo "FAIL: bash -n"
    FAIL=$((FAIL + 1))
fi

# --- --help ---------------------------------------------------------------
check "--help exits 0 and lists --interactive" 0 \
    "(-i|--interactive)" \
    "$RUN_QEMU" --help

# --- mutually-exclusive guards --------------------------------------------
DUMMY="$(mktemp --suffix=.efi)"
trap 'rm -f "$DUMMY"' EXIT

check "--interactive + --background rejected" 1 \
    "cannot be combined with --background" \
    "$RUN_QEMU" -i --background "$DUMMY"

check "--interactive + --screenshot rejected" 1 \
    "cannot be combined with --screenshot" \
    "$RUN_QEMU" --interactive --screenshot /tmp/x.png "$DUMMY"

# --- missing file guard (sanity, also exercises arg parsing) --------------
check "missing EFI file rejected" 1 \
    "file not found" \
    "$RUN_QEMU" -i /nonexistent/missing.efi

# --- bare-shell mode requires --interactive -------------------------------
check "no EFI + no -i rejected" 1 \
    "or: .* --interactive" \
    "$RUN_QEMU"

# --- --mount validation ---------------------------------------------------
check "--mount rejects missing dir" 1 \
    "is not a directory" \
    "$RUN_QEMU" -i --mount /nonexistent/dir/abc

check "--mount help text mentions virtiofs" 0 \
    "virtiofs" \
    "$RUN_QEMU" --help

# --- --cpu-report ---------------------------------------------------------
check "--help advertises --cpu-report" 0 \
    "cpu-report" \
    "$RUN_QEMU" --help

# --- --qemu-arg passthrough -----------------------------------------------
check "--help advertises --qemu-arg" 0 \
    "qemu-arg" \
    "$RUN_QEMU" --help

# Smoke-test --qemu-arg by enabling QEMU_DRYRUN — a small extension
# we'll add concurrently that prints the constructed CMD and exits
# without launching QEMU. Falling back to a "missing file" check
# when DRYRUN isn't supported keeps this test a useful arg-shape
# regression even on older run-qemu.sh.
# Each --qemu-arg is ONE literal token, appended verbatim (no word-
# split): two flags → two tokens in order, AND a token may contain a
# space (the property programmatic callers like axl-emulate rely on for
# device specs whose file paths contain spaces).
DRYRUN_OUT=$(QEMU_DRYRUN=1 "$RUN_QEMU" --qemu-arg "axl-flag-A" \
                                       --qemu-arg "axl-flag-B" \
                                       --qemu-arg "tok with space" \
                                       "$DUMMY" 2>&1 || true)
if grep -q "QEMU_DRYRUN: " <<< "$DRYRUN_OUT"; then
    if grep -qE "^QEMU_DRYRUN: axl-flag-A$" <<< "$DRYRUN_OUT" \
       && grep -qE "^QEMU_DRYRUN: axl-flag-B$" <<< "$DRYRUN_OUT"; then
        echo "PASS: --qemu-arg tokens reach CMD in order"
        PASS=$((PASS + 1))
    else
        echo "FAIL: --qemu-arg tokens not in CMD"
        echo "  output: $DRYRUN_OUT"
        FAIL=$((FAIL + 1))
    fi
    # The space-containing token must survive as ONE line, intact.
    if grep -qE "^QEMU_DRYRUN: tok with space$" <<< "$DRYRUN_OUT"; then
        echo "PASS: --qemu-arg preserves a space-containing token verbatim"
        PASS=$((PASS + 1))
    else
        echo "FAIL: --qemu-arg split a space-containing token"
        echo "  output: $DRYRUN_OUT"
        FAIL=$((FAIL + 1))
    fi
else
    echo "SKIP: --qemu-arg dryrun (QEMU_DRYRUN not supported by this run-qemu.sh)"
fi

# --- HF4: --mac ADDR ------------------------------------------------------
# Replay a captured NIC's hardware address. Sets the QEMU NIC device's
# mac= property and implies --net (like --nic-model). aa64 supports
# virtio-net too, so no arch gating here.
check "--help advertises --mac" 0 \
    "mac " \
    "$RUN_QEMU" --help

DRYRUN_MAC=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 \
                                       --mac "de:ad:be:ef:00:01" \
                                       "$DUMMY" 2>&1 || true)
if grep -qE "^QEMU_DRYRUN: virtio-net-pci,netdev=net0,mac=de:ad:be:ef:00:01$" \
        <<< "$DRYRUN_MAC"; then
    echo "PASS: --mac sets the NIC device mac= and implies --net"
    PASS=$((PASS + 1))
else
    echo "FAIL: --mac did not wire mac= onto the NIC device"
    echo "  output: $DRYRUN_MAC"
    FAIL=$((FAIL + 1))
fi

# --mac composes with --nic-model (mac= on the chosen model).
DRYRUN_MAC_E1000=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 \
                                             --nic-model e1000 \
                                             --mac "52:54:00:ab:cd:ef" \
                                             "$DUMMY" 2>&1 || true)
if grep -qE "^QEMU_DRYRUN: e1000,netdev=net0,mac=52:54:00:ab:cd:ef$" \
        <<< "$DRYRUN_MAC_E1000"; then
    echo "PASS: --mac composes with --nic-model"
    PASS=$((PASS + 1))
else
    echo "FAIL: --mac did not compose with --nic-model"
    echo "  output: $DRYRUN_MAC_E1000"
    FAIL=$((FAIL + 1))
fi

# Malformed MAC rejected up-front (clear error, not an opaque QEMU one).
check "--mac rejects malformed address" 1 \
    "MAC" \
    "$RUN_QEMU" --arch X64 --mac "not-a-mac" "$DUMMY"

check "--mac rejects too-few octets" 1 \
    "MAC" \
    "$RUN_QEMU" --arch X64 --mac "de:ad:be:ef:00" "$DUMMY"

# --- HF4: --cpu SPEC ------------------------------------------------------
# Replay a captured CPU identity by overriding QEMU's -cpu model. The
# override replaces the default model (host under KVM, cortex-a57 on
# aa64) — a single -cpu token, not an extra one.
check "--help advertises --cpu" 0 \
    "guest CPU model" \
    "$RUN_QEMU" --help

DRYRUN_CPU=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch X64 \
                                       --cpu "qemu64,vendor=GenuineIntel,family=6,model=42,stepping=7" \
                                       "$DUMMY" 2>&1 || true)
if grep -qE "^QEMU_DRYRUN: -cpu$" <<< "$DRYRUN_CPU" \
   && grep -qE "^QEMU_DRYRUN: qemu64,vendor=GenuineIntel,family=6,model=42,stepping=7$" <<< "$DRYRUN_CPU"; then
    echo "PASS: --cpu overrides the x86 -cpu model"
    PASS=$((PASS + 1))
else
    echo "FAIL: --cpu did not set the -cpu model"
    echo "  output: $DRYRUN_CPU"
    FAIL=$((FAIL + 1))
fi

# The override must REPLACE the default — exactly one -cpu in the CMD,
# not the default plus the override.
cpu_count=$(grep -cE "^QEMU_DRYRUN: -cpu$" <<< "$DRYRUN_CPU" || true)
if [[ "$cpu_count" -eq 1 ]]; then
    echo "PASS: --cpu yields exactly one -cpu (replaces the default)"
    PASS=$((PASS + 1))
else
    echo "FAIL: expected one -cpu, got $cpu_count"
    echo "  output: $DRYRUN_CPU"
    FAIL=$((FAIL + 1))
fi

# aa64: override the cortex-a57 default (e.g. MIDR replay via -cpu max).
DRYRUN_CPU_AA64=$(QEMU_DRYRUN=1 "$RUN_QEMU" --arch AARCH64 \
                                            --cpu "max,midr=0x410fd0b0" \
                                            "$DUMMY" 2>&1 || true)
if grep -qE "^QEMU_DRYRUN: max,midr=0x410fd0b0$" <<< "$DRYRUN_CPU_AA64" \
   && ! grep -qE "^QEMU_DRYRUN: cortex-a57$" <<< "$DRYRUN_CPU_AA64"; then
    echo "PASS: --cpu overrides the aa64 default model"
    PASS=$((PASS + 1))
else
    echo "FAIL: --cpu did not override the aa64 model"
    echo "  output: $DRYRUN_CPU_AA64"
    FAIL=$((FAIL + 1))
fi

# --- --monitor-socket / --qmp-socket --------------------------------------
# A caller-owned monitor for a --background guest it drives for its whole
# life (screendump on demand). Only --background hands that life over.
check "--help advertises --monitor-socket" 0 "[-]-monitor-socket PATH" \
    "$RUN_QEMU" --help
check "--help advertises --qmp-socket" 0 "[-]-qmp-socket PATH" \
    "$RUN_QEMU" --help
check "--monitor-socket without --background rejected" 1 \
    "need --background" \
    "$RUN_QEMU" --monitor-socket /nonexistent/mon.sock "$DUMMY"
check "--qmp-socket without --background rejected" 1 \
    "need --background" \
    "$RUN_QEMU" --qmp-socket /nonexistent/qmp.sock "$DUMMY"
DRYRUN_MON=$(QEMU_DRYRUN=1 "$RUN_QEMU" --background \
                                       --monitor-socket /x/mon.sock \
                                       --qmp-socket /x/qmp.sock \
                                       "$DUMMY" 2>&1 || true)
# Exactly the two tokens each, adjacent and in order.
if grep -A1 -xF "QEMU_DRYRUN: -monitor" <<< "$DRYRUN_MON" \
       | grep -qxF "QEMU_DRYRUN: unix:/x/mon.sock,server,nowait" \
   && grep -A1 -xF "QEMU_DRYRUN: -qmp" <<< "$DRYRUN_MON" \
       | grep -qxF "QEMU_DRYRUN: unix:/x/qmp.sock,server,nowait"; then
    echo "PASS: --monitor-socket/--qmp-socket reach CMD as unix sockets"
    PASS=$((PASS + 1))
else
    echo "FAIL: --monitor-socket/--qmp-socket not in CMD"
    echo "  output: $DRYRUN_MON"
    FAIL=$((FAIL + 1))
fi

# --- UNIX socket path length ------------------------------------------------
# sun_path is 108 bytes INCLUDING the NUL, so 107 is the longest usable path.
# Over it, QEMU refuses the socket with one line in its own log, and a
# screenshot suite reports every case failed with zero PNGs -- which reads as
# a render regression. run-qemu.sh must refuse first, before it makes any
# scratch dir, naming the setting to change.
# -p /tmp, not $TMPDIR: an inherited long TMPDIR would eat the very bytes
# these cases count.
SOCK_ROOT=$(mktemp -d -p /tmp)
trap 'rm -f "$DUMMY"; rm -rf "$SOCK_ROOT"' EXIT

# A directory whose path is exactly LEN bytes.
sock_base() {
    local len="$1" d="$SOCK_ROOT/"
    while (( ${#d} < len )); do d+="p"; done
    mkdir -p "$d"
    printf '%s' "$d"
}

# A file path of exactly LEN bytes (the socket itself, which need not exist).
sock_path() {
    local len="$1" p="$SOCK_ROOT/s"
    while (( ${#p} < len - 5 )); do p+="p"; done
    printf '%s' "$p.sock"
}

# check_exact NAME RC EXPECTED CMD... -- exit status AND the whole combined
# output, exactly. A substring would pass on a message that lost its numbers.
check_exact() {
    local name="$1" expected_rc="$2" expected="$3" out rc=0
    shift 3
    out=$("$@" 2>&1) || rc=$?
    if [[ "$rc" == "$expected_rc" && "$out" == "$expected" ]]; then
        echo "PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name -- rc=$rc (want $expected_rc)"
        echo "  got:  $out"
        echo "  want: $expected"
        FAIL=$((FAIL + 1))
    fi
}

# <base>/axl-qemu.XXXXXXXX/monitor.sock is base + 31 bytes.
B108=$(sock_base 77)
check_exact "--screenshot refuses a 108-byte monitor.sock path" 1 \
"ERROR: AXL_QEMU_TMPDIR is too long for a UNIX socket path: $B108/axl-qemu.XXXXXXXX/monitor.sock is 108 bytes; the limit is 107 (sun_path holds 108 including the NUL).
       Shorten AXL_QEMU_TMPDIR by at least 1 byte(s)." \
    env AXL_QEMU_TMPDIR="$B108" QEMU_DRYRUN=1 \
        "$RUN_QEMU" --screenshot "$SOCK_ROOT/x.png" "$DUMMY"
# Refused BEFORE mktemp: nothing may be left behind.
if [[ -z "$(find "$B108" -mindepth 1 -print -quit)" ]]; then
    echo "PASS: a refused run leaves no scratch dir"
    PASS=$((PASS + 1))
else
    echo "FAIL: a refused run left: $(find "$B108" -mindepth 1)"
    FAIL=$((FAIL + 1))
fi

# The boundary: 107 bytes leaves exactly room for the NUL, and runs.
B107=$(sock_base 76)
OUT107=$(AXL_QEMU_TMPDIR="$B107" QEMU_DRYRUN=1 \
         "$RUN_QEMU" --screenshot "$SOCK_ROOT/x.png" "$DUMMY" 2>&1) && rc=0 || rc=$?
if [[ "$rc" == 0 ]] && grep -q "^QEMU_DRYRUN: " <<< "$OUT107" \
   && ! grep -q "too long" <<< "$OUT107"; then
    echo "PASS: --screenshot runs with a 107-byte monitor.sock path"
    PASS=$((PASS + 1))
else
    echo "FAIL: --screenshot at the 107-byte boundary (rc=$rc)"
    echo "  output: $OUT107"
    FAIL=$((FAIL + 1))
fi

# Only sockets the run USES count: without --screenshot/--mount this run makes
# none, so the same over-long base is fine.
OUTNS=$(AXL_QEMU_TMPDIR="$B108" QEMU_DRYRUN=1 "$RUN_QEMU" "$DUMMY" 2>&1) \
    && rc=0 || rc=$?
if [[ "$rc" == 0 ]] && ! grep -q "too long" <<< "$OUTNS"; then
    echo "PASS: a run with no socket is not refused over AXL_QEMU_TMPDIR"
    PASS=$((PASS + 1))
else
    echo "FAIL: a socket-less run was refused (rc=$rc)"
    echo "  output: $OUTNS"
    FAIL=$((FAIL + 1))
fi

# The caller's own sockets, named by the option that set them.
for opt in --monitor-socket --qmp-socket --serial-socket; do
    P108=$(sock_path 108)
    check_exact "$opt refuses a 108-byte path" 1 \
"ERROR: $opt is too long for a UNIX socket path: $P108 is 108 bytes; the limit is 107 (sun_path holds 108 including the NUL).
       Shorten $opt by at least 1 byte(s)." \
        env QEMU_DRYRUN=1 "$RUN_QEMU" --background "$opt" "$P108" "$DUMMY"
    P107=$(sock_path 107)
    OUTP=$(QEMU_DRYRUN=1 "$RUN_QEMU" --background "$opt" "$P107" "$DUMMY" 2>&1) \
        && rc=0 || rc=$?
    if [[ "$rc" == 0 ]] && ! grep -q "too long" <<< "$OUTP"; then
        echo "PASS: $opt accepts a 107-byte path"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $opt at the 107-byte boundary (rc=$rc)"
        echo "  output: $OUTP"
        FAIL=$((FAIL + 1))
    fi
done

check "--help documents the socket path limit under AXL_QEMU_TMPDIR" 0 \
    "are capped at 107 bytes \\(sun_path is 108 with the" \
    "$RUN_QEMU" --help
# The figure the help gives a user is DERIVED: the limit, less run-qemu's
# deepest per-run path (/axl-qemu.XXXXXXXX/virtiofs.sock, 32 bytes). Computed
# here, so a renamed socket or template fails this rather than leaving the
# help quietly wrong.
_rq_tail="/axl-qemu.XXXXXXXX/virtiofs.sock"
check "--help gives the longest AXL_QEMU_TMPDIR that fits ($(( 107 - ${#_rq_tail} )) bytes)" 0 \
    "keep this base to $(( 107 - ${#_rq_tail} )) bytes or less" \
    "$RUN_QEMU" --help

# mktemp drops EVERY trailing slash, so they cost nothing: the 107-byte base
# written with two of them still fits.
OUTSL=$(AXL_QEMU_TMPDIR="$B107//" QEMU_DRYRUN=1 \
        "$RUN_QEMU" --screenshot "$SOCK_ROOT/x.png" "$DUMMY" 2>&1) && rc=0 || rc=$?
if [[ "$rc" == 0 ]] && ! grep -q "too long" <<< "$OUTSL"; then
    echo "PASS: trailing slashes on AXL_QEMU_TMPDIR are not counted"
    PASS=$((PASS + 1))
else
    echo "FAIL: trailing slashes counted against the limit (rc=$rc)"
    echo "  output: $OUTSL"
    FAIL=$((FAIL + 1))
fi

# The integration side: common-test.sh's test_setup makes its scratch one
# level deeper (<base>/axl-ctest.XXXXXXXX/<socket name>) and reserves
# AXL_TEST_SOCK_NAME_MAX (32) bytes for the name: base + 52 bytes.
C108=$(sock_base 56)
check_exact "test_setup refuses an AXL_QEMU_TMPDIR with no room for a socket" 1 \
"ERROR: AXL_QEMU_TMPDIR is too long for a UNIX socket path: $C108/axl-ctest.XXXXXXXX/<32-byte socket name> is 108 bytes; the limit is 107 (sun_path holds 108 including the NUL).
       Shorten AXL_QEMU_TMPDIR by at least 1 byte(s)." \
    env AXL_QEMU_TMPDIR="$C108" TEST_ARCH=X64 \
        bash -c "source '$SCRIPT_DIR/common-test.sh'; test_setup"
if [[ -z "$(find "$C108" -mindepth 1 -print -quit)" ]]; then
    echo "PASS: a refused test_setup leaves no scratch dir"
    PASS=$((PASS + 1))
else
    echo "FAIL: a refused test_setup left: $(find "$C108" -mindepth 1)"
    FAIL=$((FAIL + 1))
fi
# With no /dev/shm, test_setup falls back to a plain `mktemp -d` under $TMPDIR
# (tmp.XXXXXXXXXX), and names THAT knob. The branch itself needs an unusable
# /dev/shm to reach, so the check it calls is pinned here directly.
T108=$(sock_base 60)
check_exact "the TMPDIR fallback names TMPDIR" 1 \
"ERROR: TMPDIR is too long for a UNIX socket path: $T108/tmp.XXXXXXXXXX/<32-byte socket name> is 108 bytes; the limit is 107 (sun_path holds 108 including the NUL).
       Shorten TMPDIR by at least 1 byte(s)." \
    bash -c "source '$PROJECT_DIR/scripts/axl-common.sh'; axl_test_tmpdir_check '$T108' TMPDIR tmp.XXXXXXXXXX"
# ...and the runner refuses ONCE, up front, rather than failing every test.
# --no-pool: the runner pools by default, and its "pool:" line is not what
# this check is about (nor should it depend on this host's service).
check_exact "run-integration.sh refuses the same base before running anything" 1 \
"ERROR: AXL_QEMU_TMPDIR is too long for a UNIX socket path: $C108/axl-ctest.XXXXXXXX/<32-byte socket name> is 108 bytes; the limit is 107 (sun_path holds 108 including the NUL).
       Shorten AXL_QEMU_TMPDIR by at least 1 byte(s)." \
    env AXL_QEMU_TMPDIR="$C108" AXL_FLAGS_TEST_NESTED=1 \
        "$SCRIPT_DIR/run-integration.sh" --no-build --no-pool --only=test-run-qemu-flags.sh

# The reservation must cover what the tests actually create. Literal names
# only: a name built at runtime (test-ssh-hostile's "$2.sock", 26 bytes today;
# lib/sshd-tmux-lib.sh's "tmux-$name.sock", short names today) is not seen
# here, which is why the reserve keeps headroom over the longest.
# shellcheck source=../../scripts/axl-common.sh
source "$PROJECT_DIR/scripts/axl-common.sh"
# longest_sock_name FILE... -- the longest literal "/<name>.sock" in FILEs.
longest_sock_name() {
    local n longest=""
    while IFS= read -r n; do
        n="${n#/}"
        (( ${#n} > ${#longest} )) && longest="$n"
    done < <(grep -ohE '/[A-Za-z0-9._-]+\.sock\b' "$@" | sort -u)
    printf '%s' "$longest"
}
# The control: the scan must flag a name it is shown, or its silence below
# means nothing.
printf 'S="$TEST_TMPDIR/%s.sock"\n' "$(printf 'n%.0s' {1..40})" \
    > "$SOCK_ROOT/control.sh"
CTL=$(longest_sock_name "$SOCK_ROOT/control.sh")
LONGEST=$(longest_sock_name "$SCRIPT_DIR"/*.sh "$SCRIPT_DIR"/lib/*.sh)
if (( ${#CTL} == 45 )) && [[ -n "$LONGEST" ]] \
   && (( ${#LONGEST} <= AXL_TEST_SOCK_NAME_MAX )); then
    echo "PASS: every literal socket name fits the reserve (longest '$LONGEST', ${#LONGEST} <= $AXL_TEST_SOCK_NAME_MAX; the scan sees a 45-byte control)"
    PASS=$((PASS + 1))
else
    echo "FAIL: socket-name reserve -- control seen as '$CTL' (want 45 bytes), longest '$LONGEST' (${#LONGEST} bytes) against AXL_TEST_SOCK_NAME_MAX=$AXL_TEST_SOCK_NAME_MAX"
    FAIL=$((FAIL + 1))
fi

# --qcow2: the OVMF vars pflash AND the boot disk on qcow2, so a QEMU
# snapshot captures (and a restore reverts) the UEFI variables and the files
# a test wrote -- and so -loadvm can start a new QEMU from the snapshot,
# which refuses while any writable disk cannot hold one. The guest pool's
# requirement (docs/AXL-Test-Guest-Pool-Design.md §6.3). Both lines are
# matched WHOLE, and the raw forms must be gone, not merely joined.
check "--help advertises --qcow2" 0 "[-]-qcow2 " "$RUN_QEMU" --help
VQ=$(QEMU_DRYRUN=1 "$RUN_QEMU" --qcow2 "$DUMMY" 2>&1) || true
VR=$(QEMU_DRYRUN=1 "$RUN_QEMU" "$DUMMY" 2>&1) || true
vq_vars=$(grep -cE '^QEMU_DRYRUN: if=pflash,format=qcow2,file=/[^ ]*/vars\.qcow2$' <<< "$VQ" || true)
vq_disk=$(grep -cE '^QEMU_DRYRUN: format=qcow2,file=/[^ ]*/disk\.qcow2$' <<< "$VQ" || true)
vq_raw=$(grep -cE '^QEMU_DRYRUN: (if=pflash,)?format=raw,file=[^ ]*(vars|disk)' <<< "$VQ" || true)
vr_raw=$(grep -cE '^QEMU_DRYRUN: (if=pflash,format=raw,file=/[^ ]*/vars\.fd|format=raw,file=/[^ ]*/disk\.img)$' <<< "$VR" || true)
if [[ "$vq_vars" == 1 && "$vq_disk" == 1 && "$vq_raw" == 0 && "$vr_raw" == 2 ]]; then
    echo "PASS: --qcow2 puts the vars pflash and the boot disk on qcow2 (and only with it)"
    PASS=$((PASS + 1))
else
    echo "FAIL: --qcow2 -- qcow2 vars $vq_vars, qcow2 disk $vq_disk (want 1 each), raw vars/disk with it $vq_raw (want 0), raw without it $vr_raw (want 2)"
    grep -E 'pflash|disk' <<< "$VQ" | sed 's/^/  /'
    FAIL=$((FAIL + 1))
fi

# --state-dir DIR (with --qcow2): the qcow2 vars and disk live in DIR and
# OUTLIVE the run -- a pooled class restarts a new QEMU from them. Without
# --qcow2 it is refused (a raw image cannot hold the snapshot it is for).
SD="$(mktemp -d)"
check "--help advertises --state-dir" 0 "[-]-state-dir DIR" "$RUN_QEMU" --help
check_exact "--state-dir without --qcow2 is refused" 1 \
"ERROR: --state-dir needs --qcow2: the state it keeps is the qcow2 vars and disk a snapshot lives in" \
    env QEMU_DRYRUN=1 "$RUN_QEMU" --state-dir "$SD" "$DUMMY"
SQ=$(QEMU_DRYRUN=1 "$RUN_QEMU" --qcow2 --state-dir "$SD" "$DUMMY" 2>&1) || true
sd_vars=$(grep -cxF "QEMU_DRYRUN: if=pflash,format=qcow2,file=$SD/vars.qcow2" <<< "$SQ" || true)
sd_disk=$(grep -cxF "QEMU_DRYRUN: format=qcow2,file=$SD/disk.qcow2" <<< "$SQ" || true)
sd_kept=0
for f in vars disk; do
    qemu-img info --output=json "$SD/$f.qcow2" 2>/dev/null | grep -q '"format": "qcow2"' && sd_kept=$((sd_kept + 1))
done
if [[ "$sd_vars" == 1 && "$sd_disk" == 1 && "$sd_kept" == 2 ]]; then
    echo "PASS: --state-dir puts both qcow2 images in DIR, and they outlive the run"
    PASS=$((PASS + 1))
else
    echo "FAIL: --state-dir -- vars line $sd_vars, disk line $sd_disk (want 1 each), qcow2 images left in DIR $sd_kept (want 2)"
    FAIL=$((FAIL + 1))
fi
rm -rf "$SD"

# A dry run starts NO process: a stub virtiofsd first on PATH (run-qemu.sh
# looks there first) records any invocation. The guest pool keys its
# classes from dry runs with --mount, and each used to leave a daemon.
DR="$(mktemp -d)"
mkdir -p "$DR/bin" "$DR/share"
printf '#!/bin/bash\necho "$@" >> "%s/invoked"\nsleep 30\n' "$DR" > "$DR/bin/virtiofsd"
chmod +x "$DR/bin/virtiofsd"
PATH="$DR/bin:$PATH" QEMU_DRYRUN=1 "$RUN_QEMU" --mount "$DR/share" "$DUMMY" > "$DR/out" 2>&1 || true
if [[ ! -e "$DR/invoked" ]] && grep -q '^QEMU_DRYRUN: vhost-user-fs-pci' "$DR/out"; then
    echo "PASS: a dry run with --mount starts no virtiofsd (and still shows the device)"
    PASS=$((PASS + 1))
else
    echo "FAIL: a dry run with --mount -- virtiofsd invoked: $(cat "$DR/invoked" 2>/dev/null || echo no); device shown: $(grep -c '^QEMU_DRYRUN: vhost-user-fs-pci' "$DR/out")"
    FAIL=$((FAIL + 1))
fi

# The dry run names its temp dir, the staged disk's content and the inputs
# outside the argv -- what the guest pool's class key needs to be exact.
D1=$(QEMU_DRYRUN=1 "$RUN_QEMU" --qcow2 "$DUMMY" 2>&1) || true
tmpd=$(sed -n 's/^AXL_DRYRUN_TMPDIR=//p' <<< "$D1")
if [[ -n "$tmpd" ]] && grep -qxF "QEMU_DRYRUN: if=pflash,format=qcow2,file=$tmpd/vars.qcow2" <<< "$D1"; then
    echo "PASS: AXL_DRYRUN_TMPDIR names the run's temp dir, the one the argv uses"
    PASS=$((PASS + 1))
else
    echo "FAIL: AXL_DRYRUN_TMPDIR -- got '$tmpd', not the vars image's directory"
    FAIL=$((FAIL + 1))
fi
cp "$DUMMY" "$DR/extra.bin"
st1=$(QEMU_DRYRUN=1 "$RUN_QEMU" --extra "$DR/extra.bin" "$DUMMY" 2>&1 | sed -n 's/^AXL_DRYRUN_STAGED=//p')
st2=$(QEMU_DRYRUN=1 "$RUN_QEMU" --extra "$DR/extra.bin" "$DUMMY" 2>&1 | sed -n 's/^AXL_DRYRUN_STAGED=//p')
printf 'x' >> "$DR/extra.bin"
st3=$(QEMU_DRYRUN=1 "$RUN_QEMU" --extra "$DR/extra.bin" "$DUMMY" 2>&1 | sed -n 's/^AXL_DRYRUN_STAGED=//p')
if [[ "$st1" =~ ^[0-9a-f]{64}$ && "$st1" == "$st2" && "$st3" =~ ^[0-9a-f]{64}$ && "$st3" != "$st1" ]]; then
    echo "PASS: AXL_DRYRUN_STAGED is the staged disk's content: stable, and a changed file changes it"
    PASS=$((PASS + 1))
else
    echo "FAIL: AXL_DRYRUN_STAGED -- '$st1' / '$st2' (want equal digests) then '$st3' (want different)"
    FAIL=$((FAIL + 1))
fi
# What a booted run records as the skip cache's inputs (AXL_RQ_CACHE_INPUT,
# one line per file; printed from the same list the launch records). It once
# recorded the image and firmware only, while the pooled path recorded --extra
# and --nsh too: a booted test such as test-lsusb-qemu.sh, whose ids file
# reaches the guest by --extra, read FRESH after an edit to that file.
printf 'echo hi\r\n' > "$DR/custom.nsh"
CI=$(QEMU_DRYRUN=1 "$RUN_QEMU" --extra "$DR/extra.bin:ids.json5" --nsh "$DR/custom.nsh" "$DUMMY" 2>&1 \
    | sed -n 's/^AXL_RQ_CACHE_INPUT=//p' | LC_ALL=C sort)
want=$(printf '%s\n' "$DUMMY" "$DR/extra.bin" "$DR/custom.nsh" | LC_ALL=C sort)
if [[ "$(grep -xF -e "$DUMMY" -e "$DR/extra.bin" -e "$DR/custom.nsh" <<< "$CI")" == "$want" ]] \
        && ! grep -qF ':ids.json5' <<< "$CI"; then
    echo "PASS: a booted run's cache inputs include its --extra files (source path, not :dest) and its --nsh"
    PASS=$((PASS + 1))
else
    echo "FAIL: AXL_RQ_CACHE_INPUT -- got [$(tr '\n' ' ' <<< "$CI")], want it to include [$(tr '\n' ' ' <<< "$want")]"
    FAIL=$((FAIL + 1))
fi
fwcode=$(sed -n 's/^QEMU_DRYRUN: if=pflash,format=raw,readonly=on,file=//p' <<< "$D1")
cp "$fwcode" "$DR/fw_CODE.fd"
cp "$(dirname "$fwcode")/edk2-i386-vars.fd" "$DR/fw_VARS.fd"
D2=$(OVMF_CODE="$DR/fw_CODE.fd" QEMU_DRYRUN=1 "$RUN_QEMU" "$DUMMY" 2>&1) || true
if [[ "$(grep '^AXL_DRYRUN_INPUT=' <<< "$D2")" == "AXL_DRYRUN_INPUT=$DR/fw_VARS.fd" ]]; then
    echo "PASS: AXL_DRYRUN_INPUT names the vars template the run copies"
    PASS=$((PASS + 1))
else
    echo "FAIL: AXL_DRYRUN_INPUT -- got '$(grep '^AXL_DRYRUN_INPUT=' <<< "$D2")', want AXL_DRYRUN_INPUT=$DR/fw_VARS.fd"
    FAIL=$((FAIL + 1))
fi

# --mem SIZE: the guest's RAM, for a pool class whose tests need more than the
# 512M default (the unit class: its FAT RAM disk is taken from guest RAM).
# Both QEMU's -m and the virtio-fs memory backend (which must match it) follow.
MM=$(QEMU_DRYRUN=1 "$RUN_QEMU" --mem 768M --mount "$(dirname "$DUMMY")" "$DUMMY" 2>&1) || true
MD=$(QEMU_DRYRUN=1 "$RUN_QEMU" --mount "$(dirname "$DUMMY")" "$DUMMY" 2>&1) || true
if grep -A1 -xF 'QEMU_DRYRUN: -m' <<< "$MM" | grep -qxF 'QEMU_DRYRUN: 768M' \
   && grep -q 'memory-backend-file,id=axlmem,size=768M,' <<< "$MM" \
   && grep -A1 -xF 'QEMU_DRYRUN: -m' <<< "$MD" | grep -qxF 'QEMU_DRYRUN: 512M'; then
    echo "PASS: --mem 768M sets -m and the memory backend's size (default 512M unchanged)"
    PASS=$((PASS + 1))
else
    echo "FAIL: --mem -- with: $(grep -A1 -- '-m$' <<< "$MM" | tail -1) $(grep -o 'axlmem,size=[^,]*' <<< "$MM"); without: $(grep -A1 -- '-m$' <<< "$MD" | tail -1)"
    FAIL=$((FAIL + 1))
fi
MB=$(QEMU_DRYRUN=1 "$RUN_QEMU" --mem lots "$DUMMY" 2>&1) && MBR=0 || MBR=$?
if [[ $MBR -ne 0 ]] && grep -q 'mem.*lots' <<< "$MB"; then
    echo "PASS: --mem refuses a size that is not <N>M or <N>G"
    PASS=$((PASS + 1))
else
    echo "FAIL: --mem lots -- rc=$MBR $(head -1 <<< "$MB")"
    FAIL=$((FAIL + 1))
fi
# --guestfwd SPEC: a guest-initiated TCP connection to an address on slirp's
# network, forwarded -- with `cmd:` to a program spawned per connection on
# its stdin/stdout. What a pool class's fixed arguments use for the unit
# suite's echo and sink peers: no host port, no server to manage. Appended
# to the netdev in order, after the hostfwds; implies --net.
GF=$(QEMU_DRYRUN=1 "$RUN_QEMU" --guestfwd 'tcp:10.0.2.100:9999-cmd:cat' "$DUMMY" 2>&1) || true
GF2=$(QEMU_DRYRUN=1 "$RUN_QEMU" --net --hostfwd 18999:8080 --guestfwd 'tcp:10.0.2.100:9999-cmd:cat' \
      --guestfwd 'tcp:10.0.2.100:9997-cmd:sh -c "cat > /dev/null"' "$DUMMY" 2>&1) || true
if grep -qxF 'QEMU_DRYRUN: user,id=net0,guestfwd=tcp:10.0.2.100:9999-cmd:cat' <<< "$GF" \
   && grep -qxF 'QEMU_DRYRUN: user,id=net0,hostfwd=tcp::18999-:8080,guestfwd=tcp:10.0.2.100:9999-cmd:cat,guestfwd=tcp:10.0.2.100:9997-cmd:sh -c "cat > /dev/null"' <<< "$GF2"; then
    echo "PASS: --guestfwd appends each forward to the netdev, in order, after the hostfwds (implies --net)"
    PASS=$((PASS + 1))
else
    echo "FAIL: --guestfwd -- alone: $(grep -i 'net0\|guestfwd' <<< "$GF"); with others: $(grep -i 'net0\|guestfwd' <<< "$GF2")"
    FAIL=$((FAIL + 1))
fi
# A comma would end the guestfwd option and start another netdev option --
# QEMU would read the rest as garbage or, worse, as a different option.
GC=$(QEMU_DRYRUN=1 "$RUN_QEMU" --guestfwd 'tcp:10.0.2.100:9999-cmd:a,b' "$DUMMY" 2>&1) && GCR=0 || GCR=$?
GT=$(QEMU_DRYRUN=1 "$RUN_QEMU" --tap tap-nosuch --guestfwd 'tcp:10.0.2.100:9999-cmd:cat' "$DUMMY" 2>&1) && GTR=0 || GTR=$?
if [[ $GCR -ne 0 ]] && grep -q 'guestfwd.*comma' <<< "$GC" && [[ $GTR -ne 0 ]] && grep -q 'guestfwd.*--tap' <<< "$GT"; then
    echo "PASS: --guestfwd refuses a comma in its spec, and refuses --tap (no slirp to forward from)"
    PASS=$((PASS + 1))
else
    echo "FAIL: --guestfwd refusals -- comma: rc=$GCR $(head -1 <<< "$GC"); tap: rc=$GTR $(head -1 <<< "$GT")"
    FAIL=$((FAIL + 1))
fi
# --hostfwd-bind ADDR: the forwarded ports listen on ADDR only (the guest
# pool's agent runs whatever it is sent, so it is loopback); unchanged
# (every interface) without it.
HB=$(QEMU_DRYRUN=1 "$RUN_QEMU" --net --hostfwd 18999:8080 --hostfwd-bind 127.0.0.1 "$DUMMY" 2>&1) || true
HD=$(QEMU_DRYRUN=1 "$RUN_QEMU" --net --hostfwd 18999:8080 "$DUMMY" 2>&1) || true
if grep -qxF 'QEMU_DRYRUN: user,id=net0,hostfwd=tcp:127.0.0.1:18999-:8080' <<< "$HB" \
   && grep -qxF 'QEMU_DRYRUN: user,id=net0,hostfwd=tcp::18999-:8080' <<< "$HD"; then
    echo "PASS: --hostfwd-bind 127.0.0.1 binds the forward to loopback (default unchanged)"
    PASS=$((PASS + 1))
else
    echo "FAIL: --hostfwd-bind -- with: $(grep hostfwd <<< "$HB"); without: $(grep hostfwd <<< "$HD")"
    FAIL=$((FAIL + 1))
fi
check_exact "--hostfwd-bind refuses a non-IPv4 address" 1 \
"ERROR: --hostfwd-bind needs an IPv4 address, got 'localhost'" \
    env QEMU_DRYRUN=1 "$RUN_QEMU" --net --hostfwd 18999:8080 --hostfwd-bind localhost "$DUMMY"
rm -rf "$DR"

# --- a run that ends before its guest leaves NOTHING behind -----------------
# The temp dir is made early; a --background run handed it to the detached
# owner only at the fork, and armed no cleanup before then. So every exit in
# between leaked it: the QEMU_DRYRUN exit (the guest pool dry-runs every boot
# and every class key with --background -- seven leaked dirs in one suite
# run), and any validation failure after the mktemp. With --mount, the
# virtiofsd started before such a failure leaked too.
LB=$(mktemp -d /tmp/rqlk.XXXXXX)
# Also on an abort mid-block: the base, the share, and any virtiofsd a
# refused run leaked before its lb_check could end it.
trap 'rm -f "$DUMMY"; rm -rf "$SOCK_ROOT"; v=$(lb_virtiofsd); [[ -z "$v" ]] || kill $v 2>/dev/null; rm -rf "$LB" "$LB.share"' EXIT
leftovers() { find "$LB" -mindepth 1 -maxdepth 1 -print; }
lb_virtiofsd() {   # virtiofsd processes whose argv names this base
    local p
    for p in /proc/[0-9]*; do
        [[ "$(cat "$p/comm" 2>/dev/null)" == virtiofsd ]] || continue
        # A test, not `tr | grep -q`: under pipefail a grep that exits on
        # its match can leave tr with SIGPIPE, reading a hit as a miss.
        [[ "$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)" == *"$LB/"* ]] && echo "${p#/proc/}"
    done
    return 0
}
lb_check() {   # NAME: the run just made left no dir and no virtiofsd under $LB
    local left v
    left=$(leftovers); v=$(lb_virtiofsd)
    if [[ -z "$left" && -z "$v" ]]; then
        echo "PASS: $1"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $1 -- left: ${left:-no dir}; virtiofsd: ${v:-none}"
        FAIL=$((FAIL + 1))
        if [[ -n "$v" ]]; then kill $v 2>/dev/null || true; fi
        rm -rf "${LB:?}"/*
    fi
}
# Control: the check SEES a leftover (and its cleanup empties the base).
mkdir "$LB/axl-qemu.control"
if [[ "$(leftovers)" == "$LB/axl-qemu.control" ]]; then
    echo "PASS: leak check sees a leftover dir (control)"
    PASS=$((PASS + 1))
else
    echo "FAIL: leak check control -- saw '$(leftovers)'"
    FAIL=$((FAIL + 1))
fi
rm -rf "$LB/axl-qemu.control"

AXL_QEMU_TMPDIR="$LB" QEMU_DRYRUN=1 "$RUN_QEMU" "$DUMMY" >/dev/null 2>&1 || true
lb_check "a foreground dry run leaves no temp dir"
AXL_QEMU_TMPDIR="$LB" QEMU_DRYRUN=1 "$RUN_QEMU" --background "$DUMMY" >/dev/null 2>&1 || true
lb_check "a --background dry run leaves no temp dir"
check_exact "--background with an unwritable --serial-log is refused" 1 \
"ERROR: cannot write --serial-log '$LB/no/such/dir/s.log' (missing dir?)" \
    env AXL_QEMU_TMPDIR="$LB" "$RUN_QEMU" --background --serial-log "$LB/no/such/dir/s.log" "$DUMMY"
lb_check "a refused --background run leaves no temp dir"
if command -v virtiofsd >/dev/null 2>&1 || [[ -x /usr/libexec/virtiofsd ]]; then
    mkdir -p "$LB.share"
    check_exact "--background --mount with a bad --mac is refused" 1 \
"ERROR: --mac: 'bogus' is not a valid MAC address (expect XX:XX:XX:XX:XX:XX)" \
        env AXL_QEMU_TMPDIR="$LB" "$RUN_QEMU" --background --mount "$LB.share" \
            --net --mac bogus "$DUMMY"
    lb_check "a refused --background --mount run leaves no temp dir and no virtiofsd"
else
    echo "SKIP: --background --mount refusal (no virtiofsd)"
fi
rm -rf "$LB" "$LB.share"

echo
echo "----------------------------------------"
echo "  $PASS passed, $FAIL failed"
echo "----------------------------------------"

[[ "$FAIL" -eq 0 ]]
