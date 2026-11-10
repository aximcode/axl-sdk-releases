#!/bin/bash
# test-meta: arch=both needs=tmux,socat,mcopy,iconv est=115 local-only=0 builds=stderr-row,stderr-tick,console-device-hold,pack-fixture-w,stdio-bridge-fix guest=none
# guest=none: it types into the guest Shell (test_send_stdin) and
#   screendumps the display; a leased guest takes neither.
# test-stderr-row-qemu.sh -- standard error shares the console's one cursor
# with standard output, as on a Unix terminal: a line AXL writes to stderr
# starts where stdout left the cursor, and the Shell's next prompt starts on
# the row after it -- never over it.
#
# Why it matters: the firmware's StdErr and ConOut are two console splitters,
# each with its own cursor record, and the Shell places its prompt from
# ConOut's (`SetCursorPosition(0, ConOut->Mode->CursorRow)`). A line written
# through StdErr moves the devices' cursors but not that record, so the next
# prompt was drawn over the error lines -- the teardown's
# `mem: no leaks detected`, a tool's log line. Where ConOut has a device
# StdErr does not (OVMF's display), that device's cursor is the record, and it
# never moves for a StdErr line at all.
#
# Two oracles, both what a person sees, every row matched exactly (a log
# line's clock aside):
#   serial   the serial stream fed to a real terminal (tmux, at the console's
#            size, which the guest reports);
#   display  (x64) the GOP's text grid from a QEMU screendump: which cells
#            carry ink (lib/gop-cells.py), against the same expected rows.
#
# The rows: stderr-row.efi typed at the prompt (stdout and stderr
# interleaved, a log line, the teardown's leak verdict), an `echo`, a corrupt
# packed image (the pack stub's own lines, written before libaxl exists),
# another `echo`.
#
# Each boot is 80x25, and `wrap` writes lines measured against that width
# (lib/wrap-rows-lib.sh): a UEFI console wraps the moment a character lands
# in its last column, so a line exactly as wide -- the stub's re-read lines
# are -- must be followed by the next on the very next row, with no empty
# row, as on a Unix terminal. The same lines (and the stub's) redirected to
# files must keep every CR LF: exact bytes, read off the disk after the boot.
#
# Boots:
#   default    the firmware's own ErrOut (OVMF: the serial terminal only;
#              ConOut also has the display on x64). Then `resident`: a
#              service (stderr_tick.efi) writes to stderr on its own clock
#              while `stall > tick.txt` and then `stall 2> tick2.txt` run --
#              its lines reach the console and neither file gets one.
#   errout     (x64) ErrOut lists ConOut's devices, the display among them --
#              as firmware that puts errors on the screen does. The rows.
#
# Usage: ./test/integration/test-stderr-row-qemu.sh [--arch X64|AARCH64]

export TEST_SKIP_RATCHET=1
source "$(dirname "$0")/common-test.sh"
source "$(dirname "$0")/lib/pack-lib.sh"
source "$(dirname "$0")/lib/wrap-rows-lib.sh"
test_parse_args "$@"
test_setup

# A host without these cannot run the suite: under CI that is a FAILURE (a
# suite CI never runs proves nothing), on a dev box a SKIP.
for _tool in tmux socat mcopy iconv; do
    if ! command -v "$_tool" >/dev/null 2>&1; then
        if [[ "${CI:-}" == true || "${GITHUB_ACTIONS:-}" == true ]]; then
            echo "FAIL: no $_tool on the host PATH (CI must run this suite)"
            exit 1
        fi
        echo "SKIP: no $_tool on the host PATH (needs=tmux,socat,mcopy,iconv)"
        exit 77
    fi
done

declare -A _M=([X64]=x64 [AARCH64]=aa64); _a="${_M[$TEST_ARCH]:-x64}"
"$PROJECT_DIR/scripts/axl-build.sh" ARCH="$_a" stderr-row stderr-tick console-device-hold pack-fixture-w stdio-bridge-fix tools 2>&1 | tail -2
BUILD_DIR="$(test_build_dir "$_a")"
test_add_efi "$BUILD_DIR/stderr-row.efi"
test_add_efi "$BUILD_DIR/stderr_tick.efi"
test_add_efi "$BUILD_DIR/tools/grep.efi"
test_add_efi "$BUILD_DIR/stdio-bridge-fix.efi"
# A second Shell, for the nested case: the firmware's own, as the boot uses.
INNER_SHELL=$(find_shell_efi "$TEST_ARCH") || true
[[ -n "$INNER_SHELL" && -f "$INNER_SHELL" ]] && test_add_efi "$INNER_SHELL" inner.efi
test_add_efi "$BUILD_DIR/console-device-present-probe.efi" probe.efi
declare -A _PE=([X64]=34404 [AARCH64]=43620)
pack_corrupt_copy "$BUILD_DIR/pack-fixture-w.efi" "$TEST_TMPDIR/pack-corrupt.efi" "${_PE[$TEST_ARCH]}"
if cmp -s "$BUILD_DIR/pack-fixture-w.efi" "$TEST_TMPDIR/pack-corrupt.efi"; then
    echo "FAIL: pack-fixture-w.efi has no packed payload to damage"
    exit 1
