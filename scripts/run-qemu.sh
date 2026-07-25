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
#   --timeout SECS        QEMU timeout (default: 15). Honored in both
#                         foreground and --background modes — the
#                         background launcher wraps QEMU with `timeout`
#                         so an abandoned process can't camp on
#                         hostfwd ports forever.
#   --raw                 Show full serial log (including firmware boot)
#   --screenshot FILE     Capture framebuffer screenshot (PNG/PPM)
#   --gpu                 Wire a virtual GPU device into the machine
#                         so the guest firmware exposes a GOP for any
#                         axl_gfx_*-using app. Required on AARCH64
#                         (`virt` machine has no default display);
#                         harmless on X64. --screenshot implies this.
#   --gui / --display gtk Open a GTK window for the guest's GOP framebuffer
#                         (implies --gpu; serial stays on this terminal).
#                         Rides X11 forwarding (ssh -Y + XQuartz). NOTE:
#                         GTK over remote X11 (e.g. XQuartz) often fails to
#                         repaint the framebuffer — prefer --vnc-reverse.
#   --vnc [N]             Serve VNC on display :N (default :1, TCP 5901);
#                         connect a viewer in (tunnel the port if remote).
#   --vnc-reverse HOST:PORT
#                         Connect OUT to a VNC viewer running in listen
#                         mode at HOST:PORT — the window auto-appears on
#                         the client with no per-run reconnect. Best path
#                         for live GOP viewing over SSH/Tailscale. All
#                         three imply --gpu and have no timeout.
#   --net                 Enable user-mode networking (virtio-net)
#   --hostfwd H:G         Forward host port H to guest port G (repeatable).
#                         H may be `auto` to have a verified-free host port
#                         claimed for you and reported as HOSTFWD_<G>=<port>.
#   --extra FILE          Stage additional .efi file on disk (repeatable)
#   --sendkey "K K ..."   With --screenshot: inject QEMU monitor key tokens
#                         (e.g. "h i spc t h e r e", "ctrl-s", "ret") once
#                         the app is up, then capture — for "screenshot the
#                         app after input X". Repeatable; auto-adds a USB
#                         keyboard on aarch64.
#   --holdkey "QCODE:MS"  With --screenshot: hold a USB key down for MS ms via
#                         QMP (reproduces UsbKbDxe typematic "bounce"). Auto-adds
#                         a USB keyboard. Pair with --serial-log. Repeatable.
#   --reboot              A guest-initiated reset re-POSTs the same VM
#                         (disk image + OVMF vars pflash persist) instead
#                         of exiting QEMU. Off by default. Bound a reset
#                         loop with --timeout.
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
#   --no-cpu-warn         Disable the CPU-spike check (on by default in the
#                         default + --screenshot modes; samples QEMU's host
#                         CPU after the firmware-boot warm-up and, on a
#                         sustained spin, prints a WARN line AND fails the run
#                         with exit 8 — guest failure/timeout take precedence).
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
# Sentinel echoed at the top of the staged startup.nsh so the non-raw output
# filter knows where the app's output begins. It used to anchor on the Shell's
# "...to continue." startup-countdown prompt, but the AXL shell launcher skips
# that countdown (-delay 0), so this explicit marker replaces it (the countdown
# prompt is kept as a fallback anchor for a launcher-less direct-Shell boot).
APP_OUTPUT_SENTINEL="___AXL_APP_OUTPUT_BEGIN___"
SCREENSHOT=""
# --gpu wires a virtual GPU device into the QEMU machine so the guest
# firmware (OVMF) initializes a GOP for axl_gfx_*.  Needed for any
# AARCH64 visual demo — the `virt` machine has no default display
# device.  Harmless on X64 (q35 already provides GOP); the extra
# device is a no-op there.  --screenshot implies --gpu automatically.
ENABLE_GPU=false
NET=false
NIC_MODEL=""        # default chosen later (virtio-net-pci); --nic-model overrides
MAC_ADDR=""         # --mac XX:XX:XX:XX:XX:XX (HF4: replay a captured NIC MAC)
CPU_SPEC=""         # --cpu SPEC (HF4: replay a captured CPU model, e.g. qemu64,family=6)
HOSTFWDS=()
HOSTFWD_CHOSEN=()   # HOSTFWD_<guest>=<host> for each --hostfwd auto:<guest>
EXTRA_FILES=()
# --qemu-arg STRING: one literal QEMU command-line token, appended
# verbatim (NOT word-split). Repeatable — pass one --qemu-arg per token,
# e.g. `--qemu-arg -device --qemu-arg virtio-foo,bar=1`. Appending
# verbatim (no word-split) is what lets a token contain spaces — e.g. a
# device spec whose path has a space (`...,mem-path=/my dir/x.bin`) — so
# programmatic callers like axl-emulate can inject HF device args safely.
EXTRA_QEMU_ARGS=()
# --reboot: let a guest-initiated reset re-POST the same VM (same disk
# image, same OVMF vars pflash) instead of ending the run. OFF by
# default — every mode except --screenshot passes -no-reboot, and
# callers rely on "QEMU exited" as the observable signal that the guest
# reset itself.
REBOOT=false

CUSTOM_NSH=""
BACKGROUND=false
SERIAL_LOG=""
SERIAL_LOG_RAW=""
SERIAL_SOCKET=""
GDB_PORT=""
GDB_HALT=false
DEBUGCON_LOG=""
EFI_FILE=""
BOOT_TARGET=false   # --boot-target: stage the app AS \EFI\BOOT\BOOTx64.EFI (the
                    # default boot loader) instead of launching it from a shell's
                    # startup.nsh — so there is NO parent shell above it. Matches
                    # how a console-hosting app (axterm, SoftBMC) really ships.
EFI_ARGS=()
SENDKEY_SEQ=""   # space-separated QEMU monitor key tokens to inject (--screenshot)
SENDMOUSE_SEQ="" # space-separated "fx,fy[,click]" absolute-pointer moves (--screenshot, QMP)
HOLDKEY_SEQ=""   # space-separated "qcode:ms" held keypresses (--screenshot, QMP)
# CPU-spike detection knobs come from axl-common.sh; the KVM/TCG-aware threshold,
# warm-up and warn carve-out are applied by cpu_policy_init "$ARCH" below (after
# --arch is known). --cpu-threshold / --no-cpu-warn / --cpu-report still override,
# because cpu_policy_init only fills a value a flag has not already set.
INTERACTIVE=false
# --display BACKEND (or --gui = gtk): open a real graphical window for the
# guest's GOP framebuffer instead of running headless. Over SSH this rides
# X11 forwarding (ssh -Y + an X server like XQuartz on the client), so the
# window pops up on the client's screen. Implies --gpu. gtk is the only
# backend the bundled QEMU builds with today.
DISPLAY_BACKEND=""
MOUNT_DIR=""
MOUNT_TAG="hostfs"
MEM="512M"            # guest RAM (also used for memory-backend-file size)
BRIDGES=false         # --bridges adds a small PCI bridge tree (mirrors test runner)
TAP_IFACE=""          # --tap <ifname>: bridge the NIC to a host tap (real net)
                      # instead of SLIRP user-mode (real DHCP/ICMP; needs the
                      # tap pre-created + owned by the qemu user)

