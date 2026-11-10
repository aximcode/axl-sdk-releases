#!/bin/bash
# test-meta: arch=both needs= est=51 local-only=0 guest=none
# test-meta: builds=BUILD=RELEASE,tools,crashhandler,pack-crashtest-w
# guest=none: the boot is the subject: the image faults under the crash
#   handler and the crash names it on the console of a boot that ends there.
# test-efi-pack-crash-qemu.sh -- a RELEASE crashtest.efi, force-wrapped by the
# axl-pack stub, faults under the crash handler, and everything that names
# it names the INNER image: split from test-efi-pack-release-qemu.sh, whose
# release tree (BUILD=RELEASE, as release.yml builds it) it shares -- the
# runner pre-builds both tests' targets in one build call.
#
#   host        pack-crashtest-w.efi is the axl-pack stub
#   crash       crashtest.efi faults under the crash handler (TCG, two
#               boots): the live line and the report name crashtest.efi
#               with the INNER image's base and size, and rsod-decode.py
#               --syms <raw crashtest.so> resolves the faulting frame to
#               trigger_invalid_opcode. The stub is in the loaded-image list
#               under the same name; the decoder must not pick its base.
#
# Usage: ./test/integration/test-efi-pack-crash-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1

source "$(dirname "$0")/common-test.sh"

test_parse_args "$@"
test_setup

declare -A _NATIVE=([X64]=x64 [AARCH64]=aa64)
NATIVE="${_NATIVE[$TEST_ARCH]:-x64}"
TOTAL=9
PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
check() {
    local label="$1"
    shift
    if "$@"; then pass "$label"; else fail "$label"; fi
}
# The RELEASE tree, as release.yml builds it.
if ! "$PROJECT_DIR/scripts/axl-build.sh" ARCH="$NATIVE" \
        BUILD=RELEASE tools crashhandler pack-crashtest-w \
        > "$TEST_TMPDIR/build.log" 2>&1; then
    tail -20 "$TEST_TMPDIR/build.log"
    echo "efi-pack-crash test: FAIL (build)"
    exit 1
fi
REL="$("$PROJECT_DIR/scripts/axl-build.sh" --print-prefix --abs ARCH="$NATIVE" BUILD=RELEASE)"

echo "=== efi-pack: a wrapped RELEASE crashtest under the crash handler ($TEST_ARCH) ==="

check "host: pack-crashtest-w.efi is the axl-pack stub" \
    test "$(test_pe_module "$REL/pack-crashtest-w.efi")" = axl-pack-stub

# ---------------------------------------------------------------------------
# Crash naming: two TCG boots on a writable disk (the report is written
# beside the handler), with NVRAM that survives between them.
# ---------------------------------------------------------------------------
case "$TEST_ARCH" in
    X64)     WANT_EXC="#UD (Invalid Opcode)"; CRASH_LIMIT=60 ;;
    AARCH64) WANT_EXC="Synchronous";          CRASH_LIMIT=150 ;;
esac
RAW_CT="$REL/tools/raw/crashtest.efi"
CT_SO="$REL/tools/crashtest.so"
TEST_STAGING="$TEST_TMPDIR/crash-staging"
TEST_DISK="$TEST_TMPDIR/crash.img"
CRASH_VARS="$TEST_TMPDIR/crash-vars.fd"
cp "$FW_VARS" "$CRASH_VARS"
test_add_efi "$REL/drivers/crashhandler.efi" crashhandler.efi
test_add_efi "$REL/pack-crashtest-w.efi" crashtest.efi

# One TCG boot (the handler's vectors fire only under emulation, as in
# test-crashhandler.sh), ended as soon as $2 is a whole line of the log or
# after CRASH_LIMIT s. $1: the log.
crash_boot() {
    local log="$1" until="$2" pid i
    local -a cmd=("$TEST_QEMU_BIN")
    case "$TEST_ARCH" in
        X64)     cmd+=(-machine q35  -accel tcg -cpu max) ;;
        AARCH64) cmd+=(-machine virt -accel tcg -cpu cortex-a57) ;;
    esac
    cmd+=(-m 512M
        -drive "if=pflash,format=raw,readonly=on,file=$FW_CODE"
        -drive "if=pflash,format=raw,file=$CRASH_VARS"
        -drive "format=raw,file=$TEST_DISK"
        -nographic -no-reboot)
    test_build_image
    _prof_launch crash "${cmd[@]}"
    test_timing_begin
    "${cmd[@]}" > "$log" 2>&1 &
    pid=$!
    test_timing_watch "$pid" "$log"
    for ((i = 0; i < CRASH_LIMIT * 5; i++)); do
        kill -0 "$pid" 2>/dev/null || break
        # Not `tr | grep -q`: under pipefail a MATCH fails that pipeline
        # (grep exits first, tr takes SIGPIPE).
        [[ -f "$log" ]] || { sleep 0.2; continue; }
        tr -d '\r' < "$log" > "$log.now"
        grep -qxF -- "$until" "$log.now" && break
        sleep 0.2
    done
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    test_timing_end -
    sed -E 's/\x1b\[[0-9;]*[a-zA-Z]//g' "$log" | tr -d '\r' > "$log.clean"
}

