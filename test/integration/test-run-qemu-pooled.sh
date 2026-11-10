#!/bin/bash
# test-meta: arch=none needs= est=40 local-only=0 guest=none
# guest=none: harness test: run-qemu.sh's pooled mode and its booted
#   fallbacks, against a stub pool CLI and stub QEMU.
# test-run-qemu-pooled.sh -- scripts/run-qemu.sh's pooled mode (its header,
# "Pooled mode"; docs/AXL-Test-Guest-Pool-Design.md §6.9), host-only.
#
# A fake lease stands in for the pool: AXL_POOL_* as `axlpool run` sets them,
# and AXL_POOL_CLI pointing at a stub of the axlpool command line whose
# `exec` runs the staged script's `echo` lines and "runs" every other line
# by printing it (with ANSI codes, as a console has) into the lease's serial
# log, and whose `restore` answers as told. A stub QEMU stands in for a boot.
# Pinned, exactly:
#   - a pooled app run: the guest restored first, ONE exec of the script
#     staged in a fresh directory of the share, the script's text, the output
#     a booted run prints (ANSI-stripped, from the sentinel), exit 0, the
#     TIMING line mode=pooled -- and nothing of either in the caller's output;
#   - --setvar, --extra (both forms) and pci-ids.json5 staged beside the app;
#     a second run gets a new directory, the first one's removed;
#   - --raw: the console from the run's start, sentinel included;
#   - a --nsh script: the boilerplate dropped, the rest run as written;
#   - every machine or script the pool cannot give BOOTS, saying why, one
#     line each into the timing log -- the decision table in the header;
#   - a pool failure boots the run: a refused restore (an older service), the
#     exec's 77, no sentinel on the console;
#   - a run cut off at --timeout (exec 124) exits 0 with what it printed, as
#     a booted run does;
#   - the CPU check samples AXL_POOL_QEMU_PID for the run only (a spinning
#     "guest" fails it with exit 8; an idle one reports 0 with no warm-up).
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

WORK="$(mktemp -d -t axl-rqpool.XXXXXXXX)"
IDLE_PID="" SPIN_PID=""
cleanup() {
    [[ -n "$IDLE_PID" ]] && kill "$IDLE_PID" 2>/dev/null
    [[ -n "$SPIN_PID" ]] && kill "$SPIN_PID" 2>/dev/null
    pkill -f "[q]emu-system-x86_64 .*$WORK" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# The stub QEMU: a boot prints the sentinel (from its startup.nsh) and BOOTED.
mkdir -p "$WORK/stub"
cat > "$WORK/stub/qemu-system-x86_64" <<'EOF'
#!/bin/bash
echo "UEFI Interactive Shell v2.2"
echo "___AXL_APP_OUTPUT_BEGIN___"
[[ "${STUB_ONLY_SENTINEL:-0}" == 1 ]] || echo "BOOTED"
sleep "${STUB_QEMU_LINGER:-0}"
EOF
chmod +x "$WORK/stub/qemu-system-x86_64"
cp "$WORK/stub/qemu-system-x86_64" "$WORK/stub/qemu-system-aarch64"

# The stub axlpool: every call logged; exec "runs" the staged script.
cat > "$WORK/axlpool" <<'EOF'
#!/bin/bash
printf '%s\n' "$*" >> "$STUB_LOG"
case "$1" in
    restore)
        if [[ "${STUB_RESTORE_RC:-0}" == 0 ]]; then
            echo "axlpool restore: restored in 5 ms"
            exit 0
        fi
        echo "axlpool restore: unknown-op: no op 'restore'" >&2
        exit "$STUB_RESTORE_RC" ;;
    exec)
        shift
        while [[ "$1" != -- ]]; do shift; done
        shift
        line="$*"
        rel="${line#*\\lease-1\\}"
        script="$AXL_POOL_HOST_DIR/${rel//\\//}"
        cp "$script" "$STUB_SCRIPT_COPY"
        if [[ "${STUB_ONLY_SENTINEL:-0}" == 1 ]]; then
            printf '___AXL_APP_OUTPUT_BEGIN___\r\n' >> "$AXL_POOL_SERIAL"
        elif [[ "${STUB_SILENT:-0}" != 1 ]]; then
            skip=0
            while IFS= read -r l; do
                # The probe block: its body runs only when the share did not
                # deliver the app (STUB_APP_MISSING=1).
                case "$l" in
                    "if not exist "*) [[ "${STUB_APP_MISSING:-0}" == 1 ]] || skip=1; continue ;;
                    endif) skip=0; continue ;;
                esac
                [[ $skip == 1 ]] && continue
                l="${l#  }"
                case "$l" in
                    "echo "*) printf '%s\r\n' "${l#echo }" ;;
                    ""|@echo*|@cd*|fs1:|"cd "*|"set "*) ;;
                    *) printf '\e[1mRAN: %s\e[0m\r\n' "$l"
                       [[ -n "${STUB_APP_ECHO:-}" ]] && printf '%s\r\n' "$STUB_APP_ECHO" ;;
                esac
            done < "$script" >> "$AXL_POOL_SERIAL"
        fi
        # The firmware's DEBUG output during the run: an AARCH64 guest's
        # debug UART, an X64 guest's debugcon (its log beside the console's).
        [[ -n "${STUB_FWDEBUG:-}" ]] \
            && printf '%s\r\n' "$STUB_FWDEBUG" >> "$(dirname "$AXL_POOL_SERIAL")/firmware-debug.log"
        # As the real client: one process, so a TERM ends all of it.
        trap 'kill $! 2>/dev/null; exit 143' TERM
        sleep "${STUB_EXEC_SLEEP:-0}" &
        wait $!
        [[ "${STUB_EXEC_RC:-0}" == 77 ]] && echo "axlpool exec: outcome not-run" >&2
        exit "${STUB_EXEC_RC:-0}" ;;
esac
exit 2
EOF
chmod +x "$WORK/axlpool"

printf 'MZ' > "$WORK/probe.efi"
printf 'extra\n' > "$WORK/data.txt"
sleep 3600 & IDLE_PID=$!

# The fake lease: a plain X64 guest (its class's run-qemu.sh options: none),
# its firmware DEBUG log (the debugcon) beside its console, as the pool's are.
export AXL_POOL_AGENT=127.0.0.1:1 AXL_POOL_ARCH=X64 AXL_POOL_CLASS=plain AXL_POOL_CLASS_ARGS= \
       AXL_POOL_GUEST_DIR='fs1:\lease-1' AXL_POOL_HOST_DIR="$WORK/lease" \
       AXL_POOL_SERIAL="$WORK/serial.log" AXL_POOL_SERIAL_OFFSET=0 \
       AXL_POOL_LEASE=L1 AXL_POOL_RESTORE_TOKEN=rt AXL_POOL_SOCKET="$WORK/none.sock" \
       AXL_POOL_QEMU_PID="$IDLE_PID" AXL_POOL_CLI="$WORK/axlpool" QEMU_DIR="$WORK/stub" \
       STUB_LOG="$WORK/stub.log" STUB_SCRIPT_COPY="$WORK/script.nsh"