# Hardware-fixture platform-identity injection (SMBIOS / ACPI / SPD /
# TPM / IPMI) is NOT handled here — it lives in scripts/axl-emulate,
# which builds the QEMU device args from a fixture and passes them via
# --qemu-arg. run-qemu.sh is a generic launcher. See the HF design doc's
# 2026-06-08 architecture decision.

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)       ARCH="$2"; shift 2 ;;
        --timeout)    TIMEOUT="$2"; shift 2 ;;
        --raw)        RAW=true; shift ;;
        --screenshot) SCREENSHOT="$2"; shift 2 ;;
        --gpu)        ENABLE_GPU=true; shift ;;
        --display)    DISPLAY_BACKEND="$2"; shift 2 ;;
        --gui)        DISPLAY_BACKEND="gtk"; shift ;;
        --vnc)
            # Optional display number (default :1 → TCP 5901). QEMU serves
            # VNC; a viewer connects in (tunnel the port for a remote host).
            if [[ $# -ge 2 && "$2" =~ ^[0-9]+$ ]]; then
                DISPLAY_BACKEND="vnc=:$2"; shift 2
            else
                DISPLAY_BACKEND="vnc=:1"; shift
            fi ;;
        --vnc-reverse)
            # HOST:PORT of a VNC viewer running in listen mode; QEMU
            # connects OUT to it (auto-appears, no per-run reconnect).
            DISPLAY_BACKEND="vnc=$2,reverse=on"; shift 2 ;;
        --net)        NET=true; shift ;;
        --tap)        TAP_IFACE="$2"; NET=true; shift 2 ;;
        --bridges)    BRIDGES=true; shift ;;
        --nic-model)  NIC_MODEL="$2"; NET=true; shift 2 ;;
        --mac)        MAC_ADDR="$2"; NET=true; shift 2 ;;
        --cpu)        CPU_SPEC="$2"; shift 2 ;;
        --nic-no-rom) NIC_NO_ROM=true; NET=true; shift ;;
        --hostfwd)    HOSTFWDS+=("$2"); shift 2 ;;
        --extra)      EXTRA_FILES+=("$2"); shift 2 ;;
        --sendkey)    SENDKEY_SEQ+=" $2"; shift 2 ;;
        --sendmouse)  SENDMOUSE_SEQ+=" $2"; shift 2 ;;
        --holdkey)    HOLDKEY_SEQ+=" $2"; shift 2 ;;
        --qemu-arg)   EXTRA_QEMU_ARGS+=("$2"); shift 2 ;;
        --reboot)     REBOOT=true; shift ;;
        --nsh)        CUSTOM_NSH="$2"; shift 2 ;;
        --boot-target) BOOT_TARGET=true; shift ;;
        --shell)      SHELL_OVERRIDE="$2"; shift 2 ;;
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
        --cpu-report) CPU_REPORT=true; shift ;;
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
  --timeout SECS           QEMU timeout in seconds (default: 15;
                           honored in --background mode too — wraps
                           QEMU with timeout(1) to prevent hostfwd
                           port leaks)
  --raw                    Show full serial log (including firmware boot)
  --screenshot FILE        Capture framebuffer screenshot (PNG/PPM)
                           Implies --gpu.
  --gpu                    Wire a virtual GPU device into the machine
                           so the firmware initializes a GOP for any
                           axl_gfx_*-using app. Required for AARCH64
                           visual demos (the `virt` machine has no
                           default display device). Harmless on X64
                           (q35 already provides GOP).
  --gui / --display gtk    Open a GTK window for the guest's GOP
                           framebuffer. Implies --gpu; guest serial
                           stays on this terminal. Rides X11 forwarding
                           (ssh -Y + an X server such as XQuartz). NOTE:
                           QEMU's GTK over remote X11 frequently fails to
                           repaint the framebuffer (the window opens but
                           graphics don't update) — for SSH use prefer
                           --vnc-reverse below.
  --vnc [N]                Serve VNC on display :N (default :1, TCP
                           5901). A viewer connects in; tunnel the port
                           for a remote host
                           (ssh -L 5901:localhost:5901 <host>).
  --vnc-reverse HOST:PORT  Connect OUT to a VNC viewer running in listen
                           mode at HOST:PORT (e.g. TigerVNC
                           `vncviewer -listen PORT`, started with
                           `-SecurityTypes None`). The window auto-
                           appears on the client with no per-run
                           reconnect — the best path for live GOP
                           viewing over SSH / Tailscale.
                           All display modes imply --gpu, have no
                           timeout, and are mutually exclusive with
                           --background / --screenshot / --interactive.
  --net                    Enable user-mode (SLIRP) networking (virtio-net)
  --tap IFACE              Real L2 networking over a pre-created host tap
                           (implies --net) instead of SLIRP — gives the
                           guest a real DHCP lease + working ICMP. The tap
                           must already exist and be owned by the qemu user;
                           scripts/netcfg-testnet.sh (in agt) sets up the
                           tap + a dnsmasq DHCP server + NAT.
  --bridges                Add a small PCI bridge tree (one PCIe root
                           port + a virtio-rng device behind it) AND a
                           USB topology (qemu-xhci + usb-mouse + usb-hub
                           + usb-tablet). Mirrors the topology the unit-
                           test runner uses, so interactive smoke-tests
                           of lspci -t and lsusb -t see real bridges
                           and real USB hub-port chains.
  --nic-model MODEL        QEMU NIC model (implies --net). Examples:
                           virtio-net-pci (default), e1000, e1000e,
                           rtl8139, pcnet, ne2k_pci. Use to test
                           driver-bundle coverage on NICs OVMF lacks.
  --mac XX:XX:XX:XX:XX:XX  Set the NIC's hardware address (implies
                           --net). HF4: replay a captured NIC MAC so the
                           guest sees the fixtured machine's address.
  --cpu SPEC               Override the guest CPU model (QEMU -cpu).
                           HF4: replay a captured CPU identity, e.g.
                           "qemu64,vendor=GenuineIntel,family=6,model=42"
                           (x86) or "max,midr=0x410fd0b0" (aarch64).
                           KVM stays on; CPUID/MIDR follow the spec.
  --nic-no-rom             Suppress QEMU's bundled iPXE option ROM
                           (passes romfile= to the -device line). Use
                           to force the "firmware lacks NIC driver"
                           scenario when validating staged-driver
                           fallback. Implies --net.
  --hostfwd HOST:GUEST     Forward host port to guest (repeatable).
                           HOST may be the literal `auto`, in which case a
                           host port that is free RIGHT NOW is claimed for
                           the life of this process and reported back as
                           `HOSTFWD_<GUEST>=<port>` — on stdout alongside
                           QEMU_PID= under --background, on stderr
                           otherwise. Prefer this to hard-coding a port:
                           QEMU refuses the ENTIRE -netdev if any single
                           hostfwd cannot bind, so a port clash presents as
                           "the guest has no network" rather than as a
                           bind error.
  --extra FILE             Stage additional .efi on disk (repeatable)
  --sendkey "K K ..."      With --screenshot: inject QEMU monitor key
                           tokens (space-separated; e.g.
                           "h i spc t h e r e", "ctrl-s", "ret") after the
                           app is up, then capture. Repeatable; auto-adds
                           a USB keyboard on aarch64.
  --sendmouse "fx,fy[,click]"
                           With --screenshot: move the absolute pointer to
                           screen-fraction (fx,fy) in [0,1] via QMP (a
                           usb-tablet is auto-added), optionally pressing+
                           releasing the left button (",click"). Injected
                           after --sendkey, before capture. Repeatable.
  --holdkey "QCODE:MS"     With --screenshot: hold a USB key (QEMU qcode, e.g.
                           "a") DOWN for MS milliseconds via QMP, then release.
                           A held key past the firmware typematic delay makes
                           UsbKbDxe synthesise auto-repeats — reproducing the
                           KVM key "bounce". Auto-adds a USB keyboard (all
                           arches). Pair with --serial-log to read the result.
                           Repeatable.
  --qemu-arg STRING        Append STRING to the qemu command line as ONE
                           literal token (no word-splitting). Repeatable;
                           pass one --qemu-arg per token, e.g.
                           `--qemu-arg -device --qemu-arg virtio-foo`. A
                           token MAY contain spaces (e.g. a device spec
                           with a space in a file path) — it is preserved
                           verbatim. Useful for device emulation or debug
                           knobs
                           not natively exposed.
                           (Hardware-fixture platform injection —
                           SMBIOS / ACPI / SPD / TPM / IPMI — is not a
                           run-qemu.sh concern; use scripts/axl-emulate,
                           which builds those device args from a fixture
                           and passes them via --qemu-arg.)
                           Caller tokens are appended LAST, so an
                           explicit token overrides run-qemu's own
                           default for any option QEMU resolves
                           last-wins (-display, -action, ...).
  --reboot                 Let a guest-initiated reset re-POST the same
                           VM instead of exiting QEMU. The disk image
                           and the OVMF vars pflash persist across the
                           reset, so firmware-persistent state (NVRAM)
                           carries over exactly as on real hardware.
                           OFF by default: without it a guest reset ends
                           the run, which callers use as the signal that
                           a reset happened. A guest that resets in a
                           loop is bounded by --timeout, not by QEMU.
  --nsh FILE               Use custom startup.nsh file
  --boot-target            Stage the app AS \EFI\BOOT\BOOTx64.EFI so BdsDxe
                           launches it directly — no parent shell, no
                           startup.nsh. For a console-hosting app (axterm,
                           SoftBMC); its child Shell is staged at \Shell.efi.
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
  --no-cpu-warn            Disable the CPU-spike check. By default a sampler
                           watches QEMU's host CPU and, if it sustains ≥1.5
                           cores for ≥2 s after the firmware-boot warm-up
                           window (10 s X64 / 15 s AARCH64), prints a WARN line
                           AND fails the run with exit code 8 (distinct from a
                           guest failure/timeout, which take precedence). The
                           check runs in the default and --screenshot modes
                           (not --interactive/--display/--background). This flag
                           disables BOTH the warning and the fail-exit.
  --cpu-threshold CORES    Override spike threshold (default 1.5 cores).
  --cpu-sustain SECS       Override sustain duration (default 2 s).
  --cpu-report             Always print a `CPU-REPORT:` line with the mean
                           and peak host-CPU usage the sampler measured
                           (in cores; 1.0 = one core), after the warm-up
                           window. Useful for steady-state CPU regression
                           tests (e.g. an idle server should stay well
                           under one core).
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

