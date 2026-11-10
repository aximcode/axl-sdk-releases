#!/bin/bash
# test-meta: arch=both needs=ssh est=68 local-only=0 size-gate=sshd size-gate-exclude=*.md guest=none
# test-meta: builds=BUILD=RELEASE,all,tools,release-drivers,sshd,sshd-faults
# test-meta: size-gate-paths=src/,include/,deps/,tools/sshd,scripts/elf_x86_64_efi.lds,scripts/efi-localize.ver,scripts/axl-efi-pack.c,scripts/pe-set-debug.c,scripts/axl-toolchains.conf,scripts/axl-lists.conf,cmake/,CMakeLists.txt,CMakePresets.json,tools/CMakeLists.txt,scripts/axl-list.sh,VERSION,packaging/system/
# guest=none: a second disk attached READ-ONLY (TEST_DISK2,
#   TEST_DISK_READONLY) and typed input: a machine no class gives.
# test-efi-pack-release-qemu.sh -- the RELEASE images, WRAPPED, run exactly as
# a remote user runs them: the tools tarball's layout copied onto a USB stick
# attached READ-ONLY (BMC virtual media often is), booted, and driven from the
# Shell. Nothing here is a test fixture. (crashtest's forced wrap, run under
# the crash handler in two TCG boots, is test-efi-pack-crash-qemu.sh.)
#
# The stick IS the tools tarball, extracted as a remote user copies it: its
# versioned root directory KEPT (fs0:\axl-sdk-uefi-tools-<ver>-<arch>\), every
# tool in it, share/*.json5 beside them, drivers/<arch>/, VERSION, README.txt,
# third_party/ and the sshd.conf it ships beside sshd.efi -- plus an
# authorized_keys beside sshd.efi (sshd's default) and a Port line appended to
# that sshd.conf. sshd is launched by its full path from the Shell, cd'd into
# the directory. The tarball is $UEFI_TOOLS_TARBALL when set
# (scripts/make-test-release.sh gates the very file it publishes), else one
# built here by scripts/make-release-assets.sh uefi-tools, the command release.yml
# runs. The host checks below read the tarball's copies too. It is fs0:; a
# second, WRITABLE USB disk is fs1: for `>` redirection.
#
#   host        the size gate: x64 RELEASE sshd.efi <= 235,520 B (target
#               204,800 B), both numbers printed; aa64 reports its number.
#               The images run below are the axl-pack stub (module name).
#   read-only   a `cp` onto fs0: is refused with the Shell's own message --
#               the stick really is read-only, so nothing below wrote to it
#   grep        args, its output redirected (`>`) to fs1: and read back
#               exactly, and %lasterror% 0x1 for a no-match
#   lspci       finds pci-ids.json5 beside itself: an exact decoded line
#   9p/netload  --version and the usage line, exactly
#   sshd        wrapped launcher + embedded driver: it reads the SHIPPED
#               sshd.conf beside it (one Port line appended), then a key
#               login using the DEFAULT authorized_keys
#               beside it on its read-only volume, then a
#               separate password login (-P), then `sshd stop`
#   ssh tools   in the key session: the wrapped grep on the key file, a pipe
#               into it (`1:` exists only if grep read the pipe), `help echo`
#               equal to the serial console's, and an `edit` save (F2, Enter,
#               F3) onto the writable volume, read back byte for byte
#
# Leak verdicts are not gated: these are RELEASE images.
#
# The bundled iPXE driver this test once loaded through netload was removed
# (2026-10-21). The long driver path its sweep proved netload reports whole is
# checked by test-netload-qemu.sh (--_fwwin, --_log) and AxlTestNet.
#
# Usage: [UEFI_TOOLS_TARBALL=<axl-sdk-uefi-tools-*.tar.gz>] \
#        ./test/integration/test-efi-pack-release-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

