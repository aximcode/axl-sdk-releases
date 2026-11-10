#!/bin/bash
# test-meta: arch=none needs=script est=40 local-only=0 guest=none
# guest=none: harness test: the firmware's DEBUG channel (the AARCH64 debug
#   UART, the X64 debugcon) on the QEMU command lines run-qemu.sh and
#   common-test.sh build, against a stub QEMU.
# test-fw-debug-uart.sh -- every AARCH64 guest gets a SECOND serial port for
# the firmware's DEBUG output, so its console UART carries no DEBUG output
# (docs/AXL-Test-Guest-Pool-Design.md, "a debug UART on every AARCH64 guest"),
# and every pooled X64 guest OVMF's 0x402 debugcon ("a debugcon on every
# pooled X64 guest").
#
# On `virt` the UARTs follow -serial order and the bundled ArmVirtQemu firmware
# sends DEBUG to the second one, so the order IS the contract: the console's
# -serial first, then `-chardev file,id=axl_fwdebug,...` and its -serial. A
# -nographic launch gets its console spelled out (`-serial mon:stdio`, or
# `stdio` when a monitor option took the monitor), because an explicit -serial
# drops the one -nographic would add and the debug file would become UART0.
# X64 sends DEBUG to the 0x402 debugcon: a pooled guest's launch
# (--state-dir) and a booted one that asks (TEST_QEMU_DEBUGCON) get it on a
# file chardev that appends; a booted run's default is unchanged (no port).
#
# Pinned, exactly:
#   helper     qemu_fw_debug_uart (axl-common.sh) on the shapes it meets:
#              -nographic with and without a monitor option, an explicit
#              console, -nodefaults, X64, a second call, a comma in the path;
#   run-qemu   the -serial order of each launch mode on AARCH64 (foreground,
#              background with and without a monitor, a serial socket,
#              --state-dir, screenshot, a display, -i), the log's place (beside
#              --serial-log, in --state-dir, at --debugcon, else the run's
#              temp dir), a stale log truncated, an ASSERT surfaced on stdout,
#              the dry run's argv; X64: booted unchanged, --state-dir with
#              the debugcon (one, with --debugcon too), in its dry run too;
#   common     test_build_qemu_cmd's launch (the console before the debug
#              UART, `stdio` once a test added a monitor), the DXE-core ASSERT
#              guard and test_check_firmware_asserts reading the debug log,
#              booted and pooled, on both arches -- the pooled run reading
#              only what the firmware wrote during the run, and a pooled
#              X64 run's CONTROL failing on a run's part that is empty.
#
# Host-only: no guest boots.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
set -uo pipefail
export TEST_SKIP_RATCHET=1

PASS=0 FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
# same NAME GOT WANT: exact equality, a diff when not.
same() {
    if [[ "$2" == "$3" ]]; then
        pass "$1"
    else
        fail "$1"
        diff <(printf '%s\n' "$2") <(printf '%s\n' "$3") | sed 's/^/      /'
    fi
}

