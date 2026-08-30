#!/bin/bash
# test-meta: arch=both needs= est=110 local-only=0
# test-crashhandler.sh -- end-to-end test for the CrashHandler DXE driver
# (drivers/crashhandler) + CrashTest app (tools/crashtest.c).
#
# Boots QEMU, loads CrashHandler, triggers a CPU exception via CrashTest,
# reboots, and verifies the crash record round-trips: NVRAM capture on
# boot 1 -> crash-report.txt written on boot 2 -> idempotent re-load.
#
# x64 AND aa64 -- and the `arch=` above said x64 for as long as the header has
# existed. It was set by the bulk commit that introduced the parallel runner
# and never revisited, while this file's per-arch branches (WANT_EXC / PC_REG,
# and the aa64 register spellings they select) only make sense if the aa64 leg
# runs. So aa64 was written for, documented, and then never scheduled.
#
# Measured before flipping it, on this box, three runs: x64 41s / 23 passed,
# aa64 108s and 109s / 23 passed each. `est=` now carries the aa64 number
# because it is the one that unbalances a shard when it is wrong, and the
# figure is a single field shared by both legs.
#
# No CI cost: ci.yml runs the integration suite `--arch X64` only, so this
# schedules the aa64 leg for local runs (`run-integration.sh --arch AARCH64`)
# and nothing else. The default --timeout is 900s, comfortably above 110.
#
# Auxiliary: opt out of the test-axl.sh pass-count ratchet (it boots QEMU
# several times with its own PASS/FAIL accounting, not the guest-emitted
# PASS:/FAIL: lines the ratchet counts).
#
# IMPORTANT: this test must run QEMU WITHOUT KVM. CrashHandler installs a
# UEFI exception handler that only fires under software emulation (TCG);
# with -enable-kvm the host kernel intercepts #PF/#GP before the firmware
# handler runs. build_crash_qemu_cmd below strips KVM from the base cmd.
#
# Usage: ./test/integration/test-crashhandler.sh [--arch X64|AARCH64]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common-test.sh"

# Not the unit ratchet — we don't call test_count_results.
export TEST_SKIP_RATCHET=1

test_parse_args "$@"

ARCH_LC=$(arch_dir "$TEST_ARCH")
CRASH_HANDLER="$(test_build_dir "$ARCH_LC")/drivers/crashhandler.efi"
CRASH_TEST="$(test_build_dir "$ARCH_LC")/tools/crashtest.efi"

# Build if missing (warm tree: <2s).
if [[ ! -f "$CRASH_HANDLER" || ! -f "$CRASH_TEST" ]]; then
    echo "Building CrashHandler + CrashTest ($TEST_ARCH)..."
    make -C "$PROJECT_DIR" ARCH="$ARCH_LC" crashhandler crashtest 2>&1 | tail -3
fi
[[ -f "$CRASH_HANDLER" ]] || { echo "FAIL: not found: $CRASH_HANDLER"; exit 1; }
[[ -f "$CRASH_TEST" ]]    || { echo "FAIL: not found: $CRASH_TEST"; exit 1; }

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() {
    echo "  FAIL: $1"; FAIL=$((FAIL + 1))
    if [[ -n "${2:-}" && -f "$2" ]]; then
        echo "  --- serial log (tail) ---"; tail -40 "$2"; echo "  --- end ---"
    fi
}

test_setup   # find_qemu/find_firmware, TEST_NVRAM=copy of FW_VARS, cleanup trap

# Stage the binaries once with the PascalCase names startup.nsh references
# (FAT is case-insensitive, but match exactly to be unambiguous).
test_add_efi "$CRASH_HANDLER" CrashHandler.efi
test_add_efi "$CRASH_TEST"    CrashTest.efi

# Per-arch boot timeout (aa64 under TCG is slower).
QEMU_TIMEOUT=25
[[ "$TEST_ARCH" == "AARCH64" ]] && QEMU_TIMEOUT=$((QEMU_TIMEOUT + 60))

