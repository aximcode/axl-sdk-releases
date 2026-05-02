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
#   -i, --interactive     Hand the host TTY to QEMU so keystrokes reach the
#                         guest. Use for "press a key to exit" stubs and
#                         other apps that need real user input. Disables
#                         the timeout, the CPU-spike sampler, and the
#                         ANSI-stripping post-filter. Mutually exclusive
#                         with --background and --screenshot. Ctrl-A C
#                         drops into the QEMU monitor; Ctrl-A X quits.
#   --mount DIR[:TAG]     Expose host directory DIR to the guest as a
#                         virtiofs volume (UEFI fsN: after `map -r`).
#                         Requires virtiofsd, /dev/shm, and an OVMF
#                         build that includes VirtioFsDxe (or a
#                         standalone VirtioFsDxe.efi alongside the
#                         firmware build). Default volume tag: hostfs.
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
NIC_MODEL=""        # default chosen later (virtio-net-pci); --nic-model overrides
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
INTERACTIVE=false
MOUNT_DIR=""
MOUNT_TAG="hostfs"
MEM="512M"            # guest RAM (also used for memory-backend-file size)
BRIDGES=false         # --bridges adds a small PCI bridge tree (mirrors test runner)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)       ARCH="$2"; shift 2 ;;
        --timeout)    TIMEOUT="$2"; shift 2 ;;
        --raw)        RAW=true; shift ;;
        --screenshot) SCREENSHOT="$2"; shift 2 ;;
        --net)        NET=true; shift ;;
        --bridges)    BRIDGES=true; shift ;;
        --nic-model)  NIC_MODEL="$2"; NET=true; shift 2 ;;
        --nic-no-rom) NIC_NO_ROM=true; NET=true; shift ;;
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
        -i|--interactive) INTERACTIVE=true; shift ;;
        --mount)
            # Accept "DIR" or "DIR:tag" — tag is the UEFI volume label
            # virtiofs advertises (defaults to "hostfs"); rarely needed.
            if [[ "$2" == *:* ]]; then
                MOUNT_DIR="${2%:*}"
                MOUNT_TAG="${2##*:}"
            else
                MOUNT_DIR="$2"
            fi
            shift 2 ;;
        -h|--help)
            cat <<'HELP'
Usage: run-qemu.sh [OPTIONS] <file.efi> [args...]

Options:
  --arch X64|AARCH64       Architecture (default: X64)
  --timeout SECS           QEMU timeout in seconds (default: 15)
  --raw                    Show full serial log (including firmware boot)
  --screenshot FILE        Capture framebuffer screenshot (PNG/PPM)
  --net                    Enable user-mode networking (virtio-net)
  --bridges                Add a small PCI bridge tree (one PCIe root
                           port + a virtio-rng device behind it). Mirrors
                           the topology the unit-test runner uses, so
                           interactive smoke-tests of lspci -t / future
                           tree-walking tools see real bridges.
  --nic-model MODEL        QEMU NIC model (implies --net). Examples:
                           virtio-net-pci (default), e1000, e1000e,
                           rtl8139, pcnet, ne2k_pci. Use to test
                           driver-bundle coverage on NICs OVMF lacks.
  --nic-no-rom             Suppress QEMU's bundled iPXE option ROM
                           (passes romfile= to the -device line). Use
                           to force the "firmware lacks NIC driver"
                           scenario when validating staged-driver
                           fallback. Implies --net.
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
  -i, --interactive        Attach the host TTY to QEMU's serial so the
                           user can type into the guest. Disables the
                           timeout, the CPU-spike sampler, and the
                           ANSI-stripping post-filter. Mutually
                           exclusive with --background and --screenshot.
                           Ctrl-A C → QEMU monitor; Ctrl-A X → quit.
  --mount DIR[:TAG]        Expose host directory DIR to the guest as
                           a virtiofs volume. Mount with `map -r` from
                           the UEFI shell; appears as fsN:. Requires
                           virtiofsd, /dev/shm, and an OVMF build
                           that includes (or ships) VirtioFsDxe.
  -h, --help               Show this help