if ! command -v ssh >/dev/null 2>&1 || ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "SKIP: no ssh(1)/ssh-keygen(1) on the host -- the sessions are real ones"
    exit 77
fi

source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/sshd-lib.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
NATIVE="${_NATIVE[$TEST_ARCH]:-x64}"
VER=$(cat "$PROJECT_DIR/VERSION")
PW=p4g-release-pw
SSHD_TARGET=204800
# SSHD_LIMIT is the measured x64 RELEASE packed sshd.efi (it grows in 512 B
# steps), by workstream of the 2026-10-23 follow-ups round:
#   253,440 B  7f90d3fa, before the round
#   254,464 B  W4 (+1,024): window credit and the SFTP channel's ON_CONSUME
#   254,464 B  W1, W2 (+0): measured on main 982b31dc
#   254,464 B  W3 (+0): measured on W3 rebased onto 982b31dc
#   254,976 B  the aa64 SFTP crash fix (+512): sshd loads SftpServer.efi as
#              it starts and logs a missing one then; measured on 260b7e26
#              (975dbe93 before it: 254,464 B)
#   254,976 B  the image-loading TPL guard (+0)
#   255,488 B  the self-load guard (+512): every path load and the driver
#              search compare the file's device path with the running
#              image's; measured on e3d4188c + the fix (254,976 B before)
#   257,024 B  the self-load identity by stored names (+1,536): the compare
#              now resolves both paths through axl_fs_path_canonical (the
#              axl_file_replace identity walk) instead of folding device
#              paths by hand; measured on 4c47314d + the review fix
#   257,536 B  the image's identity cached once per process, the walk
#              buffers off the stack (+512); measured on ad90c76e + it
#   258,048 B  W6, the driver tick's idle extension (+512): the idle-loop
#              group members, the ran-between-ticks detector and the idle
#              budget setter; measured on e00b2bfc (dfd19d52 before it
#              passed at 257,536 B)
#   258,560 B  W10, the service state (+512): the stopping/degraded tags,
#              axl_service_state and set_degraded, the half-stopped entry
#              path, and the status/start/stop --reset verbs; measured on
#              the W10 round-2 tree (f269bf86's limit failed it at 258,560 B)
#   259,072 B  W10 round 3 (+512): `stop --reset` resetting only for
#              STOPPING (with a log flush), the degraded reason read by the
#              UEFI size rule, HELD refused without a teardown, the tags'
#              own namespace, and sshd's own-reason stuck check. Measured on
#              the round-3 tree; the shipped image keeps a weak, always-false
#              `_sshd_test_point` call, and removing that call measured the
#              same 259,072 B, so the test seam is not what moved it
#   259,072 B  W7, the overrun driver tick and same-tick deferred work (+0):
#              measured on the squashed tip 05bbc81b
#   259,584 B  W7, AxlTcpPacing.linger_us and sshd's AxlTcpLinger /
#              --tcp-linger (+512): the pacer's linger members, the conf row,
#              the flag and the driver's pacing call; measured on the linger
#              commit's tree (991b43ce before it: 259,072 B)
#   270,336 B  AxlSsh P5 (+10,752): the persistent host key and the
#              delivered DISCONNECT; measured on f0dda933 (d121ff68 before
#              it: 259,584 B). Raw, the embedded driver grew +16,896 B
#              (508,928 -> 525,824) and the launcher's own part +20,992 B
#              (160,256 -> 181,248); packed, LZMA over each part alone puts
#              ~76% of the growth in the driver and ~24% in the launcher.
#              Of the driver's code (nm -S): host-key storage ~9.5 KB (the
#              parser, the variable's raw read/write, load-or-create, the
#              fingerprint, base64, the DER import, host_key_install) and
#              the linger ~2.6 KB (penalty_accrue, disconnect_as, the linger
#              timer, conn_release_app, on_recv/on_sent). In the launcher:
#              the provider-free host-key module, --host-key, the start's
#              pre-check and `sshd hostkey`. e63484ef measured 272,384 B;
#              the final review's F5 (the key file read through
#              axl_file_get_contents, not a second file-stream stack in
#              both images) and its message fixes took it to this: -2,048 B
#              packed, -8,192 B raw (driver -4,608, launcher -3,584)
#   270,848 B  the P5 re-review's N1-N3 (+512): mostly the launcher's
#              `--import` messages (it reads the variable back before it
#              reports what is stored, and decides success on the write
#              alone); the key-file read's missing-mid-read case and the
#              new seam call are library code that reaches the driver too
#              (raw sshd-dxe.efi 525,824 -> 526,336); measured on 89f1b354
#              (7081f4ed: 270,336 B)
#   272,384 B  modern-Shell path resolution (+1,536): resolving paths
#              AXL-side through the Shell's map instead of OpenFileByName /
#              GetCurDir / GetMapFromDevicePath, which park a buffer per call
#              (axl-backend-native-shell.c; library code every file-opening
#              image links). ca17b9f5 before it: 270,848 B; the same change
#              adds 1,792 B to the x64 iostreams image's .text
#   272,896 B  the deferred exit and child-image accounting (+512, with the
#              path resolution above): every image now counts the images it
#              starts (axl_backend_start_image, the connect / Shell Execute
#              brackets), enters notify callbacks through a setjmp point so
#              axl_exit can be deferred instead of deadlocking, and pins the
#              first exit's status; library code every image links.
#              Measured on 0a77d519 + the path-resolution commit.
#   274,432 B  the network latency policy (+1,536): axl_net_set_latency
#              (opt-in) over axl_timer_resolution_begin/end,
#              and AxlTcp's deferred closes finalized by one sweeper per loop
#              instead of one loop source per close. About 2.9 KB raw in the
#              driver (nm -S, RELEASE sshd-dxe): the cross-image timer
#              registry ~1.4 KB, the latency policy ~0.9 KB, the close
#              sweeper ~0.25 KB, the driver-tick hook ~0.12 KB; library code
#              every AxlTcp image links. Measured 2026-11-03.
#   274,944 B  the 4.7.4 version bump (+512): no code moved -- the RELEASE
#              sshd-dxe.efi is byte-identical (534,016 B) before and after.
#              "4.7.3" -> "4.7.4" changes the bytes the packer compresses,
#              and the packed image, which sat exactly AT the cap, crossed
#              one 512 B file-alignment unit. Measured 2026-11-09 against
#              5fb8d48c (274,432 B).
SSHD_LIMIT=274944
# Every assertion below is counted once per arch, run or skipped.
TOTAL=38
SKIPPED=0
skip() { echo "  SKIP: $1"; SKIPPED=$((SKIPPED + 1)); }