# Build a TCG (NOT KVM) QEMU command. CrashHandler installs a UEFI
# exception handler that only fires under software emulation; -enable-kvm
# lets the host kernel intercept #PF/#GP first. We build the command
# explicitly rather than reuse build_qemu_base_cmd, which enables KVM
# when /dev/kvm is usable.
#
# x64: -cpu max is required — the default qemu64 cannot boot this OVMF
# under TCG (it exits instantly with no output); max exposes the CPU
# features the firmware needs. aa64 boots fine on cortex-a57 under TCG.
# FW_CODE / TEST_NVRAM / TEST_QEMU_BIN are set by test_setup.
build_crash_qemu_cmd() {
    CRASH_QEMU_CMD=("$TEST_QEMU_BIN")
    case "$TEST_ARCH" in
        X64)     CRASH_QEMU_CMD+=(-machine q35   -accel tcg -cpu max) ;;
        AARCH64) CRASH_QEMU_CMD+=(-machine virt  -accel tcg -cpu cortex-a57) ;;
    esac
    CRASH_QEMU_CMD+=(
        -m 512M
        -drive "if=pflash,format=raw,readonly=on,file=$FW_CODE"
        -drive "if=pflash,format=raw,file=$TEST_NVRAM"
        -drive "format=raw,file=$TEST_DISK"
        -nographic
        -no-reboot
    )
}

run_boot() {
    # $1 = log path. Re-stages the disk from current TEST_STAGING and runs.
    test_build_image
    build_crash_qemu_cmd
    timeout "$QEMU_TIMEOUT" "${CRASH_QEMU_CMD[@]}" > "$1" 2>&1 || true
}

echo "=== CrashHandler Integration Tests ($TEST_ARCH) ==="

# --- Test 1: crash capture -------------------------------------------------
echo "Test 1: Crash capture"
test_set_startup <<'EOF'
@echo -off
echo "=== CrashHandler Test Boot ==="
load fs0:\CrashHandler.efi
echo "CrashHandler loaded, triggering crash..."
fs0:\CrashTest.efi ud
EOF
LOG1="$TEST_TMPDIR/boot1.log"
run_boot "$LOG1"

if grep -q "CRASH:" "$LOG1"; then
    pass "CrashHandler caught exception"
elif grep -q "Exception Type" "$LOG1"; then
    pass "Exception occurred (default handler)"
else
    fail "No exception detected in serial output" "$LOG1"
fi

grep -q "NVRAM" "$LOG1" \
    && pass "Crash record saved to NVRAM" \
    || fail "NVRAM save message not found"

grep -q "CrashHandler.*active\|exception types monitored" "$LOG1" \
    && pass "CrashHandler loaded successfully" \
    || fail "CrashHandler load message not found"

grep -qi "CrashTest\|NULL pointer\|triggering\|crash" "$LOG1" \
    && pass "CrashTest.efi executed" \
    || fail "CrashTest.efi execution not confirmed"

# --- Test 2: crash report on reboot (persistent NVRAM) ---------------------
echo "Test 2: Crash report on reboot"
test_set_startup <<'EOF'
@echo -off
echo "=== CrashHandler Reboot Test ==="
load fs0:\CrashHandler.efi
if exist fs0:\crash-report.txt then
  echo "REPORT_FOUND"
  type fs0:\crash-report.txt
endif
echo "=== DONE ==="
reset -s
EOF
LOG2="$TEST_TMPDIR/boot2.log"
run_boot "$LOG2"

grep -q "crash record.*saved\|crash-report.txt" "$LOG2" \
    && pass "Crash report written on reboot" \
    || fail "Crash report not generated" "$LOG2"

grep -q "REPORT_FOUND" "$LOG2" \
    && pass "crash-report.txt exists on filesystem" \
    || fail "crash-report.txt not found on filesystem"

# EXACT exception name, not a substring. The old assertion here was
#   grep -qi "#PF\|Page Fault\|Synchronous\|Data Abort\|Exception.*0x"
# and its last alternative matches the "Exception: ... at 0x..." LINE, so it
# passed no matter WHICH exception the report named. It sat green over a
# report that called every x64 #UD a "#BR (Bound Range)" -- exception.c stored
# an AxlCpuExceptionKind in the record and report.c decoded it as an x86
# vector number, two numberings that agree nowhere. Compare the whole field.
#
# PER-ARCH, because this script accepts --arch AARCH64 and the two differ in
# both respects: x64 splits the exception space into vectors and names the
# faulting register RIP, while aa64 collapses everything into the SYNCHRONOUS
# umbrella (entrypoint.c's exception_kinds) and names it ELR. Hardcoding the
# x64 spelling would fail an aa64 run for a reason that is not a defect.
case "$TEST_ARCH" in
    X64)     WANT_EXC="#UD (Invalid Opcode)"; PC_REG="RIP" ;;
    AARCH64) WANT_EXC="Synchronous";          PC_REG="ELR" ;;