WORK="$(mktemp -d -t axl-fwdbg.XXXXXXXX)"
cleanup() {
    local p
    # By PID: every stub this started records its own -- and only one still
    # running as this test's stub (its command line names $WORK), as a pid
    # long dead may by now be another process's.
    for p in $(cat "$WORK"/stub-*.pid 2>/dev/null); do
        tr '\0' ' ' 2>/dev/null < "/proc/$p/cmdline" | grep -qF "$WORK/stub/" && kill "$p" 2>/dev/null
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

# shellcheck source=scripts/axl-common.sh
source "$PROJECT_DIR/scripts/axl-common.sh"

# ---------------------------------------------------------------------------
echo "=== qemu_fw_debug_uart ==="
# uart ARCH FILE TOKENS...: the command after the helper, '|'-joined.
uart() {
    local arch="$1" file="$2"
    shift 2
    local -a c=("$@")
    qemu_fw_debug_uart c "$arch" "$file"
    local IFS='|'
    echo "${c[*]}"
}
DBG='-chardev|file,id=axl_fwdebug,path=/w/fw.log,append=on|-serial|chardev:axl_fwdebug'
same "-nographic: the console spelled out (mon:stdio), then the debug UART" \
    "$(uart AARCH64 /w/fw.log -M virt -nographic -no-reboot)" \
    "-M|virt|-nographic|-no-reboot|-serial|mon:stdio|$DBG"
for m in -monitor -mon -qmp -qmp-pretty; do
    same "-nographic with $m: the console is stdio (the monitor is elsewhere)" \
        "$(uart AARCH64 /w/fw.log -nographic "$m" x)" "-nographic|$m|x|-serial|stdio|$DBG"
done
same "an explicit console: the debug UART after it" \
    "$(uart AARCH64 /w/fw.log -serial chardev:c0 -display none)" \
    "-serial|chardev:c0|-display|none|$DBG"
same "-nodefaults and no console: nothing (the debug UART would be UART0)" \
    "$(uart AARCH64 /w/fw.log -nographic -nodefaults)" "-nographic|-nodefaults"
XDBG='-chardev|file,id=axl_fwdebug,path=/w/fw.log,append=on|-device|isa-debugcon,iobase=0x402,chardev=axl_fwdebug'
same "X64: OVMF's 0x402 debugcon, on a file chardev that appends" \
    "$(uart X64 /w/fw.log -nographic)" "-nographic|$XDBG"
same "X64 -nodefaults: the debugcon all the same (it is no UART, and q35 keeps its ISA bus)" \
    "$(uart X64 /w/fw.log -nographic -nodefaults)" "-nographic|-nodefaults|$XDBG"
x64twice() { local -a c=(-nographic); qemu_fw_debug_uart c X64 /w/fw.log
             qemu_fw_debug_uart c X64 /w/fw.log; local IFS='|'; echo "${c[*]}"; }
same "X64: a second call adds nothing" "$(x64twice)" "-nographic|$XDBG"
twice() { local -a c=(-nographic); qemu_fw_debug_uart c AARCH64 /w/fw.log
          qemu_fw_debug_uart c AARCH64 /w/fw.log; local IFS='|'; echo "${c[*]}"; }
same "a second call adds nothing" "$(twice)" "-nographic|-serial|mon:stdio|$DBG"
same "a comma in the path is doubled (QEMU's option escape)" \
    "$(uart AARCH64 /w/a,b.log -serial stdio)" \
    "-serial|stdio|-chardev|file,id=axl_fwdebug,path=/w/a,,b.log,append=on|-serial|chardev:axl_fwdebug"

# ---------------------------------------------------------------------------
# The stub QEMU: records its argv (one token per line) in $WORK/argv, appends
# STUB_FWDEBUG to the debug UART's file when there is one, prints a boot.
mkdir -p "$WORK/stub"
cat > "$WORK/stub/qemu-system-aarch64" <<'EOF'
#!/bin/bash
printf '%s\n' "$@" > "$STUB_WORK/argv"
echo $$ >> "$STUB_WORK/stub-$$.pid"
if [[ -n "${STUB_FWDEBUG:-}" ]]; then
    for a in "$@"; do
        case "$a" in
            file,id=axl_fwdebug,path=*)
                p="${a#file,id=axl_fwdebug,path=}"
                printf '%s\r\n' "$STUB_FWDEBUG" >> "${p%,append=on}" ;;
        esac
    done
fi
echo "UEFI Interactive Shell v2.2"
echo "___AXL_APP_OUTPUT_BEGIN___"
echo "BOOTED"
[[ -n "${STUB_CONSOLE:-}" ]] && echo "$STUB_CONSOLE"
sleep "${STUB_LINGER:-0}"
EOF
chmod +x "$WORK/stub/qemu-system-aarch64"
cp "$WORK/stub/qemu-system-aarch64" "$WORK/stub/qemu-system-x86_64"
# --qcow2 converts with the qemu-img beside QEMU: a stub that makes the file.
printf '#!/bin/bash\nfor a; do last="$a"; done\n: > "$last"\n' > "$WORK/stub/qemu-img"
chmod +x "$WORK/stub/qemu-img"
printf 'MZ' > "$WORK/probe.efi"
# XDG_CACHE_HOME: a stub run is no evidence about a firmware, so whatever
# run-qemu.sh would remember of one (its shell-launcher memo) stays here.
export STUB_WORK="$WORK" QEMU_DIR="$WORK/stub" XDG_CACHE_HOME="$WORK/cache"

# serials: the recorded argv's console-shaped tokens, in order -- -nographic,
# every -serial / -chardev / -debugcon with its value, and an isa-debugcon
# -device -- the run's temp dir as TMP.
serials() {
    awk '$0 == "-nographic" { print; next }
         prev != "" { if (prev != "-device" || $0 ~ /^isa-debugcon/) print prev " " $0
                      prev = ""; next }
         $0 == "-serial" || $0 == "-chardev" || $0 == "-debugcon" || $0 == "-device" { prev = $0 }' "$WORK/argv" \
        | sed -E "s#[^ ,=:]*/axl-qemu\.[A-Za-z0-9]+/#TMP/#g; s#$WORK/#W/#g"
}
RQ=("$PROJECT_DIR/scripts/run-qemu.sh" --no-cpu-warn --timeout 20)
# rq NAME ARGS...: one run, its stdout+stderr in $WORK/NAME.out, its exit in RC.
rq() {
    local name="$1"
    shift
    rm -f "$WORK/argv"
    timeout 60 "${RQ[@]}" "$@" > "$WORK/$name.out" 2>&1
    RC=$?
    # A failed run says why, beside whichever check reads RC.
    [[ $RC -eq 0 ]] || tail -n 8 "$WORK/$name.out" | sed "s/^/      $name (rc $RC): /"
}
FW_UART='-chardev file,id=axl_fwdebug,path=TMP/firmware-debug.log,append=on
-serial chardev:axl_fwdebug'