mkdir -p "$WORK/lease"
: > "$WORK/serial.log"
printf 'Loading driver at 0x1 EntryPoint=0x2 TcpDxe.efi\r\n' > "$WORK/firmware-debug.log"

RQ=("$PROJECT_DIR/scripts/run-qemu.sh" --no-cpu-warn --timeout 20)
# rq NAME ARGS...: one run, its stdout+stderr in $WORK/NAME.out (as a caller
# captures it), its timing lines in $WORK/NAME.timing, its exit in RC.
rq() {
    local name="$1"
    shift
    : > "$WORK/stub.log"
    rm -f "$WORK/script.nsh"
    # Bounded: a sampler that never stopped (the leased guest outlives the
    # run) would hang the run, and must read as this test's failure.
    AXL_TEST_TIMING_LOG="$WORK/$name.timing" timeout 60 "${RQ[@]}" "$@" > "$WORK/$name.out" 2>&1
    RC=$?
}
# timing NAME: the run's timing lines, seconds as S -- and a booted run's boot
# reading as B, read or not (the stub's banner races its exit; a booted run's
# reading is test-timing-lines.sh's subject, not this test's).
timing() {
    sed -E -e 's/[0-9]+\.[0-9]{3}/S/g' -e '/mode=fg /s/ boot=(S|-) / boot=B /' "$WORK/$1.timing" 2>/dev/null
}
# out NAME: the run's captured output less the Shell-extraction notes a
# booted run prints when it finds the firmware's Shell (no part of this mode).
out() { grep -v 'Extracted Shell.efi' "$WORK/$1.out"; }

echo "=== a pooled app run ==="
rq a --setvar AXL_LOG_LEVEL=debug --extra "$WORK/data.txt" --extra "$WORK/data.txt:sub/d.txt" \
    --extra "$WORK/probe.efi:sub/t.efi" "$WORK/probe.efi" one two
same "exit 0" "$RC" 0
same "restored first, then ONE exec of the staged script on the console" "$(cat "$WORK/stub.log")" \
'restore
exec --timeout 20 --console -- fs1:\lease-1\rq-1\run-qemu.nsh'
# The path a boot's Shell has is its disk's root (FS0:\efi\tools\;
# FS0:\efi\boot\;FS0:\), so a bare `tool` finds tool.efi there: the run's
# own directory stands in for it.
same "the script: the volume, directory and path, the --setvar, the sentinel, the app" \
    "$(cat "$WORK/script.nsh" 2>/dev/null)" \
'@echo -off
fs1:
cd \lease-1\rq-1
set -v path fs1:\lease-1\rq-1\efi\tools\;fs1:\lease-1\rq-1\efi\boot\;fs1:\lease-1\rq-1\
set AXL_LOG_LEVEL debug
if not exist fs1:\lease-1\rq-1\probe.efi then
  echo AXL-POOL-NO-APP
endif
echo ___AXL_APP_OUTPUT_BEGIN___
probe.efi one two
@cd .'
same "the output a booted run prints: from the sentinel, ANSI-stripped" "$(cat "$WORK/a.out")" \
'RAN: probe.efi one two'
# The Shell finds a bare `tool` as tool.EFI -- the extension upper case --
# which FAT matches and the case-sensitive share does not: every staged .efi
# is reachable by that name too (a hard link), as on the boot disk.
same "staged beside the app: the --extra files (both forms), pci-ids.json5, each .efi as .EFI too" \
    "$(cd "$WORK/lease/rq-1" && find . -type f | LC_ALL=C sort)" \
'./data.txt
./pci-ids.json5
./probe.EFI
./probe.efi
./run-qemu.nsh
./sub/d.txt
./sub/t.EFI
./sub/t.efi'
same "the TIMING line says mode=pooled (restore its share)" "$(timing a)" \
'TIMING: run-qemu mode=pooled guest=S boot=- restore=S rc=0'

echo "=== a second run: a fresh directory, the first removed ==="
rq b "$WORK/probe.efi"
same "rq-2 staged, rq-1 gone; restored again" \
    "$(ls "$WORK/lease"; cat "$WORK/stub.log")" \
'rq-2
restore
exec --timeout 20 --console -- fs1:\lease-1\rq-2\run-qemu.nsh'

echo "=== --raw ==="
rq raw --raw "$WORK/probe.efi"
same "--raw: the console from the run's start, sentinel included" "$(cat "$WORK/raw.out")" \
'___AXL_APP_OUTPUT_BEGIN___
RAN: probe.efi'

echo "=== a --nsh script ==="
printf '@echo -off\r\nfs0:\r\ncd \\\r\necho one\r\nprobe.efi x\r\n\r\nreset -s\r\n' > "$WORK/boiler.nsh"
rq n --nsh "$WORK/boiler.nsh" "$WORK/probe.efi"
same "the boilerplate dropped, the rest as written, the app staged beside it" \
    "$(cat "$WORK/script.nsh" 2>/dev/null; LC_ALL=C ls "$WORK/lease/rq-4")" \
'@echo -off
fs1:
cd \lease-1\rq-4
set -v path fs1:\lease-1\rq-4\efi\tools\;fs1:\lease-1\rq-4\efi\boot\;fs1:\lease-1\rq-4\
if not exist fs1:\lease-1\rq-4\probe.efi then
  echo AXL-POOL-NO-APP
endif
echo ___AXL_APP_OUTPUT_BEGIN___
echo one
probe.efi x

@cd .
pci-ids.json5
probe.EFI
probe.efi
run-qemu.nsh'
same "...and its output" "$(cat "$WORK/n.out")" \
'one
RAN: probe.efi x'
same "...pooled" "$(timing n)" 'TIMING: run-qemu mode=pooled guest=S boot=- restore=S rc=0'
# Without `fs0:` a boot's startup.nsh has no current directory on its disk (a
# relative name finds nothing), so the run does not enter its own either --
# the Shell stays where the clean guest left it; only the path finds the app.
printf '@echo -off\necho two\n' > "$WORK/nofs0.nsh"
rq n2 --nsh "$WORK/nofs0.nsh" "$WORK/probe.efi"
same "a --nsh without fs0: the run's directory on the path, not entered" \
    "$(cat "$WORK/script.nsh" 2>/dev/null)" \
'@echo -off
set -v path fs1:\lease-1\rq-5\efi\tools\;fs1:\lease-1\rq-5\efi\boot\;fs1:\lease-1\rq-5\
if not exist fs1:\lease-1\rq-5\probe.efi then
  echo AXL-POOL-NO-APP
endif
echo ___AXL_APP_OUTPUT_BEGIN___
echo two
@cd .'

