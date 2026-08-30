#!/bin/bash
# axl-desc: sample a booting .efi and report where it spends CPU
# profile-qemu.sh — sampling profiler for an axl-sdk app running under QEMU.
#
# "perf record/report" for a UEFI app: it boots the app under QEMU with the
# GDB stub (scripts/run-qemu.sh --gdb), periodically interrupts the guest,
# records the call stack, and — using the app's DWARF via scripts/gdb-syms.py
# — reports where the CPU actually spent its time. It answers the recurring
# "QEMU is pegged at 100%, WHERE is it spinning?" with a file:line, without
# any instrumentation in the app.
#
# The guest runs under TCG (the --gdb stub disables KVM), so it is slower than
# a normal run — fine for a profiler, but pick an app duration long enough to
# cover the sampling window.
#
# Usage:
#   scripts/profile-qemu.sh [options] <app.efi> [app args...]
#
# Options:
#   --arch X64|AARCH64     target arch (default: X64)
#   --samples N            number of stack samples (default: 200)
#   --interval SECONDS     seconds between samples (default: 0.05)
#   --out STEM             report path stem (default: /tmp/axl-profile)
#   --port N               GDB stub TCP port (default: 1234)
#   --build-dir DIR        axl build dir with the app's .so (default:
#                          out/native-<arch>)
#   --ovmf-build-dir DIR   OVMF DEBUG build dir, to also symbolize firmware
#                          frames (optional; unresolved firmware frames still
#                          show as raw addresses, which distinguishes an
#                          app spin from a firmware/HLT one)
#   --warmup SECONDS       max wait for the app image to load (default: 90;
#                          TCG boot is slow)
#
# Outputs:
#   <STEM>.txt      flat profile (hottest leaf first) — printed on completion
#   <STEM>.folded   collapsed stacks for FlameGraph's flamegraph.pl
#
# Env: honors QEMU_BIN / OVMF_CODE / OVMF_VARS exactly as run-qemu.sh does.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
RUN_QEMU="$SCRIPT_DIR/run-qemu.sh"
GDB_SYMS="$SCRIPT_DIR/gdb-syms.py"
GDB_SAMPLE="$SCRIPT_DIR/gdb-sample.py"

ARCH="X64"
SAMPLES=200
INTERVAL=0.05
OUT="/tmp/axl-profile"
PORT=1234
BUILD_DIR=""
OVMF_BUILD_DIR=""
WARMUP=90
GDB_BIN="${GDB:-gdb}"

# --- parse options (stop at the first non-option = the app.efi) ------------
APP=""
APP_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)           ARCH="$2"; shift 2 ;;
        --samples)        SAMPLES="$2"; shift 2 ;;
        --interval)       INTERVAL="$2"; shift 2 ;;
        --out)            OUT="$2"; shift 2 ;;
        --port)           PORT="$2"; shift 2 ;;
        --build-dir)      BUILD_DIR="$2"; shift 2 ;;
        --ovmf-build-dir) OVMF_BUILD_DIR="$2"; shift 2 ;;
        --warmup)         WARMUP="$2"; shift 2 ;;
        -h|--help)        sed -n '2,37p' "$0"; exit 0 ;;
        --) shift; break ;;
        -*) echo "profile-qemu.sh: unknown option '$1'" >&2; exit 2 ;;
        *)  APP="$1"; shift; break ;;
    esac
done
APP_ARGS=("$@")

if [[ -z "$APP" ]]; then
    echo "profile-qemu.sh: no app.efi given (try --help)" >&2
    exit 2
fi
if [[ ! -f "$APP" ]]; then
    echo "profile-qemu.sh: app not found: $APP" >&2
    exit 2
fi

_arch_lc="$(echo "$ARCH" | tr 'A-Z' 'a-z')"
[[ "$_arch_lc" == "aarch64" ]] && _arch_lc="aa64"
[[ -z "$BUILD_DIR" ]] && BUILD_DIR="$PROJECT_DIR/out/native-$_arch_lc"
# gdb-syms.py requires a --build-dir; when no OVMF dir is given, point it at
# the axl build dir so the arg is satisfied and only app frames resolve.
[[ -z "$OVMF_BUILD_DIR" ]] && OVMF_BUILD_DIR="$BUILD_DIR"

WORK="$(mktemp -d)"
DEBUGCON="$WORK/debugcon.log"
SERIAL="$WORK/serial.log"
SYMS="$WORK/syms.gdb"
QEMU_PID=""
QEMU_TMPDIR=""   # run-qemu.sh --background's own temp dir