echo "=== run-qemu.sh: AARCH64, each launch mode ==="
rq fg --arch AARCH64 "$WORK/probe.efi"
same "foreground: -nographic, the console (mon:stdio), the debug UART in the temp dir" \
    "$RC|$(serials)" "0|-nographic
-serial mon:stdio
$FW_UART"

printf 'STALE ASSERT\r\n' > "$WORK/s-firmware-debug.log"
STUB_FWDEBUG='ASSERT [DxeCore] Pool.c(721): Head->Signature' rq fgassert \
    --arch AARCH64 --serial-log "$WORK/s.log" "$WORK/probe.efi"
same "--serial-log: the debug log beside it, <name>-firmware-debug.log" "$(serials)" "-nographic
-serial mon:stdio
-chardev file,id=axl_fwdebug,path=W/s-firmware-debug.log,append=on
-serial chardev:axl_fwdebug"
same "...truncated at launch: only this run's DEBUG output in it" \
    "$(tr -d '\r' < "$WORK/s-firmware-debug.log")" 'ASSERT [DxeCore] Pool.c(721): Head->Signature'
same "...and its ASSERT surfaced after the app's output, as the console carried it before" \
    "$RC|$(grep -v 'Extracted Shell.efi' "$WORK/fgassert.out")" \
    '0|BOOTED
[firmware-debug] ASSERT [DxeCore] Pool.c(721): Head->Signature'

STUB_CONSOLE='Synchronous Exception at 0x000000005C5E1234' STUB_FWDEBUG=$'Boot noise\r\n  SP 0x1  ELR 0x2' \
    rq fgexc --arch AARCH64 "$WORK/probe.efi"
same "a firmware exception on the console: the debug UART's whole log after it (the dump's registers)" \
    "$RC|$(grep -v 'Extracted Shell.efi' "$WORK/fgexc.out")" \
'0|BOOTED
Synchronous Exception at 0x000000005C5E1234
[firmware-debug] Boot noise
[firmware-debug]   SP 0x1  ELR 0x2'
STUB_CONSOLE='SError Exception at 0x000000005C5E1234' STUB_FWDEBUG='  SP 0x1  ELR 0x2' \
    rq fgserr --arch AARCH64 "$WORK/probe.efi"
same "...an SError's too (all four exception types)" \
    "$(grep -c '^\[firmware-debug\]   SP 0x1  ELR 0x2$' "$WORK/fgserr.out")" 1
STUB_FWDEBUG='Boot noise, no fault' rq fgquiet --arch AARCH64 "$WORK/probe.efi"
same "...and nothing of it on a run with no ASSERT and no exception" \
    "$RC|$(grep -v 'Extracted Shell.efi' "$WORK/fgquiet.out")" '0|BOOTED'

rq devnull --arch AARCH64 --serial-log /dev/null "$WORK/probe.efi"
same "--serial-log /dev/null (nothing beside it to write): the log in the temp dir, the run as before" \
    "$RC|$(serials | sed -n 3p)" "0|-chardev file,id=axl_fwdebug,path=TMP/firmware-debug.log,append=on"

rq dcon --arch AARCH64 --debugcon "$WORK/d.log" "$WORK/probe.efi"
same "--debugcon FILE: the debug UART's file (no isa-debugcon on virt)" "$RC|$(serials)" "0|-nographic
-serial mon:stdio
-chardev file,id=axl_fwdebug,path=W/d.log,append=on
-serial chardev:axl_fwdebug"

rq bg --arch AARCH64 --background "$WORK/probe.efi"
for _ in $(seq 50); do [[ -s "$WORK/argv" ]] && break; sleep 0.1; done
same "background: -nographic, the console (mon:stdio), the debug UART" "$RC|$(serials)" "0|-nographic
-serial mon:stdio
$FW_UART"
same "...and the log's path among the facts it prints" \
    "$(sed -n 's/^FIRMWARE_DEBUG_LOG=//p' "$WORK/bg.out" | sed -E 's#.*/axl-qemu\.[A-Za-z0-9]+/#TMP/#')" \
    "TMP/firmware-debug.log"

rq bgqmp --arch AARCH64 --background --qmp-socket "$WORK/q.sock" "$WORK/probe.efi"
for _ in $(seq 50); do [[ -s "$WORK/argv" ]] && break; sleep 0.1; done
same "background with a QMP socket: the console is stdio" "$RC|$(serials)" "0|-nographic
-serial stdio
$FW_UART"