# --display / --gui: open a graphical window for the guest's GOP
# framebuffer. Validate the backend, imply --gpu (the window needs a
# display device — essential on the aa64 `virt` machine, harmless on
# x64), and reject the headless/capture modes it conflicts with.
if [[ -n "$DISPLAY_BACKEND" ]]; then
    case "$DISPLAY_BACKEND" in
        gtk) ;;
        vnc=*) ;;   # vnc=:N (serve) or vnc=HOST:PORT,reverse (connect to a listening viewer)
        *)
            echo "ERROR: --display: unsupported backend '$DISPLAY_BACKEND' (supported: gtk, vnc=...)" >&2
            exit 1 ;;
    esac
    if [[ "$BACKGROUND" == "true" ]]; then
        echo "ERROR: --display cannot be combined with --background" >&2
        exit 1
    fi
    if [[ "$INTERACTIVE" == "true" ]]; then
        echo "ERROR: --display cannot be combined with --interactive" >&2
        exit 1
    fi
    if [[ -n "$SCREENSHOT" ]]; then
        echo "ERROR: --display cannot be combined with --screenshot" >&2
        exit 1
    fi
    ENABLE_GPU=true
    CPU_WARN=false   # the window is the output; spike WARN would just be noise
    # The GTK window is an X11 client; over SSH it needs a forwarded
    # display. Fail early with actionable guidance rather than QEMU's
    # terse "Could not initialize GTK" / "cannot open display".
    if [[ "$DISPLAY_BACKEND" == gtk && -z "${DISPLAY:-}" ]]; then
        cat >&2 <<'EOF'
ERROR: --display gtk needs an X11 display, but $DISPLAY is unset.
  Over SSH: reconnect with X11 forwarding and an X server running on
  your client, e.g. from a Mac with XQuartz open:
      ssh -Y <thishost>
  then re-run. (Verify with: echo $DISPLAY — should be set, e.g.
  localhost:10.0.)
EOF
        exit 1
    fi
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

# --shell FILE: boot a specific UEFI shell binary instead of the auto-found
# EDK2 one (e.g. the old EFI 1.x "newshell" Shell106.efi, which has no
# EDK2 EFI_SHELL_PROTOCOL). Tools are staged on the boot FAT via --extra and
# run from the custom nsh, so this reproduces the old-shell environment.
if [[ -n "${SHELL_OVERRIDE:-}" ]]; then
    [[ -f "$SHELL_OVERRIDE" ]] || { echo "ERROR: --shell file not found: $SHELL_OVERRIDE" >&2; exit 1; }
    SHELL_EFI="$SHELL_OVERRIDE"
    echo "[run-qemu] using shell override: $SHELL_EFI" >&2
fi

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

# Set up temp directory. Prefer a tmpfs (/dev/shm) so the per-run ~40 MB disk
# image + pflash vars are built in RAM rather than on the root filesystem —
# under a parallel integration pool, concurrent root-fs image builds were an
# I/O bottleneck that starved booting guests (empty serial output -> spurious
# test failures). Override the base with AXL_QEMU_TMPDIR; otherwise fall back
# to the default temp location when /dev/shm is missing/unwritable.
_axl_tmp_base="${AXL_QEMU_TMPDIR:-}"
if [[ -z "$_axl_tmp_base" && -d /dev/shm && -w /dev/shm ]]; then
    _axl_tmp_base=/dev/shm
fi
if [[ -n "$_axl_tmp_base" ]]; then
    # Defense-in-depth sweep: remove obviously-dead axl-qemu.* state dirs from
    # prior runs (hard-killed, where the self-clean subshell never ran, or
    # pre-fix background leaks) before creating a new one. A dir is swept only if
    # it is BOTH older than 10 minutes AND has no live process referencing its
    # path (a running guest or virtiofsd carries it in argv). The age guard is
    # what makes this safe under the parallel integration pool: a dir in its
    # brief create-then-stage window is seconds old, so it can never be mistaken
    # for a leak and rm'd out from under a concurrent run.
    while IFS= read -r local_d; do
        [[ -n "$local_d" ]] || continue
        pgrep -af -- "$local_d" >/dev/null 2>&1 || rm -rf "$local_d"
    done < <(find "$_axl_tmp_base" -maxdepth 1 -type d -name 'axl-qemu.*' -mmin +10 2>/dev/null)
    TMPDIR=$(mktemp -d -p "$_axl_tmp_base" axl-qemu.XXXXXXXX)
else
    TMPDIR=$(mktemp -d)
fi
if [[ "$BACKGROUND" != "true" ]]; then
    # Trap INT/TERM too, not just EXIT: when a caller wraps this script in
    # `timeout`, the SIGTERM on expiry would otherwise bypass the EXIT trap and
    # leak the (tmpfs- or disk-backed) temp dir. (8.6 GB of such leaks were
    # found accumulating from timeout-killed integration runs.)
    trap 'rm -rf "$TMPDIR"' EXIT INT TERM
fi

STAGING="$TMPDIR/staging"
LOG="$TMPDIR/serial.log"

