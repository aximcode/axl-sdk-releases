#!/bin/bash
# Run a .efi binary in QEMU and show its output.
#
# Uses the project's QEMU/firmware discovery infrastructure from
# axl-common.sh. Serial output is captured, ANSI codes stripped,
# and the application's stdout/stderr displayed cleanly.
#
# Usage: ./scripts/run-qemu.sh [OPTIONS] <file.efi> [args...]
#
# Options:
#   --arch X64|AARCH64    Architecture (default: X64)
#   --timeout SECS        QEMU timeout (default: 15)
#   --raw                 Show full serial log (including firmware boot)
#   --screenshot FILE     Capture framebuffer screenshot (PNG/PPM)
#   --net                 Enable user-mode networking (virtio-net)
#   --hostfwd H:G         Forward host port H to guest port G (repeatable)
#   --extra FILE          Stage additional .efi file on disk (repeatable)
#   --nsh FILE            Use custom startup.nsh instead of auto-generated
#   --background          Launch QEMU in background, print PID
#   --serial-log FILE     Save serial output to FILE
#   --gdb [PORT]          Expose QEMU GDB stub on tcp::PORT (default 1234).
#                         Boot runs free; attach with
#                         `gdb -ex 'target remote :PORT'` and `interrupt`
#                         when ready. Implies a long timeout under
#                         --background. Add --gdb-halt to start with -S
#                         (guest halted before instruction 0).
#   --gdb-halt            With --gdb, also start with -S (guest halted).
#                         Useful for breaking inside SecMain or PEI.
#   --debugcon FILE       Capture OVMF DEBUG output (port 0x402) — required
#                         for gdb-syms.py to recover module load addresses.
#   --no-cpu-warn         Disable the CPU-spike warning (on by default
#                         in foreground mode; samples QEMU's host CPU
#                         after the firmware-boot warm-up and prints
#                         a WARN line if a spin gets through).
#   --cpu-threshold N     Spike threshold in cores (default 1.5).
#   --cpu-sustain SECS    Sustain duration in seconds (default 2).
#
# Examples:
#   ./scripts/run-qemu.sh hello.efi
#   ./scripts/run-qemu.sh hello.efi world
#   ./scripts/run-qemu.sh --arch AARCH64 hello.efi
#   ./scripts/run-qemu.sh --raw hello.efi
#   ./scripts/run-qemu.sh driver.efi          # auto-detects driver, uses "load"
#   ./scripts/run-qemu.sh --net --hostfwd 18080:8080 axl-webfs.efi serve -p 8080
#   ./scripts/run-qemu.sh --net --hostfwd 18080:8080 --background axl-webfs.efi serve

set -euo pipefail

source "$(dirname "$0")/axl-common.sh"

ARCH="X64"
TIMEOUT=15
RAW=false
SCREENSHOT=""
NET=false
HOSTFWDS=()
EXTRA_FILES=()
CUSTOM_NSH=""
BACKGROUND=false
SERIAL_LOG=""
SERIAL_LOG_RAW=""
SERIAL_SOCKET=""
GDB_PORT=""
GDB_HALT=false
DEBUGCON_LOG=""
EFI_FILE=""
EFI_ARGS=()
CPU_WARN=true
CPU_THRESHOLD="1.5"   # cores; >=1.5 means a single vCPU pegged
CPU_SUSTAIN="2"       # seconds at threshold to count as a spike

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)       ARCH="$2"; shift 2 ;;
        --timeout)    TIMEOUT="$2"; shift 2 ;;
        --raw)        RAW=true; shift ;;
        --screenshot) SCREENSHOT="$2"; shift 2 ;;
        --net)        NET=true; shift ;;
        --hostfwd)    HOSTFWDS+=("$2"); shift 2 ;;
        --extra)      EXTRA_FILES+=("$2"); shift 2 ;;
        --nsh)        CUSTOM_NSH="$2"; shift 2 ;;
        --background) BACKGROUND=true; shift ;;
        --serial-log) SERIAL_LOG="$2"; shift 2 ;;
        --serial-log-raw) SERIAL_LOG_RAW="$2"; shift 2 ;;
        --serial-socket) SERIAL_SOCKET="$2"; shift 2 ;;
        --gdb)
            # Optional numeric port arg; default 1234.
            if [[ $# -ge 2 && "$2" =~ ^[0-9]+$ ]]; then
                GDB_PORT="$2"; shift 2
            else
                GDB_PORT="1234"; shift
            fi
            ;;
        --gdb-halt)   GDB_HALT=true; shift ;;
        --debugcon)   DEBUGCON_LOG="$2"; shift 2 ;;
        --no-cpu-warn) CPU_WARN=false; shift ;;
        --cpu-threshold) CPU_THRESHOLD="$2"; shift 2 ;;
        --cpu-sustain) CPU_SUSTAIN="$2"; shift 2 ;;
        -h|--help)
            cat <<'HELP'