rq bgsock --arch AARCH64 --background --serial-socket "$WORK/s.sock" "$WORK/probe.efi"
for _ in $(seq 50); do [[ -s "$WORK/argv" ]] && break; sleep 0.1; done
same "background with a serial socket: that console, then the debug UART" "$RC|$(serials)" \
"0|-chardev socket,id=serial0,path=W/s.sock,server=on,wait=on
-serial chardev:serial0
$FW_UART"

rq state --arch AARCH64 --background --qcow2 --state-dir "$WORK/st" \
    --serial-log "$WORK/st/serial-0.log" "$WORK/probe.efi"
for _ in $(seq 50); do [[ -s "$WORK/argv" ]] && break; sleep 0.1; done
same "--state-dir: the log in the state dir, over --serial-log (a pooled guest's)" "$RC|$(serials)" "0|-nographic
-serial mon:stdio
-chardev file,id=axl_fwdebug,path=W/st/firmware-debug.log,append=on
-serial chardev:axl_fwdebug"

SHOT_WAIT=1 rq shot --arch AARCH64 --screenshot "$WORK/shot.png" "$WORK/probe.efi"
same "screenshot: its console file, then the debug UART" "$(serials)" \
"-serial file:TMP/serial.log
$FW_UART"

rq vnc --arch AARCH64 --vnc 99 "$WORK/probe.efi"
same "a display: the console on stdio, then the debug UART" "$(serials)" \
"-serial stdio
$FW_UART"

# -i takes the terminal: under `script`, which gives it one.
rm -f "$WORK/argv"
timeout 60 script -qec "$(printf '%q ' "${RQ[@]}" -i --arch AARCH64 "$WORK/probe.efi")" /dev/null \
    < /dev/null > "$WORK/inter.out" 2>&1
same "interactive: its muxed console, then the debug UART" "$(serials)" \
"-chardev stdio,id=axlcon0,mux=on,signal=off
-serial chardev:axlcon0
$FW_UART"

dry=$(QEMU_DRYRUN=1 timeout 60 "${RQ[@]}" --arch AARCH64 "$WORK/probe.efi" 2>/dev/null \
      | sed -n 's/^QEMU_DRYRUN: //p' | tail -4 | sed -E 's#[^ ,=:]*/axl-qemu\.[A-Za-z0-9]+/#TMP/#g')
same "the dry run lists the debug UART (a pooled class's key sees it)" "$dry" \
'-chardev
file,id=axl_fwdebug,path=TMP/firmware-debug.log,append=on
-serial
chardev:axl_fwdebug'

echo "=== run-qemu.sh: X64 -- booted unchanged, a kept guest (--state-dir) gets the debugcon ==="
rq x64 --arch X64 "$WORK/probe.efi"
same "X64 foreground: -nographic alone" "$RC|$(serials)" "0|-nographic"
rq x64dcon --arch X64 --debugcon "$WORK/x.log" "$WORK/probe.efi"
same "X64 --debugcon: the isa-debugcon, no debug UART" "$RC|$(serials)" "0|-debugcon file:W/x.log
-nographic"
X64_DCON='-device isa-debugcon,iobase=0x402,chardev=axl_fwdebug'
printf 'STALE\r\n' > "$WORK/xst-stale.log"
mkdir -p "$WORK/xst" && cp "$WORK/xst-stale.log" "$WORK/xst/firmware-debug.log"
STUB_FWDEBUG='Loading driver at 0x1 EntryPoint=0x2 TcpDxe.efi' rq x64state --arch X64 --background --qcow2 \
    --state-dir "$WORK/xst" --serial-log "$WORK/xst/serial-0.log" "$WORK/probe.efi"
for _ in $(seq 50); do [[ -s "$WORK/argv" ]] && break; sleep 0.1; done
same "X64 --state-dir (a pooled guest's launch): the debugcon, its log in the state dir" "$RC|$(serials)" "0|-nographic
-chardev file,id=axl_fwdebug,path=W/xst/firmware-debug.log,append=on
$X64_DCON"
for _ in $(seq 50); do [[ -s "$WORK/xst/firmware-debug.log" ]] && break; sleep 0.1; done
same "...truncated at launch, then the firmware's output" \
    "$(tr -d '\r' < "$WORK/xst/firmware-debug.log")" 'Loading driver at 0x1 EntryPoint=0x2 TcpDxe.efi'
same "...and the log's path among the facts it prints" \
    "$(sed -n 's/^FIRMWARE_DEBUG_LOG=//p' "$WORK/x64state.out" | sed "s#$WORK/#W/#")" "W/xst/firmware-debug.log"