echo "=== what boots, and says why ==="
# booted NAME REASON ARGS...: the run boots (the stub QEMU's output, no
# axlpool call at all) and the timing log says REASON.
booted() {
    local name="$1" reason="$2"
    shift 2
    rq "$name" "$@"
    local got
    got="$RC|$(out "$name")|$(cat "$WORK/stub.log")|$(timing "$name")"
    same "boots: $reason" "$got" \
"0|BOOTED||run-qemu: booted inside a $AXL_POOL_CLASS lease: $reason
TIMING: run-qemu mode=fg guest=S boot=B rc=0"
}
# A machine is compared with the lease's class (AXL_POOL_CLASS_ARGS, parsed
# by run-qemu.sh's own option loop): the first fact that differs is said.
booted gpu "the machine is not the plain class's: the run has gpu=true, the class gpu=false" \
    --gpu "$WORK/probe.efi"
booted qarg "the machine is not the plain class's: the run has qemu-arg=-no-hpet, the class nothing" \
    --qemu-arg -no-hpet "$WORK/probe.efi"
booted cpu "the machine is not the plain class's: the run has cpu=qemu64, the class cpu=" \
    --cpu qemu64 "$WORK/probe.efi"
booted mem "the machine is not the plain class's: the run has mem=768M, the class mem=512M" \
    --mem 768M "$WORK/probe.efi"
booted bridges "the machine is not the plain class's: the run has bridges=true, the class bridges=false" \
    --bridges "$WORK/probe.efi"
booted std "the machine is not the plain class's: the run has bridges=true, the class bridges=false" \
    --bridges --net --nic-model e1000 "$WORK/probe.efi"
booted nic "the machine is not the plain class's: the run has nic=e1000, the class nic=virtio-net-pci" \
    --net --nic-model e1000 "$WORK/probe.efi"
booted mac "the machine is not the plain class's: the run has mac=52:54:00:12:34:99, the class mac=" \
    --mac 52:54:00:12:34:99 "$WORK/probe.efi"
# A run inside a lease with no class options to compare (a wrapper that does
# not export them) cannot be judged: it boots.
unset AXL_POOL_CLASS_ARGS
booted noargs "the lease names no machine (no AXL_POOL_CLASS_ARGS)" "$WORK/probe.efi"
export AXL_POOL_CLASS_ARGS=
booted arch "--arch AARCH64; the lease is X64" --arch AARCH64 "$WORK/probe.efi"
AXL_RUN_QEMU_POOL=0 booted optout "AXL_RUN_QEMU_POOL=0" "$WORK/probe.efi"
# common-test.sh's pooled background run is live: its server is on the guest,
# and the restore would end it.
AXL_POOL_BG_LIVE=1 booted bglive "the test's own background run holds the leased guest" "$WORK/probe.efi"
printf 'echo hi\n' > "$WORK/noecho.nsh"
booted noecho "its --nsh does not start with @echo -off (a boot echoes each command)" \
    --nsh "$WORK/noecho.nsh" "$WORK/probe.efi"
printf '@echo -off\necho hi\nreset\n' > "$WORK/reset.nsh"
booted reset "its --nsh runs reset (line 3)" --nsh "$WORK/reset.nsh" "$WORK/probe.efi"
printf '@echo -off\nif x == y then\n  exit 1\nendif\n' > "$WORK/exit.nsh"
booted exit "its --nsh runs exit (line 3)" --nsh "$WORK/exit.nsh" "$WORK/probe.efi"
printf '@echo -off\nfs0:\ncd \\\nFS0:\\probe.efi\n' > "$WORK/fs0.nsh"
booted fs0 "its --nsh names fs0: (line 4)" --nsh "$WORK/fs0.nsh" "$WORK/probe.efi"
# --net alone IS the plain machine (the class's NIC is run-qemu.sh's default).
rq net --net "$WORK/probe.efi"
same "--net alone: pooled on the plain class" "$RC|$(cat "$WORK/net.out")|$(timing net)" \
'0|RAN: probe.efi|TIMING: run-qemu mode=pooled guest=S boot=- restore=S rc=0'

echo "=== a one-off class: the run's machine IS the class's ==="
# pooled NAME ARGS...: the run pooled -- restored, executed, mode=pooled.
pooled() {
    local name="$1"
    shift
    rq "$name" "$@"
    same "pooled on the $AXL_POOL_CLASS class: $name" \
        "$RC|$(cat "$WORK/$name.out")|$(sed -n 1p "$WORK/stub.log")|$(timing "$name")" \
        '0|RAN: probe.efi|restore|TIMING: run-qemu mode=pooled guest=S boot=- restore=S rc=0'
}
SMP4=$'--qemu-arg\n-smp\n--qemu-arg\n4'
AXL_POOL_CLASS=smp4 AXL_POOL_CLASS_ARGS="$SMP4" pooled smp4 --qemu-arg -smp --qemu-arg 4 "$WORK/probe.efi"
# Controls: one word different, the words reordered, the option missing.
AXL_POOL_CLASS=smp4 AXL_POOL_CLASS_ARGS="$SMP4" booted smp2 \
    "the machine is not the smp4 class's: the run has qemu-arg=2, the class qemu-arg=4" \
    --qemu-arg -smp --qemu-arg 2 "$WORK/probe.efi"
AXL_POOL_CLASS=smp4 AXL_POOL_CLASS_ARGS="$SMP4" booted smpord \
    "the machine is not the smp4 class's: the run has qemu-arg=4, the class qemu-arg=-smp" \
    --qemu-arg 4 --qemu-arg -smp "$WORK/probe.efi"
AXL_POOL_CLASS=smp4 AXL_POOL_CLASS_ARGS="$SMP4" booted smpnone \
    "the machine is not the smp4 class's: the run has nothing, the class qemu-arg=-smp" \
    "$WORK/probe.efi"
AXL_POOL_CLASS=gpu AXL_POOL_CLASS_ARGS=--gpu pooled gpuc --gpu "$WORK/probe.efi"
AXL_POOL_CLASS=cpu AXL_POOL_CLASS_ARGS=$'--cpu\nHaswell' pooled cpuc --cpu Haswell "$WORK/probe.efi"
# A class extending standard: --bridges and X64's NIC. A run with --bridges
# names its NIC (no NIC there is another machine); --nic-model alone implies
# --net, as booted.
STD=$'--bridges\n--nic-model\ne1000'
AXL_POOL_CLASS=std AXL_POOL_CLASS_ARGS="$STD" pooled std1 --bridges --net --nic-model e1000 "$WORK/probe.efi"
AXL_POOL_CLASS=std AXL_POOL_CLASS_ARGS="$STD" pooled std2 --bridges --nic-model e1000 "$WORK/probe.efi"
AXL_POOL_CLASS=std AXL_POOL_CLASS_ARGS="$STD" booted stdnonic \
    "the machine is not the std class's: the run has nic=none, the class nic=e1000" \
    --bridges "$WORK/probe.efi"