mkdir -p "$STAGING/EFI/BOOT"
if [[ "$BOOT_TARGET" == "true" && -n "$EFI_FILE" ]]; then
    # The app IS the boot loader: BdsDxe launches \EFI\BOOT\BOOTx64.EFI directly,
    # so there is no parent shell and no startup.nsh runs. A console-hosting app
    # that StartImages its own child Shell finds it via axl_shell_locate, so stage
    # the shell at the volume root (\Shell.efi, search candidate 3.5) instead of
    # the boot slot. (No staged file → the app falls back to the firmware FV shell.)
    cp "$EFI_FILE" "$STAGING/EFI/BOOT/$BOOT_NAME"
    if [[ -n "$SHELL_EFI" && -f "$SHELL_EFI" ]]; then
        cp "$SHELL_EFI" "$STAGING/Shell.efi"
    fi
else
    # Boot the Shell via the AXL launcher ("-delay 0", skips the 5 s startup
    # countdown); see stage_boot_shell in axl-common.sh. Falls back to booting
    # the Shell directly if the launcher can't be built.
    if [[ -n "$SHELL_EFI" && -f "$SHELL_EFI" ]]; then
        stage_boot_shell "$STAGING" "$ARCH" "$BOOT_NAME" "$SHELL_EFI"
    fi
    if [[ -n "$EFI_FILE" ]]; then
        cp "$EFI_FILE" "$STAGING/$EFI_NAME"
    fi
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
    # Prepend the app-output sentinel so the non-raw filter can find where the
    # script's output begins (the launcher removes the old countdown anchor).
    { echo "echo $APP_OUTPUT_SENTINEL"; cat "$CUSTOM_NSH"; } > "$STAGING/startup.nsh"
    # Power off when a non-interactive custom nsh finishes, so the run doesn't
    # idle at the shell prompt until --timeout (the auto-generated app launch
    # gets a `reset -s` for the same reason). Skipped for background /
    # interactive / screenshot / display modes (those want QEMU to stay up), and
    # skipped when the script already issues a reset (respect its own placement).
    # Harmless for a script that hangs: the appended reset is never reached, so
    # hang-to-timeout behavior is preserved.
    if [[ -z "$SCREENSHOT" && "$BACKGROUND" != "true" \
          && "$INTERACTIVE" != "true" && -z "$DISPLAY_BACKEND" ]] \
       && ! grep -qiE '^[[:space:]]*reset([[:space:]]|$)' "$STAGING/startup.nsh"; then
        printf '\nreset -s\n' >> "$STAGING/startup.nsh"
    fi
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
        if [[ -z "$EFI_FILE" || "$BOOT_TARGET" == "true" ]]; then
            : # bare-shell mode, or --boot-target (BdsDxe launches the app as
              # \EFI\BOOT\BOOTx64.EFI directly; this startup.nsh is never run).
        else
            # Marker delimiting where the app's output begins, so the non-raw
            # filter can strip the shell banner + mapping table above it (the
            # launcher removes the old "...to continue." countdown anchor).
            echo "echo $APP_OUTPUT_SENTINEL"
            if [[ "$IS_DRIVER" == "true" ]]; then
                echo "load $EFI_NAME"
            elif [[ ${#EFI_ARGS[@]} -gt 0 ]]; then
                echo "$EFI_NAME ${EFI_ARGS[*]}"
            else
                echo "$EFI_NAME"
            fi
        fi
        if [[ -n "$EFI_FILE" && -z "$SCREENSHOT" \
              && "$BACKGROUND" != "true" \
              && "$INTERACTIVE" != "true" \
              && -z "$DISPLAY_BACKEND" ]]; then
            echo "reset -s"
        fi
    } > "$STAGING/startup.nsh"
fi

# Build disk image (shared with the integration harness — see
# qemu_stage_disk in axl-common.sh: mkimage when MKIMAGE_DIR is set, else mtools).
qemu_stage_disk "$STAGING" "$TMPDIR/disk.img" RUN || exit 1

# Prepare NVRAM
cp "$FW_VARS" "$TMPDIR/vars.fd"

# CPU-spike sampler + monitor lifecycle live in scripts/axl-common.sh so
# common-test.sh (a sibling that builds its own QEMU command and never calls this
# script) gets the same check. cpu_policy_init picks the KVM/TCG-aware threshold +
# warm-up + warn carve-out (one source of truth, shared with common-test.sh);
# --cpu-threshold / --no-cpu-warn parsed above still override it.
cpu_policy_init "$ARCH"


# Build QEMU command
mapfile -d '' -t CMD < <(build_qemu_base_cmd "$ARCH" "$QEMU_BIN" "$MEM" "$TMPDIR/vars.fd" "$CPU_SPEC")
CMD+=(-drive "format=raw,file=$TMPDIR/disk.img")

# Skip the firmware Boot Manager countdown before auto-booting the first
# option (the UEFI shell). QEMU publishes this as the `etc/boot-menu-wait`
# fw_cfg, sourced from `-boot splash-time=N` (ms); OVMF's
# GetFrontPageTimeoutFromQemu() reads it and, when 0, boots immediately
# (otherwise it defaults to ~5 s). Faster, more deterministic boots — and
# the visual-capture timing no longer rides on a 5 s wait. Set
# QEMU_BOOT_MENU_WAIT to a non-empty ms value to restore a countdown.
CMD+=(-boot "splash-time=${QEMU_BOOT_MENU_WAIT:-0}")

# --gpu / --screenshot: wire a virtual GPU device.  AARCH64's `virt`
# machine has no default display device, so axl_gfx_* + any other
# GOP-using app reports "no display" until one is added.  X64's q35
# already provides a display via OVMF; the extra device is a no-op
# there but keeps screenshots / framebuffer-capture consistent across
# arches.  --screenshot triggers this implicitly because the
# `screendump` monitor command needs a framebuffer to read.
if [[ "$ENABLE_GPU" == "true" || -n "$SCREENSHOT" ]]; then
    case "$ARCH" in
        X64)
            if [[ -n "$DISPLAY_BACKEND" ]]; then
                # Live graphical window (--display/--gui): std VGA does NOT
                # reliably mark direct linear-framebuffer writes dirty, so the
                # GTK window shows stale pixels even though the GOP app drew
                # correctly (a screendump still captures them — it reads the
                # whole FB on demand). virtio-gpu's explicit RESOURCE_FLUSH /
                # Blt model updates the live display properly. We keep std VGA
                # for the headless --gpu / --screenshot paths (and the gfx
                # present tests, which assert the x64 direct-FB GOP path).
                CMD+=(-vga none -device virtio-gpu-pci)
            else
                CMD+=(-device VGA)
            fi
            ;;
        AARCH64) CMD+=(-device virtio-gpu-pci) ;;
    esac
fi

# --sendkey needs a keyboard.  X64's q35 has a PS/2 controller already;
# the AARCH64 `virt` machine ships none, so add a USB keyboard so monitor
# `sendkey` events reach the guest.  --holdkey needs a USB keyboard on EVERY
# arch: the whole point is UsbKbDxe's typematic auto-repeat (a held key past the
# repeat delay), which the PS/2 path does not model the same way.
if [[ ( -n "$SENDKEY_SEQ" && "$ARCH" == "AARCH64" ) || -n "$HOLDKEY_SEQ" ]]; then
    CMD+=(-device "qemu-xhci,id=axl_kbd_xhci" -device "usb-kbd,bus=axl_kbd_xhci.0")
fi