Examples:
  run-qemu.sh hello.efi
  run-qemu.sh --net --hostfwd 18080:8080 axl-webfs.efi serve -p 8080
  run-qemu.sh --net --extra axl-webfs-dxe.efi --nsh test.nsh axl-webfs.efi
  run-qemu.sh --interactive noGPT.efi          # press-a-key stubs
  run-qemu.sh -i --mount ~/efi-apps             # host fs at fsN:, shell prompt
  run-qemu.sh --mount ~/efi-apps myapp.efi      # run myapp + host fs alongside
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
    if [[ "$INTERACTIVE" == "true" ]]; then
        # Bare-shell mode: no app to run, just boot OVMF + Shell.efi
        # interactively so the user can poke around or `load` things
        # off a --mount volume. Skipping EFI_FILE turns off the
        # is-driver detection and any app-staging logic below.
        :
    else
        echo "Usage: $0 [OPTIONS] <file.efi> [args...]  (try --help)" >&2
        echo "  (or: $0 --interactive [--mount DIR] for a UEFI shell)" >&2
        exit 1
    fi
fi

if [[ -n "$EFI_FILE" && ! -f "$EFI_FILE" ]]; then
    echo "ERROR: file not found: $EFI_FILE" >&2
    exit 1
fi

# Interactive mode is incompatible with anything that captures or
# multiplexes the serial console under another consumer. --gdb is
# fine because the GDB stub lives on its own TCP port.
if [[ "$INTERACTIVE" == "true" ]]; then
    if [[ "$BACKGROUND" == "true" ]]; then
        echo "ERROR: --interactive cannot be combined with --background" >&2
        exit 1
    fi
    if [[ -n "$SCREENSHOT" ]]; then
        echo "ERROR: --interactive cannot be combined with --screenshot" >&2
        exit 1
    fi
    # The CPU-spike WARN line would interleave with whatever the user
    # is reading. Disable the sampler unconditionally in interactive.
    CPU_WARN=false
fi

# --mount: validate host-side dependencies up-front so we fail loudly
# with actionable guidance instead of producing a guest with no fsN:
# volume. The OVMF-driver check happens after find_firmware below.
VIRTIOFSD_BIN=""
if [[ -n "$MOUNT_DIR" ]]; then
    if [[ ! -d "$MOUNT_DIR" ]]; then
        echo "ERROR: --mount: '$MOUNT_DIR' is not a directory" >&2
        exit 1
    fi
    # Resolve to an absolute path — virtiofsd needs one and the trap
    # cleanup below uses it for diagnostic logging.
    MOUNT_DIR="$(cd "$MOUNT_DIR" && pwd -P)"

    # Locate virtiofsd. Distros disagree about where it lives.
    for cand in \
        "${VIRTIOFSD:-}" \
        "$(command -v virtiofsd 2>/dev/null)" \
        /usr/libexec/virtiofsd \
        /usr/lib/qemu/virtiofsd \
        /usr/lib/kvm/virtiofsd
    do
        if [[ -n "$cand" && -x "$cand" ]]; then
            VIRTIOFSD_BIN="$cand"
            break
        fi
    done
    if [[ -z "$VIRTIOFSD_BIN" ]]; then
        cat <<'EOF' >&2
ERROR: --mount requires virtiofsd, but it was not found.

  Install:
    Fedora/RHEL/Alma:  sudo dnf install virtiofsd
    Debian/Ubuntu:     sudo apt install virtiofsd
    Arch:              sudo pacman -S virtiofsd

  Or set VIRTIOFSD=/path/to/virtiofsd before running this script.
EOF
        exit 1
    fi

    # virtiofs uses a memory-backend-file with share=on. /dev/shm is the
    # standard backing — fast (tmpfs), and KVM is happy mapping it.
    if [[ ! -d /dev/shm || ! -w /dev/shm ]]; then
        echo "ERROR: --mount needs a writable /dev/shm tmpfs (memory-backend-file)" >&2
        exit 1
    fi
fi

EFI_NAME=""
[[ -n "$EFI_FILE" ]] && EFI_NAME="$(basename "$EFI_FILE")"
[[ "$ARCH" == "AARCH64" ]] && TIMEOUT=$((TIMEOUT + 10))

# Detect PE subsystem: 10=app, 11=boot driver, 12=runtime driver
IS_DRIVER=false
if [[ -n "$EFI_FILE" ]] && command -v python3 &>/dev/null; then
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

