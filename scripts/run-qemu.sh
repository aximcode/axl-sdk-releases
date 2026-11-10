#!/bin/bash
# axl-desc: boot a .efi under QEMU + OVMF (-i, --mount, --gdb, --net)
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
#   --no-gpu / --headless Give the guest NO graphics adapter, so OVMF
#                         exposes no GOP and axl_gfx_available() is false.
#                         For logic/compute-only test runs: skips the
#                         render pipeline (faster) and side-steps a false
#                         CPU-spike from incidental full-screen rendering.
#                         Mutually exclusive with --gpu/--screenshot/
#                         --display/--gui (they need a framebuffer).
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
#   --setvar NAME=VALUE    `set` a shell variable before the app (repeatable)
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
#   --background          Launch QEMU in background, print PID. The caller
#                         owns the guest from the QEMU_PID= line; if this
#                         script dies before printing it, the guest goes too
#   --serial-log FILE     Save serial output to FILE
#   --monitor-socket PATH With --background: HMP monitor on a UNIX socket for
#                         the guest's life (MONITOR_SOCKET=PATH is printed)
#   --qmp-socket PATH     With --background: the same for QMP (QMP_SOCKET=)
#   --gdb [PORT]          Expose QEMU GDB stub on tcp::PORT (default 1234).
#                         Boot runs free; attach with
#                         `gdb -ex 'target remote :PORT'` and `interrupt`
#                         when ready. Implies a long timeout under
#                         --background. Add --gdb-halt to start with -S
#                         (guest halted before instruction 0).
#   --gdb-halt            With --gdb, also start with -S (guest halted).
#                         Useful for breaking inside SecMain or PEI.
#   --debugcon FILE       Capture the firmware's DEBUG output. X64: OVMF's
#                         port 0x402 -- required for gdb-syms.py to recover
#                         module load addresses. AARCH64: the guest's debug
#                         UART, which every AARCH64 guest has (the bundled
#                         firmware sends DEBUG there, so the console carries
#                         no DEBUG output); without --debugcon its log is
#                         beside --serial-log as <name>-firmware-debug.log, in
#                         --state-dir, else in the run's temp dir. A
#                         foreground or pooled run that hit a firmware ASSERT
#                         or exception (whose header alone is the console's;
#                         its registers are DEBUG) prints the log after the
#                         app's output as "[firmware-debug] ..." lines, and
#                         --raw no longer shows the firmware's DEBUG on
#                         AARCH64. --background prints FIRMWARE_DEBUG_LOG=;
#                         with no --serial-log that file is in the run's temp
#                         dir, gone with the guest, as its serial log is.
#                         X64 with --state-dir (a pooled guest's launch)
#                         always has the 0x402 port, its log in the state dir
#                         (or at --debugcon FILE); a booted X64 run has it
#                         only with --debugcon, and prints none of it. A
#                         pooled X64 run's ASSERT is printed as AARCH64's.
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
#   --workload SHAPE      'idle' (default) keeps the CPU-spike check; 'compute'
#                         swaps it for a --max-duration budget
#   --max-duration SECS   Fail (exit 9) if the run exceeds SECS
#   --no-cpu-warn         Disable the CPU-spike check (on by default in the
#                         default + --screenshot modes; samples QEMU's host
#                         CPU after the firmware-boot warm-up and, on a
#                         sustained spin, prints a WARN line AND fails the run
#                         with exit 8 — guest failure/timeout take precedence).
#   --cpu-threshold N     Spike threshold in cores (default 1.5).
#   --cpu-sustain SECS    Sustain duration in seconds (default 2).
#
# Pooled mode (docs/AXL-Test-Guest-Pool-Design.md §6.9). Inside a lease --
# AXL_POOL_AGENT set: the test declared guest=<class> and the runner leased
# it under `axlpool run` -- a run whose machine IS the lease's class does not
# boot. It restores the leased guest to its clean snapshot (`axlpool restore`:
# a fresh machine per call, as a boot gives), stages the app, the --extra
# files and pci-ids.json5 into a directory of its own in the lease's share,
# and runs the script a boot's startup.nsh would (@echo -off, the --setvar
# lines, the sentinel, the app or the --nsh script) through the agent, its
# output on the guest's console (`axlpool exec --console`).
#   Same output: the console from the sentinel, ANSI-stripped and filtered as
#   a boot's serial log is. --raw and --serial-log: the console from the
#   run's start (there is no firmware boot to show).
#   Same exit codes: 0, 1, 2, 8 (the CPU-spike check samples the leased
#   guest's QEMU, AXL_POOL_QEMU_PID, with no boot warm-up), 9.
#   The machine must be the lease's class's, fact for fact: `axlpool run`
#   exports the class's run-qemu.sh options (test/pool/classes.toml, its args
#   then its arch's) as AXL_POOL_CLASS_ARGS, one per line; they are parsed,
#   after the `--net` every pooled guest has, by this script's own option
#   loop, and compared with the run's: --bridges, the NIC (model, --nic-no-rom,
#   --mac), --gpu (or --screenshot/--display, which imply it), --no-gpu,
#   --cpu, --mem, each --guestfwd and each --qemu-arg in order. A run with no
#   NIC option and no --bridges is the bare machine and compares as `--net`
#   (every class has the agent's NIC). So plain is a bare run or `--net`;
#   standard is `--bridges --net` with its NIC (X64: --nic-model e1000); a
#   one-off class (`extends`, its own args) is the run naming exactly those.
#   The first fact that differs is said, and the run boots. So does a lease
#   with no AXL_POOL_CLASS_ARGS.
#   What is not the machine but the way a run is driven boots exactly as
#   before: --screenshot and the input options, --display/--vnc, -i,
#   --mount, --background, --debugcon, --gdb, --serial-socket/
#   --monitor-socket/--qmp-socket, --reboot, --boot-target, --shell,
#   --qcow2/--state-dir/--hostfwd-bind, --tap, --hostfwd; and no app; an
#   --arch other than the lease's; QEMU_DRYRUN; AXL_RUN_QEMU_POOL=0; the
#   test's own pooled background run live (AXL_POOL_BG_LIVE, common-test.sh:
#   the restore would end its server).
#   A --nsh script pools only when its first non-blank line is `@echo -off`
#   (a boot echoes every command before it; a comment first also boots). That line, a following `fs0:` and `cd \`, and a
#   last `reset -s` are the boot's boilerplate, dropped. It boots when what
#   remains has a `reset` or `exit` line, names fs0: (not the run's disk on a
#   leased guest), or changes to a volume's root (`cd \...`).
#   As on a boot disk: the run's directory is the Shell's `path` (a boot's is
#   its disk's root), each staged .efi is also there as .EFI (the Shell looks
#   a bare `tool` up as tool.EFI; the share is case-sensitive, FAT is not),
#   and only a run whose script had `fs0:` -- or an app run -- enters its
#   directory (a boot's script has no current directory before `fs0:`).
#   What still differs, so a test that depends on it stays booted: the
#   machine has the agent's NIC (its network stack started) and the share;
#   `ls` of the run's directory lists the .EFI copies; an app that resets the
#   machine reboots the leased guest rather than ending a QEMU of its own.
#   A pool failure -- the restore refused or failed (a service older than the
#   op answers unknown-op), the agent could not run the script, no sentinel
#   on the console -- boots the run instead: never its verdict.
#   Said, never to the caller (46 tests parse stdout+stderr): the TIMING line
#   says mode=pooled, and a run booted inside a lease says why, as
#   `run-qemu: booted inside a <class> lease: <reason>`, both to
#   $AXL_TEST_TIMING_LOG (else a terminal).
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

# Answered before anything else parses argv, so `--version` never has to
# survive a positional-hungry option loop. See axl-common.sh.
axl_handle_version "run-qemu" "$@" && exit 0

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
# --no-gpu / --headless gives the guest NO graphics adapter, so OVMF exposes no
# GOP and axl_gfx_available() is false — for logic/compute-only test runs that
# skip the render pipeline (faster, and side-steps an incidental full-screen
# render tripping the CPU-spike gate). Opt-in; mutually exclusive with the
# framebuffer modes below.
NO_GPU=false
NET=false
NIC_MODEL=""        # default chosen later (virtio-net-pci); --nic-model overrides
MAC_ADDR=""         # --mac XX:XX:XX:XX:XX:XX (HF4: replay a captured NIC MAC)
CPU_SPEC=""         # --cpu SPEC (HF4: replay a captured CPU model, e.g. qemu64,family=6)
HOSTFWDS=()
GUESTFWDS=()        # --guestfwd SPEC, each appended to the slirp netdev
HOSTFWD_CHOSEN=()   # HOSTFWD_<guest>=<host> for each --hostfwd auto:<guest>
EXTRA_FILES=()
# Shell variables to `set` before the app runs, as NAME=VALUE. Exists so a test
# can opt INTO verbosity instead of the library shouting by default: AXL reads
# AXL_LOG_LEVEL through the Shell protocol, so `--setvar AXL_LOG_LEVEL=debug`
# makes debug-level lines observable in the serial log for the one run that
# needs them. Repeatable.
SETVARS=()
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
# --monitor-socket / --qmp-socket (--background only): a caller-owned HMP /
# QMP socket for a guest the caller drives for its whole life (screendump on
# demand, sendkey, input-send-event), reported back as MONITOR_SOCKET= /
# QMP_SOCKET= next to QEMU_PID=.
MONITOR_SOCKET=""
QMP_SOCKET=""
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
SENDKEY_AFTER="" # ERE the guest must print on serial before --sendkey types
SENDKEY_SETTLE_MS=""  # trailing settle after the last key (default 1500)
SCREENSHOT_AFTER=""   # ERE the guest must print (post-injection) before capture
SCREENSHOT_AFTER_COUNT="" # wait for the Nth match, not the first (default 1)
SENDMOUSE_SEQ="" # space-separated "fx,fy[,click]" absolute-pointer moves (--screenshot, QMP)
HOLDKEY_SEQ=""   # space-separated "qcode:ms" held keypresses (--screenshot, QMP)
# CPU-spike detection knobs come from axl-common.sh; the KVM/TCG-aware threshold,
# warm-up and warn carve-out are applied by cpu_policy_init "$ARCH" below (after
# --arch is known). --cpu-threshold / --no-cpu-warn / --cpu-report still override,
# because cpu_policy_init only fills a value a flag has not already set.
# WORKLOAD picks WHICH gate applies to this run (see the case below): `idle`
# keeps the CPU-sustain check, `compute` swaps it for a --max-duration budget.
WORKLOAD="${WORKLOAD:-idle}"
INTERACTIVE=false
# --display BACKEND (or --gui = gtk): open a real graphical window for the
# guest's GOP framebuffer instead of running headless. Over SSH this rides
# X11 forwarding (ssh -Y + an X server like XQuartz on the client), so the
# window pops up on the client's screen. Implies --gpu. gtk is the only
# backend the bundled QEMU builds with today.
DISPLAY_BACKEND=""
MOUNT_DIR=""
MOUNT_TAG="hostfs"
# --qcow2: the vars pflash and the boot disk on qcow2, so a snapshot captures
# the UEFI variables and the disk (the guest pool restores both per lease).
QCOW2=false
# --hostfwd-bind ADDR: the address every forwarded host port listens on
# (default: every interface, QEMU's own default).
HOSTFWD_BIND=""
# --state-dir DIR: with --qcow2, the qcow2 vars and disk live in DIR and
# outlive the run, so a new QEMU can restore from them (the pool's cold start).
STATE_DIR=""
MEM="512M"            # guest RAM (also used for memory-backend-file size)
BRIDGES=false         # --bridges boots the standard guest (the integration tests')
TAP_IFACE=""          # --tap <ifname>: bridge the NIC to a host tap (real net)
                      # instead of SLIRP user-mode (real DHCP/ICMP; needs the
                      # tap pre-created + owned by the qemu user)

# Hardware-fixture platform-identity injection (SMBIOS / ACPI / SPD /
# TPM / IPMI) is NOT handled here — it lives in scripts/axl-emulate,
# which builds the QEMU device args from a fixture and passes them via
# --qemu-arg. run-qemu.sh is a generic launcher. See the HF design doc's
# 2026-06-08 architecture decision.

# The parse loop below shifts every argument away, so keep a verbatim copy for
# the shell-launcher fallback retry near the end of this script. Set by
# stage_boot_shell; initialized here because --boot-target never calls it.
ORIG_ARGV=("$@")
SHELL_LAUNCHER_STAGED=false
SHELL_LAUNCHER_PATH=""

# _rq_machine_sig: the machine the parsed options ask for, one fact per line
# in a fixed order -- what pooled mode compares with the lease's class (its
# options parsed by this same loop: _rq_class_sig). The way a run is DRIVEN
# (screenshots, sockets, --background...) is not here; pooled mode refuses
# those separately.
_rq_machine_sig() {
    local a nic=none gpu=false
    [[ "$NET" == true ]] && nic="${NIC_MODEL:-virtio-net-pci}"
    [[ "$ENABLE_GPU" == true || -n "$SCREENSHOT" || -n "$DISPLAY_BACKEND" ]] && gpu=true
    printf '%s\n' "bridges=$BRIDGES" "nic=$nic" "nic-no-rom=${NIC_NO_ROM:-false}" \
        "mac=$MAC_ADDR" "gpu=$gpu" "no-gpu=$NO_GPU" "cpu=$CPU_SPEC" "mem=$MEM"
    for a in ${GUESTFWDS[@]+"${GUESTFWDS[@]}"}; do printf 'guestfwd=%s\n' "$a"; done
    for a in ${EXTRA_QEMU_ARGS[@]+"${EXTRA_QEMU_ARGS[@]}"}; do printf 'qemu-arg=%s\n' "$a"; done
}

# _rq_class_sig: _rq_machine_sig of the lease's class -- its options
# (AXL_POOL_CLASS_ARGS, one per line) after the `--net` every pooled guest
# has, parsed by this script's own loop in a run of its own -- with nothing
# of this run's environment that shapes a machine (NIC_NO_ROM, an env knob
# too), and its complaints kept out of the caller's output (a failure is the
# reason the run then boots with).
_rq_class_sig() {
    local -a cargs=()
    [[ -n "$AXL_POOL_CLASS_ARGS" ]] && mapfile -t cargs <<< "$AXL_POOL_CLASS_ARGS"
    env -u NIC_NO_ROM _AXL_RQ_MACHINE_SIG=1 bash "${BASH_SOURCE[0]}" --arch "$ARCH" --net \
        ${cargs[@]+"${cargs[@]}"} 2> /dev/null
}