esac

report_text() { sed -e 's/\r$//' "$LOG2"; }
EXC_NAME=$(report_text | sed -n 's/^Exception: *\(.*\) at 0x.*$/\1/p' | head -1)
if [[ "$EXC_NAME" == "$WANT_EXC" ]]; then
    pass "Crash report names the exception that actually fired"
else
    fail "exception name: got '$EXC_NAME', want '$WANT_EXC'" "$LOG2"
fi

grep -q "Registers:" "$LOG2" \
    && pass "Crash report contains registers" \
    || fail "Registers not in crash report"

# The RIP the report persisted must be the RIP the live handler saw. This is
# the round-trip assertion, and it is also the one that notices a MIS-DECODED
# EFI_SYSTEM_CONTEXT: cpu-arch.h once carried `void *FxSaveState` where the
# spec has an inline 512-byte EFI_FX_SAVE_STATE_X64, which read every later
# field 504 bytes early and produced a RIP of 0x4F307F9B6302D008 with every
# GPR zero. Both halves agreed on that garbage, so comparing them to each
# other could not notice it -- the loaded-image check below is what does.
#
# The extraction accepts BOTH live forms: the handler prints "(name+offset)"
# once it can attribute the fault to an image and a bare "at 0xADDR" when it
# cannot. A pattern that only accepted the bare form would go GREEN on the
# broken behaviour and RED on the fixed one, which is exactly what it did.
LIVE_RIP=$(sed -e 's/\r$//' "$LOG1" \
           | sed -n 's/^.*!!! CRASH: .* at 0x\([0-9A-Fa-f]*\).*!!!.*$/\1/p' | head -1)
SAVED_RIP=$(report_text \
            | sed -n "s/^ *$PC_REG=\\([0-9A-F]*\\).*$/\\1/p" | head -1)