rq x64stdcon --arch X64 --background --qcow2 --state-dir "$WORK/xst2" --debugcon "$WORK/xd.log" "$WORK/probe.efi"
for _ in $(seq 50); do [[ -s "$WORK/argv" ]] && break; sleep 0.1; done
same "X64 --state-dir and --debugcon FILE: ONE debugcon, at FILE" "$RC|$(serials)" "0|-nographic
-chardev file,id=axl_fwdebug,path=W/xd.log,append=on
$X64_DCON"
xdry() {
    QEMU_DRYRUN=1 timeout 60 "${RQ[@]}" --arch X64 "$@" "$WORK/probe.efi" 2>/dev/null \
        | sed -n 's/^QEMU_DRYRUN: //p' | tail -4 | sed "s#$WORK/#W/#g"
}
same "the X64 dry run with --state-dir lists the debugcon (the pooled class's key sees it)" \
    "$(xdry --qcow2 --state-dir "$WORK/xdry")" '-chardev
file,id=axl_fwdebug,path=W/xdry/firmware-debug.log,append=on
-device
isa-debugcon,iobase=0x402,chardev=axl_fwdebug'
same "...and without one, no debugcon (a booted run's default is unchanged)" \
    "$(xdry | grep -c 'isa-debugcon')" 0

# ---------------------------------------------------------------------------
echo "=== common-test.sh ==="
# child NAME ARCH BODY: a test script sourcing common-test.sh, the stub QEMU
# in place of the real one.
child() {
    {
        echo '#!/bin/bash'
        echo "source '$SCRIPT_DIR/common-test.sh'"
        echo "TEST_ARCH=$2"
        echo 'test_setup'
        echo "$3"
    } > "$WORK/$1"
    chmod +x "$WORK/$1"
}
BOOTED='test_build_qemu_cmd; TEST_QEMU_CMD[0]='"'$WORK/stub/qemu-system-aarch64'"
child ct-a.sh AARCH64 "$BOOTED
test_run_foreground 10 > /dev/null
echo \"LOG=\$TEST_FW_DEBUG_LOG\"
echo \"TMP=\$TEST_TMPDIR\""
STUB_FWDEBUG='Loading driver at 0x1 EntryPoint=0x2 TcpDxe.efi' "$WORK/ct-a.sh" > "$WORK/ct-a.out" 2>&1
rc=$?
ctmp=$(sed -n 's/^TMP=//p' "$WORK/ct-a.out")
same "booted: the console (mon:stdio), then the debug UART in the test's temp dir" \
    "$rc|$(serials | sed "s#${ctmp:-NONE}/#T/#")|$(sed -n 's/^LOG=//p' "$WORK/ct-a.out" | sed "s#${ctmp:-NONE}/#T/#")" \
"0|-nographic
-serial mon:stdio
-chardev file,id=axl_fwdebug,path=T/firmware-debug.log,append=on
-serial chardev:axl_fwdebug|T/firmware-debug.log"

child ct-mon.sh AARCH64 "$BOOTED
TEST_QEMU_CMD+=(-monitor none)
test_run_foreground 10 > /dev/null"
"$WORK/ct-mon.sh" > "$WORK/ct-mon.out" 2>&1
same "booted, a monitor the test added after test_build_qemu_cmd: the console is stdio" \
    "$(serials | sed -E 's#path=[^,]*/firmware-debug\.log#path=T/firmware-debug.log#')" "-nographic
-serial stdio
-chardev file,id=axl_fwdebug,path=T/firmware-debug.log,append=on
-serial chardev:axl_fwdebug"

child ct-x64.sh X64 "test_build_qemu_cmd; TEST_QEMU_CMD[0]='$WORK/stub/qemu-system-x86_64'
test_run_foreground 10 > /dev/null
echo \"LOG=[\$TEST_FW_DEBUG_LOG]\""
"$WORK/ct-x64.sh" > "$WORK/ct-x64.out" 2>&1
same "X64 booted: -nographic alone, no debug log" "$(serials)|$(sed -n 's/^LOG=//p' "$WORK/ct-x64.out")" \
    "-nographic|[]"

XBOOTED='test_build_qemu_cmd; TEST_QEMU_CMD[0]='"'$WORK/stub/qemu-system-x86_64'"
child ct-x64dc.sh X64 "export TEST_QEMU_DEBUGCON=\$TEST_TMPDIR/dc.log
$XBOOTED
test_run_foreground 10 > /dev/null
echo SURVIVED
echo \"LOG=\$TEST_FW_DEBUG_LOG\"
echo \"TMP=\$TEST_TMPDIR\""
STUB_FWDEBUG='Loading driver at 0x1 EntryPoint=0x2 TcpDxe.efi' "$WORK/ct-x64dc.sh" > "$WORK/ct-x64dc.out" 2>&1
rc=$?
ctmp=$(sed -n 's/^TMP=//p' "$WORK/ct-x64dc.out")
same "X64 booted, TEST_QEMU_DEBUGCON: the debugcon at that file, the readers' log" \
    "$rc|$(serials | sed "s#${ctmp:-NONE}/#T/#")|$(sed -n 's/^LOG=//p' "$WORK/ct-x64dc.out" | sed "s#${ctmp:-NONE}/#T/#")" \
