#!/bin/bash
# test-meta: arch=x64 needs= est=95 local-only=1
# test-old-shell — pins the SDK's old EFI 1.x shell support (no
# EDK2 EFI_SHELL_PROTOCOL) against the real EFI Toolkit "newshell"
# (Shell106.efi), which QEMU's bundled OVMF shell can't reproduce.
#
# Two things this proves that the modern-shell suites cannot:
#   1. Shell-independent sibling-locate (Finding 1): a shared-driver
#      launcher resolves its version-pinned sibling driver from its own
#      LoadedImage device path, with no EFI_SHELL_PROTOCOL. The
#      sd-sibling "beside" fixture: probe + driver-A co-staged in app/;
#      `probe beside` must report SDSIB:sibling=OK and dispatch tag A.
#   2. mkrd on a shell with no programmatic map (Finding 2): `mkrd
#      SCRATCH` creates + connects the disk, prints a `map -r` advisory,
#      and exits 0 (NOT EFI_ABORTED); after `map -r` the disk enumerates
#      as an fsN and a write+read round-trip on it succeeds. A re-run says
#      "reused" and asks for `map -r` only while the disk is unmounted,
#      and `mkrd -d` names `map -r`, never `map -u` (this shell has none).
#
# The modern-shell regression halves of both findings are already
# covered by test-sd-sibling-qemu.sh and test-ramdisk-map-qemu.sh (both
# pass on X64 + AARCH64); this script is the old-shell-only complement.
#
# LOCAL-ONLY: needs Shell106.efi, an x86 EFI Toolkit binary not in this
# repo (nor redistributable here). Point AXL_OLD_SHELL_EFI at a copy, or
# stage it at the default corpus path below; absent it, the test SKIPs.
# Shell106 is x86 only, so this runs X64 exclusively.
#
# Ratchet-exempt (end-to-end scenario, not a unit binary's assertion count).
#
# Usage: AXL_OLD_SHELL_EFI=/path/to/Shell106.efi \
#            ./test/integration/test-old-shell-qemu.sh [--arch X64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64]"; exit 1 ;;
    esac
done

# Shell106 is an x86 binary — the old-shell path is only reachable on X64.
if [[ "$TEST_ARCH" != "X64" ]]; then
    echo "old-shell test: SKIP (Shell106 is x86 only; $TEST_ARCH not supported)"
    exit 0
fi

OLD_SHELL="${AXL_OLD_SHELL_EFI:-$HOME/work/dell/delldiags/local/legacy-corpus/EFI/Boot/Shell106.efi}"
if [[ ! -f "$OLD_SHELL" ]]; then
    echo "WARN: old EFI 1.x shell not found at '$OLD_SHELL'."
    echo "      Set AXL_OLD_SHELL_EFI to a Shell106.efi to run this test."
    echo "old-shell test: SKIP"
    exit 0
fi

RUN_QEMU="$PROJECT_DIR/scripts/run-qemu.sh"
NATIVE_DIR="$(test_build_dir)"

# Build the fixtures: the sibling-locate probe + driver, mkrd, the file-layer
# path-resolution selftest, and the resident-driver file-read + setenv fixture.
make -C "$PROJECT_DIR" ARCH=x64 ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} \
    sd-sibling tools fs-path-selftest fs-read 2>&1 | tail -2

PROBE="$NATIVE_DIR/sd-sibling-probe.efi"
DRIVER_A="$NATIVE_DIR/sd-sibling-driver-a.efi"
MKRD="$NATIVE_DIR/tools/mkrd.efi"
FSSELF="$NATIVE_DIR/fs-path-selftest.efi"
FSREAD_PROBE="$NATIVE_DIR/fs-read-probe.efi"
FSREAD_DRIVER="$NATIVE_DIR/fs-read-driver.efi"