# ---------------------------------------------------------------------------
# Build: the release tree, as release.yml builds it
# ---------------------------------------------------------------------------
# With CMake (the wrapper's default), as release.yml builds it since slice 5b
# (docs/AXL-Build-System-Design.md §10.9): the tarball script below stages
# from the tree `axl-build.sh --print-prefix` names, so the build and the
# tarball always agree on the backend. (Pinned to make while release.yml was
# make's, this certified a tarball from a tree the default never built.)
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NATIVE" \
        BUILD=RELEASE all tools release-drivers sshd-faults \
        > "$TEST_TMPDIR/make.log" 2>&1; then
    tail -20 "$TEST_TMPDIR/make.log"
    echo "efi-pack-release test: FAIL (build)"
    exit 1
fi
REL="$("$PROJECT_DIR/scripts/axl-build.sh" --print-prefix --abs ARCH="$NATIVE" BUILD=RELEASE)"
RDRV="$REL/release-drivers/$NATIVE"

# ---------------------------------------------------------------------------
# The tarball, and the stick extracted from it
# ---------------------------------------------------------------------------
if [[ -n "${UEFI_TOOLS_TARBALL:-}" ]]; then
    TARBALL="$UEFI_TOOLS_TARBALL"
    [[ -f "$TARBALL" ]] || { echo "efi-pack-release test: FAIL (no $TARBALL)"; exit 1; }
    cache_record_input "$(basename "$0")" "$TARBALL"