"0|-nographic
-chardev file,id=axl_fwdebug,path=T/dc.log,append=on
-device isa-debugcon,iobase=0x402,chardev=axl_fwdebug|T/dc.log"
STUB_FWDEBUG='ASSERT [DxeCore] /e/Pool.c(721): Head->Signature == POOL_HEAD_SIGNATURE' \
    "$WORK/ct-x64dc.sh" > "$WORK/ct-x64dc2.out" 2>&1
rc=$?
same "X64 booted: a DXE-core ASSERT on the debugcon fails the test" \
    "$rc|$(grep -c SURVIVED "$WORK/ct-x64dc2.out")|$(grep -A1 -F '*** FAIL: firmware DXE-core ASSERT' "$WORK/ct-x64dc2.out")" \
"1|0|*** FAIL: firmware DXE-core ASSERT (pool/heap corruption) in ct-x64dc.sh:
    ASSERT [DxeCore] /e/Pool.c(721): Head->Signature == POOL_HEAD_SIGNATURE"

child ct-dxe.sh AARCH64 "$BOOTED
test_run_foreground 10 > /dev/null
echo SURVIVED"
STUB_FWDEBUG='ASSERT [DxeCore] /e/Pool.c(721): Head->Signature == POOL_HEAD_SIGNATURE' \
    "$WORK/ct-dxe.sh" > "$WORK/ct-dxe.out" 2>&1
rc=$?
same "booted: a DXE-core ASSERT on the debug UART fails the test" \
    "$rc|$(grep -c SURVIVED "$WORK/ct-dxe.out")|$(grep -A1 -F '*** FAIL: firmware DXE-core ASSERT' "$WORK/ct-dxe.out")" \
"1|0|*** FAIL: firmware DXE-core ASSERT (pool/heap corruption) in ct-dxe.sh:
    ASSERT [DxeCore] /e/Pool.c(721): Head->Signature == POOL_HEAD_SIGNATURE"
STUB_FWDEBUG='Loading driver at 0x1 EntryPoint=0x2 Foo.efi' "$WORK/ct-dxe.sh" > "$WORK/ct-dxe2.out" 2>&1
same "CONTROL: no ASSERT, the test carries on" "$?|$(grep -c SURVIVED "$WORK/ct-dxe2.out")" "0|1"

child ct-twice.sh AARCH64 "$BOOTED
export STUB_FWDEBUG='first launch'
test_run_foreground 10 > /dev/null
export STUB_FWDEBUG='second launch'
test_run_foreground 10 > /dev/null
tr -d '\r' < \"\$TEST_FW_DEBUG_LOG\""
"$WORK/ct-twice.sh" > "$WORK/ct-twice.out" 2>&1
same "booted: each launch's debug log holds that launch's output alone (truncated, as TEST_LOG is)" \
    "$(grep -E 'launch$' "$WORK/ct-twice.out")" "second launch"

child ct-exc.sh AARCH64 "$BOOTED
test_run_foreground 10 > /dev/null
test_clean_log
cat \"\$TEST_CLEAN_LOG\""
STUB_CONSOLE='Synchronous Exception at 0x000000005C5E1234' STUB_FWDEBUG='  SP 0x1  ELR 0x2' \
    "$WORK/ct-exc.sh" > "$WORK/ct-exc.out" 2>&1
same "booted: on a firmware exception the debug UART's log is appended to TEST_LOG (the dump's registers)" \
    "$(sed -n '/^Synchronous Exception/,$p' "$WORK/ct-exc.out")" \
'Synchronous Exception at 0x000000005C5E1234
--- firmware debug UART ---
  SP 0x1  ELR 0x2'
STUB_FWDEBUG='  SP 0x1  ELR 0x2' "$WORK/ct-exc.sh" > "$WORK/ct-exc2.out" 2>&1
same "...not on a run with no exception" "$(grep -c 'firmware debug UART' "$WORK/ct-exc2.out")" 0

FWCHK='check() { local d="$1"; shift; if "$@"; then echo "PASS: $d"; else echo "FAIL: $d"; fi; }
test_capture_firmware_debug
'"$BOOTED"'
test_run_foreground 10 > /dev/null
test_check_firmware_asserts check TcpDxe'
child ct-fw.sh AARCH64 "$FWCHK"
STUB_FWDEBUG=$'Loading driver at 0x1 EntryPoint=0x2 TcpDxe.efi\r\nASSERT [TcpDxe] LinkedList.c(594): x' \
    "$WORK/ct-fw.sh" > "$WORK/ct-fw.out" 2>&1
same "test_check_firmware_asserts, AARCH64 booted: the debug UART's ASSERT is seen" \
    "$(grep -E '^(PASS|FAIL): ' "$WORK/ct-fw.out")" \