AXL_POOL_CLASS=bad AXL_POOL_CLASS_ARGS=--mem booted badargs \
    "run-qemu.sh could not read the bad class's options" "$WORK/probe.efi"
# NIC_NO_ROM in the environment shapes a booted run's NIC: it is the run's
# machine, never inherited by the class's parse.
NIC_NO_ROM=true booted envrom \
    "the machine is not the plain class's: the run has nic-no-rom=true, the class nic-no-rom=false" \
    "$WORK/probe.efi"
GFW="tcp:10.0.2.100:9997-cmd:sh -c 'cat > /dev/null'"
AXL_POOL_CLASS=fwd AXL_POOL_CLASS_ARGS=$'--guestfwd\n'"$GFW" pooled gfw --guestfwd "$GFW" "$WORK/probe.efi"
# Options that change how a run is DRIVEN, not its machine, still boot.
AXL_POOL_CLASS=gpu AXL_POOL_CLASS_ARGS=--gpu booted dcon "--debugcon: not done on a pooled guest" \
    --gpu --debugcon "$WORK/d.log" "$WORK/probe.efi"

# A dry run never pools: it prints the command and launches nothing.
QEMU_DRYRUN=1 rq dry "$WORK/probe.efi"
same "QEMU_DRYRUN: no axlpool call" "$(cat "$WORK/stub.log")" ""

echo "=== a pool failure boots the run ==="
STUB_RESTORE_RC=1 rq norestore "$WORK/probe.efi"
same "a refused restore (an older service): booted, said, nothing executed" \
    "$RC|$(out norestore)|$(cat "$WORK/stub.log")|$(timing norestore)" \
"0|BOOTED|restore|run-qemu: booted inside a plain lease: the pool could not restore the guest (exit 1: axlpool restore: unknown-op: no op 'restore')
TIMING: run-qemu mode=fg guest=S boot=B rc=0"
STUB_EXEC_RC=77 rq miss77 "$WORK/probe.efi"
same "exec 77: booted, said" "$RC|$(out miss77)|$(timing miss77)" \
"0|BOOTED|run-qemu: booted inside a plain lease: the agent could not run the script (exit 77: axlpool exec: outcome not-run)
TIMING: run-qemu mode=fg guest=S boot=B rc=0"
# The run's `@cd .` hides the status a share that delivered no app would
# leave, so the script probes for the app itself: its marker is a pool miss.
STUB_APP_MISSING=1 rq noapp "$WORK/probe.efi"
same "the app missing on the guest (the marker): booted, said" "$RC|$(out noapp)|$(timing noapp)" \
"0|BOOTED|run-qemu: booted inside a plain lease: the guest did not find probe.efi in its run directory
TIMING: run-qemu mode=fg guest=S boot=B rc=0"
# A --nsh run names its app too, and probes it by full path (the run need not
# enter its directory).
STUB_APP_MISSING=1 rq noappnsh --nsh "$WORK/nofs0.nsh" "$WORK/probe.efi"
same "a --nsh run whose app is missing: booted, said" "$RC|$(out noappnsh)|$(timing noappnsh)" \
"0|BOOTED|run-qemu: booted inside a plain lease: the guest did not find probe.efi in its run directory
TIMING: run-qemu mode=fg guest=S boot=B rc=0"
# Only the probe's line BEFORE the sentinel counts: an app printing the same
# words is the app's output, not a pool failure.
STUB_APP_ECHO=AXL-POOL-NO-APP rq appecho "$WORK/probe.efi"
same "the marker in the app's own output: still pooled, printed" "$RC|$(out appecho)|$(timing appecho)" \
"0|RAN: probe.efi
AXL-POOL-NO-APP|TIMING: run-qemu mode=pooled guest=S boot=- restore=S rc=0"
STUB_SILENT=1 rq silent "$WORK/probe.efi"
same "no sentinel on the console: booted, said" "$RC|$(out silent)|$(timing silent)" \
"0|BOOTED|run-qemu: booted inside a plain lease: no sentinel on the guest's console
TIMING: run-qemu mode=fg guest=S boot=B rc=0"

echo "=== --timeout ==="
STUB_EXEC_RC=124 rq tmo "$WORK/probe.efi"
same "exec 124: exit 0 with what it printed, as a booted run cut off" \
    "$RC|$(cat "$WORK/tmo.out")|$(timing tmo)" \
'0|RAN: probe.efi|TIMING: run-qemu mode=pooled guest=S boot=- restore=S rc=124'

echo "=== the CPU check samples the leased guest's QEMU ==="
RQ=("$PROJECT_DIR/scripts/run-qemu.sh" --timeout 20)
rq rep --cpu-report "$WORK/probe.efi"
same "an idle guest: reported idle, no boot warm-up" "$RC|$(cat "$WORK/rep.out")" \
'0|RAN: probe.efi
CPU-REPORT: mean 0.000 cores, peak 0.00 cores (after 0s warm-up)'
( while :; do :; done ) & SPIN_PID=$!
CPU_WARN=true AXL_POOL_QEMU_PID="$SPIN_PID" STUB_EXEC_SLEEP=2 rq spin --cpu-threshold 0.5 --cpu-sustain 1 "$WORK/probe.efi"
same "a spinning guest: exit 8, the WARN said -- and the run ends though the guest lives on" \
    "$RC|$(grep -c '^WARN: CPU spike' "$WORK/spin.out")" '8|1'
kill "$SPIN_PID" 2>/dev/null
SPIN_PID=""
# A pool failure after the pooled attempt began: the boot that follows keeps
# its real firmware warm-up (the pooled run's 0 is its own).
# (The stub QEMU lingers so the boot's sampler finds it.)
STUB_EXEC_RC=77 STUB_QEMU_LINGER=1.5 rq warm --cpu-report "$WORK/probe.efi"
same "a pool failure's boot keeps the boot warm-up" "$RC|$(grep '^CPU-REPORT' "$WORK/warm.out")" \
'0|CPU-REPORT: mean 0.000 cores, peak 0.00 cores (after 10s warm-up)'

echo "=== exit codes as booted ==="
# An app that prints nothing: a booted run's output filter finds no line,
# and run-qemu.sh exits 1 -- a pooled run exits the same.
STUB_ONLY_SENTINEL=1 rq empty "$WORK/probe.efi"
rc_pooled=$RC
STUB_ONLY_SENTINEL=1 AXL_RUN_QEMU_POOL=0 rq empty-b "$WORK/probe.efi"
same "an app that prints nothing: pooled and booted exit alike (1)" \
    "$rc_pooled $(timing empty | grep -o 'mode=[a-z]*') | $RC $(timing empty-b | grep -o 'mode=[a-z]*')" \
    "1 mode=pooled | 1 mode=fg"