# Resolve QEMU and firmware. find_qemu's `export QEMU_DIR` happens
# in the $() subshell, so it doesn't reach our parent shell — re-derive
# it here so find_firmware below can locate QEMU-bundled firmware
# ($QEMU_DIR/../share/qemu/edk2-*-code.fd) when applicable. This
# matters on hosts with both a system QEMU/OVMF (no VirtioFsDxe) and
# a custom QEMU build that bundles richer firmware: without the
# export propagating, find_firmware silently falls through to the
# system OVMF and `--mount` (and similar features) breaks.
QEMU_BIN=$(find_qemu "$ARCH") || { echo "QEMU not found for $ARCH" >&2; exit 1; }
QEMU_DIR="$(dirname "$QEMU_BIN")"
export QEMU_DIR
find_firmware "$ARCH" || { echo "Firmware not found for $ARCH" >&2; exit 1; }
SHELL_EFI=$(find_shell_efi "$ARCH") || true
BOOT_NAME=$(boot_efi_name "$ARCH")

# --mount: VirtioFsDxe needs to be in the guest. Modern OVMF/AAVMF
# builds include it (the QEMU-bundled edk2-*-code.fd, recent EDK2
# builds, most distro packages from 2023 onwards). Reliably detecting
# its presence in the active FV is hard — DXE drivers live in an
# LZMA-compressed FFS inside the FV, so a strings(1) sweep misses
# them and proper detection requires uefiextract or equivalent.
#
# Pragmatic policy: opportunistically stage a standalone
# VirtioFsDxe.efi if we can find one alongside the build (so older
# OVMF works), and emit `load VirtioFsDxe.efi` in startup.nsh when
# we did. If the firmware also has it integrated, the `load` is a
# harmless duplicate. If neither path produces a driver, the only
# symptom is fsN: not appearing in `map -r` from the shell — print
# a hint at startup so the user knows to check.
VFS_DRIVER_STAGE=""
if [[ -n "$MOUNT_DIR" ]]; then
    fw_dir="$(dirname "$FW_CODE")"
    case "$ARCH" in
        X64)     vfs_arch="X64" ;;
        AARCH64) vfs_arch="AARCH64" ;;
    esac
    for cand in \
        "$fw_dir/../$vfs_arch/VirtioFsDxe.efi" \
        "$fw_dir/$vfs_arch/VirtioFsDxe.efi" \
        "$fw_dir/VirtioFsDxe.efi"
    do
        if [[ -f "$cand" ]]; then
            VFS_DRIVER_STAGE="$cand"
            break
        fi
    done
    if [[ -z "$VFS_DRIVER_STAGE" ]]; then
        cat >&2 <<EOF
[run-qemu] --mount: no standalone VirtioFsDxe.efi found alongside
[run-qemu]   $FW_CODE
[run-qemu]   Trusting the firmware to provide it. If 'map -r' from
[run-qemu]   the UEFI shell shows no extra fsN: volume, your OVMF
[run-qemu]   lacks VirtioFsDxe — rebuild it (OvmfPkg/VirtioFsDxe)
[run-qemu]   or pass --extra path/to/VirtioFsDxe.efi explicitly.
EOF
    fi
fi

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
if [[ -n "$EFI_FILE" ]]; then
    cp "$EFI_FILE" "$STAGING/$EFI_NAME"
fi

# Auto-stage the vendor/device name sidecar so axl_pci_ids_load
# finds it via the standard companion-path resolver. Tiny file
# (~3 KB), and non-PCI tools simply ignore its presence — keeping
# this unconditional avoids the "lspci shows no names because I
# forgot --bridges" footgun.
PCI_IDS_FILE="$(dirname "$0")/../share/pci-ids.json5"
if [[ -f "$PCI_IDS_FILE" ]]; then
    cp "$PCI_IDS_FILE" "$STAGING/pci-ids.json5"
fi
PCI_CLASS_FILE="$(dirname "$0")/../share/pci-class.json5"
if [[ -f "$PCI_CLASS_FILE" ]]; then
    cp "$PCI_CLASS_FILE" "$STAGING/pci-class.json5"
fi
# Stage VirtioFsDxe.efi if the active OVMF doesn't ship it integrated.
if [[ -n "$VFS_DRIVER_STAGE" ]]; then
    cp "$VFS_DRIVER_STAGE" "$STAGING/VirtioFsDxe.efi"
fi