if [[ ! -f "$PROBE" || ! -f "$DRIVER_A" || ! -f "$MKRD" || ! -f "$FSSELF" \
   || ! -f "$FSREAD_PROBE" || ! -f "$FSREAD_DRIVER" ]]; then
    echo "WARN: fixtures not built on this box; skipping."
    echo "old-shell test: SKIP"
    exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Fixture files the file-layer selftest resolves (fs0:\dof_in.txt,
# fs0:\sub\nested.txt — staged into place by --extra at boot).
printf 'HELLO\n'  > "$WORK/dof_in.txt"
printf 'NESTED\n' > "$WORK/nested.txt"

# Run run-qemu.sh against the old shell with a custom startup.nsh, capturing
# the serial log. $1 = nsh file, $2 = positional .efi, rest = extra args.
# NOTE: startup.nsh runs the old shell in "backward compatible mode", which
# behaves differently from an interactive prompt (see run_old_shell_interactive).
run_old_shell() {
    local nsh="$1" positional="$2"; shift 2
    timeout 150 "$RUN_QEMU" --arch X64 --shell "$OLD_SHELL" --timeout 60 \
        --nsh "$nsh" "$@" "$positional" 2>&1
}

DRIVE="$TESTS_DIR/drive-serial.py"

# Drive the old shell INTERACTIVELY over a serial socket. In --background mode
# run-qemu does NOT append `reset -s`, so the shell drops to the real
# interactive prompt = its FULL mode, where `map -r` generates the device-path
# aliases that `map <name> fsN:` needs. This is the ONLY way to test the
# label-alias path — under a startup.nsh (backward-compatible mode) those
# aliases don't exist and the alias silently can't resolve. Leading
# `--extra SRC:DST` pairs stage additional files; then $1 = positional .efi,
# rest = commands to type. Echoes the ANSI-stripped transcript.
run_old_shell_interactive() {
    local extras=()
    while [[ "$1" == "--extra" ]]; do
        extras+=(--extra "$2"); shift 2
    done
    local positional="$1"; shift
    local sock="$WORK/serial.sock"
    local nsh="$WORK/interactive.nsh"
    rm -f "$sock"
    printf '@echo -off\necho AXL-INTERACTIVE-READY\n' > "$nsh"
    timeout 60 "$RUN_QEMU" --arch X64 --shell "$OLD_SHELL" --background \
        --serial-socket "$sock" --timeout 90 --nsh "$nsh" \
        "${extras[@]}" "$positional" \
        >/dev/null 2>&1
    # --ready "Shell>": wait for the interactive prompt before typing, so a
    # command can't land while the shell is still in startup.nsh (backward-
    # compat) mode — that race is what flaked the label-alias assertion.
    python3 "$DRIVE" --ready "Shell>" "$sock" "$@" 2>/dev/null \
        | sed 's/\x1b\[[0-9;]*[A-Za-z]//g'
    # Stop the background QEMU (matched by its serial-socket path). The `reset
    # -s` the caller sends usually exits it already; this is the backstop.
    local pid
    for pid in $(ps -eo pid,cmd | grep 'qemu-system' | grep -F "$sock" \
                 | grep -v grep | awk '{print $1}'); do
        kill "$pid" 2>/dev/null
    done
    rm -f "$sock"
}

# ---------------------------------------------------------------------------
# Scenario 1: shell-independent sibling-locate (Finding 1).
# ---------------------------------------------------------------------------
echo "=== old-shell sibling-locate (beside) ==="

cat > "$WORK/beside.nsh" <<'NSH'
fs0:
cd \
echo SIB_BEGIN
app\probe.efi beside
echo SIB_DONE_MARKER
reset -s
NSH

SIB_LOG=$(run_old_shell "$WORK/beside.nsh" "$PROBE" \
    --extra "$PROBE:app/probe.efi" \
    --extra "$DRIVER_A:app/sd-sibling-driver.efi")

echo "$SIB_LOG" | sed -n '/SIB_BEGIN/,/SIB_DONE_MARKER/p' | sed 's/^/  /'

if echo "$SIB_LOG" | grep -q '^SDSIB:sibling=OK$'; then
    test_host_pass "sibling-locate resolves with no EFI_SHELL_PROTOCOL"
else
    test_host_fail "sibling-locate resolves with no EFI_SHELL_PROTOCOL"
fi
if echo "$SIB_LOG" | grep -q '^SDSIB:tag=A$'; then
    test_host_pass "sibling driver A dispatched (tag=A)"
else
    test_host_fail "sibling driver A dispatched (tag=A)"
fi

# ---------------------------------------------------------------------------
# Scenario 2: mkrd exit-0 + create/connect + `map -r` usability (Finding 2).
# ---------------------------------------------------------------------------
echo "=== old-shell mkrd + map -r round-trip ==="

# fs0 is the boot volume. mkrd creates and connects the disk but does NOT map
# it: this shell has no programmatic SetMap, and a create has no business
# running the shell's `map -r`. That command probes every physical block device
# so any can report a media change (re-installing its BlockIo), and on THIS
# shell SHELL_ENVIRONMENT.Execute runs in-context, so a map rebuild lands on the
# real map list rather than a nested shell's throwaway one. mkrd names the
# manual step; the SCRIPT runs `map -r` when it is ready for it.
cat > "$WORK/mkrd.nsh" <<'NSH'
fs0:
echo MARK_CREATE
mkrd SCRATCH
echo MKRD_RC=[%lasterror%]
echo MARK_RERUN_UNMOUNTED
mkrd SCRATCH
echo MARK_AFTER_CREATE
map -r
echo MARK_AFTER_MAPR
mkrd -l
fs1:
echo axl-old-shell-proof > proof.txt
type proof.txt
fs0:
echo MARK_RERUN_MOUNTED
mkrd SCRATCH
echo MARK_DESTROY
mkrd -d SCRATCH
echo MARK_END
reset -s
NSH

MKRD_LOG=$(run_old_shell "$WORK/mkrd.nsh" "$MKRD")

echo "$MKRD_LOG" | grep -iE 'RAM disk created|MKRD_RC|map -r|VirtualDisk|proof.txt|axl-old-shell-proof' \
    | sed 's/^/  /'

if echo "$MKRD_LOG" | grep -q '^RAM disk created:$'; then
    test_host_pass "mkrd reports the disk created (no doomed SetMap)"
else
    test_host_fail "mkrd reports the disk created (no doomed SetMap)"
fi
# EFI_ABORTED shows as 8000000000000015; a clean create must leave 0.
if echo "$MKRD_LOG" | grep -q 'MKRD_RC=\[0\]'; then
    test_host_pass "mkrd exits 0 on a shell-less create (not EFI_ABORTED)"
else
    test_host_fail "mkrd exits 0 on a shell-less create (not EFI_ABORTED)"
fi
# The disk is usable once the SCRIPT has run `map -r` — create + connect worked,
# only the naming was left to the caller.
if echo "$MKRD_LOG" | grep -q '^axl-old-shell-proof$'; then
    test_host_pass "disk usable after the script's own 'map -r'"
else
    test_host_fail "disk usable after the script's own 'map -r'"
fi
# REGRESSION (this is what changed): mkrd must name the manual step and must NOT
# claim the disk is already mapped. mkrd used to drive the shell's `map -r`
# itself from inside create — in-context on this shell, so the map rebuild hit
# the real map list, and the probe let every physical block device report a
# media change. Both assertions fail against that old build.
CREATE_OUT=$(echo "$MKRD_LOG" | sed -n '/MARK_CREATE/,/MARK_RERUN_UNMOUNTED/p')
# The negative checks cover the re-run before `map -r` too.
CREATE_ALL=$(echo "$MKRD_LOG" | sed -n '/MARK_CREATE/,/MARK_AFTER_CREATE/p')
if echo "$CREATE_OUT" | grep -qiF "Run 'map -r' to mount the disk"; then
    test_host_pass "mkrd names the manual 'map -r' instead of running it"
else
    test_host_fail "mkrd names the manual 'map -r' instead of running it"
fi
if echo "$CREATE_ALL" | grep -qiE "no 'map -r' needed"; then
    test_host_fail "mkrd no longer claims 'no map -r needed' on this shell"
else
    test_host_pass "mkrd no longer claims 'no map -r needed' on this shell"
fi
# create must not run the shell's `map -r` at all, so its output can't contain
# the "Device mapping table" that this shell's in-context Execute prints. Scoped
# to the create region: the script's OWN `map -r` (after MARK_AFTER_CREATE)
# prints that table legitimately.
if echo "$CREATE_ALL" | grep -qiF 'Device mapping table'; then
    test_host_fail "create runs no 'map -r' (no 'Device mapping table' in its output)"
else
    test_host_pass "create runs no 'map -r' (no 'Device mapping table' in its output)"
fi
# mkrd -l reverse-looks-up the fsN via SHELL_ENVIRONMENT.GetMap (the disk is in
# the shell's map once the SCRIPT ran `map -r` above): lowercase 'fs1:' (the old
# shell's own casing), NOT '(unmapped)'. The modern shell shows uppercase 'FS<n>:', so the
# listing stays consistent with whichever shell is in use.
if echo "$MKRD_LOG" | grep -qE '^fs1:'; then
    test_host_pass "mkrd -l shows lowercase fs1: on the old shell (GetMap reverse-lookup)"
else
    test_host_fail "mkrd -l shows lowercase fs1: on the old shell (GetMap reverse-lookup)"
fi
# A re-run says whether it made the disk or found it, and asks for `map -r`
# only while the disk is not mounted. Before the script's `map -r` it still
# needs one; after it, GetMap's reverse lookup finds the fs1 the shell gave the
# disk, so mkrd reports it and asks for nothing -- another `map -r` would
# re-probe every block device for no reason.
RERUN_UNMOUNTED=$(echo "$MKRD_LOG" | sed -n '/MARK_RERUN_UNMOUNTED/,/MARK_AFTER_CREATE/p')
if echo "$RERUN_UNMOUNTED" | grep -q '^RAM disk reused:$' \
   && echo "$RERUN_UNMOUNTED" | grep -qiF "Run 'map -r' to mount the disk"; then
    test_host_pass "re-run before map -r: reused, still asks for map -r"
else
    test_host_fail "re-run before map -r: reused, still asks for map -r"
fi
RERUN_MOUNTED=$(echo "$MKRD_LOG" | sed -n '/MARK_RERUN_MOUNTED/,/MARK_DESTROY/p')
if echo "$RERUN_MOUNTED" | grep -q '^RAM disk reused:$' \
   && echo "$RERUN_MOUNTED" | grep -qF 'mapping : fs1:' \
   && ! echo "$RERUN_MOUNTED" | grep -qiF "Run 'map -r'"; then
    test_host_pass "re-run after map -r: reports fs1, asks for no map -r"
else
    test_host_fail "re-run after map -r: reports fs1, asks for no map -r"
fi
# This shell cannot remove a name, so -d leaves fs1 behind and says to run
# `map -r`. It has no `map -u` at all, so the advice must not mention one.
DESTROY_OUT=$(echo "$MKRD_LOG" | sed -n '/MARK_DESTROY/,/MARK_END/p')
if echo "$DESTROY_OUT" | grep -qF "run 'map -r' to clear it" \
   && ! echo "$DESTROY_OUT" | grep -qF "map -u"; then
    test_host_pass "-d on this shell names map -r and not map -u"
else
    test_host_fail "-d on this shell names map -r and not map -u"
fi

# ---------------------------------------------------------------------------
# Scenario 3: INTERACTIVE mode — the label alias. The old shell only generates
# the device-path aliases that `map <label> fsN:` needs at the interactive
# prompt, not under a startup.nsh, so this MUST be driven interactively (over
# the serial socket) or it would falsely report the alias as broken. This is
# the dual-mode guard: Scenario 2 pins backward-compatible mode, this pins
# interactive mode, so no old-shell behavior is validated in only one mode.
# ---------------------------------------------------------------------------
echo "=== old-shell mkrd label-alias (interactive) ==="

# mkrd no longer maps anything on this shell, so the alias is the USER's to make
# with the shell's own `map <label> fsN:` (which needs the device-path aliases
# only interactive mode generates). The round-trip writes through FOOBAR: and
# reads back through fs1: — it cannot pass by silently landing on fs0, and the
# assertion is anchored so the echoed keystrokes cannot satisfy it either.
#
# NOTE: this scenario was axl_volume_alias_to_fsn()'s only exercise in the repo.
# mkrd no longer calls it (the shell's own `map` command is typed here instead),
# so that published API is now untested — deliberately, not by oversight.
INT_LOG=$(run_old_shell_interactive "$MKRD" \
    "mkrd FOOBAR" "map -r" "map FOOBAR fs1:" "FOOBAR:" \
    "echo axl-alias-round-trip > p.txt" "type fs1:\\p.txt" "fs0:" \
    "mkrd -l" "mkrd -d FOOBAR" "mkrd -l" "reset -s")

echo "$INT_LOG" | grep -aiE "mapping :|alias FOOBAR|axl-alias-round-trip|No RAM disks|^fs1:|Run 'map -r'" \
    | sed 's/^/  /' | head

if echo "$INT_LOG" | grep -qiF "Run 'map -r' to mount the disk"; then
    test_host_pass "mkrd advises 'map -r' interactively too (never runs it)"
else
    test_host_fail "mkrd advises 'map -r' interactively too (never runs it)"
fi
INT_CREATE=$(echo "$INT_LOG" | sed -n '/mkrd FOOBAR/,/map -r/p')
if echo "$INT_CREATE" | grep -q 'alias FOOBAR:'; then
    test_host_fail "mkrd claims no label alias it did not set on this shell"
else
    test_host_pass "mkrd claims no label alias it did not set on this shell"
fi
# NO "a user's map name survives mkrd" PROBE HERE, and the measurement is why.
# The obvious probe is `map PRESRV fs0:` before `mkrd FOOBAR`, then ask for it
# back. Measured on this shell (Shell106, interactive), a CONTROL that asks for
# it back BEFORE mkrd runs at all already answers:
#
#     Shell> map PRESRV fs0:   ->  Device mapping table ... PRESRV :BlockDevice
#     Shell> map PRESRV:       ->  map: Cannot find required map name.
#
# So `map <name> fsN:` here leaves nothing a later `map <name>:` finds, and the
# probe never establishes a name for mkrd to lose: it measures the shell, not
# us. (That wording also differs from the modern shell's "Cannot find mapped
# device" — don't reuse that grep here.) A real detector on this shell would
# need another way to create a name that persists. The modern-shell side of the
# same question is contract-checked in test-ramdisk-map-qemu.sh.
# fs1:\p.txt is on the RAM disk: the write went through the user's FOOBAR: alias.
if echo "$INT_LOG" | grep -qE '^axl-alias-round-trip[[:space:]]*$'; then
    test_host_pass "user-made FOOBAR: alias round-trips (write FOOBAR:, read fs1:)"
else
    test_host_fail "user-made FOOBAR: alias round-trips (write FOOBAR:, read fs1:)"
fi
if echo "$INT_LOG" | grep -qE '^fs1:'; then
    test_host_pass "mkrd -l shows the disk's fsN after the user's map -r"
else
    test_host_fail "mkrd -l shows the disk's fsN after the user's map -r"
fi
# The user's `map FOOBAR fs1:` made the label an alias of this disk; -l shows
# it as one (with the colon) only when the name maps to this disk, which this
# shell answers through SHELL_ENVIRONMENT.GetMap.
if echo "$INT_LOG" | grep -qE '^fs1:[[:space:]]+FOOBAR:[[:space:]]'; then
    test_host_pass "mkrd -l shows the user-made FOOBAR: as this disk's alias"
else
    test_host_fail "mkrd -l shows the user-made FOOBAR: as this disk's alias"
fi
if echo "$INT_LOG" | grep -q 'No RAM disks found'; then
    test_host_pass "mkrd -d destroys the disk interactively (map cleaned)"
else
    test_host_fail "mkrd -d destroys the disk interactively (map cleaned)"
fi

# ---------------------------------------------------------------------------
# Scenario 4: file-layer path resolution (fsN:-qualified + cwd-relative). The
# old shell has no EFI_SHELL_PROTOCOL, so the backend resolves paths itself
# through SHELL_ENVIRONMENT (GetMap/CurDir) + EFI_FILE_PROTOCOL. The selftest
# runs the SAME battery the modern shell passes; here we assert every check
# passes (fail=0) plus a couple of critical relative-path lines by name.
#
# The selftest reports twice: a root-cwd battery (fs0:\) and a sub-cwd battery
# (fs0:\sub) that pins relative resolution to the current directory. Relative
# resolution rides on CurDir, whose value could in principle differ between
# backward-compatible (startup.nsh) and interactive mode — so this runs in
# BOTH modes (Scenario 4 = startup.nsh, Scenario 5 = interactive).
# ---------------------------------------------------------------------------
echo "=== old-shell file-layer path resolution (startup.nsh) ==="

cat > "$WORK/fspath.nsh" <<'NSH'
fs0:
cd \
echo FS_ROOT_BEGIN
fs-path-selftest.efi
cd \sub
fs0:\fs-path-selftest.efi sub
fs0:
echo FS_DONE_MARKER
reset -s
NSH

FS_LOG=$(run_old_shell "$WORK/fspath.nsh" "$FSSELF" \
    --extra "$WORK/dof_in.txt:dof_in.txt" \
    --extra "$WORK/nested.txt:sub/nested.txt")

echo "$FS_LOG" | grep -aE 'FSSELF:(results|info-rel|get-rel|set-contents=)' \
    | sed 's/^/  /'

# Two "results" footers: root battery then sub battery. Both must be clean.
if [[ $(echo "$FS_LOG" | grep -c 'FSSELF:results pass=29 fail=0') -eq 1 \
   && $(echo "$FS_LOG" | grep -c 'FSSELF:results pass=7 fail=0') -eq 1 ]]; then
    test_host_pass "file layer resolves all paths on the old shell (startup.nsh)"
else
    test_host_fail "file layer resolves all paths on the old shell (startup.nsh)"
fi
# Name the relative-resolution lines explicitly — they are the field symptom
# (Mark's `do -f<relative>` scripts) and the RW-probe (`set-contents`).
if echo "$FS_LOG" | grep -q '^FSSELF:info-rel=PASS'; then
    test_host_pass "cwd-relative stat resolves (startup.nsh)"
else
    test_host_fail "cwd-relative stat resolves (startup.nsh)"
fi
if echo "$FS_LOG" | grep -q '^FSSELF:set-contents=PASS'; then
    test_host_pass "writable-volume probe succeeds (startup.nsh; the RW: ro fix)"
else
    test_host_fail "writable-volume probe succeeds (startup.nsh; the RW: ro fix)"
fi

# ---------------------------------------------------------------------------
# Scenario 5: the CurDir-dependent (relative-path) half of the battery driven
# INTERACTIVELY, to prove relative resolution behaves the same in full mode as
# under a startup.nsh. Only the sub-cwd battery is mode-DEPENDENT (it rides on
# CurDir); the fsN:-qualified and root paths are mode-invariant and already
# pinned deterministically by Scenario 4. Running just the 7-check sub battery
# also keeps the interactive transcript short enough for drive-serial to
# capture in full (a 30-line burst can outrun its per-command drain window).
# ---------------------------------------------------------------------------
echo "=== old-shell file-layer path resolution (interactive) ==="

FS_INT_LOG=$(run_old_shell_interactive \
    --extra "$WORK/dof_in.txt:dof_in.txt" \
    --extra "$WORK/nested.txt:sub/nested.txt" \
    "$FSSELF" \
    "fs0:" "cd \\sub" "fs0:\\fs-path-selftest.efi sub" "fs0:" "reset -s")

echo "$FS_INT_LOG" | grep -aE 'FSSELF:(results|sub-cwdrel)' | sed 's/^/  /'

if echo "$FS_INT_LOG" | grep -q '^FSSELF:results pass=7 fail=0'; then
    test_host_pass "cwd-relative battery resolves interactively (full mode)"
else
    test_host_fail "cwd-relative battery resolves interactively (full mode)"
fi
if echo "$FS_INT_LOG" | grep -q '^FSSELF:sub-cwdrel-get=PASS'; then
    test_host_pass "cwd-relative read from a subdir cwd resolves (interactive)"
else
    test_host_fail "cwd-relative read from a subdir cwd resolves (interactive)"
fi

# ---------------------------------------------------------------------------
# Scenario 6: file READ + setenv from a RESIDENT DRIVER (no SHELL_INTERFACE on
# its LoadedImage). This is the `-f<file> var` consumer pattern (fopen ->
# text-wrap -> readline -> setenv) that a standalone selftest can't cover — the
# read runs in a driver context, and the setenv drives the old shell's `set`
# command through Execute (the old shell has no programmatic SetEnv). The
# launcher reads the same file standalone ("app") too, so read parity is
# checked in one run. `dof_in.txt` holds a spaced line to prove the quoted
# `set` carries the whole value.
# ---------------------------------------------------------------------------
echo "=== old-shell resident-driver file read + setenv ==="

printf 'HELLO_LINE one two\n' > "$WORK/dof_line.txt"

cat > "$WORK/fsread.nsh" <<'NSH'
fs0:
cd \
echo FSREAD_BEGIN
app\fs-read-probe.efi fs0:\dof_line.txt
echo FSREAD_SHELL_APP
set FSRVAR_app
echo FSREAD_SHELL_DRV
set FSRVAR_drv
echo FSREAD_DONE_MARKER
reset -s
NSH

FSREAD_LOG=$(run_old_shell "$WORK/fsread.nsh" "$FSREAD_PROBE" \
    --extra "$WORK/dof_line.txt:dof_line.txt" \
    --extra "$FSREAD_PROBE:app/fs-read-probe.efi" \
    --extra "$FSREAD_DRIVER:app/fs-read-driver.efi")

echo "$FSREAD_LOG" | grep -aE 'FSREAD:(app|drv)-(line|setenv|getenv)|FSRVAR_(app|drv)' \
    | sed 's/^/  /'

# 1. The driver read matches the standalone read (both the full spaced line).
if echo "$FSREAD_LOG" | grep -q '^FSREAD:drv-line=\[HELLO_LINE one two\]$'; then
    test_host_pass "resident driver reads the file stream (fopen+readline)"
else
    test_host_fail "resident driver reads the file stream (fopen+readline)"
fi
# 2. setenv from the driver succeeds (drives the shell's `set` via Execute with
#    a SHELL_INTERFACE-bearing parent handle — a driver's own handle is rejected).
if echo "$FSREAD_LOG" | grep -q '^FSREAD:drv-setenv=OK$'; then
    test_host_pass "resident driver setenv succeeds on the old shell"
else
    test_host_fail "resident driver setenv succeeds on the old shell"
fi
if echo "$FSREAD_LOG" | grep -q '^FSREAD:drv-getenv=\[HELLO_LINE one two\]$'; then
    test_host_pass "resident driver getenv reflects the value it just set"
else
    test_host_fail "resident driver getenv reflects the value it just set"
fi
# unset must DELETE the var (parity with the modern shell's empty-value SetEnv),
# driven via the old shell's `set -d`, not leave an empty var behind.
if echo "$FSREAD_LOG" | grep -q '^FSREAD:drv-unset=gone$'; then
    test_host_pass "resident driver unsetenv deletes the var (set -d)"
else
    test_host_fail "resident driver unsetenv deletes the var (set -d)"
fi
# Unsetting a var that is not set prints nothing (the old shell's `set -d`
# says "set: NAME not found") and succeeds, for the app and the driver alike.
for ctx in app drv; do
    if [[ "$(echo "$FSREAD_LOG" | tr -d '\r' | awk -v b="FSREAD:$ctx-unset-absent-begin" '
            $0 == b { getline; print; exit }')" == "FSREAD:$ctx-unset-absent=OK" ]]; then
        test_host_pass "$ctx unsetenv of an unset var is silent and OK"
    else
        test_host_fail "$ctx unsetenv of an unset var is silent and OK"
    fi
done
# 3. The var the driver set is visible to the SHELL afterward (persisted past
#    the launcher's exit) — the whole point of `do -f<file> var`.
if echo "$FSREAD_LOG" | grep -qE '^\* +FSRVAR_drv : HELLO_LINE one two'; then
    test_host_pass "driver-set var persists into the shell (do -f<file> var)"
else
    test_host_fail "driver-set var persists into the shell (do -f<file> var)"
fi

test_host_summary "old-shell test (X64)"