# Live display (--display/--gui/--vnc...): add a RELATIVE usb-mouse so a pointer
# actually works over the graphical session. Stock OVMF only binds a relative
# boot mouse (UsbMouseDxe -> EFI_SIMPLE_POINTER); a usb-tablet's
# EFI_ABSOLUTE_POINTER is never fed by OVMF (no backing) AND forces QEMU into
# absolute VNC mode, which mis-routes the relative mouse — i.e. the tablet
# yields NO usable pointer in pre-boot OVMF. Gated on a live display so plain
# headless --screenshot baselines (which expect NO pointer) stay byte-stable.
if [[ -n "$DISPLAY_BACKEND" ]]; then
    CMD+=(-device "qemu-xhci,id=axl_mouse_xhci" -device "usb-mouse,bus=axl_mouse_xhci.0,id=axl_mouse")
elif [[ -n "$SENDMOUSE_SEQ" ]]; then
    # Headless QMP --sendmouse path keeps the absolute usb-tablet (legacy; note
    # OVMF doesn't deliver discrete injected events either — see docs).
    CMD+=(-device "qemu-xhci,id=axl_tablet_xhci" -device "usb-tablet,bus=axl_tablet_xhci.0,id=axl_tablet")
fi

# --bridges: matching topology to test/integration/common-test.sh so
# tools that walk PCI bridges (lspci -t, sysinfo --pci, ...) and USB
# hubs (lsusb -t) can be smoke-tested interactively against the same
# shape unit tests use.
# slot is auto-assigned to avoid colliding with the q35 mch at 00:00.0.
if [[ "$BRIDGES" == "true" ]]; then
    CMD+=(
        -device "pcie-root-port,id=axl_rp0,bus=pcie.0,chassis=1"
        -device "virtio-rng-pci,bus=axl_rp0"
        -device "qemu-xhci,id=axl_usb0"
        -device "usb-mouse,bus=axl_usb0.0,port=1"
        -device "usb-hub,bus=axl_usb0.0,port=2"
        -device "usb-tablet,bus=axl_usb0.0,port=2.1"
    )
fi

# GDB stub: -gdb tcp::PORT exposes the GDB protocol; -S starts the
# guest CPU halted so the debugger can attach before the firmware
# runs a single instruction. KVM is incompatible with single-stepping
# many of the early boot instructions — drop -enable-kvm/-cpu host
# from the base cmd and fall back to TCG when --gdb is requested.
if [[ -n "$GDB_PORT" ]]; then
    qemu_strip_kvm CMD          # TCG fallback — KVM can't single-step early boot
    CMD+=(-gdb "tcp::$GDB_PORT")
    if [[ "$GDB_HALT" == "true" ]]; then
        CMD+=(-S)
    fi
    # Bump the watchdog so a debugging session doesn't get terminated.
    # Apply in both foreground and background — the background guard
    # was a no-op when --background ignored TIMEOUT entirely; with
    # the timeout now wired into background mode (so abandoned QEMUs
    # can't camp on hostfwd ports), --background --gdb would be
    # killed at the default 15 s without this bump.
    TIMEOUT=3600
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