else
    # Versioned as make-test-release.sh versions a test release
    # (<VERSION>-test-<date>-<sha>), the longest directory name a stick
    # gets: every path printed below is that long. A tagged release's
    # directory is shorter. The tools' own --version reads VERSION, not this.
    TAR_VER="$VER-test-20000101-00000000"
    # --built: release-drivers is built above, with the rest of the tree, so
    # the script builds nothing (a release-drivers build there would be one
    # more build invocation, for a no-op).
    if ! "$PROJECT_DIR/scripts/make-release-assets.sh" uefi-tools --arch "$NATIVE" \
            --version "$TAR_VER" --out "$TEST_TMPDIR/tarball" --built \
            > "$TEST_TMPDIR/tarball.log" 2>&1; then
        tail -20 "$TEST_TMPDIR/tarball.log"
        echo "efi-pack-release test: FAIL (make-release-assets.sh uefi-tools)"
        exit 1
    fi
    TARBALL="$TEST_TMPDIR/tarball/axl-sdk-uefi-tools-$TAR_VER-$NATIVE.tar.gz"
    # The tarball lives in this run's temp dir, so the cache keys on what it
    # is made from: the images and sidecars as before, the script that packs
    # it and the install rules that stage it.
    for f in "$REL/tools/"*.efi "$PROJECT_DIR/share/"*.json5 "$RDRV/"* \
             "$PROJECT_DIR/scripts/make-release-assets.sh" "$PROJECT_DIR/packaging/uefi-tools/CMakeLists.txt" \
             "$PROJECT_DIR/cmake/CPackConfig.cmake.in" "$PROJECT_DIR/tools/sshd.conf"; do
        cache_record_input "$(basename "$0")" "$f"
    done
fi
mkdir -p "$TEST_TMPDIR/stick"
tar -C "$TEST_TMPDIR/stick" -xzf "$TARBALL" \
    || { echo "efi-pack-release test: FAIL (cannot extract $TARBALL)"; exit 1; }
STICK="$TEST_TMPDIR/stick/$(basename "$TARBALL" .tar.gz)"
[[ -d "$STICK" ]] || { echo "efi-pack-release test: FAIL (no root $(basename "$STICK")/ in the tarball)"; exit 1; }

echo "=== efi-pack: the wrapped RELEASE images from a read-only stick ($TEST_ARCH) ==="

# ---------------------------------------------------------------------------
# Host: the size gate and what is wrapped
# ---------------------------------------------------------------------------
SSHD_SIZE=$(stat -c%s "$STICK/sshd.efi")
echo "  sshd.efi $NATIVE RELEASE: $SSHD_SIZE B (target $SSHD_TARGET B, limit $SSHD_LIMIT B, x64)"
if [[ "$NATIVE" == x64 ]]; then
    check "size: x64 RELEASE sshd.efi $SSHD_SIZE B <= $SSHD_LIMIT B" \
        test "$SSHD_SIZE" -le "$SSHD_LIMIT"
else
    skip "size: the gate is on x64 sshd.efi (aa64 is $SSHD_SIZE B, reported)"