Usage: run-qemu.sh [OPTIONS] <file.efi> [args...]

Options:
  --arch X64|AARCH64       Architecture (default: X64)
  --timeout SECS           QEMU timeout in seconds (default: 15)
  --raw                    Show full serial log (including firmware boot)
  --screenshot FILE        Capture framebuffer screenshot (PNG/PPM)
  --net                    Enable user-mode networking (virtio-net)
  --hostfwd HOST:GUEST     Forward host port to guest (repeatable)
  --extra FILE             Stage additional .efi on disk (repeatable)
  --nsh FILE               Use custom startup.nsh file
  --background             Launch QEMU in background, print PID
  --serial-log FILE        Save serial output to file (foreground:
                           ANSI-stripped clean transcript; background:
                           live raw log — symlinked).
  --serial-log-raw FILE    (foreground only) Save unprocessed serial
                           with ANSI/cursor codes. Useful for
                           firmware-level debugging.
  --serial-socket PATH     (background mode) expose serial as a UNIX
                           socket so host scripts can inject input
                           (e.g. Ctrl-C via `printf '\x03' | socat ...`)
  --no-cpu-warn            Disable CPU-spike warning. By default a
                           sampler watches QEMU's host CPU and prints
                           a WARN line if it sustains ≥1.5 cores for
                           ≥2 s after the firmware-boot warm-up
                           window (10 s X64 / 15 s AARCH64).
  --cpu-threshold CORES    Override spike threshold (default 1.5 cores).
  --cpu-sustain SECS       Override sustain duration (default 2 s).
  -h, --help               Show this help

Examples:
  run-qemu.sh hello.efi
  run-qemu.sh --net --hostfwd 18080:8080 axl-webfs.efi serve -p 8080
  run-qemu.sh --net --extra axl-webfs-dxe.efi --nsh test.nsh axl-webfs.efi
HELP
            exit 0 ;;
        *)
            if [[ -z "$EFI_FILE" ]]; then
                EFI_FILE="$1"
            else
                EFI_ARGS+=("$1")
            fi
            shift ;;
    esac
done

if [[ -z "$EFI_FILE" ]]; then
    echo "Usage: $0 [OPTIONS] <file.efi> [args...]  (try --help)" >&2
    exit 1
fi

if [[ ! -f "$EFI_FILE" ]]; then
    echo "ERROR: file not found: $EFI_FILE" >&2
    exit 1
fi

EFI_NAME="$(basename "$EFI_FILE")"
[[ "$ARCH" == "AARCH64" ]] && TIMEOUT=$((TIMEOUT + 10))