# Stage extra files. Each entry is either "PATH" (lands at staging
# root) or "PATH:DEST" where DEST is a relative target path (intermediate
# dirs get created). Use the DEST form to drop drivers under a layout
# like `drivers/x64/<name>.efi` for axl_driver_locate to find them.
if [[ ${#EXTRA_FILES[@]} -gt 0 ]]; then
    for extra in "${EXTRA_FILES[@]}"; do
        # Split src:dest. Reject any DEST that escapes via ".." to keep
        # the staging dir self-contained.
        if [[ "$extra" == *:* ]]; then
            extra_src="${extra%%:*}"
            extra_dst="${extra#*:}"
            if [[ "$extra_dst" == *..* || "$extra_dst" = /* ]]; then
                echo "ERROR: --extra dest must be a relative path without '..': $extra_dst" >&2
                exit 1
            fi
        else
            extra_src="$extra"
            extra_dst="$(basename "$extra")"
        fi
        if [[ ! -f "$extra_src" ]]; then
            echo "ERROR: extra file not found: $extra_src" >&2
            exit 1
        fi
        mkdir -p "$STAGING/$(dirname "$extra_dst")"
        cp "$extra_src" "$STAGING/$extra_dst"
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
        # In interactive mode, pick a UEFI text mode whose ROW count
        # matches the host terminal as closely as possible. UEFI's
        # TerminalDxe registers six fixed modes (80x25, 80x50,
        # 100x31, 128x40, 160x42, 240x56), and the shell's `mode`
        # command requires an exact match.
        #
        # Why row-match matters: TerminalDxe tracks CursorRow and
        # saturates it at MaxRow-1 when output scrolls. The host
        # terminal's natural scroll is independent — its cursor
        # stays at the bottom row of the visible viewport. When
        # UEFI later emits an absolute SetCursorPosition (e.g. the
        # shell backtracking one column to overwrite a `^N` color
        # token in help text), it sends \e[<MaxRow>;<col>H. If
        # host_rows > MaxRow, that jumps the host cursor BACKWARD
        # into already-painted output → prompt-appears-mid-text
        # artifact. Matching row counts keeps the two cursors in
        # lockstep.
        #
        # `stty size` works inside SSH PTYs and WSL terminals.
        # If it fails (no TTY, etc.), fall back to mode 100x31
        # — best general-purpose default.
        if [[ "$INTERACTIVE" == "true" ]]; then
            # Use 100x31 (OVMF's default + verified to be in the
            # registered mode list) so the `mode` line actually
            # takes effect. TerminalDxe defines 6 modes upstream
            # but OvmfPkg's PlatformBootManagerLib typically only
            # exposes 80x25 and 100x31 — silent failure of e.g.
            # `mode 80 50` was the cause of the prompt-mid-output
            # artifact (UEFI stayed at 100x31 while the host-side
            # alignment assumed 50). Fall back to 80x25 if the
            # host can't fit 31 rows.
            host_rows=0
            if command -v stty &>/dev/null; then
                tty_size=$(stty size 2>/dev/null) && [[ -n "$tty_size" ]] \
                    && host_rows="${tty_size% *}"
            fi
            if [[ "$host_rows" =~ ^[0-9]+$ \
                  && "$host_rows" -gt 0 \
                  && "$host_rows" -lt 31 ]]; then
                echo "mode 80 25 > NUL"
            else
                echo "mode 100 31 > NUL"
            fi
        fi
        echo "fs0:"
        echo "cd \\"
        # If we staged a standalone VirtioFsDxe.efi (firmware lacks it),
        # load it before anything else so fsN: appears for the app and
        # for any post-app shell prompt the user lands at. `connect -r`
        # rebinds drivers to handles; `map -r` refreshes the volume
        # table so the new fsN: shows up.
        if [[ -n "$VFS_DRIVER_STAGE" ]]; then
            echo "load VirtioFsDxe.efi"
            echo "connect -r"
            echo "map -r"
        elif [[ -n "$MOUNT_DIR" ]]; then
            # Driver is in firmware — still rescan so fsN: appears.
            echo "map -r"
        fi
        if [[ -z "$EFI_FILE" ]]; then
            : # bare-shell mode: no app, no reset, just stay at prompt
        elif [[ "$IS_DRIVER" == "true" ]]; then
            echo "load $EFI_NAME"
        elif [[ ${#EFI_ARGS[@]} -gt 0 ]]; then
            echo "$EFI_NAME ${EFI_ARGS[*]}"
        else
            echo "$EFI_NAME"
        fi
        if [[ -n "$EFI_FILE" && -z "$SCREENSHOT" \
              && "$BACKGROUND" != "true" \
              && "$INTERACTIVE" != "true" ]]; then
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
    # Validate up-front so a missing tool surfaces with an install
    # hint instead of dying silently under `set -e` after the next
    # command's stderr lands in /dev/null.
    missing=()
    command -v mkfs.vfat >/dev/null 2>&1 || missing+=("mkfs.vfat (dosfstools)")
    command -v mcopy     >/dev/null 2>&1 || missing+=("mcopy (mtools)")
    if [[ ${#missing[@]} -gt 0 ]]; then
        cat >&2 <<EOF
ERROR: disk-image build needs tools that are not installed:
  - ${missing[*]}

  Install:
    Debian/Ubuntu:    sudo apt install dosfstools mtools
    Fedora/RHEL:      sudo dnf install dosfstools mtools
    Arch:             sudo pacman -S dosfstools mtools

  Or set MKIMAGE_DIR=/path/to/mkimage to use the mkimage backend instead.
EOF
        exit 1
    fi

    size_kb=$(du -sk "$STAGING" | cut -f1)
    size_kb=$(( (size_kb + 4096) / 1024 * 1024 ))   # round up to MB
    [[ $size_kb -lt 40960 ]] && size_kb=40960        # min 40 MB
    dd if=/dev/zero of="$TMPDIR/disk.img" bs=1K count="$size_kb" 2>/dev/null
    # mkfs.vfat / mcopy errors are surfaced (not redirected to /dev/null) —
    # if formatting or staging fails, the user sees why instead of a
    # silent set -e exit.
    mkfs.vfat -F 32 -n RUN "$TMPDIR/disk.img" >/dev/null
    # Use mcopy -s for recursive copy. Pass top-level entries by name
    # (no leading "./") so mcopy's destination path is clean and mtools
    # creates the directory tree on the fly. The previous per-file
    # loop produced "::/./EFI/..." paths that mtools refused to write.
    (
        cd "$STAGING" || exit 1
        for entry in $(find . -maxdepth 1 -mindepth 1 -printf '%P\n'); do
            mcopy -s -i "$TMPDIR/disk.img" "$entry" "::/"
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
mapfile -d '' -t CMD < <(build_qemu_base_cmd "$ARCH" "$QEMU_BIN" "$MEM" "$TMPDIR/vars.fd")
CMD+=(-drive "format=raw,file=$TMPDIR/disk.img")

# --bridges: matching topology to test/integration/common-test.sh so
# tools that walk PCI bridges (lspci -t, sysinfo --pci, ...) can be
# smoke-tested interactively against the same shape unit tests use.
# slot is auto-assigned to avoid colliding with the q35 mch at 00:00.0.
if [[ "$BRIDGES" == "true" ]]; then
    CMD+=(
        -device "pcie-root-port,id=axl_rp0,bus=pcie.0,chassis=1"
        -device "virtio-rng-pci,bus=axl_rp0"
    )
fi

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

# --mount: spawn virtiofsd and wire the vhost-user-fs PCI device into
# QEMU. The shared-memory backend (memory-backend-file with share=on
# over /dev/shm) is mandatory — vhost-user requires the guest RAM be
# accessible from the daemon process, which only works through a
# named/file-backed shared mapping. -numa node,memdev=mem binds the
# entire guest RAM to that backend.
#
# Sandbox=none avoids the user-namespace dance (--sandbox=chroot in
# rust virtiofsd uses unshare(), which fails without privileged
# capabilities on locked-down hosts). For the dev-loop use case
# we own the directory and don't need the extra isolation.
VIRTIOFSD_PID=""
if [[ -n "$MOUNT_DIR" ]]; then
    VFS_SOCK="$TMPDIR/virtiofs.sock"
    VFS_LOG="$TMPDIR/virtiofsd.log"
    "$VIRTIOFSD_BIN" \
        --socket-path="$VFS_SOCK" \
        --shared-dir="$MOUNT_DIR" \
        --sandbox=none \
        --cache=auto \
        > "$VFS_LOG" 2>&1 &
    VIRTIOFSD_PID=$!

    # Wait for the daemon to create its socket. virtiofsd is fast
    # (~50 ms typical) but be lenient on slow hosts.
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        [[ -S "$VFS_SOCK" ]] && break
        sleep 0.1
    done
    if [[ ! -S "$VFS_SOCK" ]]; then
        echo "ERROR: virtiofsd failed to start (no socket at $VFS_SOCK)" >&2
        echo "--- virtiofsd output ---" >&2
        cat "$VFS_LOG" >&2 || true
        kill "$VIRTIOFSD_PID" 2>/dev/null || true
        exit 1
    fi

    CMD+=(
        -chardev "socket,id=axlvfs,path=$VFS_SOCK"
        -device "vhost-user-fs-pci,queue-size=1024,chardev=axlvfs,tag=$MOUNT_TAG"
        -object "memory-backend-file,id=axlmem,size=$MEM,mem-path=/dev/shm,share=on"
        -numa "node,memdev=axlmem"
    )
fi

# If --mount is active in foreground modes, extend the cleanup trap
# so virtiofsd doesn't leak after QEMU exits. Background mode emits
# the PID for the caller to manage instead (see below).
if [[ -n "$VIRTIOFSD_PID" && "$BACKGROUND" != "true" ]]; then
    trap 'kill '"$VIRTIOFSD_PID"' 2>/dev/null; rm -rf "'"$TMPDIR"'"' EXIT
fi

# Networking
if [[ "$NET" == "true" ]]; then
    NETDEV="user,id=net0"
    for fwd in "${HOSTFWDS[@]}"; do
        HOST_PORT="${fwd%%:*}"
        GUEST_PORT="${fwd##*:}"
        NETDEV="$NETDEV,hostfwd=tcp::${HOST_PORT}-:${GUEST_PORT}"
    done
    # Default NIC model is virtio-net-pci because OVMF has VirtioNetDxe
    # built in. --nic-model lets tests force other PCI NICs (e1000,
    # e1000e, rtl8139, pcnet, vmxnet3, ne2k_pci) to validate iPXE-driver
    # bundle coverage on hardware OVMF lacks a driver for.
    #
    # NIC_NO_ROM=true suppresses the QEMU-bundled iPXE option ROM.
    # Without this, QEMU loads an iPXE PXE ROM that exposes UNDI, and
    # OVMF wraps UNDI→SNP — giving the appearance that "the firmware
    # has a driver" for any common NIC. For tests that need a true
    # "firmware lacks driver" scenario (so we can prove the staged
    # iPXE driver bundle is loaded from disk), set NIC_NO_ROM=true.
    NIC_MODEL_ACTUAL="${NIC_MODEL:-virtio-net-pci}"
    NIC_DEV="${NIC_MODEL_ACTUAL},netdev=net0"
    if [[ "${NIC_NO_ROM:-false}" == "true" ]]; then
        NIC_DEV="${NIC_DEV},romfile="
    fi
    CMD+=(-device "$NIC_DEV" -netdev "$NETDEV")
else
    CMD+=(-net none)
fi

# Interactive mode — hand the host TTY to QEMU. No pipeline, no
# timeout, no ANSI strip, no CPU sampler. Ctrl-A C drops to the QEMU
# monitor; Ctrl-A X quits. mux=on,signal=off lets QEMU handle Ctrl-C
# as guest input rather than tearing itself down. logfile= preserves
# the --serial-log transcript inline (raw bytes — interactive apps
# may legitimately emit cursor moves and colors).
if [[ "$INTERACTIVE" == "true" ]]; then
    CHARDEV="stdio,id=axlcon0,mux=on,signal=off"
    if [[ -n "$SERIAL_LOG" ]]; then
        CHARDEV="$CHARDEV,logfile=$SERIAL_LOG"
    fi
    if [[ -n "$SERIAL_LOG_RAW" ]]; then
        # Same backend, same content — keep both flags wired so users
        # who already script around --serial-log-raw don't have to
        # special-case interactive.
        CHARDEV="$CHARDEV,logfile=$SERIAL_LOG_RAW"
    fi
    CMD+=(-chardev "$CHARDEV"
          -serial chardev:axlcon0
          -mon chardev=axlcon0
          -display none
          -no-reboot)

    cat >&2 <<'HINT'
[run-qemu] Interactive console — keystrokes go to the guest.
[run-qemu]   Ctrl-A C  → QEMU monitor    Ctrl-A X  → quit
[run-qemu]   Ctrl-A H  → monitor help    Ctrl-A ?  → key list
HINT

    # Set up the host terminal for the QEMU session:
    #   1. Disable terminal-to-app reports that would otherwise
    #      flow into the guest as fake keystrokes:
    #        ?1004 — focus in/out reports (\e[I, \e[O — sent
    #                when you click/switch to the terminal window)
    #        ?2004 — bracketed paste markers (\e[200~ / \e[201~)
    #        ?1000/1002/1003/1006 — mouse reporting modes
    #      Modern shells (zsh, bash with bracketed-paste) enable
    #      some of these by default; we inherit them as our
    #      child process. Without disabling, the bytes show up
    #      at the UEFI shell prompt as garbled input — most
    #      visibly as "FS1:\> [I" on screen.
    #   2. Switch to the alternate screen buffer (?1049) so
    #      UEFI's absolute-cursor positioning paints on a fresh
    #      canvas instead of clobbering scrollback. Same trick
    #      vim/less/htop use. Original screen + scrollback come
    #      back on exit.
    #   3. Set DECSTBM scroll margins (\e[1;<N>r) to confine
    #      vertical scroll to the same row count UEFI thinks
    #      the screen has. UEFI's TerminalDxe saturates CursorRow
    #      at MaxRow-1 during scroll; without DECSTBM, the host
    #      keeps scrolling to row 50, but UEFI's later absolute
    #      \e[<MaxRow>;<col>H jumps backward into stale output.
    #      With DECSTBM, scroll stops at row N, host cursor
    #      stays at row N, and UEFI's absolute moves to row N
    #      land exactly where the host cursor is. Pick N to
    #      match the UEFI mode we set in startup.nsh.
    # Skipped when stderr isn't a TTY so log files don't get
    # escape-sequence pollution.
    altscreen_used=false
    if [[ -t 2 ]]; then
        # Use the same row count we'll request via `mode` in
        # startup.nsh (100x31 normally, 80x25 when host < 31
        # rows). DECSTBM's scroll region matches UEFI's grid;
        # UEFI's saturate-at-MaxRow now lands on the same row
        # as the host's natural-scroll cursor.
        stbm_r=31
        host_rows_for_stbm=0
        if command -v stty &>/dev/null; then
            tty_size=$(stty size 2>/dev/null) && [[ -n "$tty_size" ]] \
                && host_rows_for_stbm="${tty_size% *}"
        fi
        if [[ "$host_rows_for_stbm" =~ ^[0-9]+$ \
              && "$host_rows_for_stbm" -gt 0 \
              && "$host_rows_for_stbm" -lt 31 ]]; then
            stbm_r=25
        fi
        printf '\e[?1004l\e[?2004l\e[?1000l\e[?1002l\e[?1003l\e[?1006l\e[?1049h\e[H\e[2J\e[1;%dr\e[H' "$stbm_r" >&2
        altscreen_used=true
    fi

    # QEMU puts the terminal into raw mode. If it dies abnormally
    # (segfault, OOM, killed by the user) the parent shell is left
    # without echo or line discipline. Restore on any exit path.
    # Also kill virtiofsd if --mount spawned one (otherwise it'll
    # outlive the QEMU process and hold the socket open).
    cleanup_cmd='rm -rf "'"$TMPDIR"'"; stty sane 2>/dev/null || true'
    [[ -n "$VIRTIOFSD_PID" ]] && \
        cleanup_cmd="kill $VIRTIOFSD_PID 2>/dev/null; $cleanup_cmd"
    # On exit: reset DECSTBM scroll margins (\e[r), leave
    # alt-screen, then re-enable focus and bracketed-paste
    # reporting (most modern shells expect these on; we
    # re-enable rather than try to detect what was on before,
    # since terminals don't expose a query for "is mode X
    # enabled"). Mouse modes stay off — almost no interactive
    # shell uses them.
    [[ "$altscreen_used" == "true" ]] && \
        cleanup_cmd="$cleanup_cmd; printf '\\e[r\\e[?1049l\\e[?1004h\\e[?2004h' >&2"
    trap "$cleanup_cmd" EXIT INT TERM

    "${CMD[@]}"
    exit $?
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
    [[ -n "$VIRTIOFSD_PID" ]] && echo "VIRTIOFSD_PID=$VIRTIOFSD_PID"
    echo "TMPDIR=$TMPDIR"
    # Don't clean up — caller is responsible for killing QEMU (and
    # virtiofsd, when --mount was used) and removing TMPDIR when done.

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