echo "=== a signal mid-run leaves nothing behind ==="
# Every process the run starts carries this mark in its environment.
mark="RQPOOL_MARK_$$"
: > "$WORK/stub.log"
# run-qemu.sh ALONE is signalled (not a process group: a group signal would
# reach the sentinel too and prove nothing about the cleanup).
env "$mark=1" STUB_EXEC_SLEEP=30 "${RQ[@]}" "$WORK/probe.efi" > "$WORK/sig.out" 2>&1 &
sigpid=$!
for _ in $(seq 1 50); do grep -q '^exec ' "$WORK/stub.log" 2>/dev/null && break; sleep 0.1; done
kill -TERM "$sigpid" 2>/dev/null
for _ in $(seq 1 100); do kill -0 "$sigpid" 2>/dev/null || break; sleep 0.1; done
kill -KILL "$sigpid" 2>/dev/null
wait "$sigpid" 2>/dev/null
sleep 0.5
left=$(grep -l "$mark=1" /proc/[0-9]*/environ 2>/dev/null | wc -l)
same "killed mid-exec: no sentinel, sampler or exec client of it left" "$left" "0"

echo "=== common-test.sh's pooled run after a pooled run-qemu.sh run ==="
# The guest is as run-qemu.sh's run left it (its rq-<n> is in the share):
# common-test.sh restores it before its own run and streams the console from
# then on -- the earlier run's output is not this run's log.
mkdir -p "$WORK/lease/rq-9"
printf 'EARLIER-RUN-OUTPUT\r\n' >> "$WORK/serial.log"
printf 'echo hi\n' > "$WORK/child-ct.nsh"
cat > "$WORK/child-ct.sh" <<EOF
#!/bin/bash
source '$SCRIPT_DIR/common-test.sh'
TEST_ARCH=X64
test_setup
_test_pool_exec() { case "\$2" in axl-pool-run.nsh) printf 'THIS-RUN\r\n' >> "\$AXL_POOL_SERIAL"; cp "\$AXL_POOL_HOST_DIR/axl-pool-run.nsh" '$WORK/ct-wrapper.nsh' ;; esac; return 0; }
test_use_nsh
test_build_image
test_run_foreground 10 > /dev/null
cp "\$TEST_LOG" '$WORK/ct.log'
EOF
chmod +x "$WORK/child-ct.sh"
: > "$WORK/stub.log"
AXL_POOL_CLASS=standard AXL_TEST_TIMING_LOG="$WORK/ct.timing" "$WORK/child-ct.sh" > "$WORK/ct.out" 2>&1
same "restored first, the run-qemu.sh directory gone, the log only this run's" \
    "$?|$(cat "$WORK/stub.log")|$(ls -d "$WORK"/lease/rq-* 2>/dev/null | wc -l)|$(sed -E 's/^\[[0-9.]+\] //' "$WORK/ct.log" 2>/dev/null | tr -d '\r')" \
    "0|restore|0|THIS-RUN"
# The agent is given a wrapper, not the test's line: a script whose last
# status is 0xE reads "not run" to the agent, so the wrapper ends in `@cd .`
# (status 0) -- and says itself when the test's script is missing.
same "the agent runs the wrapper: the script probed, the test's line, @cd . last (CRLF)" \
    "$(cat "$WORK/ct-wrapper.nsh" 2>/dev/null)" \
    "$(printf '@echo -off\r\nif not exist child-ct.nsh then\r\n  echo AXL-POOL-NO-SCRIPT\r\nendif\r\nchild-ct.nsh\r\n@cd .\r\n')"
# The wrapper's marker (the guest found no test script): a pool miss, 77.
cp "$WORK/child-ct.nsh" "$WORK/child-ns.nsh"
cat > "$WORK/child-ns.sh" <<EOF
#!/bin/bash
source '$SCRIPT_DIR/common-test.sh'
TEST_ARCH=X64
test_setup
_test_pool_exec() { printf 'AXL-POOL-NO-SCRIPT\r\n' >> "\$AXL_POOL_SERIAL"; return 0; }
test_use_nsh
test_build_image
test_run_foreground 10 > /dev/null
echo NOT-REACHED
EOF
chmod +x "$WORK/child-ns.sh"
AXL_POOL_CLASS=standard AXL_TEST_TIMING_LOG="$WORK/ns.timing" "$WORK/child-ns.sh" > "$WORK/ns.out" 2>&1
same "no test script on the guest: a pool miss (77), named, the test not continued" \
    "$?|$(grep -c '^AXLPOOL-MISS: child-ns.sh: the guest did not find child-ns.nsh in ' "$WORK/ns.out")|$(grep -c NOT-REACHED "$WORK/ns.out")" \
    "77|1|0"

# ...and found the same behind the escapes a UEFI console emits (a charset
# designator, a DEC private mode, an attribute reset) -- the same set
# run-qemu.sh's clean log strips, through the one axl_strip_escapes.
sed 's/AXL-POOL-NO-SCRIPT/\\e(B\\e[=3h\\e[0mAXL-POOL-NO-SCRIPT/' "$WORK/child-ns.sh" > "$WORK/child-ne.sh"
chmod +x "$WORK/child-ne.sh"
cp "$WORK/child-ct.nsh" "$WORK/child-ne.nsh"
grep -cF '\e(B\e[=3h\e[0mAXL-POOL-NO-SCRIPT' "$WORK/child-ne.sh" > "$WORK/ne.edited"
# Its OWN serial log: the shared one still holds the plain marker the case
# above wrote, which would satisfy the check whatever it does with escapes.
: > "$WORK/serial-ne.log"
AXL_POOL_CLASS=standard AXL_POOL_SERIAL="$WORK/serial-ne.log" AXL_TEST_TIMING_LOG="$WORK/ne.timing" \
    "$WORK/child-ne.sh" > "$WORK/ne.out" 2>&1
same "the marker behind a terminal escape: still a pool miss (77), named" \
    "$?|$(cat "$WORK/ne.edited")|$(grep -c '^AXLPOOL-MISS: child-ne.sh: the guest did not find child-ne.nsh in ' "$WORK/ne.out")|$(grep -c NOT-REACHED "$WORK/ne.out")" \
    "77|1|1|0"

echo "=== every run-qemu.sh option is classified for pooled mode ==="
RQS="$PROJECT_DIR/scripts/run-qemu.sh"
# The option loop's case labels, one per line.
LABELS=$(awk '/^while \[\[ \$# -gt 0 \]\]; do/ { on = 1 } on && /^done/ { exit }
              on && /^        -[-a-z|]*\)/ { sub(/^ +/, ""); sub(/\).*/, ""); print }' "$RQS" \
         | tr '|' '\n' | LC_ALL=C sort -u)