# Detect PE subsystem: 10=app, 11=boot driver, 12=runtime driver
IS_DRIVER=false
if command -v python3 &>/dev/null; then
    SUBSYSTEM=$(python3 -c "
import struct, sys
with open(sys.argv[1], 'rb') as f:
    mz = f.read(2)
    if mz != b'MZ': sys.exit(1)
    f.seek(0x3C)
    pe_off = struct.unpack('<I', f.read(4))[0]
    f.seek(pe_off + 0x5C)
    print(struct.unpack('<H', f.read(2))[0])
" "$EFI_FILE" 2>/dev/null || echo "10")
    [[ "$SUBSYSTEM" == "11" || "$SUBSYSTEM" == "12" ]] && IS_DRIVER=true
fi

# Resolve QEMU and firmware
QEMU_BIN=$(find_qemu "$ARCH") || { echo "QEMU not found for $ARCH" >&2; exit 1; }
find_firmware "$ARCH" || { echo "Firmware not found for $ARCH" >&2; exit 1; }
SHELL_EFI=$(find_shell_efi "$ARCH") || true
BOOT_NAME=$(boot_efi_name "$ARCH")

# Set up temp directory
TMPDIR=$(mktemp -d)
if [[ "$BACKGROUND" != "true" ]]; then
    trap 'rm -rf "$TMPDIR"' EXIT
fi

STAGING="$TMPDIR/staging"
LOG="$TMPDIR/serial.log"

mkdir -p "$STAGING/EFI/BOOT"
if [[ -n "$SHELL_EFI" && -f "$SHELL_EFI" ]]; then
    cp "$SHELL_EFI" "$STAGING/EFI/BOOT/$BOOT_NAME"
fi
cp "$EFI_FILE" "$STAGING/$EFI_NAME"

# Stage extra files
if [[ ${#EXTRA_FILES[@]} -gt 0 ]]; then
    for extra in "${EXTRA_FILES[@]}"; do
        if [[ ! -f "$extra" ]]; then
            echo "ERROR: extra file not found: $extra" >&2
            exit 1
        fi
        cp "$extra" "$STAGING/$(basename "$extra")"
    done
fi

# Startup script
if [[ -n "$CUSTOM_NSH" ]]; then
    if [[ ! -f "$CUSTOM_NSH" ]]; then
        echo "ERROR: nsh file not found: $CUSTOM_NSH" >&2
        exit 1
    fi
    cp "$CUSTOM_NSH" "$STAGING/startup.nsh"
else
    {
        echo "@echo -off"
        echo "fs0:"
        echo "cd \\"
        if [[ "$IS_DRIVER" == "true" ]]; then
            echo "load $EFI_NAME"
        elif [[ ${#EFI_ARGS[@]} -gt 0 ]]; then
            echo "$EFI_NAME ${EFI_ARGS[*]}"
        else
            echo "$EFI_NAME"
        fi
        if [[ -z "$SCREENSHOT" && "$BACKGROUND" != "true" ]]; then
            echo "reset -s"
        fi
    } > "$STAGING/startup.nsh"
fi

# Build disk image. Prefer mkimage when available (richer tooling,
# UDF-bridge support); fall back to plain mtools when not (CI runners
# without the mkimage repo cloned, contributors who haven't set
# MKIMAGE_DIR, etc.). The fallback is the same recipe common-test.sh
# uses — dd + mkfs.vfat + mcopy.
if [[ -n "${MKIMAGE_DIR:-}" && -f "$MKIMAGE_DIR/mkimage.py" ]]; then
    "$MKIMAGE_DIR/mkimage.py" --source "$STAGING" --target "$TMPDIR/disk.img" --label RUN > /dev/null 2>&1
else
    size_kb=$(du -sk "$STAGING" | cut -f1)
    size_kb=$(( (size_kb + 4096) / 1024 * 1024 ))   # round up to MB
    [[ $size_kb -lt 40960 ]] && size_kb=40960        # min 40 MB
    dd if=/dev/zero of="$TMPDIR/disk.img" bs=1K count="$size_kb" 2>/dev/null
    mkfs.vfat -F 32 -n RUN "$TMPDIR/disk.img" >/dev/null 2>&1
    # Use mcopy -s for recursive copy. Pass top-level entries by name
    # (no leading "./") so mcopy's destination path is clean and mtools
    # creates the directory tree on the fly. The previous per-file
    # loop produced "::/./EFI/..." paths that mtools refused to write.
    (
        cd "$STAGING" || exit 1
        for entry in $(find . -maxdepth 1 -mindepth 1 -printf '%P\n'); do
            mcopy -s -i "$TMPDIR/disk.img" "$entry" "::/" 2>/dev/null
        done
    )
fi

# Prepare NVRAM
cp "$FW_VARS" "$TMPDIR/vars.fd"

# CPU-spike sampler. Runs alongside QEMU sampling /proc/<pid>/stat
# at 5 Hz after a warm-up window (firmware boot legitimately spins
# while it walks PCI / loads drivers). Tracks peak host-CPU
# consumption (in core-units, where 1.0 = one core saturated) and
# the longest sustained-≥-threshold streak. Writes "<peak>
# <sustain_max>" to the supplied summary file when QEMU exits.
# Caller checks the summary against CPU_THRESHOLD / CPU_SUSTAIN
# and emits a WARN line if breached.
#
# Warm-up is ARCH-dependent — TCG (AARCH64 default) is slower
# through OVMF boot than KVM-X64.
CPU_WARMUP=10
[[ "$ARCH" == "AARCH64" ]] && CPU_WARMUP=15

cpu_sampler() {
    local qpid="$1" out="$2"
    local hz; hz=$(getconf CLK_TCK 2>/dev/null || echo 100)
    awk -v pid="$qpid" -v hz="$hz" -v interval=0.2 \
        -v warmup="$CPU_WARMUP" -v thr="$CPU_THRESHOLD" '
    function read_total(p,    line, n, after, f) {
        if ((getline line < ("/proc/" p "/stat")) <= 0) {
            close("/proc/" p "/stat"); return -1
        }
        close("/proc/" p "/stat")
        n = index(line, ") ")
        if (n == 0) return -1
        split(substr(line, n+2), f, " ")
        # post-comm fields: state(1) ppid(2) pgrp(3) session(4)
        # tty_nr(5) tpgid(6) flags(7) minflt(8) cminflt(9)
        # majflt(10) cmajflt(11) utime(12) stime(13) ...
        return f[12] + f[13]
    }
    function alive(p) { return (system("kill -0 " p " 2>/dev/null") == 0) }
    BEGIN {
        system("sleep " warmup)
        prev = read_total(pid)
        if (prev < 0) { print "0.00 0.00"; exit 0 }
        peak = 0; streak = 0; streak_max = 0
        while (alive(pid)) {
            system("sleep " interval)
            cur = read_total(pid)
            if (cur < 0) break
            d = cur - prev; prev = cur
            cores = d / (hz * interval)
            if (cores > peak) peak = cores
            if (cores >= thr) {
                streak += interval
                if (streak > streak_max) streak_max = streak
            } else {
                streak = 0
            }
        }
        printf "%.2f %.2f\n", peak, streak_max
    }' > "$out"
}

# Print a CPU-spike summary if the sampler captured a sustained spike.
# Reads "<peak> <sustain_max>" from the file written by cpu_sampler.
cpu_summary() {
    local summary_file="$1"
    [[ "$CPU_WARN" != "true" ]] && return 0
    [[ ! -s "$summary_file" ]] && return 0
    local peak sustain
    read -r peak sustain < "$summary_file" || return 0
    # awk for the comparison; $sustain and $CPU_SUSTAIN are floats.
    local breached
    breached=$(awk -v s="$sustain" -v t="$CPU_SUSTAIN" \
        'BEGIN{print (s+0 >= t+0) ? "1" : "0"}')
    if [[ "$breached" == "1" ]]; then
        printf "WARN: CPU spike — peak %s cores, sustained ≥%s cores for %ss (threshold %ss)\n" \
            "$peak" "$CPU_THRESHOLD" "$sustain" "$CPU_SUSTAIN" >&2
    fi
}

# Build QEMU command
mapfile -d '' -t CMD < <(build_qemu_base_cmd "$ARCH" "$QEMU_BIN" 512M "$TMPDIR/vars.fd")
CMD+=(-drive "format=raw,file=$TMPDIR/disk.img")

# GDB stub: -gdb tcp::PORT exposes the GDB protocol; -S starts the
# guest CPU halted so the debugger can attach before the firmware
# runs a single instruction. KVM is incompatible with single-stepping
# many of the early boot instructions — drop -enable-kvm/-cpu host
# from the base cmd and fall back to TCG when --gdb is requested.
if [[ -n "$GDB_PORT" ]]; then
    NEW_CMD=()
    skip=0
    for arg in "${CMD[@]}"; do
        if [[ $skip -gt 0 ]]; then skip=$((skip-1)); continue; fi
        case "$arg" in
            -enable-kvm) ;;                 # drop
            -cpu)        skip=1 ;;          # drop with its value
            *)           NEW_CMD+=("$arg") ;;
        esac
    done
    CMD=("${NEW_CMD[@]}")
    CMD+=(-gdb "tcp::$GDB_PORT")
    if [[ "$GDB_HALT" == "true" ]]; then
        CMD+=(-S)
    fi
    # Bump the watchdog so a debugging session doesn't get terminated.
    if [[ "$BACKGROUND" != "true" ]]; then
        TIMEOUT=3600
    fi
fi

# OVMF DEBUG-build firmware emits "Loading driver at 0x... NAME.efi"
# load lines via the QEMU isa-debugcon device on I/O port 0x402, NOT
# via the regular serial console. Capture them when --debugcon FILE
# is given (the symbol-loader needs these to relocate ELF debug info
# at runtime addresses).
if [[ -n "$DEBUGCON_LOG" ]]; then
    CMD+=(-debugcon "file:$DEBUGCON_LOG"
          -global "isa-debugcon.iobase=0x402")
fi

# Networking
if [[ "$NET" == "true" ]]; then
    NETDEV="user,id=net0"
    for fwd in "${HOSTFWDS[@]}"; do
        HOST_PORT="${fwd%%:*}"
        GUEST_PORT="${fwd##*:}"
        NETDEV="$NETDEV,hostfwd=tcp::${HOST_PORT}-:${GUEST_PORT}"
    done
    CMD+=(-device virtio-net-pci,netdev=net0 -netdev "$NETDEV")
else
    CMD+=(-net none)
fi

# Screenshot mode
if [[ -n "$SCREENSHOT" ]]; then
    MONSOCK="$TMPDIR/monitor.sock"
    CMD+=(-serial "file:$LOG" -display none -device VGA)
    CMD+=(-monitor "unix:$MONSOCK,server,nowait")

    set +e
    "${CMD[@]}" &
    QEMU_PID=$!

    WAIT=$((TIMEOUT - 3))
    [[ $WAIT -lt 5 ]] && WAIT=5
    sleep "$WAIT"

    for try in 1 2 3; do
        echo "screendump $TMPDIR/screenshot.ppm" | \
            socat -t 2 - "UNIX-CONNECT:$MONSOCK" >/dev/null 2>&1 && break
        sleep 1
    done
    sleep 1
    kill "$QEMU_PID" >/dev/null 2>&1
    wait "$QEMU_PID" >/dev/null 2>&1
    set -e

    if [[ -f "$TMPDIR/screenshot.ppm" ]]; then
        if command -v convert &>/dev/null; then
            convert "$TMPDIR/screenshot.ppm" "$SCREENSHOT"
        else
            cp "$TMPDIR/screenshot.ppm" "$SCREENSHOT"
        fi
        echo "Screenshot saved: $SCREENSHOT"
    else
        echo "WARNING: screenshot capture failed" >&2
    fi

# Background mode
elif [[ "$BACKGROUND" == "true" ]]; then
    if [[ -n "$SERIAL_SOCKET" ]]; then
        # Serial as a UNIX socket the host can open to read output AND
        # write input. -no-shutdown so the guest's own Exit doesn't
        # force QEMU to tear down before we've drained the log. Drop
        # -nographic (it implies serial=stdio) in favour of an
        # explicit chardev binding.
        rm -f "$SERIAL_SOCKET"
        CMD+=(
            -no-reboot
            -chardev "socket,id=serial0,path=$SERIAL_SOCKET,server=on,wait=off"
            -serial chardev:serial0
            -display none
        )
        "${CMD[@]}" > "$LOG" 2>&1 < /dev/null &
        QEMU_PID=$!
    else
        CMD+=(-nographic -no-reboot)
        "${CMD[@]}" > "$LOG" 2>&1 < /dev/null &
        QEMU_PID=$!
    fi

    # Copy serial log path if requested
    if [[ -n "$SERIAL_LOG" ]]; then
        # Create a symlink so the caller can find the log
        ln -sf "$LOG" "$SERIAL_LOG"
    fi

    echo "QEMU_PID=$QEMU_PID"
    echo "SERIAL_LOG=$LOG"
    [[ -n "$SERIAL_SOCKET" ]] && echo "SERIAL_SOCKET=$SERIAL_SOCKET"
    echo "TMPDIR=$TMPDIR"
    # Don't clean up — caller is responsible for killing QEMU and
    # removing TMPDIR when done.

# Normal foreground mode
else
    CMD+=(-nographic -no-reboot)
    # </dev/null detaches the caller's TTY from QEMU's stdio. With
    # -nographic QEMU multiplexes serial+monitor over stdio; if a real
    # TTY is on stdin (typical interactive ssh), QEMU picks up phantom
    # input and exits before producing a single byte of serial output.
    # The empty-log diagnostic below catches future surprises.
    #
    # Run QEMU under a wrapper subshell so we can grab its PID for the
    # CPU sampler. `timeout` reparents the command, but the QEMU
    # process is still a child of the subshell. After a brief settle
    # delay (QEMU is up within ~100 ms typical, give it 1 s with
    # backoff for slow hosts), pgrep -P finds it.
    ( timeout "$TIMEOUT" "${CMD[@]}" > "$LOG" 2>&1 < /dev/null ) &
    WRAPPER_PID=$!
    QPID=""
    if [[ "$CPU_WARN" == "true" ]]; then
        for _ in 1 2 3 4 5; do
            QPID=$(pgrep -P "$WRAPPER_PID" 2>/dev/null | head -1)
            [[ -n "$QPID" ]] && break
            sleep 0.2
        done
    fi
    SUMMARY=""
    SAMPLER_PID=""
    if [[ -n "$QPID" ]]; then
        SUMMARY="$TMPDIR/cpu-summary.txt"
        cpu_sampler "$QPID" "$SUMMARY" &
        SAMPLER_PID=$!
    fi
    wait "$WRAPPER_PID" 2>/dev/null || true
    if [[ -n "$SAMPLER_PID" ]]; then
        wait "$SAMPLER_PID" 2>/dev/null || true
    fi

    # Strip ANSI/DEC escape sequences and carriage returns. The param
    # byte class is the full CSI parameter range (ECMA-48 0x30-0x3F)
    # so DEC private modes like ESC[=3h and ESC[?25l strip cleanly,
    # not just numeric/semicolon CSI like ESC[2J.
    # Also strip standalone ESC, plus ESC( / ESC) charset designators
    # which UEFI consoles emit on init.
    CLEAN="$TMPDIR/clean.log"
    sed -E '
        s/\x1b\[[0-9;:<=>?]*[a-zA-Z@`{|}~]//g
        s/\x1b[()][A-Za-z0-9]//g
    ' "$LOG" | tr -d '\r' > "$CLEAN"

    # --serial-log saves the cleaned transcript by default (matches
    # what the user sees on stdout). --serial-log-raw is the explicit
    # escape hatch for firmware-level debugging.
    if [[ -n "$SERIAL_LOG" ]]; then
        cp "$CLEAN" "$SERIAL_LOG"
    fi
    if [[ -n "$SERIAL_LOG_RAW" ]]; then
        cp "$LOG" "$SERIAL_LOG_RAW"
    fi

    # If QEMU produced absolutely nothing, surface the failure
    # explicitly. Most common cause: stdin is a TTY and QEMU's
    # -nographic stdio multiplexer ate the boot — but we already
    # </dev/null above, so this catches new failure modes (KVM
    # access denied, missing firmware, vars-file collision, etc.).
    if [[ ! -s "$LOG" ]]; then
        cat <<EOF >&2
ERROR: QEMU produced no serial output (0 bytes).

  Disk image:    $TMPDIR/disk.img ($(stat -c%s "$TMPDIR/disk.img" 2>/dev/null || echo "?") bytes)
  QEMU binary:   $QEMU_BIN
  Architecture:  $ARCH

Likely causes:
  - /dev/kvm not accessible (try: ls -l /dev/kvm; id)
  - Firmware vars file in use by another QEMU process (try: pgrep -fa qemu)
  - Disk image build failed silently (rerun with bash -x)
  - QEMU build broken (try: $QEMU_BIN --version)
EOF
        exit 1
    fi

    if [[ "$RAW" == "true" ]]; then
        cat "$CLEAN"
    else
        sed -n '/to continue\./,/^Reset with/p' "$CLEAN" | \
            grep -v "to continue\." | \
            grep -v "^Reset with"
    fi

    # CPU-spike summary. Silent unless threshold breached. Runs after
    # the serial output so the warning is the last thing the user sees.
    [[ -n "$SUMMARY" ]] && cpu_summary "$SUMMARY"
fi