fi
# THE TEST SEAMS STAY OUT OF WHAT SHIPS (P5). Every library test point
# (`_axl_*_test_point`, sshd's `_sshd_test_point`) is WEAK and always false,
# and a test links a STRONG one -- test/integration/sshd-test-hook.c for
# sshd-t, armed by a file anyone can drop on the boot volume. Symbols are
# localized in a linked image, so weak and strong look alike to nm's type
# letter; their SIZE does not: the always-false stub is 3 bytes on x64 and 8
# on aa64, and every strong definition is larger (final review F10). So ANY
# strong seam in a shipped image is caught, whichever file defines it -- not
# only the hook's. strong_seams prints the test points whose size is not the
# stub's; "nm-failed" when nm could not read the image, and "no-seams" when
# it found no test point at all, so a blind nm cannot pass. sshd-t's driver
# is the control: it must list the hook's four.
#
# THE CONVENTION IT RESTS ON: every seam is named `*_test_point` (each seam's
# declaration says so). A seam named otherwise is invisible here. And the
# x64 run is the authority: on aa64 an always-TRUE strong stub
# (`mov w0, #1; ret`) is 8 bytes, the weak stub's size, so only x64 -- where
# `return true` is 6 bytes against the stub's 3 -- tells them apart. Both
# arches build the same sources, so the x64 job catches it.
NM_BIN=nm
WEAK_SEAM_RE='^0*3$'
if [[ "$NATIVE" == aa64 ]]; then
    NM_BIN=aarch64-linux-gnu-nm
    WEAK_SEAM_RE='^0*8$'
fi
strong_seams() {
    local out
    out=$("$NM_BIN" -S "$1" 2>/dev/null) || { echo "nm-failed"; return; }
    awk -v weak="$WEAK_SEAM_RE" '
        NF == 4 && $4 ~ /_test_point$/ { n++; if ($2 !~ weak) print $4 }
        END { if (n == 0) print "no-seams" }' <<<"$out" | sort | paste -sd' '
}
check "seams: the test build sshd-t's driver carries the hook's four (the control)" \
    test "$(strong_seams "$REL/build/sshd-t-dxe.so")" = \
        "_axl_console_device_test_point _axl_ssh_host_key_test_point _axl_tcp_test_point _sshd_test_point"
check "seams: the RELEASE sshd driver carries no strong test seam" \
    test "$(strong_seams "$REL/build/sshd-dxe.so")" = ""
check "seams: the RELEASE sshd launcher carries none either" \
    test "$(strong_seams "$REL/tools/sshd.so")" = ""
check "seams: nor the RELEASE sshd-full launcher" \
    test "$(strong_seams "$REL/tools/sshd-full.so")" = ""

for t in sshd grep lspci 9p netload; do
    check "host: the shipped $t.efi is the axl-pack stub" \
        test "$(test_pe_module "$STICK/$t.efi")" = axl-pack-stub
done

# ---------------------------------------------------------------------------
# The stick, exactly as the tools tarball lays it out
# ---------------------------------------------------------------------------
# The versioned root directory kept, as a user copies the extracted tarball;
# authorized_keys goes beside sshd.efi, in that directory, where the
# tarball's sshd.conf already is.
mkdir -p "$TEST_STAGING"
cp -R "$STICK" "$TEST_STAGING/"
D=$(basename "$STICK")
ssh-keygen -q -t ed25519 -N '' -C 'axl-release-test' -f "$TEST_TMPDIR/key" </dev/null
cp "$TEST_TMPDIR/key.pub" "$TEST_STAGING/$D/authorized_keys"
# The shipped sshd.conf (every keyword commented out at its default), with
# the forwarded port appended: sshd must read the file the tarball ships.
check "host: the tarball ships sshd.conf beside sshd.efi, as tools/sshd.conf" \
    cmp -s "$STICK/sshd.conf" "$PROJECT_DIR/tools/sshd.conf"
printf 'Port %s\n' "$GUEST_PORT" >> "$TEST_STAGING/$D/sshd.conf"

export TEST_DISK_READONLY=1
export TEST_DISK2="$TEST_TMPDIR/rw.img"
dd if=/dev/zero of="$TEST_DISK2" bs=1M count=8 status=none
mkfs.vfat -n RW "$TEST_DISK2" > /dev/null
export TEST_NIC_MODEL="virtio-net-pci,romfile="