MACHINE="--bridges --nic-model --mac --nic-no-rom --net --gpu --no-gpu --headless --cpu --mem --guestfwd --qemu-arg"
DRIVEN="--screenshot --sendkey --sendmouse --holdkey --display --gui --vnc --vnc-reverse -i --interactive --mount --background --debugcon --gdb --gdb-halt --serial-socket --monitor-socket --qmp-socket --reboot --boot-target --shell --qcow2 --state-dir --hostfwd-bind --tap --hostfwd"
NEITHER="--arch --timeout --raw --extra --setvar --sendkey-after --sendkey-settle --screenshot-after --screenshot-after-count --nsh --serial-log --serial-log-raw --workload --max-duration --no-cpu-warn --cpu-report --cpu-threshold --cpu-sustain -h --help"
unclassified=""
for l in $LABELS; do
    n=0
    for w in $MACHINE $DRIVEN $NEITHER; do [[ "$w" == "$l" ]] && n=$((n + 1)); done
    [[ $n -eq 1 ]] || unclassified+=" $l($n)"
done
same "each option is in exactly one list (machine, driven, neither)" "${unclassified:-none}" "none"
same "...and the loop was read (a control: it has --qemu-arg and --screenshot)" \
    "$(grep -cxE -- '--qemu-arg|--screenshot' <<< "$LABELS")" "2"
# Every DRIVEN option really boots inside a lease (its refusal names it).
bad=""
for o in --mount --debugcon --serial-socket --reboot --boot-target --tap --hostfwd; do
    case "$o" in
        --mount) a=("$o" "$WORK") ;;
        --debugcon|--serial-socket) a=("$o" "$WORK/x.sock") ;;
        --tap) a=("$o" tap9) ;;
        --hostfwd) a=("$o" 18080:8080) ;;
        *) a=("$o") ;;
    esac
    QEMU_DRYRUN=0 rq "drv${o//-/_}" "${a[@]}" "$WORK/probe.efi"
    grep -qF -- "booted inside a plain lease: $o: not done on a pooled guest" \
        "$WORK/drv${o//-/_}.timing" || bad+=" $o"
done
same "the driven options boot inside a lease, each naming itself" "${bad:-none}" "none"

echo "=== common-test.sh: a test that boots twice gets a fresh machine and disk each time ==="
# A booted test that calls test_setup / test_build_image / test_run_* again
# boots a NEW guest from a NEW disk. Pooled, each run after the first restores
# the leased guest (axlpool restore) and streams the console from then, and a
# rebuilt image replaces the share's files rather than adding to them.
printf 'echo hi\n' > "$WORK/child-two.nsh"
cat > "$WORK/child-two.sh" <<EOF
#!/bin/bash
source '$SCRIPT_DIR/common-test.sh'
TEST_ARCH=X64
_test_pool_exec() { case "\$2" in axl-pool-run.nsh) printf 'PHASE-%s\r\n' "\$PHASE" >> "\$AXL_POOL_SERIAL" ;; esac; return 0; }
for PHASE in 1 2; do
    test_setup
    test_use_nsh
    printf 'x' > "\$TEST_STAGING/only-\$PHASE.txt"
    test_build_image
    test_run_foreground 10 > /dev/null
    sed -E 's/^\[[0-9.]+\] //' "\$TEST_LOG" | tr -d '\r' > '$WORK/two-'\$PHASE'.log'
    (cd "\$AXL_POOL_HOST_DIR" && ls only-*) > '$WORK/two-'\$PHASE'.files'
done
PHASE=3
test_run_foreground 10 > /dev/null    # the same image again: a fresh machine still
sed -E 's/^\[[0-9.]+\] //' "\$TEST_LOG" | tr -d '\r' > '$WORK/two-3.log'
EOF
chmod +x "$WORK/child-two.sh"
: > "$WORK/stub.log"
: > "$WORK/serial.log"
rm -rf "${WORK:?}/lease" && mkdir -p "$WORK/lease"
AXL_POOL_CLASS=standard AXL_TEST_TIMING_LOG="$WORK/two.timing" timeout 60 "$WORK/child-two.sh" > "$WORK/two.out" 2>&1
same "three runs: each after the first restored once; each log only its own; the share only the rebuilt image's" \
    "$?|$(cat "$WORK/stub.log")|$(cat "$WORK/two-1.log")|$(cat "$WORK/two-2.log")|$(cat "$WORK/two-3.log")|$(cat "$WORK/two-1.files")|$(cat "$WORK/two-2.files")" \
    "0|restore
restore|PHASE-1|PHASE-2|PHASE-3|only-1.txt|only-2.txt"

echo "=== common-test.sh: a background run again, once the first is stopped ==="
# A booted test may stop its guest (test_guest_stop) and boot the next
# scenario. Pooled, the next background run restores the guest -- the first
# run's server, still running on it, goes with the restore -- and adds its
# host forwards again, which the pool's restore drops. A second background
# run while the first is LIVE is refused.
printf 'echo hi\n' > "$WORK/child-bg.nsh"
cat > "$WORK/child-bg.sh" <<EOF
#!/bin/bash
source '$SCRIPT_DIR/common-test.sh'
TEST_ARCH=X64
_test_pool_exec() { case "\$2" in axl-pool-run.nsh) printf 'BG-%s\r\n' "\$N" >> "\$AXL_POOL_SERIAL"; sleep 30 ;; esac; return 0; }
_test_pool_qmp() { printf '%s\n' "\$*" >> '$WORK/bg-qmp.log'; echo '""'; }
test_setup
test_use_nsh
test_build_image
test_build_qemu_cmd
test_add_port_forward 18081 8080
test_add_port_forward 18081 8080   # the same pair again (a test's boot() per scenario)
for N in 1 2; do
    test_run_background
    test_wait_boot "BG-\$N" 10 || echo "NO-BG-\$N"
    [[ "\${BG_TWICE:-0}" == 1 ]] || test_guest_stop
done
test_guest_stop
echo CHILD-DONE
EOF
chmod +x "$WORK/child-bg.sh"
: > "$WORK/stub.log"; : > "$WORK/serial.log"; : > "$WORK/bg-qmp.log"
AXL_POOL_QMP="$WORK/qmp.sock" AXL_POOL_CLASS=standard AXL_TEST_TIMING_LOG="$WORK/bg.timing" timeout 60 "$WORK/child-bg.sh" > "$WORK/bg.out" 2>&1
FWD="$WORK/qmp.sock human-monitor-command {\"command-line\": \"hostfwd_add net0 tcp:127.0.0.1:18081-:8080\"}"
same "stopped, then run again: restored once, the forward added per run, both runs seen" \
    "$?|$(cat "$WORK/stub.log")|$(cat "$WORK/bg-qmp.log")|$(grep -c '^NO-BG' "$WORK/bg.out")|$(grep -c CHILD-DONE "$WORK/bg.out")" \
    "0|restore|$FWD