cleanup() {
    [[ -n "$QEMU_PID" ]] && kill "$QEMU_PID" >/dev/null 2>&1 || true
    # run-qemu --background self-cleans its ESP image + OVMF vars temp dir (often
    # /dev/shm) when the guest exits, but if we kill QEMU here and race ahead this
    # rm reclaims it deterministically; a double-rm of an already-gone dir is a
    # harmless no-op.
    [[ -n "$QEMU_TMPDIR" ]] && rm -rf "$QEMU_TMPDIR"
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# --- 1. launch the app under QEMU + the GDB stub, in the background ---------
echo "[profile] booting $APP under QEMU (--gdb :$PORT, TCG)..." >&2
BG_OUT="$("$RUN_QEMU" --arch "$ARCH" --gdb "$PORT" --background \
    --debugcon "$DEBUGCON" --serial-log "$SERIAL" \
    "$APP" ${APP_ARGS[@]+"${APP_ARGS[@]}"})"
QEMU_PID="$(sed -n 's/^QEMU_PID=//p' <<<"$BG_OUT")"
QEMU_TMPDIR="$(sed -n 's/^TMPDIR=//p' <<<"$BG_OUT")"
if [[ -z "$QEMU_PID" ]]; then
    echo "[profile] failed to launch QEMU:" >&2
    echo "$BG_OUT" >&2
    exit 1
fi

# --- 2. wait for the app image to actually load (its base must be in the ---
#        debugcon log before gdb-syms.py can symbolize it) ------------------
app_base="$(basename "$APP")"
echo "[profile] waiting for '$app_base' to load (up to ${WARMUP}s, TCG boot is slow)..." >&2
loaded=false
for _ in $(seq 1 "$((WARMUP * 2))"); do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "[profile] QEMU exited before the app loaded — serial tail:" >&2
        tail -20 "$SERIAL" >&2 || true
        exit 1
    fi
    if grep -qa "$app_base" "$DEBUGCON" 2>/dev/null; then
        loaded=true
        break
    fi
    sleep 0.5
done
if [[ "$loaded" != true ]]; then
    echo "[profile] WARNING: never saw '$app_base' in the debugcon log; " \
         "sampling anyway (firmware frames only?)." >&2
fi
# --- 2b. measure QEMU's host CPU over a settle window ----------------------
# A quick "is it even spinning?" verdict up front, so a profile that turns out
# to be an idle/HLT wait is called out as such before it confuses anyone. We
# sample QEMU_PID's utime+stime delta over CPU_WINDOW (robust to a comm with
# spaces) and express it in cores; a busy TCG vCPU pegs ~1.0 core, an idle/
# HLT-bound guest sits near 0.
CPU_WINDOW=2
qemu_cpu_cores() {   # $1 = pid ; echoes cores over CPU_WINDOW seconds
    local pid="$1" hz s1 s2 t1 t2 st
    hz="$(getconf CLK_TCK 2>/dev/null || echo 100)"
    st="$(cat "/proc/$pid/stat" 2>/dev/null)" || { echo "0"; return; }
    # Fields after the "(comm)" are utime(#14),stime(#15) => idx 11,12 of the tail.
    read -r -a t1 <<<"${st##*) }"
    s1=$(( ${t1[11]:-0} + ${t1[12]:-0} ))
    sleep "$CPU_WINDOW"
    st="$(cat "/proc/$pid/stat" 2>/dev/null)" || { echo "0"; return; }
    read -r -a t2 <<<"${st##*) }"
    s2=$(( ${t2[11]:-0} + ${t2[12]:-0} ))
    awk -v d="$(( s2 - s1 ))" -v hz="$hz" -v w="$CPU_WINDOW" \
        'BEGIN { printf "%.2f", (hz > 0 && w > 0) ? d / hz / w : 0 }'
}
CORES="$(qemu_cpu_cores "$QEMU_PID")"
# ~half a core sustained is the busy/idle boundary (a spinning vCPU pegs one).
SPINNING=$(awk -v c="$CORES" 'BEGIN { print (c + 0 >= 0.5) ? 1 : 0 }')
if [[ "$SPINNING" == 1 ]]; then
    echo "[profile] QEMU host CPU: ${CORES} cores — busy (profiling the spin)" >&2
else
    echo "[profile] QEMU host CPU: ${CORES} cores — the app does NOT appear to" \
         "be spinning (idle / HLT-bound). Profiling anyway; expect a firmware" \
         "wait, not a hot app loop." >&2
fi

# --- 3. build the symbol-loading gdb script --------------------------------
python3 "$GDB_SYMS" "$DEBUGCON" \
    --build-dir "$OVMF_BUILD_DIR" \
    --axl-build-dir "$BUILD_DIR" \
    --gdb-port "$PORT" > "$SYMS" || {
        echo "[profile] gdb-syms.py failed" >&2; exit 1; }

# --- 4. sample via gdb -----------------------------------------------------
echo "[profile] sampling: $SAMPLES samples @ ${INTERVAL}s..." >&2
GDB_SAMPLE_SYMS="$SYMS" \
GDB_SAMPLE_COUNT="$SAMPLES" \
GDB_SAMPLE_INTERVAL="$INTERVAL" \
GDB_SAMPLE_OUT="$OUT" \
    "$GDB_BIN" -q -nx -batch -x "$GDB_SAMPLE" </dev/null || {
        echo "[profile] gdb sampling run failed" >&2; exit 1; }

# --- 5. report -------------------------------------------------------------
if [[ -f "$OUT.txt" ]]; then
    echo
    echo "QEMU host CPU during settle: ${CORES} cores ($([[ "$SPINNING" == 1 ]] \
        && echo "busy — a real spin" || echo "idle / HLT-bound — not spinning"))"
    cat "$OUT.txt"
    echo
    echo "[profile] folded stacks: $OUT.folded  (flamegraph.pl < $OUT.folded > profile.svg)"
else
    echo "[profile] no report produced (sampling failed?)" >&2
    exit 1
fi
