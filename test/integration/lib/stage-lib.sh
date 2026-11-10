#!/bin/bash
# lib/stage-lib.sh -- boots for the staged-upgrade resume tests
# (test-service-stage-resume-qemu.sh, test-service-stage-trial-qemu.sh).
#
# Each boot is its own QEMU run on ONE disk and ONE NV store, so files and
# the AxlAttempt breadcrumb carry from boot to boot as on a real box. Between
# boots the host edits the disk with mtools -- the file damage a power loss or
# a FAT failure leaves -- and the startup script; a boot may set
# AXL_SERVICE_STAGE_TEST_CUT to reset the box right after the launcher records
# a phase (a power loss at exactly that point).
#
# The service is sdk/examples/upgrade-svc.c in its network-free variants
# (upgrade-svc-dxe-qN.efi, "USVC: vN up, no network (last upgrade: ...)"),
# started by the test launcher (upgrade-svc-t.efi: 3 s grace period).
#
# Sourced after common-test.sh; call stage_setup first.

P=upgrade-svc-dxe.efi

stage_setup() {
    declare -gA _STAGE_NATIVE=([X64]=x64 [AARCH64]=aa64)
    TEST_BUILD_DIR="$(test_build_dir)"
    "$PROJECT_DIR/scripts/axl-build.sh" ARCH="${_STAGE_NATIVE[$TEST_ARCH]:-x64}" \
        all upgrade-svc-test 2>&1 | tail -2
    B="$TEST_BUILD_DIR"
    test_add_efi "$B/upgrade-svc-t.efi" upgrade-svc.efi
    test_add_efi "$B/upgrade-svc-dxe-q1.efi" "$P"
    printf '%s\r\n' '@echo -off' 'reset -s' | test_set_startup
    test_build_image
    STAGE_BOOT_SECS=60
    [[ "$TEST_ARCH" == AARCH64 ]] && STAGE_BOOT_SECS=150
    BOOTS=0
}

img_put() { mcopy -o -i "$TEST_DISK" "$1" "::/$2" 2>/dev/null; }
img_del() { mdel -i "$TEST_DISK" "::/$1" 2>/dev/null || true; }
img_has() { mdir -i "$TEST_DISK" -b "::/$1" > /dev/null 2>&1; }
# img_is NAME FILE -- the disk's NAME is FILE, byte for byte
img_is() {
    rm -f "$TEST_TMPDIR/img_is.tmp"
    mcopy -o -i "$TEST_DISK" "::/$1" "$TEST_TMPDIR/img_is.tmp" 2>/dev/null \
        && cmp -s "$TEST_TMPDIR/img_is.tmp" "$2"
}