fi
test_add_efi "$TEST_TMPDIR/pack-corrupt.efi"

PASS=0
FAIL=0
SKIPPED=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIPPED=$((SKIPPED + 1)); }

# The console's text size, from the guest at each boot. Each boot asks for
# 80x25: the stub's 80-character re-read lines then fill a row exactly, as
# stderr-row.efi's `wrap` lines do. A UEFI console wraps the moment a
# character lands in its last column, so a line's own CRLF after that was a
# second, empty row; the row after an exactly-wide line must be the next
# line, as on a Unix terminal.
COLS=""
ROWS=""

# What the screen must read after the run, the clock of a log line as `T`.
EXPECTED='SR-BOOTED
FS0:\> stderr-row.efi
SR-partial SR-line
SR-err-1
SR-out-2
SR-mix SR-err-2
T [ERROR] stderr-row: SR-log-1
SR-out-3
T [INFO]  mem: no leaks detected
FS0:\> echo %T%-after
sr-after
FS0:\> pack-corrupt.efi
axl-pack: packed image is corrupt -- re-reading from the volume (attempt 1 of 2)
axl-pack: packed image is corrupt -- re-reading from the volume (attempt 2 of 2)
axl-pack: packed image is corrupt
FS0:\> echo %T%-done
sr-done
FS0:\>'

# Exact whole lines of the clean serial log equal to $1.
lines_eq() {
    local n
    test_clean_log
    n=$(grep -acxF -- "$1" "$TEST_CLEAN_LOG" || true)
    echo "${n:-0}"
}

# Wait up to $2 s for a whole serial line exactly $1.
wait_line() {
    local i
    for ((i = 0; i < $2 * 5; i++)); do
        (( $(lines_eq "$1") >= 1 )) && return 0
        kill -0 "$TEST_QEMU_PID" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}

# DxeCore's report of the corrupt image's failed StartImage is DEBUG output,
# on neither arch's console (aa64 sends it to its debug UART), so the stream
# is drawn as the guest wrote it.
# The serial stream so far, drawn by tmux at COLSxROWS: one row per line,
# trailing blanks and blank trailing rows dropped, a log line's clock as `T`.
render_serial() {
    local sock="$TEST_TMPDIR/tmux-render.sock" conf="$TEST_TMPDIR/tmux-render.conf"
    local i screen prev=""
    printf 'set -g status off\n' > "$conf"
    timeout 10 env -u TMUX tmux -S "$sock" -f "$conf" new-session -d \
        -x "$COLS" -y "$ROWS" "cat ${TEST_LOG@Q}; sleep 60" || return 0
    # Settled when two captures 0.2 s apart agree.
    for ((i = 0; i < 50; i++)); do
        sleep 0.2
        screen=$(timeout 5 env -u TMUX tmux -S "$sock" capture-pane -p 2>/dev/null || true)
        [[ -n "$screen" && "$screen" == "$prev" ]] && break
        prev=$screen
    done
    timeout 5 env -u TMUX tmux -S "$sock" kill-server 2>/dev/null || true
    rm -f "$sock"
    printf '%s\n' "$screen" | sed -E 's/[[:space:]]+$//; s/^[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)? \[/T [/' \
        | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'
}

# The display's text grid (x64): which cells carry ink, from a QEMU
# screendump, against $2's -- each `T` a 15-character clock, and the
# cursor drawn after the last prompt. The layout a person at the screen sees,
# without reading glyphs: a row drawn over another, or missing, changes it.
check_gop() {
    local ppm="$TEST_TMPDIR/gop.ppm" want got i
    rm -f "$ppm"
    printf 'screendump %s\n' "$ppm" \
        | timeout 10 socat - "UNIX-CONNECT:$TEST_TMPDIR/mon.sock" > /dev/null 2>&1 || true
    for ((i = 0; i < 50; i++)); do
        [[ -s "$ppm" ]] && break
        sleep 0.1
    done
    sleep 0.5
    # STDERR_ROW_KEEP=<dir> keeps each boot's screendump.
    [[ -n "${STDERR_ROW_KEEP:-}" ]] && cp -f "$ppm" "$STDERR_ROW_KEEP/gop-$1.ppm"
    want=$(printf '%s _\n' "$2" | sed -E 's/^T \[/HH:MM:SS.uuuuuu [/; s/[^ ]/#/g')
    if ! got=$(python3 "$(dirname "$0")/lib/gop-cells.py" "$ppm" "$COLS" "$ROWS"); then
        fail "$1: the display's screendump could not be read"
        return
    fi
    got=$(printf '%s\n' "$got" | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}')
    if [[ "$got" == "$want" ]]; then
        pass "$1: the display shows the same rows, laid out the same"
    else
        fail "$1: the display shows the same rows, laid out the same"
        diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | sed 's/^/    | /' || true
    fi
}