"PASS: no firmware ASSERT on the serial log
PASS: CONTROL: the debug UART capture holds the boot's DEBUG output
FAIL: no firmware ASSERT on the debug UART"
STUB_FWDEBUG='Loading driver at 0x1 EntryPoint=0x2 TcpDxe.efi' "$WORK/ct-fw.sh" > "$WORK/ct-fw2.out" 2>&1
same "...CONTROL: with no ASSERT, all three pass" "$(grep -cE '^PASS: ' "$WORK/ct-fw2.out")" 3
"$WORK/ct-fw.sh" > "$WORK/ct-fw3.out" 2>&1
same "...an empty capture fails its CONTROL rather than passing blind" \
    "$(grep -E '^(PASS|FAIL): CONTROL' "$WORK/ct-fw3.out")" \
    "FAIL: CONTROL: the debug UART capture holds the boot's DEBUG output"

child ct-xfw.sh X64 "${FWCHK//qemu-system-aarch64/qemu-system-x86_64}"
STUB_FWDEBUG=$'Loading driver at 0x1 EntryPoint=0x2 TcpDxe.efi\r\nASSERT [TcpDxe] LinkedList.c(594): x' \
    "$WORK/ct-xfw.sh" > "$WORK/ct-xfw.out" 2>&1
same "test_check_firmware_asserts, X64 booted: the debugcon's ASSERT is seen" \
    "$(grep -E '^(PASS|FAIL): ' "$WORK/ct-xfw.out")" \
"PASS: no firmware ASSERT on the serial log
PASS: CONTROL: the debugcon capture holds the firmware's DEBUG output (TcpDxe loaded)
FAIL: no firmware ASSERT on the debugcon"

# A fake lease: the guest's state dir holds its console and, beside it,
# firmware-debug.log -- an AARCH64 guest's debug UART, an X64 guest's
# debugcon -- which already holds what the firmware wrote before this test's
# run (its boot's driver loads, and stale ASSERTs). The stub exec writes the
# run's console and DEBUG output.
printf 'echo hi\n' > "$WORK/ct-pool.nsh"
POOLED='_test_pool_exec() {
    case "$2" in
        axl-pool-run.nsh)
            printf "hi\r\n" >> "$AXL_POOL_SERIAL"
            [[ -n "${STUB_FWDEBUG:-}" ]] && printf "%s\r\n" "$STUB_FWDEBUG" \
                >> "$(dirname "$AXL_POOL_SERIAL")/firmware-debug.log"
            sleep 0.3 ;;
    esac
    return 0
}
test_use_nsh
test_build_image
test_build_qemu_cmd'
printf '#!/bin/bash\n[[ "$1" == restore ]] && exit 0\nexit 2\n' > "$WORK/axlpool-stub"
chmod +x "$WORK/axlpool-stub"
# pooled NAME OUT: run the child under the fake lease, a $PARCH guest.
pooled() {
    AXL_POOL_AGENT=127.0.0.1:1 AXL_POOL_ARCH="$PARCH" AXL_POOL_CLASS=standard \
        AXL_POOL_CLI="$WORK/axlpool-stub" AXL_POOL_GUEST_DIR='fs1:\lease-1' \
        AXL_POOL_HOST_DIR="$WORK/lease" AXL_POOL_SERIAL="$WORK/gstate/serial-1.log" \
        AXL_POOL_SERIAL_OFFSET="$(stat -c %s "$WORK/gstate/serial-1.log")" \
        timeout 60 "$WORK/$1" > "$WORK/$2" 2>&1
}
for PARCH in AARCH64 X64; do
echo "=== common-test.sh: pooled ($PARCH) ==="
rm -rf "$WORK/lease" "$WORK/gstate"
mkdir -p "$WORK/lease" "$WORK/gstate"
: > "$WORK/gstate/serial-1.log"
printf 'Loading driver at 0x1 EntryPoint=0x2 TcpDxe.efi\r\nASSERT [DxeCore] before-the-run.c(1): stale\r\nASSERT [TcpDxe] before.c(2): stale\r\n' \
    > "$WORK/gstate/firmware-debug.log"
if [[ "$PARCH" == X64 ]]; then
    CONTROL_LINE="CONTROL: the leased guest's debugcon log holds its boot's DEBUG output (TcpDxe loaded)
PASS: CONTROL: the run's part of the debugcon log holds its own image loads"
    NOASSERT_LINE="no firmware ASSERT on the debugcon"
    RUN_LOAD='Loading driver at 0x3 EntryPoint=0x4 ct-poolfw.efi'
else
    CONTROL_LINE="CONTROL: the leased guest's debug UART log holds its boot's DEBUG output"
    NOASSERT_LINE="no firmware ASSERT on the debug UART"
    RUN_LOAD=''