$FWD|0|1"
: > "$WORK/serial.log"
BG_TWICE=1 AXL_POOL_QMP="$WORK/qmp.sock" AXL_POOL_CLASS=standard AXL_TEST_TIMING_LOG="$WORK/bg2.timing" timeout 60 "$WORK/child-bg.sh" > "$WORK/bg2.out" 2>&1
same "a second background run while the first is live: refused" \
    "$?|$(grep -F 'FAIL: child-bg.sh:' "$WORK/bg2.out")" \
    "1|  FAIL: child-bg.sh: a second background run while the first is live (test_run_background twice) is not available on a pooled guest (its machine is fixed); this test cannot declare guest="

echo "=== common-test.sh: a packet capture on the guest's NIC (test_add_net_dump) ==="
# Booted, a -object filter-dump on net0 from QEMU's start; pooled, the same
# filter added to the leased guest's net0 over QMP before the run and removed
# when it stops -- a filter lives outside the snapshot, so one left behind
# would keep writing into the next lease.
printf 'echo hi\n' > "$WORK/child-dump.nsh"
cat > "$WORK/child-dump.sh" <<EOF
#!/bin/bash
source '$SCRIPT_DIR/common-test.sh'
TEST_ARCH=X64
_test_pool_exec() { case "\$2" in axl-pool-run.nsh) printf 'UP\r\n' >> "\$AXL_POOL_SERIAL"; sleep 30 ;; esac; return 0; }
_test_pool_qmp() { printf '%s\n' "\$*" >> '$WORK/dump-qmp.log'; [[ "\$2" == object-add ]] && echo '{}' || echo '""'; }
test_setup
test_use_nsh
test_build_image
test_build_qemu_cmd
test_add_net_dump "$WORK/cap.pcap"
echo "CMD=\${#TEST_QEMU_CMD[@]}"
test_run_background
test_wait_boot UP 10 || echo NO-UP
test_guest_stop
echo CHILD-DONE
EOF
chmod +x "$WORK/child-dump.sh"
: > "$WORK/stub.log"; : > "$WORK/serial.log"; : > "$WORK/dump-qmp.log"
AXL_POOL_QMP="$WORK/qmp.sock" AXL_POOL_CLASS=standard AXL_TEST_TIMING_LOG="$WORK/dump.timing" \
    timeout 60 "$WORK/child-dump.sh" > "$WORK/dump.out" 2>&1
same "pooled: no QEMU argument; the filter added over QMP before the run, removed at the stop" \
    "$?|$(grep -c '^CMD=0$' "$WORK/dump.out")|$(cat "$WORK/dump-qmp.log")|$(grep -c CHILD-DONE "$WORK/dump.out")" \
    "0|1|$WORK/qmp.sock object-del {\"id\": \"axl_dump0\"}