test_set_startup <<'NSH'
@echo -off
load fs0:\crashhandler.efi
fs0:\crashtest.efi ud
NSH
crash_boot "$TEST_TMPDIR/crash1.log" \
    "Crash record saved to NVRAM (slot 0). Reboot to see full report."
test_set_startup <<'NSH'
@echo -off
load fs0:\crashhandler.efi
echo REPORT-BEGIN
type fs0:\crash-report.txt
echo REPORT-END
reset -s
NSH
crash_boot "$TEST_TMPDIR/crash2.log" "REPORT-END"

live=$(grep -E '^!!! CRASH: ' "$TEST_TMPDIR/crash1.log.clean" | head -1 || true)
live_off=$(sed -nE 's/^!!! CRASH: .* at 0x[0-9A-F]+ \(crashtest\.efi\+0x([0-9A-F]+)\) !!!$/\1/p' <<< "$live")
check "crash: the live line names crashtest.efi ($live)" \
    test -n "$live_off" -a "${live#!!! CRASH: $WANT_EXC at 0x}" != "$live"
REPORT="$TEST_TMPDIR/crash-report.txt"
awk '$0 == "REPORT-END" { on = 0 } on; $0 == "REPORT-BEGIN" { on = 1 }' \
    "$TEST_TMPDIR/crash2.log.clean" > "$REPORT"
# The inner image's SizeOfImage, from the raw file's optional header.
pe=$(od -An -tu4 -j60 -N4 "$RAW_CT" | tr -d ' ')
raw_soi=$(printf '0x%X' "$(od -An -tu4 -j$((pe + 24 + 56)) -N4 "$RAW_CT" | tr -d ' ')")
img=$(grep -E '^Image: ' "$REPORT" | head -1 || true)
img_re="^Image: +crashtest\\.efi \\(base 0x[0-9A-F]+, size ${raw_soi}\\)\$"
check "crash: the report's Image is crashtest.efi with the RAW image's size $raw_soi ($img)" \
    grep -qE "$img_re" <<< "$img"
rep_off=$(sed -nE 's/^Offset: +0x([0-9A-F]+)$/\1/p' "$REPORT")
check "crash: the report's Offset is the live line's (0x$rep_off, 0x$live_off)" \
    test -n "$rep_off" -a "$rep_off" = "$live_off"
n=$(grep -cE '^  0x[0-9A-F]{16} 0x[0-9A-F]{6}  crashtest\.efi$' "$REPORT" || true)
check "crash: the stub and the inner image are both listed as crashtest.efi ($n)" \
    test "$n" = 2
check "crash: the report prints the decode command for crashtest.so" \
    grep -qxF "  rsod-decode.py --syms <build>/crashtest.so --rsod crash-report.txt" "$REPORT"
DECODED="$TEST_TMPDIR/decoded.txt"
rc=0
timeout 120 python3 "$PROJECT_DIR/scripts/rsod-decode.py" --syms "$CT_SO" --rsod "$REPORT" \
    > "$DECODED.raw" 2> "$DECODED.err" || rc=$?
sed -e 's/\x1b\[[0-9;]*m//g' "$DECODED.raw" > "$DECODED"
check "crash: rsod-decode.py --syms <raw crashtest.so> exits 0 ($rc)" test "$rc" = 0
frame0=$(grep -E '^  #0 ' "$DECODED" | head -1 || true)
f0_name=$(awk '{ print $2 }' <<< "$frame0")
check "crash: frame #0 resolves to trigger_invalid_opcode ($f0_name)" \
    test "$f0_name" = trigger_invalid_opcode
f0_off=$(sed -nE 's/.*\[0x[0-9a-f]+ \+ 0x([0-9a-f]+)\]$/\1/p' <<< "$frame0")
check "crash: frame #0's offset is the report's (0x$f0_off)" \
    test -n "$f0_off" -a "${f0_off^^}" = "$rep_off"

echo ""
printf "efi-pack-crash: %d passed, %d failed (%s)\n" "$PASS" "$FAIL" "$TEST_ARCH"
if [[ $FAIL -gt 0 ]]; then
    echo "--- crash report ---"; cat "$REPORT"
    echo "--- decoded ---"; head -20 "$DECODED"
    echo "--- crash boot 1 ---"; tail -40 "$TEST_TMPDIR/crash1.log.clean"
fi
if (( PASS + FAIL != TOTAL )); then
    echo "FAIL: $((PASS + FAIL)) assertions ran, $TOTAL expected"
    exit 1
fi
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