# check_screen NAME ROWS -- the serial screen reads exactly ROWS, and (x64)
# the display is laid out the same; on aa64, which has no display, the
# display check is a SKIP so both arches count the same checks.
check_screen() {
    local screen
    screen=$(render_serial)
    if [[ "$screen" == "$2" ]]; then
        pass "$1: the serial screen reads every row, each prompt on its own row"
    else
        fail "$1: the serial screen reads every row, each prompt on its own row"
        diff <(printf '%s\n' "$2") <(printf '%s\n' "$screen") | sed 's/^/    | /' || true
    fi
    if [[ "$TEST_ARCH" == X64 ]]; then
        check_gop "$1" "$2"
    else
        skip "$1: the display shows the same rows (no display on $TEST_ARCH)"
    fi
}

# Boot with the host typing on serial; the screen cleared, one marker row.
boot() {
    {
        echo "@echo -off"
        echo "fs0:"
        echo "cd \\"
        echo "set -v T sr"
        echo "mode 80 25"
        echo "stderr-row.efi size"
        echo "cls"
        echo "echo SR-BOOTED"
    } | test_set_startup
    test_build_image
    test_build_qemu_cmd
    test_add_no_network
    rm -f "$TEST_TMPDIR/mon.sock"
    TEST_QEMU_CMD+=(-monitor "unix:$TEST_TMPDIR/mon.sock,server,nowait")
    test_enable_stdin
    test_run_background
    if ! wait_line "SR-BOOTED" 120; then
        echo "FAIL: the guest never reached its prompt"
        test_clean_log; tail -30 "$TEST_CLEAN_LOG"
        exit 1
    fi
    local size
    # Unanchored: the mode switch puts the terminal's ESC[=3h ahead of it.
    size=$(grep -aoE 'SR-SIZE [0-9]+x[0-9]+$' "$TEST_CLEAN_LOG" | tail -1 || true)
    if [[ -z "$size" ]]; then
        echo "FAIL: the guest did not report its console size"
        grep -a 'SR-SIZE' "$TEST_CLEAN_LOG" || true
        exit 1
    fi
    size=${size#SR-SIZE }
    COLS=${size%x*}
    ROWS=${size#*x}
    echo "  console: ${COLS}x${ROWS}"
    sleep 1
}

# Power the guest off from its Shell and wait for QEMU to go.
poweroff() {
    local i
    test_send_stdin 'reset -s\r'
    for ((i = 0; i < 100; i++)); do
        kill -0 "$TEST_QEMU_PID" 2>/dev/null || break
        sleep 0.2
    done
}

# The scenario: the fixture typed at the prompt, then a command after it.
# $1 names the boot.
run_rows() {
    test_send_stdin 'stderr-row.efi\r'
    if ! wait_line "SR-out-3" 30; then
        fail "$1: stderr-row.efi ran"
        return
    fi
    sleep 1
    test_send_stdin 'echo %T%-after\r'
    wait_line "sr-after" 20 || true
    sleep 1
    # The pack stub, which writes its own lines before libaxl exists.
    test_send_stdin 'pack-corrupt.efi\r'
    wait_line "axl-pack: packed image is corrupt" 30 || true
    sleep 1
    test_send_stdin 'echo %T%-done\r'
    wait_line "sr-done" 20 || true
    sleep 1
    check_screen "$1" "$EXPECTED"
    test_clean_log
    if test_check_leaks; then
        pass "$1: the fixture's leak verdict is clean"
    else
        fail "$1: the fixture's leak verdict is clean"
    fi
}

# Ticks seen in a stretch of the clean serial log: `SR-TICK <n>` anywhere on
# a line (a tick arrives on its own clock, wherever the cursor is).
ticks_in() { { grep -aoE 'SR-TICK [0-9]+' "$1" || true; } | wc -l; }

# Wait up to $2 s for tick $1 on the serial log.
wait_tick() {
    local i
    for ((i = 0; i < $2 * 5; i++)); do
        test_clean_log
        grep -aqE "SR-TICK $1([^0-9]|\$)" "$TEST_CLEAN_LOG" && return 0
        kill -0 "$TEST_QEMU_PID" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}

# The resident case: a service whose standard error was the console when it
# started writes on its own clock while OTHER commands run under `>` and
# then `2>`, and while a shared-driver launcher runs `2> d.txt` -- its stdio
# bridge is aimed at THAT driver, not at the service. Its lines are the
# console's: they reach the screen and land in no file. Routed by
# gST->ConOut they would land in the `>` file, by gST->StdErr in the `2>`
# one, and by any live bridge in d.txt -- each window fails one wrong
# routing.
run_resident() {
    local n_win n_out n_err n_brg
    # Typed in one go: the service's first line is due 1 s after it starts,
    # inside the first stall; its 70 lines span all three.
    test_send_stdin 'echo %T%-r1\r'
    test_send_stdin 'stderr_tick.efi start --detach\r'
    test_send_stdin 'stall 2000000 > tick.txt\r'
    test_send_stdin 'stall 2000000 2> tick2.txt\r'
    test_send_stdin 'stdio-bridge-fix.efi wait 2> tick3.txt\r'
    # The next marker is typed only once the ticks are over: a tick landing
    # in the middle of a marker's line would hide the marker.
    if ! wait_tick 70 30; then
        fail "resident: the service wrote its 70 ticks"
        return
    fi
    sleep 0.5
    test_send_stdin 'echo %T%-r3\r'
    test_send_stdin 'type tick.txt\r'
    test_send_stdin 'echo %T%-r4\r'
    test_send_stdin 'type tick2.txt\r'
    test_send_stdin 'echo %T%-r5\r'
    test_send_stdin 'type tick3.txt\r'
    test_send_stdin 'echo %T%-r6\r'
    wait_line "sr-r6" 20 || true
    test_clean_log
    if ! test_slice_log "sr-r1" "sr-r3" "$TEST_TMPDIR/r12.txt" \
       || ! test_slice_log "sr-r3" "sr-r4" "$TEST_TMPDIR/r34.txt" \
       || ! test_slice_log "sr-r4" "sr-r5" "$TEST_TMPDIR/r45.txt" \
       || ! test_slice_log "sr-r5" "sr-r6" "$TEST_TMPDIR/r56.txt"; then
        fail "resident: the markers are in the serial log"
        return
    fi
    # The launcher's dispatch prints WAIT-BEGIN and WAIT-END (its stdout,
    # the console) around its 2 s: the ticks between them were written while
    # its bridge was live, so `tick3.txt` staying empty means something.
    tr -d '\r' < "$TEST_CLEAN_LOG" | tr '\n' ' ' \
        | { grep -aoE 'WAIT-BEGIN.*WAIT-END' || true; } > "$TEST_TMPDIR/rwait.txt"
    n_live=$(ticks_in "$TEST_TMPDIR/rwait.txt")
    n_win=$(ticks_in "$TEST_TMPDIR/r12.txt")
    n_out=$(ticks_in "$TEST_TMPDIR/r34.txt")
    n_err=$(ticks_in "$TEST_TMPDIR/r45.txt")
    n_brg=$(ticks_in "$TEST_TMPDIR/r56.txt")
    if (( n_win >= 40 )); then
        pass "resident: ticks reach the console while the three commands run ($n_win)"
    else
        fail "resident: ticks reach the console while the three commands run ($n_win)"
    fi
    if (( n_out == 0 )); then
        pass "resident: none is in the \`>\` command's file"
    else
        fail "resident: none is in the \`>\` command's file ($n_out)"
    fi
    if (( n_err == 0 )); then
        pass "resident: none is in the \`2>\` command's file"
    else
        fail "resident: none is in the \`2>\` command's file ($n_err)"
    fi
    if (( n_live >= 10 )); then
        pass "resident: ticks fall inside the launcher's live dispatch ($n_live)"
    else
        fail "resident: ticks fall inside the launcher's live dispatch ($n_live)"
    fi
    if (( n_brg == 0 )); then
        pass "resident: none is in the shared-driver launcher's \`2>\` file"
    else
        fail "resident: none is in the shared-driver launcher's \`2>\` file ($n_brg)"
    fi
}

# The redirect case, from a cleared screen: stdout to a file, then to a pipe,
# with stderr on the console both times. The error lines are rows of their
# own and no prompt is drawn over them; the file and the pipe get stdout only
# (grep.efi prints every `SR-` line it is given, so a stderr line in the pipe
# would be drawn twice). `type` ends what it prints with an empty row.
EXPECTED_REDIR='FS0:\> stderr-row.efi > sr.txt
SR-err-1
SR-err-2
T [ERROR] stderr-row: SR-log-1
T [INFO]  mem: no leaks detected
FS0:\> type sr.txt
SR-partial SR-line
SR-out-2
SR-mix SR-out-3

FS0:\> stderr-row.efi | grep.efi SR-
SR-err-1
SR-err-2
T [ERROR] stderr-row: SR-log-1
T [INFO]  mem: no leaks detected
SR-partial SR-line
SR-out-2
SR-mix SR-out-3
T [INFO]  mem: no leaks detected
FS0:\>'

run_redirect_rows() {
    test_send_stdin 'cls\r'
    sleep 1
    test_send_stdin 'stderr-row.efi > sr.txt\r'
    wait_line "SR-err-1" 30 || true
    sleep 1
    test_send_stdin 'type sr.txt\r'
    wait_line "SR-mix SR-out-3" 20 || true
    sleep 1
    test_send_stdin 'stderr-row.efi | grep.efi SR-\r'
    wait_line_n "SR-mix SR-out-3" 2 30 || true
    sleep 1
    check_screen "$1/redirect" "$EXPECTED_REDIR"
}

# The bridge case, from a cleared screen: a resident shared driver has no
# Shell handles of its own -- each launcher lends it its own through the
# stdio bridge for the one dispatch. Its standard error is the LAUNCHER's,
# decided again for every launcher. In the default boot the driver is
# loaded under `2> d1.txt` and its line goes to that file; the next, console
# launcher's goes to the screen on a row of its own; a third launcher's
# `2> d2.txt` takes it again. In the errout boot the order is the other way
# round: loaded by a console launcher, then a `2> d1.txt` one must still
# take the line.
EXPECTED_BRIDGE='FS0:\> stdio-bridge-fix.efi err 2> d1.txt
FS0:\> stdio-bridge-fix.efi err
DRIVERERR
T [INFO]  mem: no leaks detected
FS0:\> stdio-bridge-fix.efi err 2> d2.txt
FS0:\> type d1.txt
DRIVERERR
T [INFO]  mem: no leaks detected

FS0:\> type d2.txt
DRIVERERR
T [INFO]  mem: no leaks detected

FS0:\>'

EXPECTED_BRIDGE_CONSOLE_FIRST='FS0:\> stdio-bridge-fix.efi err
DRIVERERR
T [INFO]  mem: no leaks detected
FS0:\> stdio-bridge-fix.efi err 2> d1.txt
FS0:\> type d1.txt
DRIVERERR
T [INFO]  mem: no leaks detected

FS0:\>'

run_bridge_rows_console_first() {
    test_send_stdin 'cls\r'
    sleep 1
    test_send_stdin 'stdio-bridge-fix.efi err\r'
    wait_line "DRIVERERR" 30 || true
    sleep 1
    test_send_stdin 'stdio-bridge-fix.efi err 2> d1.txt\r'
    sleep 2
    test_send_stdin 'type d1.txt\r'
    wait_line_n "DRIVERERR" 2 20 || true
    sleep 1
    check_screen "$1/bridge" "$EXPECTED_BRIDGE_CONSOLE_FIRST"
}

run_bridge_rows() {
    test_send_stdin 'cls\r'
    sleep 1
    test_send_stdin 'stdio-bridge-fix.efi err 2> d1.txt\r'
    sleep 2
    test_send_stdin 'stdio-bridge-fix.efi err\r'
    wait_line "DRIVERERR" 30 || true
    sleep 1
    test_send_stdin 'stdio-bridge-fix.efi err 2> d2.txt\r'
    sleep 2
    test_send_stdin 'type d1.txt\r'
    sleep 1
    test_send_stdin 'type d2.txt\r'
    wait_line_n "DRIVERERR" 3 20 || true
    sleep 1
    check_screen "$1/bridge" "$EXPECTED_BRIDGE"
}

# The nested case: a Shell started from the Shell. A command there inherits
# the OUTER Shell's console StdErr file, while the inner Shell's own `>e` is
# its own copy of that file -- the same static, in a second image of the same
# binary. It must count as the console all the same. (The inner Shell clears
# the screen as it starts.)
EXPECTED_NESTED='FS0:\> stderr-row.efi
SR-partial SR-line
SR-err-1
SR-out-2
SR-mix SR-err-2
T [ERROR] stderr-row: SR-log-1
SR-out-3
T [INFO]  mem: no leaks detected
FS0:\> echo %T%-nested
sr-nested
FS0:\>'

run_nested_rows() {
    if [[ -z "$INNER_SHELL" ]]; then
        skip "$1/nested: the serial screen reads every row (no Shell.efi to nest)"
        skip "$1/nested: the display shows the same rows (no Shell.efi to nest)"
        return
    fi
    test_send_stdin 'cls\r'
    sleep 1
    test_send_stdin 'inner.efi -nostartup -noversion -nomap\r'
    sleep 3
    test_send_stdin 'stderr-row.efi\r'
    wait_line_n "SR-out-3" 2 30 || true
    sleep 1
    test_send_stdin 'echo %T%-nested\r'
    wait_line "sr-nested" 20 || true
    sleep 1
    check_screen "$1/nested" "$EXPECTED_NESTED"
    test_send_stdin 'exit\r'
    sleep 2
}

# How many whole serial lines are $1; wait up to $3 s for $2 of them.
wait_line_n() {
    local i
    for ((i = 0; i < $3 * 5; i++)); do
        (( $(lines_eq "$1") >= $2 )) && return 0
        kill -0 "$TEST_QEMU_PID" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}

# The pager case: with the Shell's page break on (as `-b` turns it on), 70
# stderr lines in a row. The Shell's console logger -- gST->ConOut under the
# Shell -- stops at each screenful and WAITS for a key, and it is not built
# to be entered from a timer callback; standard error goes to the console
# splitter beneath it, so all 70 lines arrive with no prompt to answer.
run_pager() {
    local n
    test_send_stdin 'cls\r'
    sleep 1
    test_send_stdin 'stderr-row.efi many\r'
    if wait_line "SR-MANY 70" 30; then
        pass "pager: 70 stderr lines arrive with page break on, no key pressed"
    else
        fail "pager: 70 stderr lines arrive with page break on, no key pressed"
        test_send_stdin 'q'
        sleep 1
    fi
    test_clean_log
    n=$(grep -acF "Press ENTER to continue" "$TEST_CLEAN_LOG" || true)
    if [[ "$n" == 0 ]]; then
        pass "pager: no page-break prompt stopped them"
    else
        fail "pager: no page-break prompt stopped them ($n)"
    fi
}

# The screen after `stderr-row.efi wrap` from a cleared one (wrap_rows).
wrap_expected() {
    printf 'FS0:\\> stderr-row.efi wrap\n'
    wrap_rows
    printf 'FS0:\\> echo %%T%%-wrapped\nsr-wrapped\nFS0:\\>'
}

# The width case: lines exactly the console's width (stdout, two in a row,
# stderr, a log line), one short of it and one past it. Then the same run
# redirected, `>` and `2>` to files, checked after the boot (check_wrap_files).
run_wrap_rows() {
    test_send_stdin 'cls\r'
    sleep 1
    test_send_stdin 'stderr-row.efi wrap\r'
    wait_line 'SR-W-END' 30 || true
    sleep 1
    test_send_stdin 'echo %T%-wrapped\r'
    wait_line "sr-wrapped" 20 || true
    sleep 1
    check_screen "$1/wrap" "$(wrap_expected)"
    check_wrap_wire "$1/wrap"
}

# The serial WIRE keeps one line per line. TerminalDxe never sends its own
# wrap, so the terminal is still on the full row and needs the newline; a
# screen of the console's width cannot tell (a pending wrap draws a missing
# newline the same), so this reads the stream itself: every exactly-wide
# line, and the log line, is a whole serial line.
check_wrap_wire() {
    local tag missing=""
    test_clean_log
    for tag in SR-W1 SR-W4 SR-W5 SR-W6 SR-W8 SR-W9; do
        (( $(lines_eq "$(wrap_text "$tag" "$COLS")") >= 1 )) || missing+=" $tag"
    done
    grep -aqE "^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6} \[ERROR\] stderr-row: $(wrap_text SR-W7 $((COLS - 36)) | sed 's/\./\\./g')\$" \
        "$TEST_CLEAN_LOG" || missing+=" SR-W7"
    if [[ -z "$missing" ]]; then
        pass "$1: on the serial wire each exactly-wide line is a line of its own"
    else
        fail "$1: on the serial wire each exactly-wide line is a line of its own (not:$missing)"
    fi
}

# A line exactly the width on standard output, its newline on standard
# error: the two share one cursor, as on a terminal, so the newline ends the
# wrapped line and SR-X-NEXT is on the next row.
run_xstream_rows() {
    local x
    x=$(wrap_text SR-X "$COLS")
    test_send_stdin 'cls\r'
    sleep 1
    test_send_stdin 'stderr-row.efi xstream\r'
    wait_line 'SR-X-NEXT' 30 || true
    sleep 1
    test_send_stdin 'echo %T%-xs\r'
    wait_line "sr-xs" 20 || true
    sleep 1
    check_screen "$1/xstream" "$(printf 'FS0:\\> stderr-row.efi xstream\n%s\nSR-X-NEXT\nT [INFO]  mem: no leaks detected\nFS0:\\> echo %%T%%-xs\nsr-xs\nFS0:\\>' "$x")"
}

run_wrap_files() {
    test_send_stdin 'stderr-row.efi wrap > w1.txt 2> w2.txt\r'
    test_send_stdin 'pack-corrupt.efi 2> w3.txt\r'
    test_send_stdin 'echo %T%-wfiles\r'
    wait_line "sr-wfiles" 30 || true
}

# The same lines in the mode the firmware boots in, with no `mode` command:
# there ConSplitter can hold a device at a geometry of its own (x64: the
# display at 100 columns, and OVMF's serial terminal does not wrap a
# 100-character line with it), so a line as wide as the display wraps one
# device and not the other, and the one it did not wrap must still get its
# newline. Its own boot; the wire is read, and the fixture reads every
# device's cursor after a line as wide as the narrowest.
run_wrap_boot_mode() {
    local cols=$COLS size
    {
        echo "@echo -off"
        echo "fs0:"
        echo "cls"
        echo "stderr-row.efi rows"
        echo "stderr-row.efi size"
        echo "stderr-row.efi wrap"
        echo "reset -s"
    } | test_set_startup
    test_build_image
    test_build_qemu_cmd
    test_add_no_network
    test_run_foreground 60
    test_clean_log
    size=$(grep -aoE 'SR-SIZE [0-9]+x[0-9]+$' "$TEST_CLEAN_LOG" | tail -1 || true)
    size=${size#SR-SIZE }
    COLS=${size%x*}
    echo "  boot mode: $size"
    grep -aE '^SR-ROWS [0-9]' "$TEST_CLEAN_LOG" | sed 's/^/    /' || true
    # Each device decides from its own cursor: the narrowest one wraps the
    # line, the wider ones do not, and each must end one row down.
    if grep -aqx 'SR-ROWS ok' "$TEST_CLEAN_LOG"; then
        pass "boot-mode/rows: every device one row down after a line as wide as the narrowest"
    else
        fail "boot-mode/rows: every device one row down after a line as wide as the narrowest"
    fi
    # The console's own record, which the Shell places its prompt from.
    if grep -aqx 'SR-ROWS-SPLITTER ok' "$TEST_CLEAN_LOG"; then
        pass "boot-mode/rows: the console's cursor record is the devices' cursor after it"
    else
        fail "boot-mode/rows: the console's cursor record is the devices' cursor after it ($(grep -a 'SR-ROWS-SPLITTER' "$TEST_CLEAN_LOG" || true))"
    fi
    if [[ -z "$COLS" ]]; then
        fail "boot-mode/wrap: the guest reported its console size"
    else
        check_wrap_wire "boot-mode/wrap"
    fi
    COLS=$cols
}

# The Shell's scroll-back (x64): the Shell's console logger keeps its own
# copy of the screen, and Page Up redraws the display from it. In the mode
# the firmware boots in (display 100 columns, serial terminal 80), a line of
# 80 wraps the terminal and not the display, so the logger -- 100 columns,
# like the display -- must still be given its line break, or the copy has
# the next line on the same row. Read off the display after Page Up: a row
# of the 80 wide line's cells, then a row of SR-H-NEXT's 9.
run_history_boot() {
    local ppm="$TEST_TMPDIR/hist.ppm" grid i
    if [[ "$TEST_ARCH" != X64 ]]; then
        skip "history: the scroll-back has the wide line and the next on rows of their own (no display on $TEST_ARCH)"
        return
    fi
    {
        echo "@echo -off"
        echo "fs0:"
        echo "cd \\"
        echo "echo SR-HBOOT"
    } | test_set_startup
    test_build_image
    test_build_qemu_cmd
    test_add_no_network
    rm -f "$TEST_TMPDIR/mon.sock"
    TEST_QEMU_CMD+=(-monitor "unix:$TEST_TMPDIR/mon.sock,server,nowait")
    test_enable_stdin
    test_run_background
    if ! wait_line "SR-HBOOT" 120; then
        fail "history: the guest reached its prompt"
        # It never reached the Shell, so `reset -s` cannot be typed: stop it.
        kill "$TEST_QEMU_PID" 2>/dev/null || true
        wait "$TEST_QEMU_PID" 2>/dev/null || true
        return
    fi
    sleep 1
    test_send_stdin 'cls\r'
    sleep 1
    test_send_stdin 'stderr-row.efi history\r'
    wait_line "SR-H-END" 30 || true
    sleep 1
    printf 'sendkey pgup\n' | timeout 10 socat - "UNIX-CONNECT:$TEST_TMPDIR/mon.sock" > /dev/null 2>&1 || true
    local wide next ok=0 t
    wide=$(printf '%*s' 80 '' | tr ' ' '#')
    next=$(printf '%*s' 9 '' | tr ' ' '#')
    # The redraw takes its own time: read the display until the two rows are
    # there, for up to 10 s, rather than once after a guess.
    for ((t = 0; t < 20 && ok == 0; t++)); do
        sleep 0.5
        rm -f "$ppm"
        printf 'screendump %s\n' "$ppm" \
            | timeout 10 socat - "UNIX-CONNECT:$TEST_TMPDIR/mon.sock" > /dev/null 2>&1 || true
        for ((i = 0; i < 50; i++)); do
            [[ -s "$ppm" ]] && break
            sleep 0.1
        done
        sleep 0.2
        grid=$(python3 "$(dirname "$0")/lib/gop-cells.py" "$ppm" 100 31 2>/dev/null || true)
        if [[ "$(grep -x -A1 -- "$wide" <<< "$grid" | sed -n 2p)" == "$next" ]]; then
            ok=1
        fi
    done
    if (( ok == 1 )); then
        pass "history: the scroll-back has the wide line and the next on rows of their own"
    else
        fail "history: the scroll-back has the wide line and the next on rows of their own"
        printf '%s\n' "$grid" | awk '{ print length($0) }' | tr '\n' ' ' | sed 's/^/    row widths: /'
        echo
    fi
    poweroff
}

# The line writer against fake standard-error devices (stderr-row.c,
# fakes): a device tagged StdErr and not ConOut gets each standard-error line
# directly, with its handle, so each case reads exactly what one device was
# sent. Per terminal type, the break after a wrap (CR LF and the cursor back
# for EDK2 TerminalDxe's eight types that send no wrap of their own, a space
# and a backspace for TtyTerm and for no type); a CR after the wrap with
# its LF in the next write, still one break; a new device on a gone one's
# Mode memory, whose first newline is its own.
FAKE_CASES="pcansi vt100 vt100+ vtutf8 linux xtermr6 vt400 sco ttyterm no-type cr-then-lf reused-mode"

run_fakes() {
    local c line i
    test_send_stdin 'stderr-row.efi fakes\r'
    for ((i = 0; i < 150; i++)); do
        test_clean_log
        grep -aqE '^SR-FAKE reused-mode ' "$TEST_CLEAN_LOG" && break
        sleep 0.2
    done
    for c in $FAKE_CASES; do
        line=$(grep -aF "SR-FAKE $c " "$TEST_CLEAN_LOG" | tail -1 || true)
        if [[ "$line" == "SR-FAKE $c ok" ]]; then
            pass "fake device: $c"
        else
            fail "fake device: $c (${line:-no verdict})"
        fi
    done
}

# A file is not a console: `>` and `2>` get every line with its CR LF, exact
# bytes (UCS-2, after the Shell's byte-order mark), wherever the console's
# cursor stood. Read off the boot disk once QEMU is gone.
check_wrap_files() {
    local d="$TEST_TMPDIR/wfiles" s want got
    rm -rf "$d"; mkdir -p "$d"
    if ! mcopy -n -i "$TEST_DISK" ::w1.txt ::w2.txt ::w3.txt "$d/" 2>/dev/null; then
        fail "wrap > file: w1.txt, w2.txt and w3.txt are on the disk"
        fail "wrap 2> file: the same lines, each with its CR LF"
        fail "pack stub 2> file: its three lines, each with its CR LF"
        return
    fi
    # The pack stub writes its own lines, before libaxl exists.
    { printf '\xff\xfe'
      printf '%s\r\n' \
          'axl-pack: packed image is corrupt -- re-reading from the volume (attempt 1 of 2)' \
          'axl-pack: packed image is corrupt -- re-reading from the volume (attempt 2 of 2)' \
          'axl-pack: packed image is corrupt' | iconv -f UTF-8 -t UTF-16LE
    } > "$d/want-p"
    if cmp -s "$d/want-p" "$d/w3.txt"; then
        pass "pack stub 2> file: its three lines, each with its CR LF, exact bytes"
    else
        fail "pack stub 2> file: its three lines, each with its CR LF, exact bytes"
        diff <(od -c "$d/want-p") <(od -c "$d/w3.txt") | head -20 | sed 's/^/    | /' || true
    fi
    for s in o e; do
        want="$d/want-$s"
        got="$d/w$([[ $s == o ]] && echo 1 || echo 2).txt"
        { printf '\xff\xfe'
          wrap_lines | sed -n "s/^$s //p" | sed 's/$/\r/' | iconv -f UTF-8 -t UTF-16LE
        } > "$want"
        # The clock of a log line is the run's own: `T` in the file too.
        { head -c 2 "$got"
          tail -c +3 "$got" | iconv -f UTF-16LE -t UTF-8 \
              | sed -E 's/^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{6} \[/T [/' \
              | iconv -f UTF-8 -t UTF-16LE
        } > "$d/norm-$s"
        if cmp -s "$want" "$d/norm-$s"; then
            pass "wrap $([[ $s == o ]] && echo '>' || echo '2>') file: every line, each with its CR LF, exact bytes"
        else
            fail "wrap $([[ $s == o ]] && echo '>' || echo '2>') file: every line, each with its CR LF, exact bytes"
            diff <(od -c "$want") <(od -c "$d/norm-$s") | head -20 | sed 's/^/    | /' || true
        fi
    done
}

echo "=== stderr shares the console's cursor ($TEST_ARCH) ==="

# --- default: the firmware's own ErrOut ---------------------------------------
boot
run_rows default
run_wrap_rows default
run_xstream_rows default
run_wrap_files
run_fakes
run_redirect_rows default
run_bridge_rows default
run_nested_rows default
run_resident
run_pager
poweroff
check_wrap_files
run_wrap_boot_mode
run_history_boot

# --- errout: ErrOut lists the display (x64; aa64 has no display) --------------
if [[ "$TEST_ARCH" == X64 ]]; then
    {
        echo "@echo -off"
        echo "fs0:"
        echo "probe.efi errout"
        echo "reset -s"
    } | test_set_startup
    test_build_image
    test_build_qemu_cmd
    test_add_no_network
    test_run_foreground 40
    test_clean_log
    if grep -aqE '^PROBE errout=[1-9][0-9]*$' "$TEST_CLEAN_LOG"; then
        pass "errout: ErrOut now lists ConOut's devices, the display among them"
        boot
        run_rows errout
        run_wrap_rows errout
        run_redirect_rows errout
        run_bridge_rows_console_first errout
        poweroff
    else
        fail "errout: ErrOut now lists ConOut's devices (saw '$(grep -a 'PROBE errout' "$TEST_CLEAN_LOG" || true)')"
    fi
else
    for c in "errout: ErrOut now lists ConOut's devices" \
             "errout: the serial screen reads every row" \
             "errout: the display shows the same rows" \
             "errout: the fixture's leak verdict is clean" \
             "errout/wrap: the serial screen reads every row" \
             "errout/wrap: the display shows the same rows" \
             "errout/wrap: on the serial wire each exactly-wide line is a line of its own" \
             "errout/redirect: the serial screen reads every row" \
             "errout/redirect: the display shows the same rows" \
             "errout/bridge: the serial screen reads every row" \
             "errout/bridge: the display shows the same rows"; do
        skip "$c (the errout boot needs a display; none on $TEST_ARCH)"
    done
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIPPED skipped"
(( FAIL == 0 ))