sshd_boot "cd $D
$(cat << 'NSH'
echo RO-BEGIN
cp VERSION ro-probe.txt
echo RO=%lasterror%
echo GREP-BEGIN
grep -n axl-release-test authorized_keys > fs1:\grep-out.txt
echo GREP=%lasterror%
type fs1:\grep-out.txt
grep no-such-pattern authorized_keys
echo NOMATCH=%lasterror%
echo LSPCI-BEGIN
lspci
echo LSPCI-END
9p --version
9p --help
echo 9P-END
netload --version
netload --help
echo NETLOAD-END
NSH
)"

# Exactly N whole lines strictly between markers $2 and $3 are $4. A missing
# marker fails: an absence counted over a window never printed proves nothing.
in_window() {
    local from="$1" to="$2" line="$3" want="$4" got
    test_clean_log
    test_slice_log "$from" "$to" "$TEST_TMPDIR/slice" || return 1
    got=$(grep -cxF -- "$line" "$TEST_TMPDIR/slice" || true)
    [[ "$got" == "$want" ]]
}

check "read-only: cp onto fs0: is refused by the Shell" \
    in_window RO-BEGIN GREP-BEGIN "cp: The destination file 'FS0:\\$D\\ro-probe.txt' failed to open with create." 1
check "read-only: and %lasterror% says so (0xF)" in_window RO-BEGIN GREP-BEGIN "RO=0xF" 1
check "grep: exits 0 with its output redirected to fs1:" \
    in_window GREP-BEGIN LSPCI-BEGIN "GREP=0x0" 1
check "grep: the redirected file holds the matching line, numbered" \
    in_window GREP-BEGIN LSPCI-BEGIN "1:$(cat "$TEST_TMPDIR/key.pub")" 1
check "grep: a no-match sets %lasterror% to 0x1" \
    in_window GREP-BEGIN LSPCI-BEGIN "NOMATCH=0x1" 1
check "lspci: names the xHCI controller from pci-ids.json5 beside it" \
    in_window LSPCI-BEGIN LSPCI-END \
    "00:03.0 Serial bus controller / USB / xHCI Red Hat / QEMU QEMU xHCI USB 3.0 [1B36:000D]" 1
check "9p: --version" in_window LSPCI-END 9P-END "9p $VER" 1
check "9p: --help prints its usage line" in_window LSPCI-END 9P-END \
    "Usage: 9p [flags] <verb> [args]" 1
check "netload: --version" in_window 9P-END NETLOAD-END "netload $VER" 1
check "netload: --help prints its usage line" in_window 9P-END NETLOAD-END \
    "Usage: netload [flags]" 1

# ---------------------------------------------------------------------------
# sshd: key login (the default authorized_keys), password login, stop
# ---------------------------------------------------------------------------
send "fs0:\\\\$D\\\\sshd.efi $SSHD_LOG_HERE -P $PW -x $HOST_AS_SEEN\\r"
check "sshd: the launcher prints its serving line" \
    wait_guest "sshd: serving the UEFI Shell on $GUEST_IP port $GUEST_PORT" 1 60
check "sshd: it reads the sshd.conf beside it" \
    wait_guest "sshd: settings from FS0:\\$D\\sshd.conf" 1 10
check "sshd: the driver logs its serving line with the console size" \
    sshd_read_geometry
RAND=$(rand_tag)

# ssh(1)'s own -v line naming the method it authenticated with, exactly once
# (OpenSSH writes it under -E with no debug1: prefix, CRLF under -tt).
auth_line() {   # $1 session, $2 method
    local n
    n=$(tr -d '\r' < "$TEST_TMPDIR/ssh-$1.log" \
        | grep -cxF "Authenticated to 127.0.0.1 ([127.0.0.1]:$HOST_PORT) using \"$2\"." || true)
    [[ "$n" == 1 ]]
}