if [[ -n "$LIVE_RIP" && -n "$SAVED_RIP" \
      && $((16#$SAVED_RIP)) -eq $((16#$LIVE_RIP)) ]]; then
    pass "Reported RIP round-trips through NVRAM unchanged"
else
    fail "$PC_REG: live '${LIVE_RIP:-<none>}', report '${SAVED_RIP:-<none>}'" "$LOG2"
fi

# The live print must NAME the faulting image, not just its address. That is
# the half a reader needs to rebase a backtrace, and the crash handler used to
# be unable to produce it for any application: it snapshotted the loaded-image
# table once at driver init, before the application that goes on to fault is
# even loaded.
# The NAME is asserted, not merely the shape. `[^()]+` would be satisfied by
# "(Unknown+0x1204)" -- which is exactly what copy_basename emits for an image
# whose firmware FilePath will not decode, so the loose form could not tell a
# named image from an unnamed one.
if sed -e 's/\r$//' "$LOG1" \
   | grep -qF "(CrashTest.efi+0x"; then
    pass "Live crash line attributes the fault to a loaded image"
else
    fail "live crash line does not name CrashTest.efi (stale image table?)" "$LOG1"
fi

# A crash record whose frame pointer is zero yields NO backtrace, which is the
# whole point of the report. crashtest.c builds six non-inlined frames on
# purpose (main -> run_crashtest -> initialize_test -> prepare_crash_context ->
# validate_environment -> dispatch_crash -> trigger_*), so anything under five
# means the frame-pointer walk failed rather than that the stack was shallow.
FRAMES=$(report_text | sed -n '/^Stack Trace:/,/^$/p' \
         | grep -cE '^  0x[0-9A-F]{16}  ' || true)
if [[ "$FRAMES" -ge 5 ]]; then
    pass "Crash report contains a stack trace ($FRAMES frames)"
else
    fail "stack trace: $FRAMES frames, want >= 5" "$LOG2"
fi

# ... and the faulting address must land inside a loaded image. Plausible-
# looking hex that resolves to nothing is exactly what a mis-decoded context
# produces, and frame COUNT alone would not catch it.
if report_text | "$PROJECT_DIR/test/integration/lib/rip-in-image.py"; then
    pass "Faulting RIP falls inside a loaded image"
else
    fail "RIP is not within any image in the report's loaded-image table" "$LOG2"
fi

grep -q "Loaded Images:" "$LOG2" \
    && pass "Crash report contains loaded image table" \
    || fail "Loaded image table not in crash report"

# --- Test 2b: the report decodes with rsod-decode.py -----------------------
#
# The report's LAST LINE is a rsod-decode.py command line for the reader to
# run. That makes the writer and the decoder a pair, and nothing checked the
# pair: rsod-decode could not read this format at all -- it took the registers
# and the faulting PC, then ignored the `Image:` line stating the load base,
# the whole `Loaded Images:` table, and every frame of the `Stack Trace:`
# section. The advice printed at the bottom of every crash report was for a
# decoder that had to be told the base by hand and then answered with one
# frame.
#
# This is also the only place in the tree where the decoder meets a REAL
# crash: real registers from a real exception, real frames, and a .so with
# real DWARF, on whichever arch is under test. The host-only suite
# (test-rsod-decode-pe-map.sh) has to synthesize all of that.
echo "Test 2b: Decode the crash report"
REPORT="$TEST_TMPDIR/crash-report.txt"
# The report is `type`d between these two markers by the boot-2 startup.nsh.
report_text | sed -n '/^REPORT_FOUND/,/^=== DONE ===/p' | sed -e '1d' -e '$d'     > "$REPORT"

DEBUG_SO="$(test_build_dir "$ARCH_LC")/tools/crashtest.so"
DECODED="$TEST_TMPDIR/decoded.txt"
if [[ ! -s "$REPORT" ]]; then
    fail "crash report extracted from the boot log" "$LOG2"
elif [[ ! -f "$DEBUG_SO" ]]; then
    fail "crashtest.so (DWARF) present at $DEBUG_SO"
else
    pass "crash report extracted from the boot log"
    # NO :BASE and no --base. The report states the load base three times over
    # (the `Image:` line, the `Loaded Images:` table, the printed command) and
    # having to supply it by hand is the defect, not the interface.
    # Strip ANSI before asserting: the report is decoded for a terminal, and a
    # `^Image 0:` anchor does not match a line that starts with a colour escape.
    if timeout 120 python3 "$PROJECT_DIR/scripts/rsod-decode.py" \
            --syms "$DEBUG_SO" --rsod "$REPORT" --detail \
            > "$DECODED.raw" 2>"$DECODED.err"; then
        pass "rsod-decode.py exits 0 on the report"
    else
        fail "rsod-decode.py failed on the report" "$DECODED.err"
    fi
    sed -e 's/\x1b\[[0-9;]*m//g' "$DECODED.raw" > "$DECODED"

    # The crash is a deliberate #UD in trigger_invalid_opcode(); resolving the
    # faulting frame to that name is the end-to-end claim -- the report's base,
    # the decoder's arithmetic and the .so's DWARF all have to agree for it.
    if grep -q 'trigger_invalid_opcode' "$DECODED"; then
        pass "faulting frame resolves to trigger_invalid_opcode"
    else
        fail "faulting frame not resolved to trigger_invalid_opcode" "$DECODED"
    fi

    # The base must come from the report, not from a guess.
    if grep -qE '^Image 0:.*base=0x[0-9a-f]+' "$DECODED"; then
        pass "load base inferred from the report (no :BASE given)"
    else
        fail "load base not inferred from the report" "$DECODED"
    fi

    # More than the seeded faulting PC: the report carries a real frame list,
    # so the decoder must render it rather than fall back to a one-frame trace.
    DFRAMES=$(grep -cE '^  #[0-9]+ ' "$DECODED" || true)
    if [[ "$DFRAMES" -ge 5 ]]; then
        pass "decoded backtrace has $DFRAMES frames"
    else
        fail "decoded backtrace: $DFRAMES frames, want >= 5" "$DECODED"
    fi

    # Every register the report prints must survive into the decode. report.c
    # padded short names for column alignment (`R8 =`, and `X0 =`..`X9 =`,
    # `FP =` on aa64) while every consumer scans for NAME=VALUE, so those were
    # dropped silently -- ten of the X registers on aa64. Producer and consumer
    # are both in this tree, and nothing compared them until now.
    case "$TEST_ARCH" in
        X64)     WANT_REGS="R8 R9 RAX RIP" ;;
        AARCH64) WANT_REGS="X0 X8 FP LR ELR" ;;
    esac
    MISSING=""
    for r in $WANT_REGS; do
        grep -qE "(^| )$r 0x" "$DECODED" || MISSING="$MISSING $r"
    done
    if [[ -z "$MISSING" ]]; then
        pass "every padded register name survives the decode ($WANT_REGS)"
    else
        fail "registers dropped by the decoder:$MISSING" "$DECODED"
    fi

    # The image the report describes IS the image we handed over, so the
    # wrong-image gates must stay silent. A validator that cries wolf on the
    # correct binary is worse than none.
    if grep -q 'IMAGE DOES NOT MATCH' "$DECODED"; then
        fail "correct image wrongly flagged as mismatched" "$DECODED"
    else
        pass "correct image raises no mismatch warning"
    fi

    # RUN THE COMMAND THE REPORT PRINTS, rather than a command that happens to
    # work. The last line of every crash report is advice, and advice that was
    # never executed is how it came to name a flag (`--file`) and an artifact
    # (`<build>/<name>`, the stripped .efi) that would not have got the reader
    # where they were going. Substituting the build directory for the
    # `<build>/` placeholder is the only edit a reader would not have to make.
    HINT=$(sed -n 's|^  rsod-decode\.py |&|p' "$REPORT" | head -1)
    if [[ -z "$HINT" ]]; then
        fail "report prints a rsod-decode.py command" "$REPORT"
    else
        pass "report prints a rsod-decode.py command"
        # It must name the ELF that HAS the DWARF, and must not have pasted
        # `.so` onto a name that already ends in `.efi`.
        if grep -qE -- '--syms <build>/[A-Za-z0-9_-]+\.so( |$)' <<<"$HINT"; then
            pass "printed command names a .so, not the stripped .efi"
        else
            fail "printed command names the wrong artifact: $HINT"
        fi

        # Now RUN it. Only the image path is substituted -- that is the single
        # edit a reader must make, since `<build>/` is a placeholder and the
        # loaded name need not match the build artifact's. Everything else
        # (the flag spellings, the ordering) is executed exactly as printed,
        # which is what would have caught `--file` had it ever been run.
        HINT_ARGS=$(sed -E "s#--syms <build>/[A-Za-z0-9_.-]+#--syms $DEBUG_SO#" \
                    <<<"${HINT#  rsod-decode.py }")
        HINT_OUT="$TEST_TMPDIR/hint.txt"
        # shellcheck disable=SC2086  # deliberate: the printed line IS the argv
        if ( cd "$TEST_TMPDIR" \
             && timeout 120 python3 "$PROJECT_DIR/scripts/rsod-decode.py" $HINT_ARGS ) \
                > "$HINT_OUT" 2>&1 \
             && grep -q 'trigger_invalid_opcode' "$HINT_OUT"; then
            pass "the command the report prints actually decodes it"
        else
            fail "the printed command did not decode the report" "$HINT_OUT"
        fi
    fi
fi

# --- Test 3: idempotency (fresh NVRAM, double load) ------------------------
echo "Test 3: Idempotency"
cp "$FW_VARS" "$TEST_NVRAM"   # reset NVRAM so no stale crash record
test_set_startup <<'EOF'
@echo -off
load fs0:\CrashHandler.efi
load fs0:\CrashHandler.efi
echo "=== DONE ==="
reset -s
EOF
LOG3="$TEST_TMPDIR/boot3.log"
run_boot "$LOG3"

grep -q "already active" "$LOG3" \
    && pass "Second load detected as already active" \
    || fail "Idempotency check not working" "$LOG3"

# --- Summary ---------------------------------------------------------------
echo ""
echo "Results: $PASS passed, $FAIL failed ($TEST_ARCH)"
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