# If --mount spawned a helper daemon (virtiofsd) in foreground mode,
# extend the cleanup trap so they don't leak after QEMU exits.
# Background mode emits the PIDs for the caller to manage instead
# (see below).
if [[ "$BACKGROUND" != "true" ]]; then
    helper_pids=()
    [[ -n "$VIRTIOFSD_PID" ]] && helper_pids+=("$VIRTIOFSD_PID")
    if [[ ${#helper_pids[@]} -gt 0 ]]; then
        # Pre-format the kill argument list with a literal space so
        # the trap doesn't depend on $IFS at trap-fire time. Using
        # ${helper_pids[*]} would pick up the first byte of $IFS;
        # safer to spell the join here.
        kill_args=""
        for pid in "${helper_pids[@]}"; do
            kill_args+="${kill_args:+ }$pid"
        done
        trap 'kill '"$kill_args"' 2>/dev/null; rm -rf "'"$TMPDIR"'"' EXIT INT TERM
    fi
fi

# Networking
if [[ "$NET" == "true" ]]; then
    # Validate --mac up-front — QEMU's own error for a bad mac= is opaque.
    if [[ -n "$MAC_ADDR" \
          && ! "$MAC_ADDR" =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]]; then
        echo "ERROR: --mac: '$MAC_ADDR' is not a valid MAC address" \
             "(expect XX:XX:XX:XX:XX:XX)" >&2
        exit 1
    fi
    if [[ -n "$TAP_IFACE" ]]; then
        # Real L2 networking over a pre-created host tap (script=no: run-qemu
        # does NOT create/destroy it — see scripts/netcfg-testnet.sh, which
        # owns the tap + a dnsmasq DHCP server + NAT). Gives the guest a real
        # DHCP lease and working ICMP, unlike SLIRP. hostfwd is SLIRP-only and
        # ignored here (use the tap subnet directly).
        if ! ip link show "$TAP_IFACE" >/dev/null 2>&1; then
            echo "ERROR: --tap '$TAP_IFACE' does not exist (create it first," \
                 "e.g. scripts/netcfg-testnet.sh up)" >&2
            exit 1
        fi
        NETDEV="tap,id=net0,ifname=${TAP_IFACE},script=no,downscript=no"
    else
        NETDEV="user,id=net0"
        for fwd in "${HOSTFWDS[@]}"; do
            HOST_PORT="${fwd%%:*}"
            GUEST_PORT="${fwd##*:}"
            # `auto` asks the harness for a host port that is verified free
            # right now and claimed for this process's lifetime, instead of
            # the caller hard-coding one and hoping. The chosen port is
            # reported back as HOSTFWD_<guest>=<host>.
            if [[ "$HOST_PORT" == "auto" ]]; then
                axl_alloc_host_port HOST_PORT || exit 1
                HOSTFWD_CHOSEN+=("HOSTFWD_${GUEST_PORT}=${HOST_PORT}")
            fi
            NETDEV="$NETDEV,hostfwd=tcp::${HOST_PORT}-:${GUEST_PORT}"
        done
        # Non-machine-readable modes get it on stderr; --background repeats
        # it on stdout next to QEMU_PID= where scripts already parse.
        for _hf in "${HOSTFWD_CHOSEN[@]}"; do
            echo "[run-qemu] $_hf" >&2
        done
    fi
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
    if [[ -n "$MAC_ADDR" ]]; then
        NIC_DEV="${NIC_DEV},mac=${MAC_ADDR}"
    fi
    if [[ "${NIC_NO_ROM:-false}" == "true" ]]; then
        NIC_DEV="${NIC_DEV},romfile="
    fi
    CMD+=(-device "$NIC_DEV" -netdev "$NETDEV")
else
    CMD+=(-net none)
fi

# Final append, called by every launch path immediately before QEMU
# starts (and by the QEMU_DRYRUN dump below).
#
# --qemu-arg passthrough: each accumulated value is one literal QEMU
# token, appended verbatim (no word-splitting), so a token may contain
# spaces — see the --qemu-arg contract in --help.
#
# These land LAST, after the per-mode defaults each branch below adds
# (-serial / -display / -monitor / -qmp / -nographic / -no-reboot), so
# a caller's explicit token beats run-qemu's default for every option
# QEMU resolves last-wins. Without that, `--qemu-arg -action
# --qemu-arg reboot=reset` could never undo -no-reboot and no consumer
# could test what its firmware does on the NEXT boot. Options QEMU
# treats as additive (-device, -drive, -netdev, -chardev, -smbios,
# and the monitor family) are unaffected by the position.
finalize_qemu_cmd() {
    # --reboot: let a guest-initiated reset re-POST the same VM instead
    # of ending the run. -no-reboot is shorthand for `-action
    # reboot=shutdown`; `-action reboot=reset` is its explicit inverse,
    # and the later token wins. Placed before the caller's own tokens
    # so an explicit --qemu-arg still overrides it.
    if [[ "$REBOOT" == "true" ]]; then
        CMD+=(-action reboot=reset)
    fi
    if [[ ${#EXTRA_QEMU_ARGS[@]} -gt 0 ]]; then
        # Append each token verbatim — no word-splitting, so a token may
        # contain spaces (e.g. a device spec with a space in a file path).
        CMD+=( "${EXTRA_QEMU_ARGS[@]}" )
    fi
}

# QEMU_DRYRUN=1 prints the constructed CMD and exits without launching
# qemu. Useful for argument-shape regression tests and for "what
# would run-qemu.sh do?" debugging. Each CMD token is printed on its
# own line prefixed with "QEMU_DRYRUN: " — no shell-quoting noise so
# tests can grep for token literals (commas in -device strings, etc.)
# without escaping.
if [[ "${QEMU_DRYRUN:-0}" == "1" ]]; then
    finalize_qemu_cmd
    for tok in "${CMD[@]}"; do
        printf 'QEMU_DRYRUN: %s\n' "$tok"
    done
    exit 0
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
    # Also kill virtiofsd if --mount spawned it
    # (otherwise they'll outlive the QEMU process and hold sockets
    # open).
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

    finalize_qemu_cmd
    "${CMD[@]}"
    exit $?
fi

# Windowed mode — open a real graphical window for the guest's GOP
# framebuffer (--display gtk / --gui). Over SSH the GTK window is an X11
# client that rides the forwarded display, so it pops up on the client
# (e.g. a Mac running XQuartz). The guest serial console still goes to
# this terminal, so app text + the UEFI shell are readable here while the
# graphics render in the window. Like interactive: no timeout, no ANSI
# strip, no CPU sampler — the window stays up until the app resets or you
# close it. gl=off keeps GTK on software surfaces (this QEMU is built
# --disable-opengl, and GL over forwarded X11 is the thing that breaks).
if [[ -n "$DISPLAY_BACKEND" ]]; then
    if [[ "$DISPLAY_BACKEND" == gtk ]]; then
        # gl=off keeps GTK on software surfaces (GL over forwarded X11 breaks).
        CMD+=(-display "$DISPLAY_BACKEND,gl=off")
        cat >&2 <<HINT
[run-qemu] Windowed mode (gtk) — a QEMU window should open on your X server
[run-qemu]   (DISPLAY=${DISPLAY:-}). Guest serial is on this terminal; graphics
[run-qemu]   render in the window. Close the window (or Ctrl-C here) to quit.
HINT
    else
        # VNC backend. Two shapes:
        #   vnc=:N                     — serve VNC on TCP 5900+N (viewer connects in)
        #   vnc=HOST:PORT,reverse      — connect out to a listening viewer at HOST:PORT
        CMD+=(-display "$DISPLAY_BACKEND")
        spec="${DISPLAY_BACKEND#vnc=}"
        if [[ "$spec" == *,reverse* ]]; then
            target="${spec%,reverse*}"
            cat >&2 <<HINT
[run-qemu] VNC reverse mode — connecting out to a listening viewer at $target.
[run-qemu]   Start your viewer in listen mode first (e.g. TigerVNC
[run-qemu]   'vncviewer -listen <port>'). Guest serial is on this terminal;
[run-qemu]   graphics appear in the viewer. Ctrl-C here to quit.
HINT
        elif [[ "$spec" =~ ^:([0-9]+)$ ]]; then
            n="${BASH_REMATCH[1]}"
            cat >&2 <<HINT
[run-qemu] VNC mode — QEMU serves VNC on display :$n (TCP $((5900 + n))).
[run-qemu]   Connect a viewer; for a remote host, tunnel first, e.g.:
[run-qemu]     ssh -L $((5900 + n)):localhost:$((5900 + n)) <thishost>
[run-qemu]   then point your viewer at localhost:$n. Guest serial is on this
[run-qemu]   terminal. Ctrl-C here to quit.
HINT
        else
            echo "[run-qemu] VNC mode ($DISPLAY_BACKEND). Guest serial on this terminal; Ctrl-C to quit." >&2
        fi
    fi
    CMD+=(-serial stdio
          -no-reboot)

    # QEMU may put this terminal into raw mode for the serial console;
    # restore line discipline on any exit path, and reap helper daemons.
    cleanup_cmd='rm -rf "'"$TMPDIR"'"; stty sane 2>/dev/null || true'
    [[ -n "$VIRTIOFSD_PID" ]] && \
        cleanup_cmd="kill $VIRTIOFSD_PID 2>/dev/null; $cleanup_cmd"
    trap "$cleanup_cmd" EXIT INT TERM

    finalize_qemu_cmd
    "${CMD[@]}"
    exit $?
fi

# Screenshot mode
if [[ -n "$SCREENSHOT" ]]; then
    MONSOCK="$TMPDIR/monitor.sock"
    # GPU device already wired above (--screenshot implies --gpu). Serial goes to
    # the internal log unless the caller asked for a persistent --serial-log (a
    # serial-driven scenario like the kbtune bounce A/B reads it back).
    CMD+=(-serial "file:${SERIAL_LOG:-$LOG}" -display none)
    CMD+=(-monitor "unix:$MONSOCK,server,nowait")
    # HMP `sendkey` covers key taps, but HMP has no absolute-pointer move and no
    # key-hold; the QMP `input-send-event` does both. Add a QMP socket when
    # injecting a mouse move or holding a key.
    QMPSOCK="$TMPDIR/qmp.sock"
    if [[ -n "$SENDMOUSE_SEQ" || -n "$HOLDKEY_SEQ" ]]; then
        CMD+=(-qmp "unix:$QMPSOCK,server,nowait")
    fi

    set +e
    finalize_qemu_cmd
    "${CMD[@]}" &
    QEMU_PID=$!
    # Sample host CPU for the life of this QEMU (same monitor the default branch
    # runs). A short SHOT_WAIT may kill QEMU before the warm-up clears -> no
    # post-warm-up data, so no detection (fail-safe): give the guest enough
    # SHOT_WAIT past the 10-15s warm-up to measure a spike.
    cpu_monitor_start "$QEMU_PID"

    # Pre-screenshot settle.  Decoupled from TIMEOUT so a caller can keep a
    # safe kill-timeout but capture sooner: set SHOT_WAIT to the seconds the
    # guest needs to boot + render (the visual suite tunes this).  Default =
    # the old TIMEOUT-3 behaviour.
    WAIT="${SHOT_WAIT:-$((TIMEOUT - 3))}"
    [[ $WAIT -lt 3 ]] && WAIT=3
    sleep "$WAIT"

    # --sendkey: inject key tokens via the monitor once the app is up, then
    # let it settle/repaint before the dump.  Generous per-key delay (TCG is
    # slow on aarch64) so no keystroke is dropped.
    if [[ -n "$SENDKEY_SEQ" ]]; then
        key_delay=0.4
        [[ "$ARCH" == "AARCH64" ]] && key_delay=1.0
        for key in $SENDKEY_SEQ; do
            echo "sendkey $key" | socat -t 2 - "UNIX-CONNECT:$MONSOCK" >/dev/null 2>&1
            sleep "$key_delay"
        done
        sleep 1.5   # let the app process + repaint before capture
    fi

    # --holdkey "qcode:ms": press a key DOWN, hold it for ms, then release — via
    # QMP input-send-event (HMP `sendkey` can only tap). Holding a key past the
    # firmware's typematic delay makes UsbKbDxe synthesise auto-repeats, which is
    # exactly how a KVM's delayed key-up surfaces as a "bounce" at the shell — so
    # this reproduces that bounce in QEMU. Each socat connection is a fresh QMP
    # session, so capabilities must be re-negotiated on every call.
    if [[ -n "$HOLDKEY_SEQ" ]]; then
        for hk in $HOLDKEY_SEQ; do
            IFS=':' read -r hk_code hk_ms <<<"$hk"
            [[ -z "$hk_ms" ]] && hk_ms=500
            # Validate before use: hk_ms is interpolated into awk, and a bad value
            # would otherwise silently hold for 0 ms (a no-op tap).
            if [[ -z "$hk_code" || ! "$hk_ms" =~ ^[0-9]+$ ]]; then
                echo "[run-qemu] --holdkey: bad spec '$hk' (want QCODE:MS); skipping" >&2
                continue
            fi
            {
                printf '%s\n' '{"execute":"qmp_capabilities"}'
                printf '%s\n' "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":true,\"key\":{\"type\":\"qcode\",\"data\":\"$hk_code\"}}}]}}"
            } | socat -t 2 - "UNIX-CONNECT:$QMPSOCK" >/dev/null 2>&1
            sleep "$(awk "BEGIN{printf \"%.3f\", $hk_ms/1000}")"
            {
                printf '%s\n' '{"execute":"qmp_capabilities"}'
                printf '%s\n' "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":false,\"key\":{\"type\":\"qcode\",\"data\":\"$hk_code\"}}}]}}"
            } | socat -t 2 - "UNIX-CONNECT:$QMPSOCK" >/dev/null 2>&1
        done
        sleep 1.5   # let the reader drain the synthesised repeats before capture
    fi

    # --sendmouse: inject absolute-pointer moves via QMP after any keys.
    # fx,fy are screen fractions in [0,1] → QEMU's abs axis range 0..32767
    # (INPUT_EVENT_ABS_MAX), so the caller needn't know the guest resolution.
    # A trailing ",click" presses+releases the left button at that point.
    if [[ -n "$SENDMOUSE_SEQ" ]]; then
        move_delay=0.4
        [[ "$ARCH" == "AARCH64" ]] && move_delay=1.0
        for m in $SENDMOUSE_SEQ; do
            IFS=',' read -r fx fy click <<<"$m"
            vx=$(awk "BEGIN{printf \"%d\", $fx*32767}")
            vy=$(awk "BEGIN{printf \"%d\", $fy*32767}")
            {
                printf '%s\n' '{"execute":"qmp_capabilities"}'
                printf '%s\n' "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$vx}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$vy}}]}}"
                if [[ "$click" == "click" ]]; then
                    printf '%s\n' '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"left","down":true}}]}}'
                    printf '%s\n' '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"left","down":false}}]}}'
                fi
            } | socat -t 2 - "UNIX-CONNECT:$QMPSOCK" >/dev/null 2>&1
            sleep "$move_delay"
        done
        sleep 1.5   # let the app process the motion + repaint before capture
    fi

    for try in 1 2 3; do
        echo "screendump $TMPDIR/screenshot.ppm" | \
            socat -t 2 - "UNIX-CONNECT:$MONSOCK" >/dev/null 2>&1 && break
        sleep 1
    done
    sleep 1
    kill "$QEMU_PID" >/dev/null 2>&1
    wait "$QEMU_PID" >/dev/null 2>&1
    set -e

    # Summarize host CPU (WARN + optional CPU-REPORT). A sustained post-warm-up
    # spike returns CPU_SPIKE_EXIT, surfaced as the run's exit below -- AFTER the
    # PNG is written, and only when a capture/convert error (which outranks it)
    # did not already fail the run.
    cpu_spike_rc=0
    cpu_monitor_finish || cpu_spike_rc=$?

    if [[ -f "$TMPDIR/screenshot.ppm" ]]; then
        # QEMU's `screendump` always writes PPM. Convert to the format
        # implied by the destination extension. PPM destinations skip
        # conversion; PNG/JPG/etc. try ImageMagick, then Pillow, then
        # error out (refuse to ship a misnamed PPM as a PNG).
        ext="${SCREENSHOT##*.}"
        ext="${ext,,}"  # lowercase
        if [[ "$ext" == "ppm" ]]; then
            cp "$TMPDIR/screenshot.ppm" "$SCREENSHOT"
            echo "Screenshot saved: $SCREENSHOT"
        elif command -v convert &>/dev/null; then
            convert "$TMPDIR/screenshot.ppm" "$SCREENSHOT"
            echo "Screenshot saved: $SCREENSHOT"
        elif python3 -c 'import PIL' 2>/dev/null; then
            python3 -c '
import sys
from PIL import Image
Image.open(sys.argv[1]).save(sys.argv[2])
' "$TMPDIR/screenshot.ppm" "$SCREENSHOT"
            echo "Screenshot saved: $SCREENSHOT"
        else
            # Bail rather than silently mislabel a PPM as PNG. A capture/convert
            # error outranks a CPU spike.
            echo "ERROR: --screenshot $SCREENSHOT requires ImageMagick (convert) or Python Pillow for .${ext} output" >&2
            echo "       install one of:  dnf install ImageMagick   |   pip install pillow" >&2
            echo "       or use a .ppm destination to skip conversion" >&2
            cp "$TMPDIR/screenshot.ppm" "${SCREENSHOT%.*}.ppm"
            echo "Raw PPM saved instead: ${SCREENSHOT%.*}.ppm" >&2
            exit 1
        fi
        # The PNG was written; now let a CPU spike fail the run (PNG kept).
        exit "$cpu_spike_rc"
    else
        # Capture failed: keep the historical soft exit (0). Nothing to mask -- the
        # spike is not surfaced when there is no screenshot to accompany it.
        echo "WARNING: screenshot capture failed" >&2
        exit 0
    fi

# Background mode
elif [[ "$BACKGROUND" == "true" ]]; then
    # Wrap with `timeout` so a forgotten/orphaned background QEMU can't
    # camp on hostfwd ports forever. Caller's --timeout governs the
    # upper bound; default falls back to TIMEOUT (15 s base, +10 s
    # AARCH64 — see ARCH adjust above). Without this guard, scripts
    # whose driver dies mid-run (SIGPIPE from a tail filter, manual
    # abort, …) leak QEMU and the next run's hostfwd binding fails
    # silently with "server did not start".
    #
    # `timeout` reparents the command, but QEMU stays a direct child
    # of the wrapper subshell — `pgrep -P` finds it the same way the
    # foreground branch does, so anything that grabs QEMU_PID for
    # later kill/screenshot still works.
    if [[ -n "$SERIAL_SOCKET" ]]; then
        # Serial as a UNIX socket the host can open to read output AND
        # write input. Drop -nographic (it implies serial=stdio) in
        # favour of an explicit chardev binding.
        #
        # No -no-shutdown here (an earlier version of this comment
        # claimed one, and never passed it): the caller reads the
        # socket LIVE while the guest runs, so there is no post-mortem
        # log to drain and nothing to keep QEMU alive for. Keeping the
        # guest's own Exit fatal is what lets callers use "QEMU is
        # gone" as the end-of-run signal.
        rm -f "$SERIAL_SOCKET"
        # wait=on: block the guest until the caller connects its serial reader,
        # so no early output is lost. Callers launch QEMU here, then connect a
        # socat reader a beat later; with wait=off the guest boots immediately
        # and a fast app can print its ready-marker before the reader attaches
        # (the marker is then dropped and the caller waits forever). The old
        # 5 s Shell startup countdown used to cover that gap by accident; the
        # -delay 0 launcher removed it, exposing the latent race. QEMU is still
        # bounded by the `timeout` wrapper below, so a caller that never connects
        # can't hang past --timeout.
        CMD+=(
            -no-reboot
            -chardev "socket,id=serial0,path=$SERIAL_SOCKET,server=on,wait=on"
            -serial chardev:serial0
            -display none
        )
    else
        CMD+=(-nographic -no-reboot)
    fi
    finalize_qemu_cmd
    # Serial destination. When the caller asked for --serial-log, write serial
    # DIRECTLY to their path (which they own and which lives OUTSIDE our $TMPDIR)
    # rather than to $LOG-in-$TMPDIR plus a symlink. The self-clean below rm's
    # $TMPDIR the instant the guest exits, so a serial log kept inside it would
    # vanish out from under a caller that reads the transcript AFTER the guest
    # is gone (the common "guest resets/exits on its own, then grep the log"
    # pattern). A caller-owned path survives the cleanup untouched. With no
    # --serial-log the internal $LOG is fine — that caller opted out of a
    # persistent transcript, so losing it on cleanup is expected.
    BG_SERIAL_DEST="${SERIAL_LOG:-$LOG}"
    # Pre-create the destination so we fail LOUD, before launch, if the caller's
    # --serial-log path isn't writable (e.g. a missing parent dir). Otherwise the
    # subshell's `> "$BG_SERIAL_DEST"` open would fail, `|| :` would swallow it,
    # qemu would never start, and we'd report a dead subshell PID as if it were a
    # live guest. ($LOG lives in $TMPDIR, which always exists, so the no-serial-log
    # case can't trip this.)
    if ! : > "$BG_SERIAL_DEST" 2>/dev/null; then
        echo "ERROR: cannot write --serial-log '$BG_SERIAL_DEST' (missing dir?)" >&2
        exit 1
    fi
    # Self-clean the state dir when the guest exits (stop, crash, or the timeout
    # wrapper firing). --background returns immediately so it can't use an EXIT
    # trap (the disk/serial must outlive THIS script), but the detached subshell
    # can rm $TMPDIR once qemu is gone — tying cleanup to the guest's real
    # lifetime. Without this, every --background run orphaned its ~40 MB
    # /dev/shm/axl-qemu.* dir forever. The reported PID still points at qemu
    # (qemu_child_pid walks into the subshell), so consumers grep/kill it as
    # before; the subshell just reaps and cleans.
    # `|| :` so timeout's non-zero exit (124 on expiry, or the guest's own exit
    # code) doesn't abort the subshell under `set -e` before the rm runs.
    # The `>/dev/null 2>&1` on the SUBSHELL (not just the inner qemu) is
    # load-bearing: the inner `> "$BG_SERIAL_DEST"` only redirects the
    # timeout+qemu command, so without this the detached subshell would inherit
    # run-qemu's own stdout and stderr and hold them open for the whole guest
    # lifetime. A caller that captures our output via command substitution
    # (out=$(run-qemu … --background …)) then blocks forever — the $() never
    # sees EOF until the guest dies. The QEMU_PID=/SERIAL_LOG=/TMPDIR= lines
    # below are echoed after this on run-qemu's real stdout, so they still reach
    # the caller.
    ( timeout "$TIMEOUT" "${CMD[@]}" > "$BG_SERIAL_DEST" 2>&1 < /dev/null || :; rm -rf "$TMPDIR" ) >/dev/null 2>&1 &
    WRAPPER_PID=$!
    QEMU_PID=$(qemu_child_pid "$WRAPPER_PID")
    if [[ -z "$QEMU_PID" ]]; then
        # Fall back to the wrapper PID. `kill $WRAPPER_PID` won't
        # propagate to a still-execing `timeout`+QEMU pair (they'd
        # be reparented to init and run until the timeout fires),
        # so this is a best-effort handle for the caller — the
        # `timeout SECS` watchdog above is the actual guarantee
        # that QEMU goes away. In practice pgrep finds the child
        # within tens of ms, so this branch is rare.
        QEMU_PID=$WRAPPER_PID
    fi
    # No CPU monitor here: --background returns with QEMU still running (the caller
    # owns its lifecycle), so there is no synchronous point to wait the sampler,
    # summarize, or fail the run on a spike. A caller that wants CPU data in
    # background mode should sample the emitted QEMU_PID itself.

    # No symlink needed: with --serial-log, qemu already writes serial straight
    # to the caller's path (BG_SERIAL_DEST above); without it there is nothing to
    # link. The old `ln -sf "$LOG" "$SERIAL_LOG"` pointed the caller's path into
    # $TMPDIR, which the self-clean then deleted the moment the guest exited.

    # A refused -netdev is fatal to QEMU and lands in the serial log within
    # milliseconds. Catch it here rather than returning a QEMU_PID for a
    # process that is already dead and letting the caller time out waiting
    # for a guest that never had a network.
    for _i in $(seq 1 20); do
        kill -0 "$QEMU_PID" 2>/dev/null || break
        grep -qa "Could not set up host forwarding rule" "$BG_SERIAL_DEST" 2>/dev/null && break
        sleep 0.1
    done
    axl_report_hostfwd_failure "$BG_SERIAL_DEST" "run-qemu.sh --background" || exit 1

    echo "QEMU_PID=$QEMU_PID"
    echo "SERIAL_LOG=$BG_SERIAL_DEST"
    for _hf in "${HOSTFWD_CHOSEN[@]}"; do
        echo "$_hf"
    done
    [[ -n "$SERIAL_SOCKET" ]] && echo "SERIAL_SOCKET=$SERIAL_SOCKET"
    [[ -n "$VIRTIOFSD_PID" ]] && echo "VIRTIOFSD_PID=$VIRTIOFSD_PID"
    echo "TMPDIR=$TMPDIR"
    # TMPDIR is self-cleaned by the detached subshell above when the guest exits
    # (see the launch site), so the caller only needs to kill QEMU (and
    # virtiofsd, when --mount was used). TMPDIR is still reported for callers
    # that read it while the guest runs.

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
    finalize_qemu_cmd
    ( timeout "$TIMEOUT" "${CMD[@]}" > "$LOG" 2>&1 < /dev/null ) &
    WRAPPER_PID=$!
    QPID=""
    if [[ "$CPU_WARN" == "true" || "$CPU_REPORT" == "true" ]]; then
        QPID=$(qemu_child_pid "$WRAPPER_PID")
    fi
    cpu_monitor_start "$QPID"
    wait "$WRAPPER_PID" 2>/dev/null || true

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

    # A hostfwd that could not bind makes QEMU refuse the whole -netdev and
    # exit. The log is NOT empty in that case (it holds QEMU's error), so the
    # guard below does not fire; and the non-raw output filter below slices
    # only the region between "to continue." and "Reset with", which drops a
    # startup error entirely — the run then exited 0 with no output at all.
    # Diagnose it before either of those get a chance to hide it.
    axl_report_hostfwd_failure "$LOG" "run-qemu.sh" || exit 1

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
        # App output runs from the start sentinel (or, if the launcher fell back
        # to booting the Shell directly, the "...to continue." countdown prompt)
        # down to the "Reset with" trailer. Strip the anchor lines themselves.
        sed -n "/$APP_OUTPUT_SENTINEL\|to continue\./,/^Reset with/p" "$CLEAN" | \
            grep -vF "$APP_OUTPUT_SENTINEL" | \
            grep -v "to continue\." | \
            grep -v "^Reset with"
    fi

    # CPU-spike summary + optional CPU-REPORT. Runs after the serial output so a
    # WARN is the last thing the user sees. A sustained post-warm-up spike exits
    # CPU_SPIKE_EXIT -- but only here, AFTER the empty-log guest-failure check above
    # (exit 1) has taken precedence.
    cpu_spike_rc=0
    cpu_monitor_finish || cpu_spike_rc=$?
    exit "$cpu_spike_rc"
fi