fi
child ct-pool.sh "$PARCH" "$POOLED
test_run_foreground 10 > /dev/null
echo SURVIVED
echo \"SLICE=\$(tr -d '\r' < \"\$TEST_FW_DEBUG_LOG\")\""
STUB_FWDEBUG='Loading driver at 0x1 EntryPoint=0x2 ct-pool.efi' pooled ct-pool.sh ct-pool.out
rc=$?
same "$PARCH pooled: the run's DEBUG output only -- not the stale ASSERTs before it -- and the test carries on" \
    "$rc|$(grep -c SURVIVED "$WORK/ct-pool.out")|$(sed -n 's/^SLICE=//p' "$WORK/ct-pool.out")" \
    "0|1|Loading driver at 0x1 EntryPoint=0x2 ct-pool.efi"
STUB_FWDEBUG='ASSERT [DxeCore] /e/Pool.c(721): during' pooled ct-pool.sh ct-pool2.out
rc=$?
same "$PARCH pooled: a DXE-core ASSERT during the run fails the test" \
    "$rc|$(grep -c SURVIVED "$WORK/ct-pool2.out")|$(grep -A1 -F '*** FAIL: firmware DXE-core ASSERT' "$WORK/ct-pool2.out")" \
"1|0|*** FAIL: firmware DXE-core ASSERT (pool/heap corruption) in ct-pool.sh:
    ASSERT [DxeCore] /e/Pool.c(721): during"

child ct-poolfw.sh "$PARCH" "check() { local d=\"\$1\"; shift; if \"\$@\"; then echo \"PASS: \$d\"; else echo \"FAIL: \$d\"; fi; }
test_capture_firmware_debug
$POOLED
test_run_foreground 10 > /dev/null
test_check_firmware_asserts check TcpDxe"
cp "$WORK/ct-pool.nsh" "$WORK/ct-poolfw.nsh"
STUB_FWDEBUG="$RUN_LOAD" pooled ct-poolfw.sh ct-poolfw.out
same "$PARCH pooled test_check_firmware_asserts: a run that printed nothing of note passes, its CONTROL the guest's boot output" \
    "$(grep -E '^(PASS|FAIL): ' "$WORK/ct-poolfw.out")" \
"PASS: no firmware ASSERT on the serial log
PASS: $CONTROL_LINE
PASS: $NOASSERT_LINE"
STUB_FWDEBUG="${RUN_LOAD:+$RUN_LOAD$'\r\n'}ASSERT [TcpDxe] LinkedList.c(594): x" pooled ct-poolfw.sh ct-poolfw2.out
same "...an ASSERT during the run is seen" \
    "$(grep -E '^(PASS|FAIL): ' "$WORK/ct-poolfw2.out")" \
"PASS: no firmware ASSERT on the serial log
PASS: $CONTROL_LINE
FAIL: $NOASSERT_LINE"
cp "$WORK/gstate/firmware-debug.log" "$WORK/gstate/keep.log"
: > "$WORK/gstate/firmware-debug.log"
STUB_FWDEBUG="$RUN_LOAD" pooled ct-poolfw.sh ct-poolfw3.out
# Every CONTROL line: on X64 the run's-part one still passes (they are apart).
want3="FAIL: ${CONTROL_LINE%%$'\n'*}"
[[ "$PARCH" == X64 ]] && want3+=$'\n'"PASS: CONTROL: the run's part of the debugcon log holds its own image loads"
same "...and a guest whose log is empty fails its CONTROL rather than passing blind" \
    "$(grep -E '^(PASS|FAIL): CONTROL' "$WORK/ct-poolfw3.out")" "$want3"
mv "$WORK/gstate/keep.log" "$WORK/gstate/firmware-debug.log"
if [[ "$PARCH" == X64 ]]; then
    pooled ct-poolfw.sh ct-poolfw4.out
    same "...and an X64 run whose own part holds no image load (a blind stream) fails its second CONTROL" \
        "$(grep -E '^(PASS|FAIL): ' "$WORK/ct-poolfw4.out")" \
"PASS: no firmware ASSERT on the serial log
PASS: CONTROL: the leased guest's debugcon log holds its boot's DEBUG output (TcpDxe loaded)
FAIL: CONTROL: the run's part of the debugcon log holds its own image loads
PASS: no firmware ASSERT on the debugcon"
fi

mv "$WORK/gstate/firmware-debug.log" "$WORK/gstate/gone.log"
pooled ct-pool.sh ct-pool3.out
rc=$?
same "$PARCH pooled: a lease with no debug log is a pool miss (the test reruns booted), never a blind pass" \
    "$rc|$(grep -c "^AXLPOOL-MISS: ct-pool.sh: the leased $PARCH guest has no firmware debug log" "$WORK/ct-pool3.out")" \
    "77|1"
done

printf 'fw-debug-uart: %d passed, %d failed\n' "$PASS" "$FAIL"
[[ $FAIL -eq 0 && $PASS -gt 0 ]]