# _rq_machine_diff: the first fact in which the parsed run's machine differs
# from the lease's class's, as "the run has X, the class Y" ("nothing" past
# the end of either); nothing printed when they are the same. A run with no
# NIC option and no --bridges is the bare machine, which every class has the
# agent's NIC beside, so it compares as `--net`. Returns 2 when there is no
# class to compare with (AXL_POOL_CLASS_ARGS unset) or its options cannot be
# read. common-test.sh asks the same question of a pooled test's own QEMU
# arguments (_AXL_RQ_MACHINE_SIG=compare).
_rq_machine_diff() {
    local csig i
    local -a mine=() theirs=()
    [[ -n "${AXL_POOL_CLASS_ARGS+set}" ]] || return 2
    if ! csig=$(_rq_class_sig) || [[ -z "$csig" ]]; then return 2; fi
    mapfile -t theirs <<< "$csig"
    mapfile -t mine < <(
        [[ "$NET" != true && "$BRIDGES" != true ]] && NET=true
        _rq_machine_sig
    )
    for (( i = 0; i < ${#mine[@]} || i < ${#theirs[@]}; i++ )); do
        if [[ "${mine[i]:-}" != "${theirs[i]:-}" ]]; then
            printf 'the run has %s, the class %s\n' "${mine[i]:-nothing}" "${theirs[i]:-nothing}"
            return 0
        fi
    done
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)       ARCH="$2"; shift 2 ;;
        --timeout)    TIMEOUT="$2"; shift 2 ;;
        --raw)        RAW=true; shift ;;
        --screenshot) SCREENSHOT="$2"; shift 2 ;;
        --gpu)        ENABLE_GPU=true; shift ;;
        --no-gpu|--headless) NO_GPU=true; shift ;;
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
        --guestfwd)   GUESTFWDS+=("$2"); NET=true; shift 2 ;;
        --mem)        MEM="$2"; shift 2 ;;
        --hostfwd-bind) HOSTFWD_BIND="$2"; shift 2 ;;
        --extra)      EXTRA_FILES+=("$2"); shift 2 ;;
        --setvar)
            # NAME=VALUE, checked here rather than silently emitting `set FOO
            # FOO` for a bare `--setvar FOO`.
            [[ "$2" == *=* ]] || { echo "ERROR: --setvar wants NAME=VALUE, got '$2'" >&2; exit 1; }
            SETVARS+=("$2"); shift 2 ;;
        --sendkey)    SENDKEY_SEQ+=" $2"; shift 2 ;;
        --sendkey-after) SENDKEY_AFTER="$2"; shift 2 ;;
        --sendkey-settle) SENDKEY_SETTLE_MS="$2"; shift 2 ;;
        --screenshot-after) SCREENSHOT_AFTER="$2"; shift 2 ;;
        --screenshot-after-count) SCREENSHOT_AFTER_COUNT="$2"; shift 2 ;;
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
        --monitor-socket) MONITOR_SOCKET="$2"; shift 2 ;;
        --qmp-socket) QMP_SOCKET="$2"; shift 2 ;;
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
        --workload) WORKLOAD="${2:-}"; shift 2 ;;
        --max-duration) MAX_DURATION="${2:-}"; shift 2 ;;
        --no-cpu-warn) CPU_WARN=false; shift ;;
        --cpu-report) CPU_REPORT=true; shift ;;
        --cpu-threshold) CPU_THRESHOLD="$2"; shift 2 ;;
        --cpu-sustain) CPU_SUSTAIN="$2"; shift 2 ;;
        -i|--interactive) INTERACTIVE=true; shift ;;
        --qcow2) QCOW2=true; shift ;;
        --state-dir) STATE_DIR="$2"; shift 2 ;;
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
  --no-gpu / --headless    Give the guest NO graphics adapter: OVMF
                           exposes no GOP and axl_gfx_available() is
                           false. For logic/compute-only test runs —
                           skips the render pipeline (faster) and
                           side-steps a false CPU-spike from incidental
                           full-screen rendering. Mutually exclusive
                           with --gpu/--screenshot/--display/--gui.
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
  --bridges                Add the integration tests' standard devices: a
                           PCIe root port (00:02.0) with a virtio-rng
                           behind it, and qemu-xhci (00:03.0) with
                           usb-mouse + usb-hub + usb-tablet. So interactive
                           smoke-tests of lspci -t and lsusb -t see real
                           bridges and real hub-port chains. (The NIC stays
                           this script's: none unless --net.)
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
  --mem SIZE               Guest RAM as <N>M or <N>G (default 512M); the
                           virtio-fs memory backend follows it. For a pool
                           class whose tests need more (the unit class: its
                           FAT RAM disk comes out of guest RAM).
  --guestfwd SPEC          Forward a guest-initiated TCP connection (slirp
                           guestfwd; repeatable; implies --net), e.g.
                           `tcp:10.0.2.100:9999-cmd:cat` -- with `cmd:` a
                           program spawned per connection on its stdio, so
                           no host port or server is needed. No commas.
  --extra FILE             Stage additional .efi on disk (repeatable)
  --monitor-socket PATH    With --background: serve the QEMU HMP monitor on
                           the UNIX socket PATH for the guest's whole life
                           (e.g. `screendump` on demand) and report it as
                           MONITOR_SOCKET=PATH next to QEMU_PID=. The
                           caller owns PATH; a stale socket there is removed.
  --qmp-socket PATH        With --background: the same for QMP
                           (QMP_SOCKET=PATH), e.g. input-send-event.
  --setvar NAME=VALUE      `set` a shell variable before the app (repeatable).
                           e.g. --setvar AXL_LOG_LEVEL=debug to see debug lines
  --sendkey "K K ..."      With --screenshot: inject QEMU monitor key
                           tokens (space-separated; e.g.
                           "h i spc t h e r e", "ctrl-s", "ret") after the
                           app is up, then capture. Repeatable; auto-adds
                           a USB keyboard on aarch64.
  --sendkey-after ERE      With --sendkey: wait until the guest prints a line
                           matching ERE on serial BEFORE typing, instead of
                           typing at SHOT_WAIT. Use it whenever the host may
                           be loaded: SHOT_WAIT is a wall clock and does not
                           move when the guest boots slower, so the leading
                           keystrokes land before the app binds ConIn and the
                           app sees a truncated sequence. Have the app print a
                           marker once it is ready and gate on that. Bounded
                           by --timeout; on expiry it types anyway and warns.
  --sendkey-settle MS      With --sendkey: how long to wait after the LAST key
                           before capturing, for the app to process and
                           repaint. Default 1500. This is a wall clock and does
                           not move when the host slows down -- prefer
                           --screenshot-after, and reach for this only when the
                           guest cannot print a marker.
  --screenshot-after ERE   With --screenshot: wait until the guest prints a
                           line matching ERE AFTER the keys are injected, then
                           capture. The trailing-edge counterpart to
                           --sendkey-after: gates the capture on the guest
                           having stopped writing the framebuffer rather than
                           on a timer. Without it, `screendump` can read a
                           frame mid-write and produce a TORN capture whose
                           widgets disagree with each other.
                           THE MARKER MUST IDENTIFY THE FINAL PAINT. An app
                           that repaints after every key emits its marker at
                           key 1; that match lands past the offset and
                           satisfies this gate immediately, capturing the torn
                           frame it was meant to prevent. Either make the
                           marker self-identifying (e.g. "PAINT n=3" and match
                           that exact line) or use --screenshot-after-count.
                           Only matches output produced after injection began,
                           so a stale earlier marker cannot satisfy it.
                           Bounded by --timeout; on expiry it captures anyway
                           and warns.
  --screenshot-after-count N
                           With --screenshot-after: wait for the Nth match past
                           the injection point instead of the first, so an app
                           whose marker is dumb ("painted") still gets a correct
                           gate. Default 1. If the pattern ends up matching MORE
                           than N times the run warns, because that means the
                           capture was gated on an earlier paint.
                           PREFER A SELF-IDENTIFYING MARKER. This ordinal counts
                           PAINTS, host-side, so it needs you to know how many
                           paints an interaction causes -- and paints are
                           routinely NOT 1:1 with keys: fades, repaints issued
                           part-way through a handler, and a deliberate
                           opacity-0 frame all break it, each of which a
                           consumer hit in practice. A marker naming the INPUT
                           ("PAINT n=20") splits the problem the right way: the
                           host says which KEY to wait for, the guest decides
                           when that key has settled, and neither side has to
                           model the other's frame cadence. Use this flag when
                           you cannot change the guest.
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
  --background             Launch QEMU in background, print PID. You own
                           the guest from the QEMU_PID= line on; if this
                           script is killed before printing it, the guest
                           is taken down with it.
  --serial-log FILE        Save serial output to file (foreground:
                           ANSI-stripped clean transcript; background:
                           live raw log — symlinked).
  --serial-log-raw FILE    (foreground only) Save unprocessed serial
                           with ANSI/cursor codes. Useful for
                           firmware-level debugging.
  --serial-socket PATH     (background mode) expose serial as a UNIX
                           socket so host scripts can inject input
                           (e.g. Ctrl-C via `printf '\x03' | socat ...`)
  --workload SHAPE         Declare what shape this run is, which picks the
                           gate that applies. 'idle' (default) keeps the
                           CPU-spike check below -- right for an app that
                           should be sitting at a prompt. 'compute' turns it
                           off and REQUIRES --max-duration instead: a
                           compute-bound guest pegs its vCPU from boot to exit,
                           so the sampler is really measuring run length while
                           reporting a spike.
  --max-duration SECS      Fail (exit 9) if the run takes longer than SECS.
                           A duration signal, reported as such -- it is what
                           catches an accidental O(n^2) showing up as "the
                           suite got slower".
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
  --qcow2                  Put the OVMF vars pflash and the boot disk on
                           qcow2 instead of raw, so a QEMU snapshot
                           (savevm / snapshot-save) captures the UEFI
                           variables and the disk, a restore reverts both,
                           and a new QEMU process can restore it (QEMU
                           refuses while any writable disk cannot hold a
                           snapshot) -- what a pooled guest needs.
  --state-dir DIR          With --qcow2: create the qcow2 vars and disk in
                           DIR (made if missing) instead of the run's temp
                           directory, and LEAVE them there when the run
                           ends -- a later QEMU restores a snapshot from
                           them (the guest pool's cold start).
  --hostfwd-bind ADDR      The IPv4 address every --hostfwd host port
                           listens on (default: every interface). The guest
                           pool binds its agents to 127.0.0.1: an agent runs
                           whatever command it is sent.
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

Environment:
  AXL_SHELL_LAUNCHER       Boot the Shell through the AXL launcher, which
                           chains it with "-delay 0" and skips its 5 s
                           startup countdown (~4.6 s per boot).
                             unset  auto (default): use it, but if the Shell
                                    is never reached, record that against
                                    this firmware and retry without it. The
                                    verdict is cached, so the retry is paid
                                    once, not every run.
                             0      never use it (bisect a launcher fault)
                             1      always use it, ignoring the cache, with
                                    NO fallback -- a failure stays loud
  AXL_SHELL_LAUNCHER_BIN   Stage this binary as the launcher instead of
                           building one. Must be a PE image.
  AXL_QEMU_TMPDIR          Base directory for per-run scratch (default
                           /dev/shm when usable). --screenshot and --mount
                           put UNIX sockets under it, and UNIX socket paths
                           are capped at 107 bytes (sun_path is 108 with the
                           NUL): keep this base to 75 bytes or less. A longer
                           one is refused before QEMU starts. The
                           --monitor-socket, --qmp-socket and --serial-socket
                           paths have the same 107-byte cap.

  Cached launcher verdicts live in
  ${XDG_CACHE_HOME:-~/.cache}/axl-sdk/shell-launcher-blocklist/ -- delete
  a file there to give that firmware another try.
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

# The machine questions, asked of the parsed options and nothing else done:
# =1 prints the machine (_rq_class_sig); =compare prints how it differs from
# the lease's class and exits 0 (the same), 3 (differs) or 2 (no class to
# compare with) -- common-test.sh's check of a pooled test's own arguments.
# (Not 1: this script's own refusals exit 1.)
case "${_AXL_RQ_MACHINE_SIG:-}" in
    1) _rq_machine_sig; exit 0 ;;
    compare)
        _rq_diff=$(_rq_machine_diff) || exit 2
        [[ -z "$_rq_diff" ]] && exit 0
        printf '%s\n' "$_rq_diff"
        exit 3 ;;
esac

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

# --monitor-socket / --qmp-socket: only --background hands the guest's life
# to the caller; every other mode owns (or has no) monitor of its own.
if [[ -n "$MONITOR_SOCKET$QMP_SOCKET" && "$BACKGROUND" != "true" ]]; then
    echo "ERROR: --monitor-socket/--qmp-socket need --background" >&2
    exit 1
fi

# The caller's own sockets, named by the option that set them. See
# axl_sock_path_check: over the limit QEMU refuses the socket, and the trace
# is one line in a log nobody is reading.
[[ -z "$MONITOR_SOCKET" ]] || axl_sock_path_check --monitor-socket "$MONITOR_SOCKET" || exit 1
[[ -z "$QMP_SOCKET" ]]     || axl_sock_path_check --qmp-socket "$QMP_SOCKET"         || exit 1
[[ -z "$SERIAL_SOCKET" ]]  || axl_sock_path_check --serial-socket "$SERIAL_SOCKET"   || exit 1

if [[ -n "$HOSTFWD_BIND" && ! "$HOSTFWD_BIND" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]]; then
    echo "ERROR: --hostfwd-bind needs an IPv4 address, got '$HOSTFWD_BIND'" >&2
    exit 1
fi
if [[ -n "$STATE_DIR" && "$QCOW2" != true ]]; then
    echo "ERROR: --state-dir needs --qcow2: the state it keeps is the qcow2 vars and disk a snapshot lives in" >&2
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

# --no-gpu: a GOP-less guest cannot also drive a framebuffer. Reject the
# adapter-requiring modes, mirroring the --interactive/--display guards.
# Checked on the RAW flags (before the --display block below, so its own
# X11/$DISPLAY validation can't preempt this with a less-relevant error):
# --gpu / --display / --gui need a framebuffer; --screenshot has none to read.
if [[ "$NO_GPU" == "true" ]]; then
    if [[ "$ENABLE_GPU" == "true" || -n "$DISPLAY_BACKEND" ]]; then
        echo "ERROR: --no-gpu cannot be combined with --gpu/--display/--gui (they need a framebuffer)" >&2
        exit 1
    fi
    if [[ -n "$SCREENSHOT" ]]; then
        echo "ERROR: --no-gpu cannot be combined with --screenshot (nothing to capture without a GOP)" >&2
        exit 1
    fi
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