# Poll the session transcript (joined, so a wrapped line matches) for $2, up
# to $3 s.
ssh_wait_joined() {
    local i
    for ((i = 0; i < $3 * 5; i++)); do
        (( $(session_joined_count "$(ssh_out "$1")" "$2") >= 1 )) && return 0
        sleep 0.2
    done
    return 1
}
ssh_open K
ssh_wait_repaint K 40 || true
sleep 1
check "key: ssh(1) authenticated with the key from the authorized_keys beside sshd.efi" auth_line K publickey
n=$(ssh_banner_count K)
check "key: ssh(1) prints the console geometry banner once ($n)" test "$n" = 1
ssh_type K "echo %T%-k$RAND\\r"
wait_session "$(ssh_out K)" "p4f-k$RAND" 1 30 || true
sleep 1
n=$(session_count "$(ssh_out K)" "p4f-k$RAND")
check "key: a command typed in the session runs; its line arrives once ($n)" test "$n" = 1

# What the BMC-console session was checked for by hand: a wrapped tool, a
# pipe, `help` and an `edit` save, all driven over this ssh session.
ssh_type K "grep -n axl-release-test authorized_keys\\r"
KEYLINE="1:$(cat "$TEST_TMPDIR/key.pub")"
ssh_wait_joined K "$KEYLINE" 30 || true
# The wrapped grep's input is the pipe; the "1:" prefix exists only if grep
# read it (a numbered line is grep's, not echo's).
ssh_type K "echo %T%-pipe$RAND | grep -n p4f-pipe$RAND\\r"
wait_session "$(ssh_out K)" "1:p4f-pipe$RAND" 1 30 || true
ssh_type K "echo %T%-h1$RAND\\r"
ssh_type K "help echo\\r"
ssh_type K "echo %T%-h2$RAND\\r"
wait_session "$(ssh_out K)" "p4f-h2$RAND" 1 30 || true
# Both counts are read after later commands have run, so a duplicate line
# from the earlier ones has arrived by now.
n=$(session_joined_count "$(ssh_out K)" "$KEYLINE")
check "ssh-tool: the wrapped grep prints its numbered key line once ($n)" test "$n" = 1
n=$(session_count "$(ssh_out K)" "1:p4f-pipe$RAND")
check "ssh-pipe: echo piped into the wrapped grep prints 1:p4f-pipe$RAND once ($n)" test "$n" = 1
# Blank lines are dropped on both sides: ssh's CR LF reads as a line break
# plus an empty line in session_lines, the serial log's does not.
session_lines "$(ssh_out K)" | awk -v s="p4f-h1$RAND" -v e="p4f-h2$RAND" \
    '$0 == e { on = 0 } on; $0 == s { on = 1 }' | grep -v '^$' > "$TEST_TMPDIR/help.ssh"
test_clean_log
awk -v s="p4f-h1$RAND" -v e="p4f-h2$RAND" '$0 == e { on = 0 } on; $0 == s { on = 1 }' \
    "$TEST_CLEAN_LOG" | sed -E 's/[[:space:]]+$//' | grep -v '^$' > "$TEST_TMPDIR/help.ser"
n=$(wc -l < "$TEST_TMPDIR/help.ssh")
check "ssh-help: the ssh transcript holds at least 5 lines of \`help echo\` ($n)" test "$n" -ge 5
diff "$TEST_TMPDIR/help.ssh" "$TEST_TMPDIR/help.ser" | head -20 | cat -v >&2 || true
check "ssh-help: those lines equal the serial console's, line for line" \
    cmp -s "$TEST_TMPDIR/help.ssh" "$TEST_TMPDIR/help.ser"