# boot TAG [CUT] [START_ARGS] -- one boot: the launcher's `start --detach`,
# optionally with a test cut; the cleaned serial log is $TEST_TMPDIR/TAG.log.
# A boot that is not cut ends with BOOT-END. With STAGE_FAIL=<what> in the
# environment (`STAGE_FAIL=trial boot TAG`), the launcher's NV write <what>
# fails (AXL_SERVICE_STAGE_TEST_FAIL; test/integration/upgrade-svc-test-hook.c);
# with STAGE_CLOBBER=<phase>, fs0:\restage.efi is written over P.new right
# after <phase> is recorded (AXL_SERVICE_STAGE_TEST_CLOBBER); with
# STAGE_STALL=<us>, the Shell stalls that long after the start.
boot() {
    local tag="$1" cut="${2:-}" args="${3:-}"
    BOOTS=$((BOOTS + 1))
    {
        printf '%s\r\n' '@echo -off' 'fs0:' 'cd \'
        # -v: volatile, so the cut does not outlive this boot
        [[ -n "$cut" ]] && printf '%s\r\n' "set -v AXL_SERVICE_STAGE_TEST_CUT $cut"
        [[ -n "${STAGE_FAIL:-}" ]] \
            && printf '%s\r\n' "set -v AXL_SERVICE_STAGE_TEST_FAIL $STAGE_FAIL"
        [[ -n "${STAGE_CLOBBER:-}" ]] \
            && printf '%s\r\n' "set -v AXL_SERVICE_STAGE_TEST_CLOBBER $STAGE_CLOBBER"
        printf '%s\r\n' "upgrade-svc.efi start --detach $args"
        # STAGE_STALL=<us>: hold the Shell after the start, so the service's
        # own timers run on before the boot ends
        [[ -n "${STAGE_STALL:-}" ]] && printf '%s\r\n' "stall $STAGE_STALL"
        printf '%s\r\n' 'echo BOOT-END' 'reset -s'
    } > "$TEST_TMPDIR/startup.nsh"
    img_put "$TEST_TMPDIR/startup.nsh" startup.nsh
    test_build_qemu_cmd
    _test_default_nic
    _test_fw_debug_uart
    BOOT_RC=0
    # Backgrounded and waited, so the boot reading has a pid to watch; stdin
    # from /dev/null, as a foreground launch under -nographic should have.
    test_timing_begin
    timeout "$STAGE_BOOT_SECS" "${TEST_QEMU_CMD[@]}" > "$TEST_TMPDIR/$tag.raw" 2>&1 < /dev/null &
    local boot_pid=$!
    test_timing_watch "$boot_pid" "$TEST_TMPDIR/$tag.raw"
    wait "$boot_pid" || BOOT_RC=$?
    test_timing_end "$BOOT_RC"
    echo "$BOOT_RC" > "$TEST_TMPDIR/$tag.rc"
    sed -E 's/\x1b\[[0-9;]*[a-zA-Z]//g; s/^\[[0-9]+\.[0-9]+\] //' "$TEST_TMPDIR/$tag.raw" \
        | tr -d '\r' > "$TEST_TMPDIR/$tag.log"
}

# Counted assertions.
PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
# has TAG TEXT -- the boot's log holds TEXT (fixed string) on some line
has() { grep -qF -- "$2" "$TEST_TMPDIR/$1.log"; }
# line TAG REGEX -- the boot's log holds a line matching REGEX exactly
line() { grep -qxE -- "$2" "$TEST_TMPDIR/$1.log"; }
check() {   # NAME COMMAND...
    local name="$1"; shift
    if "$@"; then pass "$name"; else fail "$name"; fi
}
not() { ! "$@"; }
# ended_itself TAG -- QEMU exited on the guest's own reset (rc 0), not on the
# harness's timeout (124)
ended_itself() { [[ "$(cat "$TEST_TMPDIR/$1.rc")" == 0 ]]; }

# The launcher's verdict line, and the service's.
APPLIED='upgrade-svc: staged upgrade applied'
RETRIED='upgrade-svc: staged upgrade applied on a retry'
ROLLED='upgrade-svc: staged upgrade rolled back'
up() { echo "USVC: v$1 up, no network (last upgrade: $2)"; }
# A trial's candidate comes up BEFORE the launcher records the outcome, so it
# shows the previous one: only its version is checked.
up_v() { echo "USVC: v$1 up, no network (last upgrade: "; }

# reset_disk -- P is v1, nothing else of ours, nothing recorded.
reset_disk() {
    img_put "$B/upgrade-svc-dxe-q1.efi" "$P"
    img_del "$P.new"; img_del "$P.bak"; img_del "$P.failed"; img_del "$P.new.tmp"
}

stage_finish() {
    echo ""
    printf "%s: %d passed, %d failed, %d boots (%s)\n" "$1" "$PASS" "$FAIL" "$BOOTS" "$TEST_ARCH"
    if [[ $FAIL -gt 0 ]]; then
        local f
        for f in "$TEST_TMPDIR"/*.log; do
            echo "--- $(basename "$f")"; grep -aE 'USVC|upgrade-svc:|staged upgrade|BOOT-END|test cut' "$f" | tail -8
        done
    fi
    [[ $FAIL -eq 0 && $PASS -eq $2 ]]
}