# CPU-spike sampler + monitor lifecycle live in scripts/axl-common.sh so
# common-test.sh (a sibling that builds its own QEMU command and never calls this
# script) gets the same check. cpu_policy_init picks the KVM/TCG-aware threshold +
# warm-up + warn carve-out (one source of truth, shared with common-test.sh);
# --cpu-threshold / --no-cpu-warn parsed above still override it.
# --workload declares the SHAPE of this run, which decides WHICH gate applies.
# `compute` must never mean "no gate": the duration signal is the only thing
# the sampler could legitimately see for a compute-bound guest, and it is what
# catches an accidental O(n^2) surfacing as "the suite got slower". So it
# requires an explicit budget rather than defaulting to one that would be wrong
# for every caller.
case "$WORKLOAD" in
    idle) ;;
    compute)
        if [[ -z "$MAX_DURATION" ]]; then
            echo "ERROR: --workload compute requires --max-duration SECS" >&2
            echo "       (it turns the CPU-sustain check off, so the budget is" >&2
            echo "        what keeps the run gated at all)" >&2
            exit 2
        fi
        CPU_WARN=false   # measuring duration instead -- see duration_summary
        ;;
    *)
        echo "ERROR: --workload must be 'idle' or 'compute' (got '$WORKLOAD')" >&2
        exit 2
        ;;
esac

cpu_policy_init "$ARCH"

# While a guest runs, a kill of this script must take the guest with it:
# prepend a `kill` of its PID to the EXIT trap, and drop it again once the PID
# is reaped -- a reaped PID can be recycled to an unrelated process.
#
# The guard goes up AROUND the launch, not after it:
#
#   _rq_guard_begin
#   ( timeout ... ) &          # the guest
#   WRAPPER_PID=$!
#   _rq_tm_watch "$WRAPPER_PID" "$LOG"   # inside, so its pid is recorded
#   _rq_guard_guest "$WRAPPER_PID"
#
# A bash trap runs between any two commands, so a guard armed after the `&`
# left an instant -- the `$!` assignment, the timing watcher's start, the
# guard's own capture of the EXIT trap -- where a signal to this script ran
# the cleanup WITHOUT the kill. A booted guest's `timeout` keeps it in a
# process group of its own, so nothing else reached it and it ran on to
# --timeout; a --screenshot guest shares this script's group, so only a
# signal to the script alone missed it -- and it has no watchdog, so it ran
# for ever. _rq_guard_begin captures the EXIT trap while no guest exists yet,
# then has INT and TERM only RECORDED until _rq_guard_guest has the kill in
# place; that re-arms this script's handlers (axl_trap_cleanup's, INT and
# TERM) and acts on a recorded signal at once. Inside the bracket nothing
# runs that a signal could cut short (the watcher's fork is a background one;
# losing it loses a timing reading, not the guard), and the watcher's pid is
# known before any cleanup can need it -- lib/fwd-signal.sh's
# record-then-check, for the launch here.
_rq_exit_prev=""
_rq_guard_sig=""
_rq_guard_begin() {
    _rq_exit_prev=$(axl_exit_trap_cmd)
    _rq_guard_sig=""
    trap '_rq_guard_sig=${_rq_guard_sig:-INT}' INT
    trap '_rq_guard_sig=${_rq_guard_sig:-TERM}' TERM
}
_rq_guard_guest() {
    # shellcheck disable=SC2064  # the PID is meant to expand NOW
    trap "kill -TERM $1 2>/dev/null; _rq_tm_end unfinished; $_rq_exit_prev" EXIT
    axl_exit_on_signal INT TERM
    [[ -z "$_rq_guard_sig" ]] || axl_die_by_signal "$_rq_guard_sig"
}
_rq_unguard_guest() {
    if [[ -n "$_rq_exit_prev" ]]; then trap "$_rq_exit_prev" EXIT; else trap - EXIT; fi
}

# One `TIMING: run-qemu` line per guest, always (the format is defined in the
# "Wall-time accounting" block of axl-common.sh). It goes to
# $AXL_TEST_TIMING_LOG -- the test's own log under the runner -- or to a
# terminal, NEVER to a stderr that is not one: 46 tests capture this script's
# stdout+stderr in a variable and parse it. _rq_tm_begin MODE stamps the
# launch; _rq_tm_watch PID LOG starts the boot reading on LOG (the serial
# file); _rq_tm_end RC prints the line, once (a killed run prints
# rc=unfinished from the guard trap above).
_rq_tm_start=""
_rq_tm_mode=""
_rq_tm_wpid=""
_rq_tm_begin() {
    _rq_tm_mode="$1"
    axl_timing_now _rq_tm_start
}
_rq_tm_watch() {
    # noend: this script reads only the boot stamp, so the watcher writes
    # nothing after the guest is gone. Its pid is left in TMPDIR as well, for
    # the --background watchdog subshell, which forked before it existed.
    axl_timing_watch "$1" "$2" "$TMPDIR/.timing" noend
    _rq_tm_wpid="$AXL_TIMING_WATCH_PID"
    printf '%s\n' "$_rq_tm_wpid" > "$TMPDIR/.timing.wpid" 2>/dev/null || true
}
_rq_tm_end() {
    [[ -n "$_rq_tm_start" ]] || return 0
    local now b="" g bs="-"
    axl_timing_now now
    # The watcher stopped and gone before its stamp is read, and before the
    # caller's cleanup removes TMPDIR, which it writes into.
    # (Not a pooled run: it boots nothing, starts no watcher, owns no TMPDIR.)
    if [[ "$_rq_tm_mode" != pooled ]]; then
        [[ -n "$_rq_tm_wpid" ]] || axl_timing_read _rq_tm_wpid "$TMPDIR/.timing.wpid"
        axl_timing_unwatch "$_rq_tm_wpid"
        axl_timing_read b "$TMPDIR/.timing.boot"
    fi
    axl_timing_secs g $(( now - _rq_tm_start ))
    if [[ -n "$b" ]]; then
        (( b > now )) && b=$now
        axl_timing_secs bs $(( b - _rq_tm_start ))
    fi
    axl_timing_emit "TIMING: run-qemu mode=$_rq_tm_mode guest=$g boot=$bs${_rq_tm_extra:+ $_rq_tm_extra} rc=$1"
    _rq_tm_start=""
}
_rq_tm_extra=""   # pooled: `restore=<s>`, between boot= and rc=

# _rq_staged_inputs -- the tree files _rq_stage_files and --nsh hand the guest
# on EITHER path, one per line: the skip cache's inputs both launches record
# (with their own image, firmware or agent). One copy, because two hand-kept
# lists drifted: the booted one lacked --extra and --nsh.
_rq_staged_inputs() {
    local pci; pci="$(dirname "$0")/../share/pci-ids.json5"
    [[ -f "$pci" ]] && printf '%s\n' "$pci"
    local extra
    for extra in ${EXTRA_FILES[@]+"${EXTRA_FILES[@]}"}; do printf '%s\n' "${extra%%:*}"; done
    [[ -n "$CUSTOM_NSH" ]] && printf '%s\n' "$CUSTOM_NSH"
    return 0
}

# _rq_stage_files DIR: what every run stages beside its app -- the PCI
# vendor/device name sidecar, so axl_pci_ids_load finds it via the standard
# companion-path resolver (tiny, ~3 KB, and non-PCI tools ignore it; staged
# unconditionally to avoid the "lspci shows no names because I forgot
# --bridges" footgun), and the --extra files. Each --extra is "PATH" (lands
# at DIR's root) or "PATH:DEST", DEST a relative target path (intermediate
# dirs created) -- e.g. `drivers/x64/<name>.efi` for axl_driver_locate. A
# booted run stages into its disk image, a pooled one into its directory in
# the lease's share.
_rq_stage_files() {
    local dir="$1" pci extra extra_src extra_dst
    pci="$(dirname "$0")/../share/pci-ids.json5"
    if [[ -f "$pci" ]]; then
        cp "$pci" "$dir/pci-ids.json5"
    fi
    for extra in ${EXTRA_FILES[@]+"${EXTRA_FILES[@]}"}; do
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
        mkdir -p "$dir/$(dirname "$extra_dst")"
        cp "$extra_src" "$dir/$extra_dst"
    done
}

# _rq_print_output CLEAN: what a foreground run prints of its ANSI-stripped
# console log -- all of it with --raw, else the app's output: from the start
# sentinel (or, if the launcher fell back to booting the Shell directly, the
# "...to continue." countdown prompt) down to the "Reset with" trailer, the
# anchor lines themselves stripped. One copy: a pooled run's output is a
# booted run's, byte for byte.
_rq_print_output() {
    if [[ "$RAW" == "true" ]]; then
        cat "$1"
    else
        sed -n "/$APP_OUTPUT_SENTINEL\|to continue\./,/^Reset with/p" "$1" | \
            grep -vF "$APP_OUTPUT_SENTINEL" | \
            grep -v "to continue\." | \
            grep -v "^Reset with"
    fi
}

# _rq_fw_debug_path DIR: where a run keeps the firmware's DEBUG output (an
# AARCH64 guest's debug UART, a kept X64 guest's debugcon -- FW_DEBUG_LOG, and
# qemu_fw_debug_uart in axl-common.sh), and where a pooled run copies its part
# of the leased guest's (beside --serial-log: pooled mode boots for
# --debugcon instead, since gdb-syms.py needs the boot's load lines a lease's part lacks): the
# --debugcon file when one is named (X64's option, the same capture); a guest kept in
# --state-dir DIR keeps it there (a pooled guest's, which a lease reads beside
# its console); beside --serial-log as <name>-firmware-debug.log; else in DIR,
# the run's temp dir.
_rq_fw_debug_path() {
    if [[ -n "$DEBUGCON_LOG" ]]; then
        echo "$DEBUGCON_LOG"
    elif [[ -z "$STATE_DIR" && -n "$SERIAL_LOG" ]]; then
        echo "${SERIAL_LOG%.log}-firmware-debug.log"
    else
        echo "$1/firmware-debug.log"
    fi
}

# _rq_print_fw_debug FILE CLEAN: after a run's output, what the
# firmware's DEBUG channel (FILE, this run's part) said, each line as
# "[firmware-debug] <line>" -- when it matters: it holds an ASSERT, or the
# console (CLEAN) shows a firmware exception, whose header alone is the
# console's (the AArch64 handler prints its registers and backtrace as DEBUG).
# Before AARCH64 had a debug UART all of it was the console's, and so part of
# what this script printed; a caller reading that for an ASSERT or a dump
# still finds one. A clean run prints nothing of it.
_rq_print_fw_debug() {
    local slice
    [[ -s "$1" ]] || return 0
    slice=$(tr -d '\r' < "$1")
    if grep -aq 'ASSERT' <<< "$slice" || grep -aqE "$AXL_FW_EXCEPTION_RE" "$2" 2>/dev/null; then
        sed 's/^/[firmware-debug] /' <<< "$slice"
    fi
}

# Emit the caller's `--setvar NAME=VALUE` pairs as shell `set` commands. One
# definition used by every startup script (booted app, booted --nsh, pooled),
# so a --setvar cannot work in one and silently do nothing in another.
emit_setvars() {
    local sv
    for sv in ${SETVARS[@]+"${SETVARS[@]}"}; do
        echo "set ${sv%%=*} ${sv#*=}"
    done
}

# ---------------------------------------------------------------------------
# Pooled mode -- the header's "Pooled mode" is the contract (design §6.9).
# Decided before anything boot-specific runs (QEMU, firmware, the Shell's
# extraction): a pooled run needs none of it.
# ---------------------------------------------------------------------------
_rq_pool_why=""     # why a run inside a lease boots ("" = it pools)
_rq_pool_fail=""    # a pooled attempt's failure, which then boots
_rq_nsh_kept=()     # the --nsh lines a pooled run executes (boilerplate dropped)
_RQ_NO_APP="AXL-POOL-NO-APP"   # the pooled script's probe: the app was not delivered
_rq_nsh_enters=1    # the run enters its directory (a --nsh without `fs0:` does not)