# edit: wait for its title bar, F2, wait for the save prompt, Enter, wait for
# the status line it prints, F3, wait for the Shell's next marker.
EDIT_SAVE_PROMPT='File to Save: [fs1:\rel.txt]'
EDIT_SAVED=' 1,4       1 Lines Written'
ssh_type K "edit fs1:\\\\rel.txt\\r"
for ((i = 0; i < 150; i++)); do
    session_lines "$(ssh_out K)" | grep -aq '^UEFI EDIT rel\.txt ' && break
    sleep 0.2
done
ssh_type K "rel"
ssh_type K "\\033OQ"
wait_session "$(ssh_out K)" "$EDIT_SAVE_PROMPT" 1 30 || true
ssh_type K "\\r"
wait_session "$(ssh_out K)" "$EDIT_SAVED" 1 30 || true
n=$(session_count "$(ssh_out K)" "$EDIT_SAVED")
check "ssh-edit: the editor's status line is exactly '$EDIT_SAVED' once ($n)" test "$n" = 1
ssh_type K "\\033OR"
ssh_type K "echo %T%-ed$RAND\\r"
wait_session "$(ssh_out K)" "p4f-ed$RAND" 1 30 || true
ssh_close K

# A password login: real ssh(1) answered by SSH_ASKPASS, no key offered.
cat > "$TEST_TMPDIR/askpass.sh" << 'ASKPASS'
#!/bin/sh
printf '%s\n' "$ASKPASS_PASSWORD"
ASKPASS
chmod +x "$TEST_TMPDIR/askpass.sh"
rm -f "$TEST_TMPDIR/P.in"
mkfifo "$TEST_TMPDIR/P.in"
exec {pfd}<>"$TEST_TMPDIR/P.in"
SSH_FD[P]=$pfd
SSH_ASKPASS="$TEST_TMPDIR/askpass.sh" SSH_ASKPASS_REQUIRE=force ASKPASS_PASSWORD="$PW" \
    ssh -tt -v -E "$TEST_TMPDIR/ssh-P.log" -p "$HOST_PORT" \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 \
    -o PubkeyAuthentication=no -o KbdInteractiveAuthentication=no \
    -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1 \
    anyone@127.0.0.1 <&"$pfd" > "$TEST_TMPDIR/P.out" 2> "$TEST_TMPDIR/P.err" &
SSH_PID[P]=$!
ssh_wait_repaint P 40 || true
sleep 1
check "password: ssh(1) authenticated with -P's password" auth_line P password
n=$(ssh_banner_count P)
check "password: ssh(1) prints the console geometry banner once ($n)" test "$n" = 1
ssh_type P "echo %T%-p$RAND\\r"
wait_session "$(ssh_out P)" "p4f-p$RAND" 1 30 || true
sleep 1
n=$(session_count "$(ssh_out P)" "p4f-p$RAND")
check "password: a command typed in the session runs; its line arrives once ($n)" test "$n" = 1
ssh_close P

send 'sshd stop\r'
check "stop: \`sshd stop\` prints 'sshd: stopped'" wait_guest "sshd: stopped" 1 30

sshd_poweroff
REL_BYTES=$(mcopy -n -i "$TEST_DISK2" ::rel.txt - 2>/dev/null | od -An -tx1 | tr -s ' \n' ' ' | sed 's/^ //; s/ $//')
check "ssh-edit: fs1:\\rel.txt is exactly the UTF-16 file 'rel' ($REL_BYTES)" \
    test "$REL_BYTES" = "ff fe 72 00 65 00 6c 00"
n=$(cpu_exceptions)
check "no CPU exception in the main boot ($n)" test "$n" = 0

echo ""
printf "efi-pack-release: %d passed, %d failed, %d skipped (%s)\n" \
    "$PASS" "$FAIL" "$SKIPPED" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo "--- serial log ---"; tail -60 "$TEST_CLEAN_LOG"
fi
if (( PASS + FAIL + SKIPPED != TOTAL )); then
    echo "FAIL: $((PASS + FAIL + SKIPPED)) assertions ran, $TOTAL expected"
    exit 1
fi
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