$WORK/qmp.sock object-add {\"qom-type\": \"filter-dump\", \"id\": \"axl_dump0\", \"netdev\": \"net0\", \"file\": \"$WORK/cap.pcap\"}
$WORK/qmp.sock object-del {\"id\": \"axl_dump0\"}|1"
# A foreground run, then the test exits (no test_guest_stop): removed by
# test_cleanup.
: > "$WORK/dump-qmp.log"; : > "$WORK/serial.log"
sed -e 's/^test_run_background$/test_run_foreground 10 > \/dev\/null/' -e 's/^test_wait_boot UP 10.*$/exit 0/' \
    "$WORK/child-dump.sh" > "$WORK/child-dumpx.sh"
chmod +x "$WORK/child-dumpx.sh"; cp "$WORK/child-dump.nsh" "$WORK/child-dumpx.nsh"
AXL_POOL_QMP="$WORK/qmp.sock" AXL_POOL_CLASS=standard AXL_TEST_TIMING_LOG="$WORK/dumpx.timing" \
    timeout 60 "$WORK/child-dumpx.sh" > "$WORK/dumpx.out" 2>&1
same "a foreground run, then exit: the capture removed on the way out" \
    "$?|$(grep -c 'object-del' "$WORK/dump-qmp.log")|$(grep -c 'object-add' "$WORK/dump-qmp.log")" "0|2|1"
# Booted (no lease): the -object on the command line.
same "booted: the filter-dump on net0 as a QEMU argument" \
    "$(env -u AXL_POOL_AGENT bash -c "source '$SCRIPT_DIR/common-test.sh'; TEST_ARCH=X64; test_setup > /dev/null; TEST_DISK=/x.img; test_build_qemu_cmd; test_add_net_dump /c.pcap; printf '%s\n' \"\${TEST_QEMU_CMD[@]: -2}\"" 2>/dev/null)" \
    "-object
filter-dump,id=axl_dump0,netdev=net0,file=/c.pcap"

echo "=== common-test.sh: test_use_nsh --keep-up (the guest stays up after the script) ==="
# Booted, test_use_nsh's startup.nsh ends with `reset -s` -- unless the test
# needs the guest to keep running after its script returns (a resident driver
# serving the host), when it ends at the Shell prompt, as a pooled guest stays
# up after the agent's run.
printf 'echo hi\n' > "$WORK/child-keep.nsh"
cat > "$WORK/child-keep.sh" <<EOF
#!/bin/bash
source '$SCRIPT_DIR/common-test.sh'
TEST_ARCH=X64
test_setup > /dev/null
test_use_nsh \$KEEP one
cat "\$TEST_STAGING/startup.nsh"
EOF
chmod +x "$WORK/child-keep.sh"
same "booted: the startup ends with reset -s" "$(env -u AXL_POOL_AGENT KEEP= "$WORK/child-keep.sh" 2>&1)" \
    "$(printf '@echo -off\nfs0:\ncd \\\nchild-keep.nsh one\nreset -s')"
same "booted, --keep-up: it ends at the Shell prompt" "$(env -u AXL_POOL_AGENT KEEP=--keep-up "$WORK/child-keep.sh" 2>&1)" \
    "$(printf '@echo -off\nfs0:\ncd \\\nchild-keep.nsh one')"

echo "=== common-test.sh: a pooled test's own QEMU arguments are its class's machine ==="
# A test that adds a device (TEST_QEMU_CMD after test_build_qemu_cmd, or an
# adder such as test_add_ipmi_bmc_sim_kcs) runs pooled only on a class whose
# machine is the standard guest plus exactly those arguments -- compared by
# run-qemu.sh's own machine check; any other lease refuses it, loudly.
printf 'echo hi\n' > "$WORK/child-dev.nsh"
cat > "$WORK/child-dev.sh" <<EOF
#!/bin/bash
source '$SCRIPT_DIR/common-test.sh'
TEST_ARCH=X64
test_setup
_test_pool_exec() { printf 'RAN\r\n' >> "\$AXL_POOL_SERIAL"; return 0; }
test_use_nsh
test_build_image
test_build_qemu_cmd
TEST_QEMU_CMD+=(\$CHILD_ADD)
test_run_foreground 10 > /dev/null
echo CHILD-RAN
EOF
chmod +x "$WORK/child-dev.sh"
DEVCLASS=$'--bridges\n--qemu-arg\n-device\n--qemu-arg\nipmi-bmc-sim,id=bmc0\n--nic-model\ne1000'
REFUSED="is not available on a pooled guest (its machine is fixed); this test cannot declare guest="
# devrun NAME CLASS_ARGS ADDED: the child under a lease of class `dev`.
devrun() {
    : > "$WORK/stub.log"
    : > "$WORK/serial.log"     # the earlier children's console is not this run's
    AXL_POOL_CLASS=dev AXL_POOL_CLASS_ARGS="$2" CHILD_ADD="$3" \
        AXL_TEST_TIMING_LOG="$WORK/$1.timing" timeout 60 "$WORK/child-dev.sh" > "$WORK/$1.out" 2>&1
    RC=$?
}
devrun devok "$DEVCLASS" "-device ipmi-bmc-sim,id=bmc0"
same "the class's machine: run pooled (the agent ran it)" \
    "$RC|$(grep -c CHILD-RAN "$WORK/devok.out")|$(grep -c '^RAN' "$WORK/serial.log")" "0|1|3"
devrun devother "$DEVCLASS" "-device ipmi-bmc-sim,id=bmc1"
same "another device: refused, the difference named" \
    "$RC|$(grep -F 'FAIL: child-dev.sh:' "$WORK/devother.out")" \
    "1|  FAIL: child-dev.sh: its own QEMU arguments (a machine that is not the dev class's: the run has qemu-arg=ipmi-bmc-sim,id=bmc1, the class qemu-arg=ipmi-bmc-sim,id=bmc0) $REFUSED"
devrun devstd $'--bridges\n--nic-model\ne1000' "-device ipmi-bmc-sim,id=bmc0"
same "a device on a standard lease: refused" \
    "$RC|$(grep -F 'FAIL: child-dev.sh:' "$WORK/devstd.out")" \
    "1|  FAIL: child-dev.sh: its own QEMU arguments (a machine that is not the dev class's: the run has qemu-arg=-device, the class nothing) $REFUSED"
devrun devnone "$DEVCLASS" ""
same "a device class leased by a test that adds nothing: refused" \
    "$RC|$(grep -F 'FAIL: child-dev.sh:' "$WORK/devnone.out")" \
    "1|  FAIL: child-dev.sh: the standard guest (a machine that is not the dev class's: the run has nothing, the class qemu-arg=-device) $REFUSED"
# A test that brings its own NIC boots with no standard one beside it: a
# pooled guest always has the agent's, so that is never the class's machine.
devrun devnic $'--bridges\n--qemu-arg\n-netdev\n--qemu-arg\nuser,id=u0\n--nic-model\ne1000' "-netdev user,id=u0"
same "its own NIC (no standard one): refused" \
    "$RC|$(grep -F 'FAIL: child-dev.sh:' "$WORK/devnic.out")" \
    "1|  FAIL: child-dev.sh: its own QEMU arguments (a machine that is not the dev class's: the run has nic=none, the class nic=e1000) $REFUSED"
AXL_POOL_CLASS=dev CHILD_ADD="-device ipmi-bmc-sim,id=bmc0" AXL_TEST_TIMING_LOG="$WORK/devnoargs.timing" \
    timeout 60 env -u AXL_POOL_CLASS_ARGS "$WORK/child-dev.sh" > "$WORK/devnoargs.out" 2>&1
RC=$?
same "no class options to compare with: refused" \
    "$RC|$(grep -F 'FAIL: child-dev.sh:' "$WORK/devnoargs.out")" \
    "1|  FAIL: child-dev.sh: its own QEMU arguments (the lease names no machine to compare them with) $REFUSED"

for LARCH in AARCH64 X64; do
echo "=== an $LARCH lease: the firmware's DEBUG output (its debug UART / debugcon) ==="
printf 'ASSERT [DxeCore] stale.c(1): before the run\r\n' > "$WORK/firmware-debug.log"
AXL_POOL_ARCH=$LARCH STUB_FWDEBUG='ASSERT [TcpDxe] LinkedList.c(594): during' \
    rq aa --arch "$LARCH" --serial-log "$WORK/aa.log" "$WORK/probe.efi"
same "$LARCH: the run's ASSERT after its output, as a booted run prints it; not one from before the run" \
    "$RC|$(out aa)" '0|RAN: probe.efi
[firmware-debug] ASSERT [TcpDxe] LinkedList.c(594): during'
same "...and this run's part kept beside --serial-log, where a booted run keeps it" \
    "$(tr -d '\r' < "$WORK/aa-firmware-debug.log" 2>/dev/null)" 'ASSERT [TcpDxe] LinkedList.c(594): during'
rm -f "$WORK/aa-firmware-debug.log"
AXL_POOL_ARCH=$LARCH STUB_FWDEBUG='Loading driver at 0x1 EntryPoint=0x2 probe.efi' \
    rq aaquiet --arch "$LARCH" "$WORK/probe.efi"
same "$LARCH: a run with no ASSERT prints nothing of it" "$RC|$(out aaquiet)" '0|RAN: probe.efi'
AXL_POOL_ARCH=$LARCH STUB_FWDEBUG='ASSERT [TcpDxe] LinkedList.c(594): during' \
    rq aanull --arch "$LARCH" --serial-log /dev/null "$WORK/probe.efi"
same "$LARCH --serial-log /dev/null: no place beside it for the debug log, and the run still prints" \
    "$RC|$(out aanull)" '0|RAN: probe.efi
[firmware-debug] ASSERT [TcpDxe] LinkedList.c(594): during'
rm -f "$WORK/firmware-debug.log" "$WORK/aanolog.timing"   # the timing log appends
AXL_POOL_ARCH=$LARCH rq aanolog --arch "$LARCH" "$WORK/probe.efi"
same "an $LARCH lease with no debug log: the run boots, saying why (never a blind pooled run)" \
    "$RC|$(out aanolog)|$(cat "$WORK/stub.log")|$(sed -n 's/^run-qemu: //p' "$WORK/aanolog.timing")" \
    "0|BOOTED|restore|booted inside a plain lease: the leased $LARCH guest has no firmware debug log ($WORK/firmware-debug.log)"
done
printf 'boot\r\n' > "$WORK/firmware-debug.log"
AXL_POOL_ARCH=AARCH64 STUB_APP_ECHO='Synchronous Exception at 0x000000005C5E1234' \
    STUB_FWDEBUG='  SP 0x1  ELR 0x2' rq aaexc --arch AARCH64 "$WORK/probe.efi"
same "an exception on the console: the run's debug UART after its output (the dump's registers)" \
    "$RC|$(out aaexc)" '0|RAN: probe.efi
Synchronous Exception at 0x000000005C5E1234
[firmware-debug]   SP 0x1  ELR 0x2'

printf 'run-qemu-pooled: %d passed, %d failed\n' "$PASS" "$FAIL"
[[ $FAIL -eq 0 && $PASS -gt 0 ]]