# _rq_nsh_word LINE: LINE trimmed and lowercased, as a Shell command compares.
_rq_nsh_word() {
    local w="${1#"${1%%[![:space:]]*}"}"
    w="${w%"${w##*[![:space:]]}"}"
    printf '%s' "${w,,}"
}

# _rq_nsh_check FILE: sets _rq_pool_why when FILE cannot run pooled, else
# _rq_nsh_kept. Line numbers in a reason are FILE's own (1-based).
_rq_nsh_check() {
    local -a lines
    local -A drop=()
    local i n first=-1 last=-1 w nxt
    mapfile -t lines < "$1"
    lines=("${lines[@]%$'\r'}")
    n=${#lines[@]}
    for (( i = 0; i < n; i++ )); do
        [[ -n "$(_rq_nsh_word "${lines[i]}")" ]] || continue
        (( first < 0 )) && first=$i
        last=$i
    done
    if (( first < 0 )) || [[ "$(_rq_nsh_word "${lines[first]}")" != "@echo -off" ]]; then
        _rq_pool_why="its --nsh does not start with @echo -off (a boot echoes each command)"
        return
    fi
    drop[$first]=1
    # The boilerplate after it: `fs0:`, then `cd \` -- each on the next
    # non-blank line, in that order. `fs0:` is what gives a boot's script a
    # current directory on its disk; without it a relative name finds
    # nothing, so a pooled run then does not enter its directory either.
    _rq_nsh_enters=0
    nxt=$first
    for w in "fs0:" "cd \\"; do
        for (( i = nxt + 1; i < n; i++ )); do
            [[ -n "$(_rq_nsh_word "${lines[i]}")" ]] && break
        done
        if (( i >= n )) || [[ "$(_rq_nsh_word "${lines[i]}")" != "$w" ]]; then
            break
        fi
        [[ "$w" == "fs0:" ]] && _rq_nsh_enters=1
        drop[$i]=1
        nxt=$i
    done
    if (( last > first )) && [[ -z "${drop[$last]:-}" && "$(_rq_nsh_word "${lines[last]}")" == "reset -s" ]]; then
        drop[$last]=1
    fi
    _rq_nsh_kept=()
    for (( i = 0; i < n; i++ )); do
        [[ -n "${drop[$i]:-}" ]] && continue
        _rq_nsh_kept+=("${lines[i]}")
        w=$(_rq_nsh_word "${lines[i]}")
        [[ "$w" == "#"* ]] && continue
        if [[ "$w" =~ ^(reset|exit)([[:space:]]|$) ]]; then
            _rq_pool_why="its --nsh runs ${BASH_REMATCH[1]} (line $(( i + 1 )))"
            return
        fi
        if [[ "$w" == *fs0:* ]]; then
            _rq_pool_why="its --nsh names fs0: (line $(( i + 1 )))"
            return
        fi
        if [[ "$w" =~ ^cd[[:space:]]+\\ ]]; then
            _rq_pool_why="its --nsh changes to a volume's root (line $(( i + 1 )))"
            return
        fi
    done
}

# _rq_pool_decide: _rq_pool_why = why this run, inside a lease, boots; empty
# when its machine is the lease's class and nothing else stands in the way.
_rq_pool_decide() {
    local opt="" diff
    _rq_pool_why=""
    if [[ "${AXL_RUN_QEMU_POOL:-}" == 0 ]]; then _rq_pool_why="AXL_RUN_QEMU_POOL=0"; return; fi
    if [[ "${QEMU_DRYRUN:-0}" == 1 ]]; then _rq_pool_why="QEMU_DRYRUN (it launches nothing)"; return; fi
    if [[ -z "$EFI_FILE" ]]; then _rq_pool_why="no app to run"; return; fi
    if [[ "${AXL_POOL_BG_LIVE:-}" == 1 ]]; then
        # common-test.sh's pooled background run: its server lives on the
        # leased guest, and a restore would end it.
        _rq_pool_why="the test's own background run holds the leased guest"; return
    fi
    if [[ "$ARCH" != "${AXL_POOL_ARCH:-}" ]]; then
        _rq_pool_why="--arch $ARCH; the lease is ${AXL_POOL_ARCH:-of no arch}"
        return
    fi
    # The way of running a pooled guest cannot give. First wins.
    if   [[ -n "$SCREENSHOT" ]];        then opt=--screenshot
    elif [[ -n "$SENDKEY_SEQ$SENDMOUSE_SEQ$HOLDKEY_SEQ" ]]; then opt="--sendkey/--sendmouse/--holdkey"
    elif [[ -n "$DISPLAY_BACKEND" ]];   then opt="--display/--gui/--vnc"
    elif [[ "$INTERACTIVE" == true ]];  then opt=--interactive
    elif [[ -n "$MOUNT_DIR" ]];         then opt=--mount
    elif [[ "$BACKGROUND" == true ]];   then opt=--background
    elif [[ -n "$DEBUGCON_LOG" ]];      then opt=--debugcon
    elif [[ -n "$GDB_PORT" ]];          then opt=--gdb
    elif [[ -n "$SERIAL_SOCKET" ]];     then opt=--serial-socket
    elif [[ -n "$MONITOR_SOCKET$QMP_SOCKET" ]]; then opt="--monitor-socket/--qmp-socket"
    elif [[ "$REBOOT" == true ]];       then opt=--reboot
    elif [[ "$BOOT_TARGET" == true ]];  then opt=--boot-target
    elif [[ -n "${SHELL_OVERRIDE:-}" ]]; then opt=--shell
    elif [[ "$QCOW2" == true || -n "$STATE_DIR" ]]; then opt="--qcow2/--state-dir"
    elif [[ -n "$HOSTFWD_BIND" ]];      then opt=--hostfwd-bind
    elif [[ -n "$TAP_IFACE" ]];         then opt=--tap
    elif (( ${#HOSTFWDS[@]} > 0 ));     then opt=--hostfwd
    fi
    if [[ -n "$opt" ]]; then _rq_pool_why="$opt: not done on a pooled guest"; return; fi
    # The machine: the lease's class's, fact for fact (_rq_machine_diff).
    if [[ -z "${AXL_POOL_CLASS_ARGS+set}" ]]; then
        _rq_pool_why="the lease names no machine (no AXL_POOL_CLASS_ARGS)"; return
    fi
    if ! diff=$(_rq_machine_diff); then
        _rq_pool_why="run-qemu.sh could not read the ${AXL_POOL_CLASS:-?} class's options"; return
    fi
    if [[ -n "$diff" ]]; then
        _rq_pool_why="the machine is not the ${AXL_POOL_CLASS:-?} class's: $diff"; return
    fi
    # Interpolated into Shell lines: only what a lease directory can be.
    if ! [[ "${AXL_POOL_GUEST_DIR:-}" =~ ^[A-Za-z0-9]+:\\[A-Za-z0-9._-]+$ ]]; then
        _rq_pool_why="an unexpected AXL_POOL_GUEST_DIR [${AXL_POOL_GUEST_DIR:-}]"; return
    fi
    if [[ -n "$CUSTOM_NSH" ]]; then
        if [[ ! -f "$CUSTOM_NSH" ]]; then _rq_pool_why="its --nsh is not a file"; return; fi
        _rq_nsh_check "$CUSTOM_NSH"
    fi
}

# The axlpool command line, run to completion (AXL_POOL_CLI replaces it: a
# test's stub).
_rq_axlpool() {
    if [[ -n "${AXL_POOL_CLI:-}" ]]; then
        "$AXL_POOL_CLI" "$@"
    else
        (cd "$(dirname "$0")/../test/pool" && exec python3 -m axlpool "$@")
    fi
}

# _rq_record_inputs FILE...: the skip cache's inputs for this test (the
# booted path records its own; lib/test-cache.sh). Through test-cache.sh's own
# cache_record_input, never a hand-built file name: this once wrote
# "<test>.inputs" itself, which missed the pooled "@pool" file and then the
# per-arch "@<arch>" one -- recording into a file nothing reads, so the test
# could never be skipped.
_rq_record_inputs() {
    local lib f
    lib="$(dirname "${BASH_SOURCE[0]}")/../test/integration/lib/test-cache.sh"
    [[ -n "${AXL_TEST_CACHE:-}" && -d "${AXL_TEST_CACHE:-/nonexistent}" \
       && -n "${AXL_TEST_PROFILE_NAME:-}" && -f "$lib" ]] || return 0
    # shellcheck source=../test/integration/lib/test-cache.sh
    source "$lib"
    for f in "$@"; do
        [[ -n "$f" && -f "$f" ]] || continue
        cache_record_input "$AXL_TEST_PROFILE_NAME" "$f"
    done
}

# _rq_clean_log RAW CLEAN: a serial log with ANSI/DEC escape sequences and
# carriage returns stripped. The param byte class is the full CSI parameter
# range (ECMA-48 0x30-0x3F) so DEC private modes like ESC[=3h and ESC[?25l
# strip cleanly, not just numeric/semicolon CSI like ESC[2J; standalone ESC(
# / ESC) charset designators, which UEFI consoles emit on init, go too.
_rq_clean_log() {
    axl_strip_escapes "$1" | tr -d '\r' > "$2"
}

# _rq_run_pooled: the run on the leased guest. On success it prints what a
# booted run prints and EXITS; on a pool failure it returns 0 with the reason
# in _rq_pool_fail, having printed nothing. (Never a non-zero return: it is
# called as a plain command so that `set -e` holds inside it, as it does for
# the booted path -- `f || ...` would turn errexit off for its whole body.)
_rq_sentinel=""
_rq_run_pooled() {
    local vol dir host d k n=0 call gcall rc=0 out t0 t1 off size prev stable epid fwlog fwoff
    local restore_s warmup="$CPU_WARMUP"
    vol="${AXL_POOL_GUEST_DIR%%:*}:"
    dir="${AXL_POOL_GUEST_DIR#*:}"
    host="$AXL_POOL_HOST_DIR"
    # A directory of its own: the next number, the earlier runs' removed.
    for d in "$host"/rq-*; do
        k="${d##*/rq-}"
        [[ -d "$d" && "$k" =~ ^[0-9]+$ ]] && (( k > n )) && n=$k
        rm -rf "$d"
    done
    n=$(( n + 1 ))
    call="$host/rq-$n"
    gcall="$dir\\rq-$n"
    mkdir -p "$call" || { _rq_pool_fail="cannot make $call"; return 0; }
    cp "$EFI_FILE" "$call/$EFI_NAME"
    _rq_stage_files "$call"
    # The Shell looks a bare `tool` up as tool.EFI -- the extension upper
    # case. FAT matches it; the share (virtio-fs over a Linux directory) is
    # case-sensitive and does not. So each staged .efi is reachable by that
    # name too, as on a boot disk: a COPY -- a hard link (one inode, two
    # names) makes the guest's VirtioFsDxe fail to run either name
    # (measured: the agent answers not-run for `tool.efi` too).
    while IFS= read -r -d '' d; do
        cp -f "$d" "${d%.efi}.EFI"
    done < <(find "$call" -type f -name '*.efi' -print0)
    {
        echo "@echo -off"
        if [[ "$_rq_nsh_enters" == 1 ]]; then
            echo "$vol"
            echo "cd $gcall"
        fi
        # A boot's Shell searches its disk's root (FS0:\efi\tools\;
        # FS0:\efi\boot\;FS0:\), so a bare `tool` finds tool.efi there; the
        # leased Shell's path names its own volumes. This run's directory
        # stands in for that root.
        echo "set -v path $vol$gcall\\efi\\tools\\;$vol$gcall\\efi\\boot\\;$vol$gcall\\"
        emit_setvars
        # The `@cd .` below hides the status a share that did not deliver the
        # app would leave (0xE, which the agent reported as not-run and this
        # mode then booted), so a run that names an app -- a --nsh run too --
        # probes for it and says so: its marker is a pool failure. By full
        # path (a --nsh run need not enter its directory), and before the
        # sentinel, so the caller's output never holds it.
        if [[ -n "$EFI_NAME" ]]; then
            echo "if not exist $vol$gcall\\$EFI_NAME then"
            echo "  echo $_RQ_NO_APP"
            echo "endif"
        fi
        echo "echo $APP_OUTPUT_SENTINEL"
        if [[ -n "$CUSTOM_NSH" ]]; then
            printf '%s\n' ${_rq_nsh_kept[@]+"${_rq_nsh_kept[@]}"}
        elif [[ "$IS_DRIVER" == true ]]; then
            echo "load $EFI_NAME"
        elif [[ ${#EFI_ARGS[@]} -gt 0 ]]; then
            echo "$EFI_NAME ${EFI_ARGS[*]}"
        else
            echo "$EFI_NAME"
        fi
        # Last, `@cd .` -- the one silent command that sets %lasterror% to 0.
        # The agent reads a script whose last status is 0xE as "not run" (it
        # cannot tell that from a script never found: axl-shell.h, the 0xE
        # row), so a run whose last command was a missing-file `type`, or an
        # app returning 14, booted instead. A boot discards the status too.
        # The `@`: a kept `echo -on` line must not make the Shell echo it.
        echo "@cd ."
    } > "$call/run-qemu.nsh"

    # Global: the EXIT trap reads it after this function has returned.
    _rq_ptmp=$(mktemp -d -t axl-rqpool.XXXXXXXX)
    local ptmp="$_rq_ptmp"
    # The sentinel goes with the temp dir on ANY exit: a signal mid-run would
    # otherwise leave it, and the sampler polling a guest that lives on.
    axl_trap_cleanup '[[ -n "$_rq_sentinel" ]] && kill "$_rq_sentinel" 2>/dev/null; rm -rf "$_rq_ptmp"'

    # A fresh machine, as a boot gives (the restore drops what an earlier
    # run, or the test itself, left in the guest).
    _rq_tm_begin pooled
    axl_timing_now t0
    out=$(_rq_axlpool restore 2>&1) || rc=$?
    if (( rc != 0 )); then
        _rq_pool_fail="the pool could not restore the guest (exit $rc: ${out%%$'\n'*})"
        _rq_tm_start=""
        _rq_pool_end
        return 0
    fi
    axl_timing_now t1
    axl_timing_secs restore_s $(( t1 - t0 ))

    off=$(stat -c %s "$AXL_POOL_SERIAL" 2>/dev/null || echo 0)
    # The guest's firmware DEBUG output (an AARCH64 guest's debug UART, an
    # X64 guest's debugcon): its log, beside the console's in the guest's
    # state dir, from here on.
    fwlog="$(dirname "$AXL_POOL_SERIAL")/firmware-debug.log"
    if [[ ! -f "$fwlog" ]]; then
        # Nothing to read an ASSERT from: a boot, not a blind run.
        _rq_pool_fail="the leased $ARCH guest has no firmware debug log ($fwlog)"
        _rq_tm_start=""
        _rq_pool_end
        return 0
    fi
    fwoff=$(stat -c %s "$fwlog" 2>/dev/null || echo 0)
    # The CPU check samples the leased guest's QEMU, for this run only (the
    # guest outlives it: the sentinel ends the sampling). No boot to skip.
    sleep 3600 &
    _rq_sentinel=$!
    # No boot to skip -- for this run only: a pool failure below boots, and
    # that boot's warm-up must be the real one.
    CPU_WARMUP=0
    if [[ "${AXL_POOL_QEMU_PID:-}" =~ ^[1-9][0-9]*$ ]]; then
        cpu_monitor_start "$AXL_POOL_QEMU_PID" "$ptmp" "$_rq_sentinel"
    fi
    CPU_WARMUP="$warmup"
    RUN_START_MS=$(now_ms)
    # Started as the client process itself -- not through _rq_axlpool, whose
    # function body would be a shell between $! and the client, so the
    # guard's TERM would leave the client running.
    local -a ex=(exec --timeout "$TIMEOUT" --console -- "$vol$gcall\\run-qemu.nsh")
    _rq_guard_begin
    if [[ -n "${AXL_POOL_CLI:-}" ]]; then
        "$AXL_POOL_CLI" "${ex[@]}" > "$ptmp/exec.out" 2>&1 &
    else
        (cd "$(dirname "$0")/../test/pool" && exec python3 -m axlpool "${ex[@]}") \
            > "$ptmp/exec.out" 2>&1 &
    fi
    epid=$!
    _rq_guard_guest "$epid"
    rc=0
    wait "$epid" || rc=$?
    _rq_unguard_guest
    kill "$_rq_sentinel" 2>/dev/null || true
    wait "$_rq_sentinel" 2>/dev/null || true
    _rq_sentinel=""
    # The console's last bytes reach the file: until it stops growing.
    prev=-1 stable=0
    for (( k = 0; k < 40 && stable < 2; k++ )); do
        size=$(stat -c %s "$AXL_POOL_SERIAL" 2>/dev/null || echo 0)
        if (( size == prev )); then stable=$(( stable + 1 )); else stable=0; fi
        prev=$size
        sleep 0.05
    done
    tail -c "+$(( off + 1 ))" "$AXL_POOL_SERIAL" > "$ptmp/serial.log" 2>/dev/null || true
    _rq_clean_log "$ptmp/serial.log" "$ptmp/clean.log"
    tail -c "+$(( fwoff + 1 ))" "$fwlog" > "$ptmp/firmware-debug.log" 2>/dev/null || true

    case "$rc" in
        0|1|124) ;;   # it ran (its status is not this script's), or --timeout cut it off
        *)
            out=$(head -1 "$ptmp/exec.out")
            _rq_pool_fail="the agent could not run the script (exit $rc: ${out:-no message})"
            ;;
    esac
    # The escape-stripped log, up to the sentinel: a terminal escape before
    # the line cannot hide it, and the app printing the same words after the
    # sentinel is the app's output, not the probe's.
    if [[ -z "$_rq_pool_fail" ]] \
            && sed "/$APP_OUTPUT_SENTINEL/q" "$ptmp/clean.log" | grep -cxF "$_RQ_NO_APP" > /dev/null; then
        _rq_pool_fail="the guest did not find $EFI_NAME in its run directory"
    fi
    if [[ -z "$_rq_pool_fail" ]] && ! grep -qF "$APP_OUTPUT_SENTINEL" "$ptmp/clean.log"; then
        _rq_pool_fail="no sentinel on the guest's console"
    fi
    if [[ -n "$_rq_pool_fail" ]]; then
        cpu_monitor_finish > /dev/null 2>&1 || true
        _rq_tm_start=""
        _rq_pool_end
        return 0
    fi
    _rq_tm_extra="restore=$restore_s"
    _rq_tm_end "$rc"
    # What ran is this test's input, as a booted run's is -- plus what ran it,
    # the agent image. The pool's code is in every key already, as harness
    # (test/integration/lib/test-cache.sh).
    local -a _rq_staged; mapfile -t _rq_staged < <(_rq_staged_inputs)
    _rq_record_inputs "$EFI_FILE" ${_rq_staged[@]+"${_rq_staged[@]}"} "${AXL_POOL_AGENT_IMAGE:-}"

    if [[ -n "$SERIAL_LOG" ]]; then cp "$ptmp/clean.log" "$SERIAL_LOG"; fi
    if [[ -n "$SERIAL_LOG_RAW" ]]; then cp "$ptmp/serial.log" "$SERIAL_LOG_RAW"; fi
    # This run's part of the firmware's DEBUG output, where a booted run
    # keeps it (beside --serial-log).
    # Best effort, as the booted path's fallback: beside a --serial-log with
    # nowhere writable next to it (/dev/null) there is no place to keep it.
    if [[ -n "$SERIAL_LOG" ]]; then
        cp "$ptmp/firmware-debug.log" "$(_rq_fw_debug_path "$ptmp")" 2>/dev/null || true
    fi
    _rq_print_output "$ptmp/clean.log"
    _rq_print_fw_debug "$ptmp/firmware-debug.log" "$ptmp/clean.log"

    local cpu_spike_rc=0
    CPU_WARMUP=0          # what this run's sampler used (the report says it)
    cpu_monitor_finish || cpu_spike_rc=$?
    if [[ "$cpu_spike_rc" == "0" ]]; then
        duration_summary $(( $(now_ms) - RUN_START_MS )) || cpu_spike_rc=$?
    fi
    _rq_pool_end
    exit "$cpu_spike_rc"
}
# The pooled attempt's temp dir gone, its cleanup trap with it (a booted run
# after a pool failure sets its own).
_rq_pool_end() {
    rm -rf "$_rq_ptmp"
    trap - EXIT
}

if [[ -n "${AXL_POOL_AGENT:-}" && "${AXL_SHELL_LAUNCHER_RETRIED:-0}" != 1 ]]; then
    _rq_pool_decide
    if [[ -z "$_rq_pool_why" ]]; then
        _rq_run_pooled                 # exits when it ran; back here on a pool failure
        _rq_pool_why="$_rq_pool_fail"
    fi
    axl_timing_emit "run-qemu: booted inside a ${AXL_POOL_CLASS:-?} lease: $_rq_pool_why"
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

# The per-run sockets this run will actually make, checked BEFORE mktemp so a
# refusal leaves nothing behind. The dir name is a fixed-length template and
# mktemp drops a trailing slash from the base, so the length is known now.
# With no usable base, mktemp -d uses $TMPDIR (still the caller's here), so
# that is the knob to name.
_rq_socks=()
if [[ -n "$SCREENSHOT" ]]; then
    _rq_socks+=(monitor.sock)
    if [[ -n "$SENDMOUSE_SEQ$HOLDKEY_SEQ" ]]; then _rq_socks+=(qmp.sock); fi
fi
if [[ -n "$MOUNT_DIR" ]]; then _rq_socks+=(virtiofs.sock); fi
if [[ ${#_rq_socks[@]} -gt 0 ]]; then
    if [[ -n "$_axl_tmp_base" ]]; then
        _rq_knob=AXL_QEMU_TMPDIR
        _rq_dir=$(axl_mktemp_dir_template "$_axl_tmp_base" axl-qemu.XXXXXXXX)
    else
        _rq_knob=TMPDIR
        _rq_dir=$(axl_mktemp_dir_template "${TMPDIR:-/tmp}" tmp.XXXXXXXXXX)
    fi
    _rq_longest=""
    for _rq_s in "${_rq_socks[@]}"; do
        if [[ ${#_rq_s} -gt ${#_rq_longest} ]]; then _rq_longest="$_rq_s"; fi
    done
    axl_sock_path_check "$_rq_knob" "$_rq_dir/$_rq_longest" || exit 1
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
    #
    # Depth 2, not 1. During a parallel run this makes no difference: the
    # runner exports its axl-itest.* base AS $AXL_QEMU_TMPDIR, so the per-guest
    # dirs sit at depth 1 from here. The blind spot is AFTERWARDS -- once that
    # run is gone, a later standalone invocation searches /dev/shm, where the
    # same dirs are at depth 2 under a leftover axl-itest.*/. At depth 1 a leak
    # from a hard-killed parallel run could never be swept, which is exactly
    # how five orphans sat for three days.
    #
    # BOTH prefixes, and the second one is why a base could never empty. This
    # matched only `axl-qemu.*`, but an `axl-itest.*` base left by a C-test run
    # also holds `axl-ctest.*` children -- nothing swept those, so the `rmdir`
    # below could never succeed on such a base. Reported by a consumer whose
    # every run-qemu.sh invocation exited 1 instantly against a five-hour-old
    # /dev/shm/axl-itest.*/ holding six axl-ctest.* dirs.
    while IFS= read -r local_d; do
        [[ -n "$local_d" ]] || continue
        pgrep -af -- "$local_d" >/dev/null 2>&1 || rm -rf "$local_d"
    done < <(find "$_axl_tmp_base" -maxdepth 2 -type d \
                  \( -name 'axl-qemu.*' -o -name 'axl-ctest.*' \) \
                  -mmin +10 2>/dev/null)
    # Then the parent bases the loop above may have just emptied. `rmdir`, not
    # `rm -rf`: it removes a base only if nothing is left in it, so a base
    # still holding a live guest's dir is left alone even if the age and pgrep
    # guards were somehow both wrong.
    #
    # Emptying a base refreshes its mtime, so it fails the -mmin +10 guard on
    # THIS pass and is collected by a later one. That two-pass behaviour is
    # deliberate, not an oversight: dropping the age guard here would let a
    # standalone run rmdir another run's freshly-created base during the window
    # between its mktemp and its first worker populating it. An empty directory
    # costs an inode; the race costs a whole run.
    #
    # Note the pgrep guard is conservative by construction: it matches any
    # process whose command line merely CONTAINS the path, so an unrelated
    # shell that echoes or greps the path defers the sweep. That errs toward
    # keeping a dead dir rather than removing a live one, which is the right
    # direction for a defence-in-depth sweep.
    #
    # `|| true` IS LOAD-BEARING, and its absence was a silent total failure.
    # `rmdir` returns 1 on a NON-EMPTY directory; `2>/dev/null` hides the
    # message but not the status, and the command after the final `||` is NOT
    # exempt from `set -e` -- so one un-emptied base made this script exit 1
    # with NO output at all, no error line and empty .log files, on every
    # invocation until someone cleared /dev/shm by hand. A consumer lost a day
    # to it and read it as a rendering regression.
    #
    # Best-effort cleanup must never be able to fail the run it is cleaning up
    # for. The sweep above now empties both child prefixes so this can usually
    # succeed; `|| true` is what guarantees that when it cannot, nothing breaks.
    while IFS= read -r local_b; do
        [[ -n "$local_b" ]] || continue
        pgrep -af -- "$local_b" >/dev/null 2>&1 || rmdir "$local_b" 2>/dev/null || true
    done < <(find "$_axl_tmp_base" -maxdepth 1 -type d -name 'axl-itest.*' -mmin +10 2>/dev/null)
    TMPDIR=$(mktemp -d -p "$_axl_tmp_base" axl-qemu.XXXXXXXX)
else
    TMPDIR=$(mktemp -d)
fi
# INT/TERM as well as EXIT: when a caller wraps this script in `timeout`, the
# temp dir must still go (8.6 GB of such leaks were once found from
# timeout-killed integration runs). And a signal must END the script: this was
# `trap ... EXIT INT TERM`, whose handler removed the dir and then RESUMED --
# into `sed` over the serial log it had just deleted. See axl_trap_cleanup in
# axl-common.sh; test-signal-cleanup-selftest.sh pins it. The handlers read the
# EXIT trap when a signal arrives, so the replacements below need only set EXIT.
#
# --background too, until the fork hands the dir to the detached owner (which
# drops this trap). It was armed for the foreground only, so every exit between
# here and that fork leaked the dir: the QEMU_DRYRUN exit -- the guest pool
# dry-runs every boot and class key with --background, seven dirs in one suite
# run -- and every validation failure below. test-run-qemu-flags.sh pins it.
axl_trap_cleanup 'rm -rf "$TMPDIR"'

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
    # the Shell directly if the launcher can't be built. stage_boot_shell sets
    # SHELL_LAUNCHER_STAGED/_PATH (initialized with ORIG_ARGV above, since
    # --boot-target skips this branch entirely).
    if [[ -n "$SHELL_EFI" && -f "$SHELL_EFI" ]]; then
        # "auto": this script owns the serial log, so it can tell whether the
        # boot reached the Shell and undo the launcher if it did not.
        stage_boot_shell "$STAGING" "$ARCH" "$BOOT_NAME" "$SHELL_EFI" auto || exit 1
    fi
    if [[ -n "$EFI_FILE" ]]; then
        cp "$EFI_FILE" "$STAGING/$EFI_NAME"
    fi
fi

_rq_stage_files "$STAGING"
# Stage VirtioFsDxe.efi if the active OVMF doesn't ship it integrated.
if [[ -n "$VFS_DRIVER_STAGE" ]]; then
    cp "$VFS_DRIVER_STAGE" "$STAGING/VirtioFsDxe.efi"
fi

# There is no startup script to carry them in these modes, so a --setvar would
# be silently ignored -- say so instead.
if [[ ${#SETVARS[@]} -gt 0 ]]; then
    if [[ "$BOOT_TARGET" == "true" ]]; then
        echo "ERROR: --setvar needs the shell, but --boot-target bypasses it" >&2
        exit 1
    fi
    if [[ -z "$EFI_FILE" ]]; then
        echo "ERROR: --setvar needs an application to run before" >&2
        exit 1
    fi
fi

# Startup script
if [[ -n "$CUSTOM_NSH" ]]; then
    if [[ ! -f "$CUSTOM_NSH" ]]; then
        echo "ERROR: nsh file not found: $CUSTOM_NSH" >&2
        exit 1
    fi
    # Prepend the app-output sentinel so the non-raw filter can find where the
    # script's output begins (the launcher removes the old countdown anchor).
    # The `set`s go BEFORE the sentinel so their echo stays out of the region
    # the filter treats as application output.
    { emit_setvars; echo "echo $APP_OUTPUT_SENTINEL"; cat "$CUSTOM_NSH"; } > "$STAGING/startup.nsh"
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
            # Shell variables the caller asked for, before the app so it can
            # read them on its first log emission (AXL_LOG_LEVEL is applied
            # lazily, on the first line emitted).
            emit_setvars
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
DISK_FILE="$TMPDIR/disk.img"
DISK_FMT=raw

# Prepare NVRAM
VARS_FILE="$TMPDIR/vars.fd"
if [[ "$QCOW2" == true ]]; then
    _qdir="$TMPDIR"
    if [[ -n "$STATE_DIR" ]]; then
        mkdir -p "$STATE_DIR" || { log_error "--state-dir: cannot create $STATE_DIR"; exit 1; }
        _qdir="$(cd "$STATE_DIR" && pwd)"
    fi
    VARS_FILE="$_qdir/vars.qcow2"
    _qemu_img="$(dirname "$QEMU_BIN")/qemu-img"
    [[ -x "$_qemu_img" ]] || _qemu_img=qemu-img
    if ! "$_qemu_img" convert -q -f raw -O qcow2 "$FW_VARS" "$VARS_FILE" \
       || ! "$_qemu_img" convert -q -f raw -O qcow2 "$DISK_FILE" "$_qdir/disk.qcow2"; then
        log_error "--qcow2: '$_qemu_img convert' could not make qcow2 copies of $FW_VARS and the boot disk"
        exit 1
    fi
    rm -f "$DISK_FILE"
    DISK_FILE="$_qdir/disk.qcow2"
    DISK_FMT=qcow2
else
    cp "$FW_VARS" "$VARS_FILE"
fi

# The firmware's DEBUG output on a channel of its own (finalize_qemu_cmd adds
# it; qemu_fw_debug_uart in axl-common.sh says why), in FW_DEBUG_LOG: every
# AARCH64 guest's debug UART, and a kept X64 guest's debugcon (--state-dir: a
# pooled guest, which a lease reads from). A booted X64 run keeps its default
# -- no debugcon, its cost a VM exit per byte -- and --debugcon FILE alone
# stays the plain -debugcon below. It holds THIS launch's output: truncated
# here, as the console's log is.
FW_DEBUG_LOG=""
if [[ "$ARCH" == AARCH64 || ( "$ARCH" == X64 && -n "$STATE_DIR" ) ]]; then
    FW_DEBUG_LOG=$(_rq_fw_debug_path "${_qdir:-$TMPDIR}")
    if [[ "${QEMU_DRYRUN:-0}" != 1 ]] && ! { : > "$FW_DEBUG_LOG"; } 2>/dev/null; then
        # Beside a --serial-log that is no regular file in a writable dir (say
        # /dev/null), the log goes to the run's own dir: a by-product must not
        # fail a run that used to work. A --debugcon FILE was asked for.
        if [[ -n "$DEBUGCON_LOG" ]]; then
            echo "ERROR: cannot write --debugcon '$FW_DEBUG_LOG' (missing dir?)" >&2
            exit 1
        fi
        FW_DEBUG_LOG="${_qdir:-$TMPDIR}/firmware-debug.log"
        : > "$FW_DEBUG_LOG"
    fi
fi

# Build QEMU command
_devices=""
[[ "$BRIDGES" == "true" ]] && _devices=standard
# --mem: QEMU's own size syntax, restricted to what both -m and the
# memory-backend-file size= accept alike.
if [[ ! "$MEM" =~ ^[1-9][0-9]*[MG]$ ]]; then
    echo "ERROR: --mem '$MEM' is not <N>M or <N>G" >&2
    exit 1
fi
mapfile -d '' -t CMD < <(build_qemu_base_cmd "$ARCH" "$QEMU_BIN" "$MEM" "$VARS_FILE" "$CPU_SPEC" "$_devices")
CMD+=(-drive "format=$DISK_FMT,file=$DISK_FILE")

# No Boot Manager countdown (qemu_boot_wait_args, axl-common.sh).
mapfile -d '' -t _boot < <(qemu_boot_wait_args)
CMD+=("${_boot[@]}")

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

# --no-gpu: give the guest NO graphics adapter so OVMF exposes no GOP
# (axl_gfx_available() == false). x64's q35 provides a built-in std-VGA by
# default — that is why a plain run has a GOP — so it must be explicitly
# disabled with `-vga none`. AARCH64's `virt` has no default display device, so
# there is nothing to disable (a plain run is already GOP-less). Note this is
# NOT what -nographic does: -nographic only detaches the HOST display; the guest
# adapter/GOP stays. Mutually exclusive with --gpu/--screenshot/--display above,
# so this never collides with the block that adds a device.
if [[ "$NO_GPU" == "true" && "$ARCH" == "X64" ]]; then
    CMD+=(-vga none)
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
# at runtime addresses). X64 only: `virt` has no ISA bus, and an AARCH64
# guest's --debugcon FILE is its debug UART's log (_rq_fw_debug_path); so is
# a kept X64 guest's (FW_DEBUG_LOG, one debugcon, not two).
if [[ -n "$DEBUGCON_LOG" && "$ARCH" == X64 && -z "$FW_DEBUG_LOG" ]]; then
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
fi
# A dry run starts no process: it shows the device and the socket path only.
if [[ -n "$MOUNT_DIR" && "${QEMU_DRYRUN:-0}" != "1" ]]; then
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
fi
if [[ -n "$MOUNT_DIR" ]]; then
    CMD+=(
        -chardev "socket,id=axlvfs,path=$VFS_SOCK"
        -device "vhost-user-fs-pci,queue-size=1024,chardev=axlvfs,tag=$MOUNT_TAG"
        -object "memory-backend-file,id=axlmem,size=$MEM,mem-path=/dev/shm,share=on"
        -numa "node,memdev=axlmem"
    )
fi

# If --mount spawned a helper daemon (virtiofsd), extend the cleanup trap so it
# doesn't leak: in the foreground, after QEMU exits; with --background, only
# until the QEMU_PID= line hands it to the caller (with VIRTIOFSD_PID=), since
# a refusal between here and the launch -- a bad --mac, say -- otherwise left
# it running with its socket in the deleted dir.
kill_args=""
helper_pids=()
[[ -n "$VIRTIOFSD_PID" ]] && helper_pids+=("$VIRTIOFSD_PID")
if [[ ${#helper_pids[@]} -gt 0 ]]; then
    # Pre-format the kill argument list with a literal space so
    # the trap doesn't depend on $IFS at trap-fire time. Using
    # ${helper_pids[*]} would pick up the first byte of $IFS;
    # safer to spell the join here.
    for pid in "${helper_pids[@]}"; do
        kill_args+="${kill_args:+ }$pid"
    done
    trap 'kill '"$kill_args"' 2>/dev/null; rm -rf "'"$TMPDIR"'"' EXIT
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
        # A usage conflict first: it is wrong whatever taps the host has.
        if (( ${#GUESTFWDS[@]} )); then
            echo "ERROR: --guestfwd needs slirp; it cannot be combined with --tap" >&2
            exit 1
        fi
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
            NETDEV="$NETDEV,hostfwd=tcp:${HOSTFWD_BIND}:${HOST_PORT}-:${GUEST_PORT}"
        done
        # --guestfwd: a guest connection to an address on slirp's network,
        # forwarded (`cmd:` spawns a program per connection on its stdio).
        # A comma would end the option and start another netdev option.
        for gfwd in "${GUESTFWDS[@]}"; do
            if [[ "$gfwd" == *,* ]]; then
                echo "ERROR: --guestfwd '$gfwd' contains a comma, which would end the" \
                     "guestfwd option inside -netdev" >&2
                exit 1
            fi
            NETDEV="$NETDEV,guestfwd=$gfwd"
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
# The tokens every launch mode appends LAST, into the array named $1. One list,
# read by both finalize_qemu_cmd and the census record below, so the record
# cannot describe a different guest from the one launched.
qemu_cmd_tail() {
    local -n _tail="$1"
    _tail=()
    # --reboot: let a guest-initiated reset re-POST the same VM instead
    # of ending the run. -no-reboot is shorthand for `-action
    # reboot=shutdown`; `-action reboot=reset` is its explicit inverse,
    # and the later token wins. Placed before the caller's own tokens
    # so an explicit --qemu-arg still overrides it.
    if [[ "$REBOOT" == "true" ]]; then
        _tail+=(-action reboot=reset)
    fi
    if [[ ${#EXTRA_QEMU_ARGS[@]} -gt 0 ]]; then
        # Append each token verbatim — no word-splitting, so a token may
        # contain spaces (e.g. a device spec with a space in a file path).
        _tail+=( "${EXTRA_QEMU_ARGS[@]}" )
    fi
}

finalize_qemu_cmd() {
    local tail
    qemu_cmd_tail tail
    CMD+=( ${tail[@]+"${tail[@]}"} )
    # Last: the AARCH64 debug UART goes after every launch mode's console
    # (a kept X64 guest's debugcon has no order to keep).
    # The dry run comes before a mode adds its console, so its argv lists the
    # debug UART alone -- which is what a pooled class's key needs to see.
    [[ -n "$FW_DEBUG_LOG" ]] && qemu_fw_debug_uart CMD "$ARCH" "$FW_DEBUG_LOG"
    return 0
}

# --monitor-socket / --qmp-socket (validated above to be --background only).
# Added before the QEMU_DRYRUN dump so the argument shape is testable.
if [[ -n "$MONITOR_SOCKET" ]]; then
    CMD+=(-monitor "unix:$MONITOR_SOCKET,server,nowait")
fi
if [[ -n "$QMP_SOCKET" ]]; then
    CMD+=(-qmp "unix:$QMP_SOCKET,server,nowait")
fi

# The skip cache's inputs for a booted run: everything it hands the guest from
# the tree. One list, recorded after the launch point below and printed by the
# dry run (AXL_RQ_CACHE_INPUT, outside the AXL_DRYRUN_ facts the pool's class
# key reads) so test-run-qemu-flags.sh can see it. It once held the image and
# firmware only, while the pooled path recorded --extra and --nsh too: a
# booted test's ids file or script could change and the test read FRESH.
mapfile -t _RQ_CACHE_INPUTS < <(printf '%s\n' "${EFI_FILE:-}" "${FW_CODE:-}" \
    "${FW_VARS:-}" "${SHELL_EFI:-}"; _rq_staged_inputs)

# QEMU_DRYRUN=1 prints the constructed CMD and exits without launching
# qemu. Useful for argument-shape regression tests and for "what
# would run-qemu.sh do?" debugging. Each CMD token is printed on its
# own line prefixed with "QEMU_DRYRUN: " — no shell-quoting noise so
# tests can grep for token literals (commas in -device strings, etc.)
# without escaping.
# Three facts beyond the argv, for a caller that keys on the machine (the
# guest pool's class key): the run's temp dir (to normalise its paths
# away), the vars template it copied, and a digest of every staged file's
# name and bytes -- the launcher, Shell, options, scripts and images the
# boot disk holds, none of which the argv names.
if [[ "${QEMU_DRYRUN:-0}" == "1" ]]; then
    finalize_qemu_cmd
    for tok in "${CMD[@]}"; do
        printf 'QEMU_DRYRUN: %s\n' "$tok"
    done
    echo "AXL_DRYRUN_TMPDIR=$TMPDIR"
    echo "AXL_DRYRUN_INPUT=$FW_VARS"
    for f in "${_RQ_CACHE_INPUTS[@]}"; do
        [[ -n "$f" ]] && echo "AXL_RQ_CACHE_INPUT=$f"
    done
    if [[ -d "${STAGING:-}" ]]; then
        echo "AXL_DRYRUN_STAGED=$(cd "$STAGING" && find . -type f -print0 | LC_ALL=C sort -z \
            | xargs -0 -r sha256sum | sha256sum | cut -c1-64)"
    fi
    exit 0
fi

# Profiling: ONE record per guest LAUNCH, when AXL_TEST_PROFILE is set.
#
# AFTER the dry-run exit, and that placement is the whole correctness of the
# count. It sat above, at command assembly -- the single point every launch
# passes through -- which is true and was not enough: QEMU_DRYRUN=1 ASSEMBLES
# the command and exits without a guest, and so do the late validation
# failures. test-run-qemu-flags.sh is host-only argument parsing and was
# credited with SEVEN boots in ONE second, which is what exposed it. Every
# real launch is below this line; nothing that exits above it boots anything.
#
# A COUNT and not a duration: run-qemu.sh installs and REPLACES an EXIT trap in
# several places, so another for the stop stamp would clobber a cleanup that
# removes the guest's TMPDIR. Seconds-per-boot is derived from the runner's own
# per-test time instead, which needs no exit hook.
#
# 71 of the integration tests reach QEMU this way and are invisible to the
# hooks in common-test.sh, which instruments the OTHER, disjoint path
# (common-test.sh launches QEMU itself -- see its note at test_build_qemu_cmd).
# Instrumenting only that path measured 41 tests and 15% of the suite's work
# while looking like a complete report.
if [[ -n "${AXL_TEST_PROFILE:-}" ]]; then
    printf 'qemu|%s\n' "${AXL_TEST_PROFILE_NAME:-run-qemu}" >> "$AXL_TEST_PROFILE"
    # ...and the launch with its command line, for the guest-pool census
    # (test/integration/lib/profile.sh, _prof_launch, has the record's shape).
    # CMD here is the guest plus the tail every mode appends (qemu_cmd_tail:
    # --reboot and every --qemu-arg, which is where TPMs, extra NICs and
    # replayed fixtures come in). What each mode adds beyond that is harness
    # plumbing -- serial, display, sockets -- and is recorded as the via.
    _launch_via="run-qemu:bg=$BACKGROUND,reboot=${REBOOT:-false},shot=${SCREENSHOT:+1}"
    _launch_via+=",serial=${SERIAL_SOCKET:+1},qmp=${QMP_SOCKET:+1},mon=${MONITOR_SOCKET:+1}"
    _launch_via+=",i=$INTERACTIVE"
    qemu_cmd_tail _launch_tail
    printf 'launch|%s|%s|%s|%s\n' "${AXL_TEST_PROFILE_NAME:-run-qemu}" "$_launch_via" \
        "${FW_VARS:-}" "$(axl_census_argv "${CMD[@]}" ${_launch_tail[@]+"${_launch_tail[@]}"})" \
        >> "$AXL_TEST_PROFILE"
    # ...and WHAT this launch put in the guest. test_add_efi records the same
    # thing for the common-test.sh path, and misses every test that reaches a
    # guest through here -- 87 of 169 tests staged no recordable artifact until
    # this line existed, which made an artifact-keyed cache unable to prove
    # anything about half the suite (AXL-CI-Release-Speed-Design.md §12.15).
    if [[ -n "${EFI_FILE:-}" && -f "${EFI_FILE:-}" ]]; then
        printf 'efi|%s|%s|%s\n' "${AXL_TEST_PROFILE_NAME:-run-qemu}" \
            "$(basename "$EFI_FILE")" \
            "$(sha256sum "$EFI_FILE" 2>/dev/null | cut -d' ' -f1)" \
            >> "$AXL_TEST_PROFILE"
    fi
fi

# Skip-cache inputs, for the tests that reach a guest through here rather than
# through common-test.sh's launcher. Separate from the profile above because the
# cache is on in ordinary runs while the profile is a measurement tool. See
# test/integration/lib/test-cache.sh.
_rq_record_inputs "${_RQ_CACHE_INPUTS[@]}"

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
    trap "$cleanup_cmd" EXIT

    finalize_qemu_cmd
    _rq_tm_begin interactive
    _rq_rc=0
    "${CMD[@]}" || _rq_rc=$?
    _rq_tm_end "$_rq_rc"
    exit "$_rq_rc"
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
    trap "$cleanup_cmd" EXIT

    finalize_qemu_cmd
    _rq_tm_begin window
    _rq_rc=0
    "${CMD[@]}" || _rq_rc=$?
    _rq_tm_end "$_rq_rc"
    exit "$_rq_rc"
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
    _rq_tm_begin shot
    _rq_guard_begin
    "${CMD[@]}" &
    QEMU_PID=$!
    _rq_tm_watch "$QEMU_PID" "${SERIAL_LOG:-$LOG}"
    _rq_guard_guest "$QEMU_PID"
    # Sample host CPU for the life of this QEMU (same monitor the default branch
    # runs). A short SHOT_WAIT may kill QEMU before the warm-up clears -> no
    # post-warm-up data, so no detection (fail-safe): give the guest enough
    # SHOT_WAIT past the 10-15s warm-up to measure a spike.
    RUN_START_MS=$(now_ms)   # --max-duration budget clock
    cpu_monitor_start "$QEMU_PID"

    # Send one HMP monitor command, and CHECK that it went.
    #
    # socat's exit status used to be discarded here (`>/dev/null 2>&1`, no
    # test), so a send that never reached QEMU was indistinguishable from a
    # delivered keystroke. That is the failure mode that makes a harness bug
    # present as an application bug: input silently missing, with nothing in
    # any log to say so. Retry briefly, then say so loudly.
    mon_cmd() {
        local cmd="$1" tries=0
        while (( tries < 3 )); do
            if printf '%s\n' "$cmd" \
                 | socat -t 2 - "UNIX-CONNECT:$MONSOCK" >/dev/null 2>&1; then
                return 0
            fi
            tries=$(( tries + 1 ))
            sleep 0.2
        done
        echo "[run-qemu] monitor send FAILED after 3 tries: $cmd" >&2
        return 1
    }

    # Same, for QMP. Each socat connection is a fresh session, so capabilities
    # are re-negotiated per call; payload lines are passed as arguments.
    qmp_cmd() {
        local tries=0
        while (( tries < 3 )); do
            if { printf '%s\n' '{"execute":"qmp_capabilities"}'
                 printf '%s\n' "$@"; } \
                 | socat -t 2 - "UNIX-CONNECT:$QMPSOCK" >/dev/null 2>&1; then
                return 0
            fi
            tries=$(( tries + 1 ))
            sleep 0.2
        done
        echo "[run-qemu] QMP send FAILED after 3 tries" >&2
        return 1
    }

    # Both gates below sample at 0.25s, so report EXACT quarter-seconds plus the
    # raw tick count rather than whole seconds. This is not cosmetic: with
    # --sendkey-settle 0 the gate's wait time is the only readout of guest
    # post-key repaint latency anyone has, and a consumer measured ~1s for their
    # heaviest cases against a 1500ms default -- a margin that thin is invisible
    # at 1s granularity, and it is exactly the margin that closes under load.
    # ${1:-0}, not $1: an unset argument would interpolate to nothing and hand
    # awk a bare `/4`, i.e. an unterminated-regexp syntax error on stderr in
    # place of a number. Callers always pass an initialised counter, so this is
    # belt-and-braces -- but a diagnostic line that can emit a parse error is a
    # poor instrument, and this one is about to be used as a bisect metric.
    _ticks_secs() { awk "BEGIN{printf \"%.2f\", ${1:-0}/4}"; }

    # Pre-screenshot settle.  Decoupled from TIMEOUT so a caller can keep a
    # safe kill-timeout but capture sooner: set SHOT_WAIT to the seconds the
    # guest needs to boot + render (the visual suite tunes this).  Default =
    # the old TIMEOUT-3 behaviour.
    WAIT="${SHOT_WAIT:-$((TIMEOUT - 3))}"
    [[ $WAIT -lt 3 ]] && WAIT=3
    if [[ $WAIT -ge $TIMEOUT ]]; then
        # These two are separate knobs that share one budget, and raising the
        # wait to give a loaded guest more time silently eats the kill margin
        # -- the run dies mid-capture and reports as a timeout, not as a wait
        # that was set too long. Say which knob is wrong instead.
        echo "[run-qemu] WARNING: SHOT_WAIT ($WAIT s) >= --timeout ($TIMEOUT s):" \
             "QEMU will be killed at or before the capture. Raise --timeout." >&2
    fi

    # --sendkey-after REGEX: wait for the GUEST to say it is ready, instead of
    # guessing with a wall clock.
    #
    # This is the fix for keystrokes going missing under host load. Injection
    # used to start at SHOT_WAIT, a fixed timer that does not move when the
    # host slows down -- so a loaded guest that booted slower had its FIRST
    # keys typed at the Shell prompt before the app bound ConIn, and the app
    # saw a truncated sequence. Measured: at SHOT_WAIT=8 under 8 busy loops the
    # app received 9 of 10 keys ('a' lost); at 40, 10 of 10. Same load, same
    # binaries. The keys were never dropped in transit -- they were typed too
    # early, which reads as an application bug and is not one.
    #
    # Gating on a marker the guest prints makes delivery independent of how
    # busy the host is, which is the property worth having. Bounded so a guest
    # that never prints it cannot hang the run; on expiry it types anyway and
    # says why, leaving the old behaviour as the degraded case rather than a
    # silent one.
    if [[ -n "$SENDKEY_AFTER" ]]; then
        _sk_log="${SERIAL_LOG:-$LOG}"
        _sk_cap=$(( TIMEOUT - 5 )); [[ $_sk_cap -lt 5 ]] && _sk_cap=5
        _sk_cap_ticks=$(( _sk_cap * 4 ))   # the loop sleeps 0.25s per tick
        _sk_ticks=0
        # grep -a: the serial capture can carry a stray NUL, and one NUL makes
        # GNU grep call the whole file binary -- -q would still answer, but the
        # habit is what keeps the diagnostics below readable.
        while ! /usr/bin/grep -qaE "$SENDKEY_AFTER" "$_sk_log" 2>/dev/null; do
            if (( _sk_ticks >= _sk_cap_ticks )); then
                echo "[run-qemu] WARNING: --sendkey-after '$SENDKEY_AFTER' not seen" \
                     "in ${_sk_cap}s; typing anyway (keys may be lost)" >&2
                break
            fi
            sleep 0.25
            _sk_ticks=$(( _sk_ticks + 1 ))
        done
        echo "[run-qemu] --sendkey-after: guest ready after $(_ticks_secs $_sk_ticks)s" "($_sk_ticks ticks)" >&2
        sleep 0.5   # let the app finish the paint that followed its marker
    else
        sleep "$WAIT"
    fi

    # --sendkey: inject key tokens via the monitor once the app is up, then
    # let it settle/repaint before the dump.  Generous per-key delay (TCG is
    # slow on aarch64) so no keystroke is dropped.
    # Byte offset into the serial log BEFORE any key is typed. --screenshot-after
    # matches only beyond this, so a marker the app printed earlier (a readiness
    # line, a previous interaction) cannot satisfy the gate and let the capture
    # race ahead exactly as it does today.
    _sa_log="${SERIAL_LOG:-$LOG}"
    _sa_offset=$(stat -c %s "$_sa_log" 2>/dev/null || echo 0)

    if [[ -n "$SENDKEY_SEQ" ]]; then
        key_delay=0.4
        [[ "$ARCH" == "AARCH64" ]] && key_delay=1.0
        for key in $SENDKEY_SEQ; do
            mon_cmd "sendkey $key" || true
            sleep "$key_delay"
        done

        # Trailing settle: let the app PROCESS and REPAINT before the dump.
        #
        # This is a wall clock with the same weakness --sendkey-after removed
        # from the front of this block: it does not move when the host slows
        # down. Measured here, the guest finishes RECEIVING the last key ~1.2s
        # before the dump under both spinner and 6-VM load, so delivery is not
        # the exposure -- REPAINT is, and a consumer whose repaint is heavy
        # (a syntax scanner over a style buffer, say) can still be mid-frame.
        #
        # --screenshot-after below is the real answer: gate on the guest.
        # This knob stays for consumers that cannot emit a marker, and the
        # default preserves the historical 1.5s exactly.
        sleep "$(awk "BEGIN{printf \"%.3f\", ${SENDKEY_SETTLE_MS:-1500}/1000}")"
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
            qmp_cmd "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":true,\"key\":{\"type\":\"qcode\",\"data\":\"$hk_code\"}}}]}}" || true
            sleep "$(awk "BEGIN{printf \"%.3f\", $hk_ms/1000}")"
            qmp_cmd "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":false,\"key\":{\"type\":\"qcode\",\"data\":\"$hk_code\"}}}]}}" || true
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
            if [[ "$click" == "click" ]]; then
                qmp_cmd \
                    "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$vx}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$vy}}]}}" \
                    '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"left","down":true}}]}}' \
                    '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"left","down":false}}]}}' || true
            else
                qmp_cmd \
                    "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$vx}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$vy}}]}}" || true
            fi
            sleep "$move_delay"
        done
        sleep 1.5   # let the app process the motion + repaint before capture
    fi

    # --screenshot-after ERE [--screenshot-after-count N]: wait for the guest to
    # say it has finished painting, instead of trusting the trailing settle.
    #
    # Symmetric with --sendkey-after. The failure it prevents is not "the last
    # keystroke was lost" -- no keystroke is lost. It is that `screendump` reads
    # the framebuffer with no synchronisation while the guest is still writing
    # it, so a single capture can be TORN: a consumer measured one frame whose
    # hex pane reflected 2 of 3 keys while its status bar and inspector
    # reflected 0 of 3, disagreeing with each other. No settle VALUE expresses
    # "has stopped writing"; only the guest knows.
    #
    # COUNT exists because paints are CONTINUOUS during typing. An app that
    # repaints after every key emits a "painted" marker at key 1, which lands
    # past _sa_offset and satisfies a first-match gate the instant it is
    # reached -- capturing the very torn frame the gate exists to prevent, and
    # being green by construction. Waiting for the Nth match fixes that without
    # asking the guest to say WHICH paint it just finished.
    #
    # Moving _sa_offset cannot substitute: after the last key races the guest
    # painting inside the trailing key_delay (a paint that already happened is
    # never seen -> timeout + spurious warning every run), and before the last
    # key just matches key N-1. Every positional variant races; an ordinal does
    # not.
    #
    # Counting is done past _sa_offset so this composes with the stale-marker
    # guard rather than replacing it.
    if [[ -n "$SCREENSHOT_AFTER" ]]; then
        _sa_want="${SCREENSHOT_AFTER_COUNT:-1}"
        if [[ ! "$_sa_want" =~ ^[0-9]+$ ]] || (( _sa_want < 1 )); then
            echo "[run-qemu] --screenshot-after-count: '$_sa_want' is not a positive" \
                 "integer; using 1" >&2
            _sa_want=1
        fi
        _sa_cap=$(( TIMEOUT - 3 )); [[ $_sa_cap -lt 3 ]] && _sa_cap=3
        _sa_ticks=0
        _sa_cap_ticks=$(( _sa_cap * 4 ))
        _sa_seen=0
        while :; do
            _sa_seen=$(tail -c "+$(( _sa_offset + 1 ))" "$_sa_log" 2>/dev/null \
                         | /usr/bin/grep -acE "$SCREENSHOT_AFTER" || true)
            (( _sa_seen >= _sa_want )) && break
            if (( _sa_ticks >= _sa_cap_ticks )); then
                echo "[run-qemu] WARNING: --screenshot-after '$SCREENSHOT_AFTER' seen" \
                     "$_sa_seen time(s), wanted $_sa_want, in ${_sa_cap}s;" \
                     "capturing anyway (frame may be mid-repaint)" >&2
                break
            fi
            sleep 0.25
            _sa_ticks=$(( _sa_ticks + 1 ))
        done
        echo "[run-qemu] --screenshot-after: $_sa_seen/$_sa_want match(es)" \
             "after $(_ticks_secs $_sa_ticks)s ($_sa_ticks ticks)" >&2
        # A marker that has ALREADY fired more times than we waited for means
        # the gate stopped at an earlier paint and the guest kept going -- the
        # green-by-construction trap. Say so; a silent false green here is
        # worse than no gate, because it looks measured.
        if (( _sa_seen > _sa_want )); then
            echo "[run-qemu] WARNING: '$SCREENSHOT_AFTER' matched $_sa_seen times but the" \
                 "gate waited for $_sa_want. If your app emits this marker per REPAINT," \
                 "pass --screenshot-after-count $_sa_seen or make the marker" \
                 "self-identifying (e.g. 'PAINT n=3'); otherwise the capture was gated" \
                 "on an EARLIER paint." >&2
        fi
    fi

    for try in 1 2 3; do
        echo "screendump $TMPDIR/screenshot.ppm" | \
            socat -t 2 - "UNIX-CONNECT:$MONSOCK" >/dev/null 2>&1 && break
        sleep 1
    done
    sleep 1
    kill "$QEMU_PID" >/dev/null 2>&1
    wait "$QEMU_PID" >/dev/null 2>&1
    _rq_shot_rc=$?
    _rq_unguard_guest
    _rq_tm_end "$_rq_shot_rc"
    set -e

    # Summarize host CPU (WARN + optional CPU-REPORT). A sustained post-warm-up
    # spike returns CPU_SPIKE_EXIT, surfaced as the run's exit below -- AFTER the
    # PNG is written, and only when a capture/convert error (which outranks it)
    # did not already fail the run.
    cpu_spike_rc=0
    cpu_monitor_finish || cpu_spike_rc=$?
    # The duration budget is the compute-shaped run's gate (see --workload).
    # A spike verdict outranks it only because it is the more specific
    # diagnosis; either way the run fails with its own distinct code.
    if [[ "$cpu_spike_rc" == "0" ]]; then
        duration_summary $(( $(now_ms) - RUN_START_MS )) || cpu_spike_rc=$?
    fi

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
    # What `QEMU_PID=` below actually reports is the `timeout` pid, NOT
    # QEMU's: GNU timeout fork+execs the command, so the guest is a
    # GRANDCHILD of the wrapper subshell and `pgrep -P "$WRAPPER_PID"`
    # can only ever reach `timeout`. The name is historical and callers
    # parse it, so it stays — and it stays a usable handle, because
    # `timeout` forwards signals to its child, so kill/kill -0 on it
    # behave as callers expect. Anything that needs the REAL qemu pid
    # (for /proc sampling, say) must walk one more level down with
    # `pgrep -P "$QEMU_PID"`.
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
    # A stale socket from an earlier guest would make QEMU's bind fail.
    [[ -n "$MONITOR_SOCKET" ]] && rm -f "$MONITOR_SOCKET"
    [[ -n "$QMP_SOCKET" ]] && rm -f "$QMP_SOCKET"
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
    if ! { : > "$BG_SERIAL_DEST"; } 2>/dev/null; then   # grouped: a failing > is reported before a trailing 2> applies
        echo "ERROR: cannot write --serial-log '$BG_SERIAL_DEST' (missing dir?)" >&2
        exit 1
    fi
    # Self-clean the state dir when the guest exits (stop, crash, or the timeout
    # wrapper firing). --background returns immediately so it can't use an EXIT
    # trap (the disk/serial must outlive THIS script), but the detached subshell
    # can rm $TMPDIR once qemu is gone — tying cleanup to the guest's real
    # lifetime. Without this, every --background run orphaned its ~40 MB
    # /dev/shm/axl-qemu.* dir forever.
    #
    # The `rm` belongs to a TRAP, not to a trailing command. As a trailing
    # command it was the last statement of a killable subshell: signal the
    # subshell (the exact case a `timeout`-wrapped caller produces on expiry)
    # and the guest was still reaped correctly by the watchdog below — orphans
    # do not die with their parent — but the `rm` never ran, leaking ~42 MB
    # with nothing left holding it. The trap covers the signalled paths as well
    # as the normal one, mirroring the foreground branch's own trap above.
    # Cleanup is NOT pulled earlier by this: bash defers a trap until the
    # running foreground command returns, so the `rm` still happens after
    # `timeout` (and therefore the guest) is done, never under a live guest.
    # `|| :` so timeout's non-zero exit (124 on expiry, or the guest's own exit
    # code) doesn't abort the subshell under `set -e` before the trap runs.
    # The `>/dev/null 2>&1` on the SUBSHELL (not just the inner qemu) is
    # load-bearing: the inner `> "$BG_SERIAL_DEST"` only redirects the
    # timeout+qemu command, so without this the detached subshell would inherit
    # run-qemu's own stdout and stderr and hold them open for the whole guest
    # lifetime. A caller that captures our output via command substitution
    # (out=$(run-qemu … --background …)) then blocks forever — the $() never
    # sees EOF until the guest dies. The QEMU_PID=/SERIAL_LOG=/TMPDIR= lines
    # below are echoed after this on run-qemu's real stdout, so they still reach
    # the caller.
    # Tie the guest's life to its watchdog with PR_SET_PDEATHSIG. Killing the
    # cleanup-owner subshell leaves an orphaned `timeout` that still fires, so
    # the guest is bounded — but SIGKILL the watchdog too and nothing bounds it
    # at all: it survives at ppid 1 forever, and because it carries its state
    # dir in argv, the stale-dir sweeper above (which skips any dir a live
    # process references) can never collect that dir either. The leak protects
    # itself. PR_SET_PDEATHSIG survives the execve into QEMU, so however
    # `timeout` dies the kernel takes the guest with it, and the sweeper gets
    # its shot. This does NOT shorten a normal --background guest: the death
    # signal is tied to `timeout`, which lives the guest's whole lifetime, not
    # to run-qemu.sh (which exits immediately by design).
    #
    # axl_pdeath_init (axl-common.sh) probes setpriv. No setpriv, or no
    # --pdeathsig support: the plain invocation, which is still bounded by
    # `timeout` in every case except a SIGKILLed watchdog.
    axl_pdeath_init
    # Its TIMING line is this subshell's too: it alone sees the guest end. A
    # signal to it prints rc=unfinished from the trap (once: _rq_tm_end clears
    # its stamp). The boot watcher is started BELOW, by this script, and never
    # from in here: QEMU_PID is found as this subshell's child (`pgrep -P |
    # head -1`), and a watcher forked first was found instead -- callers then
    # killed the watcher and left the guest running.
    # THE OWNER GUARDS THE GUEST UNTIL THE HANDOVER. Until the caller has the
    # QEMU_PID= line it cannot know the guest exists, so if this script is gone
    # before printing it -- killed by a signal, by SIGKILL, or mid-way through
    # its own EXIT trap, which a group TERM was measured to do (3 runs in 10 at
    # one timing, cause never pinned down) -- the guest must go with it. A guard
    # in THIS script's traps cannot promise that; the owner subshell outlives
    # the script by design, so it is the one that watches: until
    # $TMPDIR/.handover exists it polls this script's pid and TERMs the guest's
    # `timeout` when the script is gone, and a signal to the owner itself ends
    # the guest. After the handover the caller owns the guest, and a signal to
    # the owner only marks the cleanup for when the guest ends (it must not
    # pull the disk from under a live guest -- bg-leak shape A).
    #
    # The 100 ms tick is `read -t` on a FIFO made here, BEFORE the fork: the
    # owner forks nothing but `timeout`, so qemu_child_pid (the owner's first
    # child) still finds the watchdog -- a `sleep` it found instead once made
    # callers kill the wrong process. Every command that can fail is guarded:
    # this subshell inherits set -e, and a read that times out "fails".
    mkfifo "$TMPDIR/.owner-tick"
    ( _rq_t=""; _rq_sig=""; _rq_launcher=$$
      _rq_handed() { [[ -e "$TMPDIR/.handover" ]]; }
      # Gone, or a zombie (kill -0 succeeds on one, and a parent that never
      # reaps would otherwise keep the guest alive to its --timeout).
      _rq_launcher_gone() {
          local _st=""
          kill -0 "$_rq_launcher" 2>/dev/null || return 0
          read -r _ _ _st _ < "/proc/$_rq_launcher/stat" 2>/dev/null || return 0
          [[ "$_st" == Z ]]
      }
      _rq_owner_signal() {
          _rq_sig="$1"
          if [[ -n "$_rq_t" ]] && ! _rq_handed; then kill -TERM "$_rq_t" 2>/dev/null || true; fi
      }
      trap '_rq_owner_signal INT' INT
      trap '_rq_owner_signal TERM' TERM
      trap '_rq_owner_signal HUP' HUP
      trap '_rq_tm_end unfinished; rm -rf "$TMPDIR"' EXIT
      exec {_rq_tick}<> "$TMPDIR/.owner-tick"
      _rq_tm_begin bg
      # {_rq_tick}<&-: the tick's fd is the owner's alone -- bash does not
      # close it on exec, and QEMU would hold the FIFO for its whole life.
      timeout "$TIMEOUT" ${AXL_PDEATH[@]+"${AXL_PDEATH[@]}"} "${CMD[@]}" > "$BG_SERIAL_DEST" 2>&1 < /dev/null {_rq_tick}<&- &
      _rq_t=$!
      # A signal that landed before $_rq_t was known.
      if [[ -n "$_rq_sig" ]] && ! _rq_handed; then kill -TERM "$_rq_t" 2>/dev/null || true; fi
      while kill -0 "$_rq_t" 2>/dev/null; do
          if _rq_handed; then
              wait "$_rq_t" 2>/dev/null || true    # a trapped signal returns it early
          else
              # The marker is RE-CHECKED after the launcher is seen gone: it
              # is written before the launcher can exit, so a launcher that
              # handed over and exited between the two tests is not mistaken
              # for one that died before handing over.
              if _rq_launcher_gone && ! _rq_handed; then
                  kill -TERM "$_rq_t" 2>/dev/null || true
              fi
              read -r -t 0.1 -u "$_rq_tick" _ 2>/dev/null || true
          fi
      done
      _rq_bg_rc=0
      wait "$_rq_t" 2>/dev/null || _rq_bg_rc=$?
      if [[ -n "$_rq_sig" ]]; then exit $(( 128 + $(kill -l "$_rq_sig") )); fi
      _rq_tm_end "$_rq_bg_rc" ) >/dev/null 2>&1 &
    WRAPPER_PID=$!
    # The dir is the owner's now: this script must not remove it under the
    # guest. Its virtiofsd stays this script's to end until the QEMU_PID= line.
    if [[ -n "$kill_args" ]]; then
        trap 'kill '"$kill_args"' 2>/dev/null' EXIT
    else
        trap - EXIT
    fi
    QEMU_PID=$(qemu_child_pid "$WRAPPER_PID")
    # The boot reading, for the subshell's line (it reads $TMPDIR/.timing.boot).
    # Disowned, output to /dev/null, ends with the watchdog: it outlives this
    # script without holding a caller's $(...) open. No file on a serial
    # SOCKET, so no reading there.
    [[ -n "$SERIAL_SOCKET" ]] || _rq_tm_watch "$WRAPPER_PID" "$BG_SERIAL_DEST"
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

    # THE HANDOVER: from here the caller owns the guest. Marked before the first
    # line, so a caller that has read QEMU_PID= never sees the owner kill it.
    # A guest that already exited during startup took $TMPDIR with it (its
    # owner removes it): the marker then has nowhere to go and nothing to
    # guard, and the lines below still report the run as they always did.
    { : > "$TMPDIR/.handover"; } 2>/dev/null || true
    trap - EXIT        # virtiofsd, too, is the caller's from here
    echo "QEMU_PID=$QEMU_PID"
    echo "SERIAL_LOG=$BG_SERIAL_DEST"
    for _hf in "${HOSTFWD_CHOSEN[@]}"; do
        echo "$_hf"
    done
    [[ -n "$SERIAL_SOCKET" ]] && echo "SERIAL_SOCKET=$SERIAL_SOCKET"
    [[ -n "$MONITOR_SOCKET" ]] && echo "MONITOR_SOCKET=$MONITOR_SOCKET"
    [[ -n "$QMP_SOCKET" ]] && echo "QMP_SOCKET=$QMP_SOCKET"
    [[ -n "$VIRTIOFSD_PID" ]] && echo "VIRTIOFSD_PID=$VIRTIOFSD_PID"
    [[ -n "$FW_DEBUG_LOG" ]] && echo "FIRMWARE_DEBUG_LOG=$FW_DEBUG_LOG"
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
    _rq_tm_begin fg
    # `timeout` runs QEMU in a process group of its own, so neither a caller's
    # group signal nor a kill of this script reaches it: without the guard a
    # killed run-qemu.sh left its guest running to --timeout, on a deleted
    # disk. WRAPPER_PID is the `timeout` itself (the subshell execs it), and
    # it forwards the TERM to QEMU.
    _rq_guard_begin
    ( timeout "$TIMEOUT" "${CMD[@]}" > "$LOG" 2>&1 < /dev/null ) &
    WRAPPER_PID=$!
    _rq_tm_watch "$WRAPPER_PID" "$LOG"
    _rq_guard_guest "$WRAPPER_PID"
    QPID=""
    if [[ "$CPU_WARN" == "true" || "$CPU_REPORT" == "true" ]]; then
        QPID=$(qemu_child_pid "$WRAPPER_PID")
    fi
    RUN_START_MS=$(now_ms)   # --max-duration budget clock
    cpu_monitor_start "$QPID"
    _rq_fg_rc=0
    wait "$WRAPPER_PID" 2>/dev/null || _rq_fg_rc=$?
    _rq_unguard_guest
    _rq_tm_end "$_rq_fg_rc"

    CLEAN="$TMPDIR/clean.log"
    _rq_clean_log "$LOG" "$CLEAN"

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

  Disk image:    $DISK_FILE ($(stat -c%s "$DISK_FILE" 2>/dev/null || echo "?") bytes)
  QEMU binary:   $QEMU_BIN
  Architecture:  $ARCH

Likely causes:
  - /dev/kvm not accessible (try: ls -l /dev/kvm; id)
  - Firmware vars file in use by another QEMU process (try: pgrep -fa qemu)
  - Disk image build failed silently (rerun with bash -x)
  - QEMU build broken (try: $QEMU_BIN --version)
EOF
        if [[ -s "$FW_DEBUG_LOG" ]]; then
            echo "The firmware's debug log ($FW_DEBUG_LOG) ends:" >&2
            tail -n 20 "$FW_DEBUG_LOG" | tr -d '\r' | sed 's/^/  /' >&2
        fi
        exit 1
    fi

    # Shell-launcher self-healing. The staged startup.nsh echoes the sentinel
    # BEFORE the app line, so the sentinel can only be missing when the Shell
    # never ran startup.nsh at all — never for an app-level failure. That is
    # exactly the reported launcher fault (the guest loads BOOTX64.EFI and
    # never chains to the Shell), so record it against this firmware and redo
    # the whole run without the launcher.
    #
    # Deliberately AFTER the two guards above: a hostfwd bind failure or a
    # zero-byte log is a broken environment, and must report its own error
    # instead of being retried into a second identical failure.
    #
    # Re-running the whole script rather than retrying in place: staging happens
    # ~1250 lines above, so this is both the smaller change and the more
    # faithful one — the second attempt is a genuine launcher-less run, not a
    # partially rewound one. AXL_SHELL_LAUNCHER=1 opts out: forced-on means a
    # launcher failure must stay loud. The guard var caps this at a single retry.
    #
    # CALL + exit, deliberately NOT exec: exec replaces this process, so neither
    # the EXIT trap nor the INT/TERM traps ever fire — leaking $TMPDIR (disk
    # image + logs) and, under --mount, ORPHANING virtiofsd, which keeps holding
    # the shared directory with an unlinked socket. Keeping this process alive
    # as the parent keeps that cleanup exactly where it already works, instead
    # of duplicating the trap's knowledge of what needs killing.
    #
    # The memo is written BEFORE the retry on purpose: a caller who wrapped this
    # script in a `timeout` sized for a single boot will see the retry killed,
    # but the verdict survives, so their next run takes the direct path and
    # succeeds. Recording after the retry would lose that.
    if [[ "$SHELL_LAUNCHER_STAGED" == "true" &&
          "${AXL_SHELL_LAUNCHER:-auto}" != "1" &&
          "${AXL_SHELL_LAUNCHER_RETRIED:-0}" != "1" ]] &&
       ! grep -qF "$APP_OUTPUT_SENTINEL" "$CLEAN"; then
        shell_launcher_memo_record "$ARCH" "$SHELL_LAUNCHER_PATH"
        echo "[run-qemu] shell launcher did not reach the Shell on this firmware;" >&2
        echo "[run-qemu] retrying with a direct Shell boot (recorded, so later runs skip it)." >&2
        export AXL_SHELL_LAUNCHER=0
        export AXL_SHELL_LAUNCHER_RETRIED=1
        # A bare $0 (invoked as `bash run-qemu.sh` from its own directory) would
        # PATH-search as a command and die 127. `source $(dirname $0)/...` above
        # resolves relative to the cwd, so match that.
        _self="$0"
        [[ "$_self" == */* ]] || _self="./$_self"
        "$_self" "${ORIG_ARGV[@]}"
        exit $?
    fi

    _rq_print_output "$CLEAN"
    _rq_print_fw_debug "$FW_DEBUG_LOG" "$CLEAN"

    # CPU-spike summary + optional CPU-REPORT. Runs after the serial output so a
    # WARN is the last thing the user sees. A sustained post-warm-up spike exits
    # CPU_SPIKE_EXIT -- but only here, AFTER the empty-log guest-failure check above
    # (exit 1) has taken precedence.
    cpu_spike_rc=0
    cpu_monitor_finish || cpu_spike_rc=$?
    # The duration budget is the compute-shaped run's gate (see --workload).
    # A spike verdict outranks it only because it is the more specific
    # diagnosis; either way the run fails with its own distinct code.
    if [[ "$cpu_spike_rc" == "0" ]]; then
        duration_summary $(( $(now_ms) - RUN_START_MS )) || cpu_spike_rc=$?
    fi
    exit "$cpu_spike_rc"
fi
