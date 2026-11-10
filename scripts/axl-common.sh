# AXL Common Library
# Generic build infrastructure for AXL and consumer projects.
# Provides logging, QEMU/firmware discovery, and arch helpers.

# Source guard — safe to source multiple times
[[ -n "${_AXL_COMMON_SH_SOURCED:-}" ]] && return 0
_AXL_COMMON_SH_SOURCED=1

# --------------------------------------------------------------------------
# AXL_DIR — root of libaxl, resolved from this file's location
# --------------------------------------------------------------------------

AXL_DIR="${AXL_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# --------------------------------------------------------------------------
# Defaults — override via environment
#
# QEMU_DIR / MKIMAGE_DIR are intentionally NOT defaulted here.
# find_qemu() does a 3-tier search:
#   1. Honor an explicit QEMU_DIR override (power-user opt-in)
#   2. command -v on $PATH (system install — apt/dnf qemu-system-*)
#   3. Fall back to $HOME/projects/qemu/install/bin (legacy custom build)
# Hard-coding the legacy path as a default broke downstream consumers
# on machines that have system QEMU/OVMF installed via the package
# manager but no AXL custom build tree.
# --------------------------------------------------------------------------

# --------------------------------------------------------------------------
# Colors and logging
# --------------------------------------------------------------------------

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[INFO]${NC} $1" >&2; }
log_success() { echo -e "${GREEN}[OK]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[WARN]${NC} $1" >&2; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1" >&2; }

# --------------------------------------------------------------------------
# axl_trap_cleanup / axl_exit_on_signal / axl_die_by_signal — a signal ENDS
# the script, and runs its cleanup on the way
#
#   axl_trap_cleanup 'rm -rf "$WORK"'  # EXIT trap + INT/TERM handlers
#   axl_trap_cleanup 'rm -rf "$D"' INT TERM HUP    # choose the signals
#   trap 'rm -rf "$OTHER"' EXIT        # replacing the EXIT trap later is
#                                      # fine: handlers read it on arrival
#
# THE SHAPE THIS REPLACES, and why it is wrong: `trap cleanup EXIT INT TERM`.
# A bash trap handler that does not exit RESUMES the script where the signal
# interrupted it, so a `timeout` kill ran the cleanup (deleting the script's
# own work dir) and then carried on running against the deleted files. In a
# test that meant polling a deleted serial log until its wait expired and then
# reporting a verdict -- CI read "the event loop never reached its deadline --
# it is wedged" for a test the runner had killed.
#
# THE OBVIOUS FIX IS ALSO WRONG: re-raise the signal with its default action
# and trust bash to run the EXIT trap on the way out. It does that for a
# signal that was never trapped -- but a signal trapped BEFORE any EXIT trap
# existed has SIG_DFL recorded as its original disposition, so `trap - TERM;
# kill -TERM $$` kills the shell outright and skips every EXIT trap (measured,
# bash 5.2). So the handler runs the EXIT trap ITSELF, clears it so it cannot
# run twice, and only then dies by the signal -- whose death, not an exit
# status, is what a caller waiting on Ctrl-C needs to see.
#
# It reads the EXIT trap at signal time, so a script may install or replace
# its EXIT trap after this, any number of times. $BASHPID, not $$: in a
# subshell $$ is the PARENT, which this must never signal. If the re-raise
# does not end the shell (the signal was ignored when the script started), it
# exits with 128 + the signal number, so it can never resume either way.
#
# HARDENED AGAINST THREE MORE EDGE CASES (review finding N-1), none of which
# any shipped trap hits today, but the eval-in-a-function shape invites them
# for the next one that does:
#   - a second signal landing DURING the cleanup below used to kill the shell
#     outright, mid-cleanup (`trap - EXIT "$sig"` cleared this signal's trap
#     before running the cleanup it guards). INT/TERM/HUP are ignored for the
#     cleanup's own duration instead.
#   - a bare `return` in a captured EXIT trap, eval'd inline, would return
#     from axl_die_by_signal ITSELF -- skipping the kill below and resuming
#     the script, the exact bug class this file exists to fix, reopened one
#     level down. The eval now runs inside a nested wrapper function, so
#     `return` only returns from that.
#   - `exit $?` in a captured EXIT trap exited 0, because $? by that point was
#     whatever axl_die_by_signal's OWN prior command left it as (normally 0),
#     not the signal death this function reports. A subshell exit right
#     before the eval presets $? to the conventional 128+n first.
# --------------------------------------------------------------------------

# The current EXIT trap's command, or nothing. `trap -p` prints
# `trap -- '<cmd>' EXIT`, quoted for re-reading, so this recovers <cmd>
# exactly -- also from inside $(...), where bash still reports the parent's
# traps.
axl_exit_trap_cmd() {
    local t
    t=$(trap -p EXIT)
    [[ -n "$t" ]] || return 0
    eval "set -- $t"
    printf '%s' "$3"
}

# axl_die_by_signal's helper: eval CMD ($1) with $? preset to RC ($2). A
# nested function rather than eval'ing CMD directly in axl_die_by_signal so a
# bare `return` inside CMD returns only from here.
_axl_run_exit_trap() {
    ( exit "$2" )
    eval "$1"
}

axl_die_by_signal() {
    local sig="$1" on_exit rc
    # IGNORE INT/TERM/HUP -- every signal this file's trap helpers install by
    # default -- and the one that came, FIRST, before anything forks, and for the whole cleanup.
    # The signal that brought us here rarely comes alone: coreutils `timeout`
    # sends it to its child AND then to the child's whole process group, and
    # a runner or a user may send more. A handler (even `:`) is reset to the
    # default action in every subshell, so a repeat landing while one of the
    # $(...) below runs KILLED that subshell: an empty `kill -l` made the
    # arithmetic a syntax error, which aborted this function and RESUMED the
    # script -- a run-qemu.sh then dropped the guard that kills its guest and
    # exited leaving the guest running to its --timeout (measured: 8 runs in
    # 48 under parallel load, and 9 in 10 under a TERM storm). An empty
    # capture of the EXIT trap skipped the cleanup outright; a truncated one
    # failed to parse. SIG_IGN, unlike a handler, is inherited by subshells
    # and commands, so the captures and the cleanup's own children (an
    # `rm -rf` cut short left the state dir behind) run to completion. The
    # cost, deliberate: a command the cleanup starts cannot be stopped by a
    # repeat INT/TERM/HUP either -- a cleanup must not wait on anything
    # unbounded (KILL still ends it).
    trap '' INT TERM HUP "$sig"
    rc=$(( 128 + $(kill -l "$sig") ))
    on_exit=$(axl_exit_trap_cmd)
    trap - EXIT
    # `|| true`: under set -e a failing cleanup step must not cut the rest of
    # the cleanup short.
    if [[ -n "$on_exit" ]]; then _axl_run_exit_trap "$on_exit" "$rc" || true; fi
    trap - INT TERM HUP "$sig"
    kill -s "$sig" "$BASHPID"
    exit "$rc"
}

axl_exit_on_signal() {
    local sig
    [[ $# -gt 0 ]] || set -- INT TERM
    for sig in "$@"; do
        # shellcheck disable=SC2064  # $sig is meant to expand NOW
        trap "axl_die_by_signal $sig" "$sig"
    done
}

# axl_trap_cleanup CMD [SIG...] -- CMD as the EXIT trap, and the signals
# (default INT TERM) running it once and ending the script. The drop-in for
# `trap CMD EXIT INT TERM`.
axl_trap_cleanup() {
    trap "$1" EXIT
    shift
    axl_exit_on_signal "$@"
}

# axl_pdeath_init -- set AXL_PDEATH to the command prefix that makes a child
# die with its parent (`setpriv --pdeathsig KILL`), or to () where that is not
# available. Run a long-lived child as `"${AXL_PDEATH[@]}" cmd ...`.
#
# PR_SET_PDEATHSIG survives the exec into the command, so when the process
# that started it dies -- by ANY signal, SIGKILL included, which runs no trap
# -- the kernel kills the child too. run-qemu.sh ties a --background guest to
# its `timeout` watchdog this way, and test_run_background ties a test's guest
# to the test shell: without it, a SIGKILLed test left its QEMU running at
# ppid 1, holding its ports and /dev/shm memory.
#
# setpriv is util-linux >= 2.33; probed rather than assumed, because these
# scripts run on CI runners and dev boxes alike and must not gain a hard
# dependency. `command -v` alone is not enough -- an older setpriv exists but
# rejects --pdeathsig, and a launch that fails at exec is worse than the leak
# -- so the probe actually runs the flag once.
axl_pdeath_init() {
    AXL_PDEATH=()
    if command -v setpriv >/dev/null 2>&1 \
            && setpriv --pdeathsig KILL true >/dev/null 2>&1; then
        AXL_PDEATH=(setpriv --pdeathsig KILL)
    fi
}

# --------------------------------------------------------------------------
# axl_version_string / axl_handle_version — one answer to `--version`
#
#   axl_handle_version <prog> "$@" && exit 0
#
# Every AXL command answers `--version` in the same shape the target-side
# tools do -- "<prog> <version>", from AXL_TOOL_MAIN via axl_version_handle()
# in axl-version.h. Before this, 8 of the 11 host commands answered `rc=2,
# unknown argument` while all 37 target tools answered correctly.
#
# WHERE THE VERSION COMES FROM is the part with a history. A checkout answers
# from its OWN VERSION file; a staged or installed prefix answers from
# share/axl/version beside it. Never the other way round: axl-cc once printed
# a six-week-old staged version while running out of a tree that had just
# built something else, because `install.sh --prefix .` had left a
# share/axl/version at the repo root. That string gets pasted into reports as
# evidence of which build was measured, so the checkout's own VERSION wins
# whenever there is one.
# --------------------------------------------------------------------------

axl_version_string() {
    local d

    # A checkout: the tree's own VERSION is the truth, even if a stale
    # staged prefix was installed on top of it.
    if [[ -r "$AXL_DIR/VERSION" ]]; then
        cat "$AXL_DIR/VERSION"
        return 0
    fi

    # A staged/installed prefix: share/axl/version sits above this file.
    d="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    while [[ "$d" != "/" ]]; do
        if [[ -r "$d/share/axl/version" ]]; then
            cat "$d/share/axl/version"
            return 0
        fi
        d="$(dirname "$d")"
    done

    # Say so rather than inventing one. A wrong version is worse than none.
    echo "unknown"
    return 1
}

# ONLY the first argument, which is where this DIVERGES from the target-side
# axl_version_handle() on purpose. That one scans all of argv, which is safe
# for a tool whose argv is entirely its own -- but these host commands WRAP
# another command line. run-qemu.sh forwards everything after the .efi to the
# guest, so a full scan makes `run-qemu.sh lsacpi.efi --version` print
# run-qemu's version and never boot; profile-qemu.sh shadows APP_ARGS the same
# way. Both were live regressions caught in review.
#
# Scanning to the first NON-option instead would not work either: without
# knowing which options take arguments, `--timeout 90 --version` stops at the
# 90. Position 1 is the only rule that cannot be ambiguous, and it is what
# `axl` itself already does with its `case "${1-}"`.
axl_handle_version() {
    local prog="$1"; shift

    case "${1-}" in
        --version|-V)
            printf '%s %s\n' "$prog" "$(axl_version_string)"
            return 0
            ;;
    esac
    return 1
}

# --------------------------------------------------------------------------
# axl_warn_stale_sdk_prefix — report a staged SDK that nothing refreshes
#
#   axl_warn_stale_sdk_prefix <installed-prefix> <sdk-source-root>
#
# A staged SDK left at a location that is no longer anyone's --prefix does not
# go away and does not get updated; it just keeps serving whatever build it was
# when it was written. Two such locations exist, both historical defaults:
#
#   <root>/out   the pre-O1 default, before O1 moved it to ./stage
#   <root>       `install.sh --prefix .`, from before ./out was the default
#
# The second is the dangerous one, because scripts/axl-cc resolves SDK_DIR to
# the repo root: leftovers there make the source script behave like a staged
# driver and answer for the version it was installed at. Reported from a
# consumer build against the dev tip, where `scripts/axl-cc --version` said
# 2.8.7 (2026-07-08) inside a checkout that had just staged 4.2.0 -- and the
# four leftover paths are gitignored by name, so `git status` showed clean
# throughout. That corrupts a MEASUREMENT rather than a build: it compiles,
# links, boots and passes, against a library six weeks old.
#
# WARNED, NOT DELETED, and the asymmetry with the compat/ removal in install.sh
# is deliberate: compat/ sits inside the prefix that run owns and is its to
# clean, whereas both of these are directories the invocation was not asked to
# touch. Removing files outside your own --prefix is a surprise nobody asked
# for.
#
# The removal advice differs per candidate and that is load-bearing. At the
# source root, `share/` and `include/` hold TRACKED SOURCES (share/*.json5,
# include/axl/*.h), so the ./out message's `{bin,lib,include,share}` brace
# expansion would delete the tree it is trying to protect.
# --------------------------------------------------------------------------

# _axl_abs_path — canonical form, so two spellings of one directory compare
# equal. `install.sh --prefix .` is a deliberate in-tree install and must not
# be warned about, but PREFIX arrives from the command line verbatim and is
# never normalised, so a literal `!=` compares "." against an absolute path.
_axl_abs_path() {
    if [[ -d "$1" ]]; then
        ( cd -P "$1" 2>/dev/null && pwd -P ) || printf '%s\n' "${1%/}"
    else
        printf '%s\n' "${1%/}"
    fi
}

# _axl_staged_sdk_version — the version a staged SDK at <dir> would serve, or
# rc 1 if <dir> does not look like one.
#
# Three signals, any one of which is enough, because a HALF-cleaned prefix
# still lies. The reported failure came through scripts/axl-cc, which reads
# share/axl/version and links against lib/axl/<arch> and never looks at
# bin/axl-cc -- so keying on the driver alone (as the original ./out check did)
# misses a tree that has lost its bin/ and kept its answer.
_axl_staged_sdk_version() {
    local d="$1"
    [[ -f "$d/share/axl/version" || -d "$d/lib/axl" || -x "$d/bin/axl-cc" ]] \
        || return 1
    local v
    v="$(cat "$d/share/axl/version" 2>/dev/null)" || v=""
    printf '%s\n' "${v:-unknown}"
}

axl_warn_stale_sdk_prefix() {
    local prefix_abs root_abs old_abs ver
    prefix_abs="$(_axl_abs_path "$1")"
    root_abs="$(_axl_abs_path "$2")"
    old_abs="$root_abs/out"

    if [[ "$prefix_abs" != "$root_abs" ]] && ver="$(_axl_staged_sdk_version "$root_abs")"; then
        log_warning "a staged SDK (version $ver) is sitting IN THE SOURCE TREE at $root_abs."
        log_warning "Nothing refreshes it now, and scripts/axl-cc resolves its SDK root"
        log_warning "there -- so it answers for that build, not for this checkout."
        # -e, not -d: in a git WORKTREE .git is a file, and this tree uses
        # worktrees. Testing for a directory sends worktree users down the
        # tarball branch -- still correct advice, just not the tidier one.
        if [[ -e "$root_abs/.git" ]]; then
            # Those four paths are gitignored by name, so -X removes exactly
            # them; nothing tracked is reachable by this command. Carries its
            # own cd: the reader is not necessarily standing in the tree being
            # named, and these are relative pathspecs.
            log_warning "Remove it with: cd $root_abs && git clean -Xdf -- bin lib include/axl-sdk share/axl"
        else
            # A source tarball has no .git. Name the paths -- and name them
            # SUFFIXED, since include/ and share/ hold sources of their own.
            log_warning "Remove it with: rm -rf $root_abs/bin $root_abs/lib $root_abs/include/axl-sdk $root_abs/share/axl"
        fi
    fi

    if [[ "$prefix_abs" != "$old_abs" ]] && ver="$(_axl_staged_sdk_version "$old_abs")"; then
        log_warning "a staged SDK (version $ver) remains at $old_abs (the pre-O1 default)."
        log_warning "Nothing refreshes it now -- it will go stale silently."
        # Safe here where it is not one level up: out/ holds build trees and
        # Sphinx output, never sources.
        log_warning "Remove it with: rm -rf $old_abs/{bin,lib,include,share}"
    fi
}

# --------------------------------------------------------------------------
# UNIX socket path length
# --------------------------------------------------------------------------
#
# A UNIX socket path lives in sockaddr_un.sun_path, which is 108 bytes on
# Linux INCLUDING the terminating NUL -- so 107 is the longest usable path.
# QEMU refuses a longer one with a single line in its own log, and what a
# caller sees is something else entirely: a screenshot suite reporting every
# case failed with zero PNGs, which reads exactly like a render regression.
# So every place that builds a socket path checks it here first, and names the
# setting that has to change.
AXL_SUN_PATH_MAX=107

# The longest socket NAME a test may create in its own scratch dir
# (common-test.sh's TEST_TMPDIR). That dir sits one level below
# $AXL_QEMU_TMPDIR, and which names a test uses is up to the test, so the
# check up front can only reserve room. test-run-qemu-flags.sh fails if a
# literal *.sock name under test/integration/ (and its lib/) outgrows it.
AXL_TEST_SOCK_NAME_MAX=32

# axl_sock_path_check KNOB PATH [RESERVE] -- 0 if PATH, plus RESERVE more
# bytes of socket name still to be appended, fits in sun_path. Otherwise
# print one error naming KNOB (the setting the caller must change), the
# length and the limit, and return 1. Byte length, not characters: sun_path
# counts bytes.
axl_sock_path_check() {
    local knob="$1" path="$2" reserve="${3:-0}" len shown
    len=$(( $(printf '%s' "$path" | wc -c) + reserve ))
    (( len <= AXL_SUN_PATH_MAX )) && return 0
    shown="$path"
    (( reserve > 0 )) && shown="$path<$reserve-byte socket name>"
    echo "ERROR: $knob is too long for a UNIX socket path: $shown is $len bytes; the limit is $AXL_SUN_PATH_MAX (sun_path holds 108 including the NUL)." >&2
    echo "       Shorten $knob by at least $(( len - AXL_SUN_PATH_MAX )) byte(s)." >&2
    return 1
}

# axl_mktemp_dir_template BASE TEMPLATE -- the path `mktemp -d -p BASE
# TEMPLATE` will create, X's and all. mktemp drops EVERY trailing slash from
# BASE ("/x//" gives "/x/..."), so this strips them all; stripping one would
# count a byte that is not there and refuse a path that fits.
axl_mktemp_dir_template() {
    local base="$1"
    while [[ "$base" == */ && "$base" != / ]]; do base="${base%/}"; done
    printf '%s/%s' "${base%/}" "$2"
}

# axl_test_tmpdir_check BASE [KNOB] [TEMPLATE] -- the check for a
# test-scratch base: BASE is where common-test.sh's test_setup makes its
# scratch dir (default template axl-ctest.XXXXXXXX), the deepest place the
# suite puts a socket. run-qemu.sh's own dir (axl-qemu.XXXXXXXX/virtiofs.sock)
# is shallower than this, so one check covers both. KNOB (default
# AXL_QEMU_TMPDIR) is the setting the error names; a plain `mktemp -d` takes
# TMPDIR and tmp.XXXXXXXXXX instead.
axl_test_tmpdir_check() {
    axl_sock_path_check "${2:-AXL_QEMU_TMPDIR}" \
        "$(axl_mktemp_dir_template "$1" "${3:-axl-ctest.XXXXXXXX}")/" \
        "$AXL_TEST_SOCK_NAME_MAX"
}

# --------------------------------------------------------------------------
# arch_dir — map EDK2 arch name to image directory name
# --------------------------------------------------------------------------

arch_dir() {
    case "$1" in
        X64)     echo "x64" ;;
        AARCH64) echo "aa64" ;;
        *)       echo "$1" ;;
    esac
}

# boot_efi_name <arch> — return the EFI boot binary filename
boot_efi_name() {
    case "$1" in
        X64)     echo "BOOTX64.EFI" ;;
        AARCH64) echo "BOOTAA64.EFI" ;;
    esac
}

# The header line of a firmware CPU-exception dump on AARCH64 (ArmPkg's
# DefaultExceptionHandler: its four exception types). The header is the
# console's; the registers, backtrace and stack dump after it are DEBUG, on the
# debug UART -- which is how a reader knows to fetch them from there.
AXL_FW_EXCEPTION_RE='(Synchronous|SError|IRQ|FIQ) Exception at 0x'

# --------------------------------------------------------------------------
# qemu_fw_debug_uart <array-name> <arch> <file>
# Gives a guest a channel of its own for the firmware's DEBUG output (ASSERTs
# included), written to <file>, so the console carries none of it.
#
# AARCH64: a SECOND serial port. The bundled ArmVirtQemu firmware
# (edk2-stable202408) sends DEBUG to the second PL011 when there is one; with
# one it shares the console. A CPU exception is split: its "<type> Exception
# at ..." header stays on the console (SerialPortWrite), its registers and
# backtrace are DEBUG (run-qemu.sh and common-test.sh bring them back beside
# the console's log). Call it LAST, on the whole command: the UARTs follow
# -serial order on `virt`, so the debug one goes after the console's, and a
# -nographic command gets its console spelled out first -- any explicit
# -serial drops the one -nographic would add, and the debug file would become
# UART0. The spelling is QEMU's own (system/vl.c): `mon:stdio`, unless an
# option took the monitor (-monitor, -mon, -qmp, -qmp-pretty), then `stdio`.
# A -nodefaults command with no console gets nothing: there is no console UART
# to come second to.
#
# X64: OVMF's debug port, an isa-debugcon at I/O port 0x402 (OVMF already
# keeps DEBUG off the console; without the port it is simply lost, and a
# firmware ASSERT shows only as a hang). The device on a file chardev is
# `-debugcon file:<file> -global isa-debugcon.iobase=0x402` spelled so that
# it can append. It is no UART, so order and -nodefaults do not matter. OVMF
# probes the port and writes nothing when it is absent; when it is present
# every byte is a VM exit -- which is why a booted X64 run does not get it
# unasked (docs/AXL-Test-Guest-Pool-Design.md, "a debugcon on every pooled
# X64 guest"). Neither can be added to a running guest.
#
# append=on: a pooled guest's cold start relaunches the recorded argv, and must
# not truncate the record a lease reads from an offset. A caller that wants
# this launch's output alone truncates <file> itself. Idempotent.
# --------------------------------------------------------------------------
qemu_fw_debug_uart() {
    local -n _fq="$1"
    local chardev="file,id=axl_fwdebug,path=${3//,/,,},append=on"
    local a serial=false nographic=false mon=false nodefaults=false
    for a in ${_fq[@]+"${_fq[@]}"}; do
        case "$a" in
            chardev:axl_fwdebug|*,chardev=axl_fwdebug) return 0 ;;
            -serial|--serial) serial=true ;;
            -nographic|--nographic) nographic=true ;;
            -nodefaults|--nodefaults) nodefaults=true ;;
            -monitor|--monitor|-mon|--mon|-qmp|--qmp|-qmp-pretty|--qmp-pretty) mon=true ;;
        esac
    done
    case "$2" in
        X64)
            _fq+=(-chardev "$chardev" -device isa-debugcon,iobase=0x402,chardev=axl_fwdebug)
            return 0 ;;
        AARCH64) ;;
        *) return 0 ;;
    esac
    if [[ "$serial" == false && "$nodefaults" == true ]]; then
        return 0
    fi
    if [[ "$serial" == false && "$nographic" == true ]]; then
        if [[ "$mon" == true ]]; then _fq+=(-serial stdio); else _fq+=(-serial mon:stdio); fi
    fi
    _fq+=(-chardev "$chardev" -serial chardev:axl_fwdebug)
}

# --------------------------------------------------------------------------
# build_qemu_base_cmd <arch> <qemu_bin> <mem> <vars_file> [cpu_override] [devices]
# Outputs the base QEMU arguments (machine, CPU, memory, pflash) to stdout,
# NUL-separated for safe consumption via mapfile -d ''.
#
# devices (optional): `standard` appends the standard guest's devices
# (qemu_standard_devices below) -- what common-test.sh and run-qemu.sh
# --bridges boot, so both build ONE guest shape. Without it the output is the
# bare machine, as every other caller (and an outside consumer) has always
# had it.
# Requires FW_CODE to be set by find_firmware() before calling.
# Usage: mapfile -d '' -t cmd < <(build_qemu_base_cmd X64 /path/qemu 512M vars.fd)
#
# cpu_override (optional): replaces the default `-cpu` model with a
# caller-supplied spec (HF4 --cpu-from-fixture replay, e.g.
# "qemu64,vendor=GenuineIntel,family=6,model=42" on x86 or
# "max,midr=0x410fd0b0" on aarch64). KVM stays enabled when usable — the
# guest CPUID/MIDR is synthesised from the chosen model + overrides.
# --------------------------------------------------------------------------

build_qemu_base_cmd() {
    local arch="$1" qemu_bin="$2" mem="$3" vars_file="$4" cpu_override="${5:-}"
    local devices="${6:-}"

    if [[ -z "${FW_CODE:-}" ]]; then
        log_error "build_qemu_base_cmd: FW_CODE not set (call find_firmware first)"
        return 1
    fi

    printf '%s\0' "$qemu_bin"

    case "$arch" in
        X64)
            printf '%s\0' "-machine" "q35"
            # KVM acceleration only when /dev/kvm is actually usable.
            # Some hosts (CI runners, locked-down workstations) expose
            # the device node but block read/write — `-enable-kvm` then
            # makes QEMU exit immediately with no diagnostic.
            if [[ -r /dev/kvm && -w /dev/kvm ]]; then
                printf '%s\0' "-enable-kvm"
                printf '%s\0' "-cpu" "${cpu_override:-host}"
            elif [[ -n "$cpu_override" ]]; then
                printf '%s\0' "-cpu" "$cpu_override"
            fi
            ;;
        AARCH64)
            printf '%s\0' "-machine" "virt" "-cpu" "${cpu_override:-cortex-a57}"
            # NetworkPkg drivers (MnpDxe, Ip4Dxe, ...) gained a DEPEX on
            # gEfiRngProtocolGuid after the PixieFail CVE fix. The QEMU
            # virt machine has no hardware TRNG, so RngDxe's entry point
            # fails and the protocol is never installed — leaving every
            # network driver in "discovered but not loaded" state forever.
            # virtio-rng-pci lets VirtioRngDxe (in the firmware) bind and
            # install the protocol, unblocking the entire network stack.
            # The standard devices carry their own, behind the root port;
            # a second one here was an accidental duplicate.
            [[ "$devices" == standard ]] || printf '%s\0' "-device" "virtio-rng-pci"
            ;;
    esac

    printf '%s\0' "-m" "$mem"
    printf '%s\0' "-drive" "if=pflash,format=raw,readonly=on,file=$FW_CODE"
    # The vars file's format follows its name: a .qcow2 copy is what a
    # snapshot can capture (run-qemu.sh --qcow2); everything else is raw.
    local vars_fmt=raw
    [[ "$vars_file" == *.qcow2 ]] && vars_fmt=qcow2
    printf '%s\0' "-drive" "if=pflash,format=$vars_fmt,file=$vars_file"
    [[ "$devices" == standard ]] && qemu_standard_devices
    return 0
}

# --------------------------------------------------------------------------
# qemu_boot_wait_args
# Skip the firmware Boot Manager countdown before auto-booting the first
# option (the UEFI shell). QEMU publishes this as the `etc/boot-menu-wait`
# fw_cfg, sourced from `-boot splash-time=N` (ms); OVMF's
# GetFrontPageTimeoutFromQemu() reads it and, when 0, boots immediately
# (otherwise it defaults to ~5 s). Faster, more deterministic boots -- and
# visual-capture timing no longer rides on a 5 s wait. Set QEMU_BOOT_MENU_WAIT
# to a non-empty ms value to restore a countdown. Both launchers use it.
# --------------------------------------------------------------------------
qemu_boot_wait_args() {
    printf '%s\0' "-boot" "splash-time=${QEMU_BOOT_MENU_WAIT:-0}"
}

# --------------------------------------------------------------------------
# qemu_standard_devices
# The standard guest's devices, NUL-separated: the census's commonest guest
# (docs/AXL-Test-Guest-Pool-Design.md §7), one shape a resident pool serves.
#
#   PCI   one PCIe root port (a PCI-PCI bridge per the spec) with a virtio-rng
#         behind it, so the AxlPci tree walker has a bridge to cross -- stock
#         q35 and virt are otherwise flat, and shipping bridge code without
#         that coverage is how the aa64 cap-walk infinite loop (commit
#         8b90954) reached CI. On AARCH64 the RNG is also what NetworkPkg's
#         DEPEX waits for (see build_qemu_base_cmd), and it is the only one.
#   USB   a qemu-xhci with a usb-mouse (port 1) and a usb-hub (port 2)
#         carrying a usb-tablet (port 2.1): three EFI_USB_IO handles, one a
#         hub tier deep. The unit tests pin the mouse (QEMU vid 0x0627, the
#         HID Boot Mouse triplet) and need the tablet, the only source of
#         EFI_ABSOLUTE_POINTER_PROTOCOL.
#
# SLOTS ARE PINNED: the root port at 00:02.0 and xHCI at 00:03.0, on both
# arches. q35's VGA holds 00:01.0 and virt's slot 1 is free, so left to QEMU
# the same guest numbered differently per arch and shifted whenever a device
# ahead of it came or went (removing AARCH64's duplicate RNG moved its xHCI
# from 00:03.0 to 00:02.0). A test pins BDFs in exact strings; a pool needs
# the topology fixed. The bridge's secondary bus is still the firmware's.
#
# Cost, measured 2026-11-03: the PCI half nothing; each USB device ~0.2 s of
# firmware enumeration per boot (X64 2.25 s -> 2.78 s, AARCH64 6.7 s -> 7.3 s).
# --------------------------------------------------------------------------
qemu_standard_devices() {
    printf '%s\0' "-device" "pcie-root-port,id=axl_rp0,bus=pcie.0,addr=0x2,chassis=1"
    printf '%s\0' "-device" "virtio-rng-pci,bus=axl_rp0"
    printf '%s\0' "-device" "qemu-xhci,id=axl_usb0,bus=pcie.0,addr=0x3"
    printf '%s\0' "-device" "usb-mouse,bus=axl_usb0.0,port=1"
    printf '%s\0' "-device" "usb-hub,bus=axl_usb0.0,port=2"
    printf '%s\0' "-device" "usb-tablet,bus=axl_usb0.0,port=2.1"
}

# --------------------------------------------------------------------------
# qemu_stage_disk <staging_dir> <target_img> [label]
#
# Build a FAT32 disk image from a staging directory. Prefers mkimage.py when
# MKIMAGE_DIR points at it (private convenience tooling); otherwise plain mtools
# (mkfs.vfat + mcopy) — the path CI and the released host-tools actually use.
# Validates the mtools deps up front with an install hint, and surfaces
# mkfs/mcopy failures rather than swallowing them. Shared by run-qemu.sh and the
# integration harness (common-test.sh) so the two cannot drift. Returns non-zero
# on a missing tool, or when dd, mkfs.vfat or any mcopy fails. `find` prints top-level entries so mcopy -s auto-creates the
# destination tree at the FAT root (a per-file loop trips mtools on nested paths).
# --------------------------------------------------------------------------
qemu_stage_disk() {
    local staging="$1" target="$2" label="${3:-AXL}"

    if [[ -n "${MKIMAGE_DIR:-}" && -f "$MKIMAGE_DIR/mkimage.py" ]]; then
        "$MKIMAGE_DIR/mkimage.py" --source "$staging" --target "$target" \
            --label "$label" >/dev/null 2>&1
        return
    fi

    local missing=()
    command -v mkfs.vfat >/dev/null 2>&1 || missing+=("mkfs.vfat (dosfstools)")
    command -v mcopy     >/dev/null 2>&1 || missing+=("mcopy (mtools)")
    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "disk-image build needs tools that are not installed: ${missing[*]}
  Install:  Debian/Ubuntu: sudo apt install dosfstools mtools
            Fedora/RHEL:   sudo dnf install dosfstools mtools
            Arch:          sudo pacman -S dosfstools mtools
  Or set MKIMAGE_DIR=/path/to/mkimage to use the mkimage backend instead."
        return 1
    fi

    local size_kb
    size_kb=$(du -sk "$staging" | cut -f1)
    size_kb=$(( (size_kb + 4096) / 1024 * 1024 ))   # round up to a whole MB
    [[ $size_kb -lt 40960 ]] && size_kb=40960        # minimum 40 MB
    dd if=/dev/zero of="$target" bs=1K count="$size_kb" 2>/dev/null || return $?
    mkfs.vfat -F 32 -n "$label" "$target" >/dev/null || return $?
    (
        cd "$staging" || exit 1
        # NUL-separated, so a staged name with a space in it is one entry;
        # `./` so one starting with `-` is not read as an option; stdin from
        # /dev/null so mcopy (which may prompt on a name clash) cannot eat
        # the list; and the first failure is the result, as promised above.
        local entry
        while IFS= read -r -d '' entry; do
            mcopy -s -i "$target" "./$entry" "::/" < /dev/null || exit $?
        done < <(find . -maxdepth 1 -mindepth 1 -printf '%P\0')
    )
}

# --------------------------------------------------------------------------
# qemu_strip_kvm <array_name>
#
# Drop -enable-kvm and -cpu <model> from the named QEMU command array in place
# (a nameref). KVM is incompatible with single-stepping early-boot instructions,
# so a --gdb / TEST_QEMU_GDB session must fall back to TCG. Shared by run-qemu.sh
# and common-test.sh.
# --------------------------------------------------------------------------
qemu_strip_kvm() {
    local -n _cmd="$1"
    local out=() arg skip=0
    for arg in "${_cmd[@]}"; do
        if (( skip )); then skip=0; continue; fi
        case "$arg" in
            -enable-kvm) ;;          # drop
            -cpu)        skip=1 ;;   # drop with its value
            *)           out+=("$arg") ;;
        esac
    done
    _cmd=("${out[@]}")
}

# --------------------------------------------------------------------------
# axl_census_argv ARG... — a QEMU argv as the guest-pool census records it
# (test/integration/lib/profile.sh, _prof_launch; scripts/run-qemu.sh).
#
# Prints the arguments joined with US (0x1f), which no QEMU argument contains,
# so the reader splits them exactly. Every fixture FILE handed to the guest --
# `-smbios`/`-acpitable`/`-fw_cfg`/`-device`/`-tpmdev` `file=` or `romfile=`
# -- is replaced by `sha256:<16 hex>` of its contents: these are usually
# per-run temp copies, so once the run is over the path names nothing, and two
# fixtures written under the same temp name would otherwise read as one guest.
# A newline inside an argument would tear the one-line record; it becomes a
# space.
# --------------------------------------------------------------------------

axl_census_argv() {
    local out=() prev="" a key f d
    for a in "$@"; do
        case "$prev" in
            -smbios|-acpitable|-fw_cfg|-device|-tpmdev)
                if [[ "$a" =~ (^|,)(file|romfile)=([^,]+) ]]; then
                    key="${BASH_REMATCH[2]}" f="${BASH_REMATCH[3]}"
                    # Unreadable or failed: keep the raw value. An empty digest
                    # would key every such fixture alike, and a message here
                    # would land in the test's own log.
                    if [[ -f "$f" && -r "$f" ]] \
                       && d=$(sha256sum "$f" 2>/dev/null) && [[ -n "$d" ]]; then
                        a="${a/"$key=$f"/"$key=sha256:${d:0:16}"}"
                    fi
                fi ;;
        esac
        prev="$a"
        out+=("${a//$'\n'/ }")
    done
    local IFS=$'\x1f'
    printf '%s' "${out[*]}"
}

# --------------------------------------------------------------------------
# find_qemu — locate QEMU binary for a given architecture
# Usage: QEMU_BIN=$(find_qemu <arch>) || exit 1
#
# 3-tier search:
#   1. $QEMU_DIR (explicit power-user override)
#   2. command -v on $PATH (system install — apt/dnf qemu-system-*)
#   3. $HOME/projects/qemu/install/bin (legacy AXL custom build)
# --------------------------------------------------------------------------

find_qemu() {
    local arch="$1"
    local binary

    case "$arch" in
        X64)     binary="qemu-system-x86_64" ;;
        AARCH64) binary="qemu-system-aarch64" ;;
        *)
            log_error "find_qemu: unknown arch '$arch'"
            return 1
            ;;
    esac

    # 1. Explicit override
    if [[ -n "${QEMU_DIR:-}" && -x "$QEMU_DIR/$binary" ]]; then
        echo "$QEMU_DIR/$binary"
        return 0
    fi

    # 2. System install — search PATH
    if command -v "$binary" &>/dev/null; then
        local resolved
        resolved=$(command -v "$binary")
        # Set QEMU_DIR for downstream callers (find_firmware reads it
        # to discover bundled firmware via $QEMU_DIR/../share/qemu).
        QEMU_DIR=$(dirname "$resolved")
        export QEMU_DIR
        echo "$resolved"
        return 0
    fi

    # 3. Legacy AXL custom build (last resort, kept so existing
    # power-user setups that don't set QEMU_DIR keep working)
    if [[ -x "$HOME/projects/qemu/install/bin/$binary" ]]; then
        QEMU_DIR="$HOME/projects/qemu/install/bin"
        export QEMU_DIR
        echo "$QEMU_DIR/$binary"
        return 0
    fi

    cat >&2 <<EOF
[ERROR] $binary not found in \$PATH or any known location.

  Install:
    Debian/Ubuntu:  sudo apt install qemu-system-x86 qemu-system-arm \\
                                     ovmf qemu-efi-aarch64 \\
                                     virtiofsd mtools dosfstools
    Fedora/RHEL:    sudo dnf install qemu-system-x86 qemu-system-aarch64 \\
                                     edk2-ovmf edk2-aarch64 \\
                                     virtiofsd mtools dosfstools
    Arch:           sudo pacman -S qemu-system-x86 qemu-system-aarch64 \\
                                   edk2-ovmf edk2-armvirt \\
                                   virtiofsd mtools dosfstools
    macOS:          brew install qemu

  Or set QEMU_DIR=/path/to/your/qemu/install/bin
EOF
    return 1
}

# --------------------------------------------------------------------------
# find_firmware — locate OVMF/AAVMF firmware files
# Usage: find_firmware <arch>  (sets FW_CODE and FW_VARS)
# Uses QEMU's bundled RELEASE firmware (no debug spew).
# Override via OVMF_CODE/AAVMF_CODE environment variables.
# --------------------------------------------------------------------------

find_firmware() {
    local arch="$1"
    # QEMU_DIR may be unset if find_firmware/find_shell_efi is called
    # before find_qemu has populated it. Use the :- default to keep
    # `set -u` safe; with QEMU_DIR="" the qemu_share resolves to
    # "/share/qemu" which won't match any real path — fine, the
    # function falls through to the system-package locations.
    local qemu_dir="${QEMU_DIR:-}"
    local qemu_share="${qemu_dir%/bin}/share/qemu"

    case "$arch" in
        X64)
            # Environment override
            if [[ -n "${OVMF_CODE:-}" && -f "$OVMF_CODE" ]]; then
                FW_CODE="$OVMF_CODE"
                FW_VARS="${OVMF_VARS:-${OVMF_CODE%_CODE.fd}_VARS.fd}"
                return 0
            fi
            # QEMU bundled firmware (custom build)
            if [[ -f "$qemu_share/edk2-x86_64-code.fd" ]]; then
                FW_CODE="$qemu_share/edk2-x86_64-code.fd"
                FW_VARS="$qemu_share/edk2-i386-vars.fd"
                return 0
            fi
            # RHEL/AlmaLinux/Fedora: edk2-ovmf package
            if [[ -f /usr/share/edk2/ovmf/OVMF_CODE.fd ]]; then
                FW_CODE=/usr/share/edk2/ovmf/OVMF_CODE.fd
                FW_VARS=/usr/share/edk2/ovmf/OVMF_VARS.fd
                return 0
            fi
            # Ubuntu/Debian: ovmf package
            if [[ -f /usr/share/OVMF/OVMF_CODE.fd ]]; then
                FW_CODE=/usr/share/OVMF/OVMF_CODE.fd
                FW_VARS=/usr/share/OVMF/OVMF_VARS.fd
                return 0
            fi
            # Ubuntu 24.04+: 4MB flash variant
            if [[ -f /usr/share/OVMF/OVMF_CODE_4M.fd ]]; then
                FW_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
                FW_VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
                return 0
            fi
            # Arch Linux
            if [[ -f /usr/share/edk2-ovmf/x64/OVMF_CODE.fd ]]; then
                FW_CODE=/usr/share/edk2-ovmf/x64/OVMF_CODE.fd
                FW_VARS=/usr/share/edk2-ovmf/x64/OVMF_VARS.fd
                return 0
            fi
            cat >&2 <<'EOF'
[ERROR] OVMF firmware not found in any known location.

  Searched: /usr/share/edk2/ovmf, /usr/share/OVMF, /usr/share/edk2-ovmf

  Install:
    Debian/Ubuntu:  sudo apt install ovmf
    Fedora/RHEL:    sudo dnf install edk2-ovmf
    Arch:           sudo pacman -S edk2-ovmf
    macOS:          brew install qemu  # bundles edk2-x86_64-code.fd

  Or set OVMF_CODE=/path/to/OVMF_CODE.fd (and OVMF_VARS=/path/to/OVMF_VARS.fd)
EOF
            return 1
            ;;
        AARCH64)
            # Environment override
            if [[ -n "${AAVMF_CODE:-}" && -f "$AAVMF_CODE" ]]; then
                FW_CODE="$AAVMF_CODE"
                FW_VARS="${AAVMF_VARS:-${AAVMF_CODE%_CODE.fd}_VARS.fd}"
                return 0
            fi
            # QEMU bundled firmware (custom build)
            if [[ -f "$qemu_share/edk2-aarch64-code.fd" ]]; then
                FW_CODE="$qemu_share/edk2-aarch64-code.fd"
                FW_VARS="$qemu_share/edk2-arm-vars.fd"
                return 0
            fi
            # RHEL/AlmaLinux/Fedora: edk2-aarch64 package
            if [[ -f /usr/share/edk2/aarch64/QEMU_EFI-pflash.raw ]]; then
                FW_CODE=/usr/share/edk2/aarch64/QEMU_EFI-pflash.raw
                FW_VARS=/usr/share/edk2/aarch64/vars-template-pflash.raw
                return 0
            fi
            # Ubuntu/Debian: qemu-efi-aarch64 package
            if [[ -f /usr/share/AAVMF/AAVMF_CODE.fd ]]; then
                FW_CODE=/usr/share/AAVMF/AAVMF_CODE.fd
                FW_VARS=/usr/share/AAVMF/AAVMF_VARS.fd
                return 0
            fi
            cat >&2 <<'EOF'
[ERROR] AAVMF firmware not found in any known location.

  Searched: /usr/share/edk2/aarch64, /usr/share/AAVMF

  Install:
    Debian/Ubuntu:  sudo apt install qemu-efi-aarch64
    Fedora/RHEL:    sudo dnf install edk2-aarch64
    Arch:           sudo pacman -S edk2-armvirt

  Or set AAVMF_CODE=/path/to/QEMU_EFI.fd (and AAVMF_VARS=/path/to/QEMU_VARS.fd)
EOF
            return 1
            ;;
        *)
            log_error "find_firmware: unknown arch '$arch'"
            return 1
            ;;
    esac
}

# --------------------------------------------------------------------------
# find_debug_ovmf — locate a DEBUG build of OVMF (x64), for the tests that
# need its cleared-on-free pool, compiled-in ASSERTs or DEBUG log.
# Usage: find_debug_ovmf  (sets DEBUG_OVMF_CODE and DEBUG_OVMF_VARS; returns 1
#        when there is none, with DEBUG_OVMF_SEARCHED naming each place it
#        looked and why it had none: "DIR: missing", "DIR: unreadable",
#        "DIR: no CODE file", "DIR: no VARS file", joined by "; ")
#
# THE one discovery point: every such test goes through it, so where a
# DEBUG OVMF is found is set in ONE place and cannot differ per test or per
# runner account.
#   AXL_OVMF_DEBUG_DIR  a directory holding OVMF_CODE.fd and OVMF_VARS.fd.
#                       When set it is the ONLY place looked: an explicit
#                       choice never quietly falls back to another build.
#   unset               $EDK2_DIR/Build/OvmfX64/DEBUG_GCC5/FV when EDK2_DIR
#                       is set, then $HOME/uefi/Build/OvmfX64/DEBUG_GCC5/FV --
#                       not searched at all when HOME is unset, which would
#                       otherwise make it /uefi/...
# "Not there" and "could not look" are different facts and are reported as
# such: a directory or file this account cannot read is "unreadable".
# --------------------------------------------------------------------------

find_debug_ovmf() {
    local dirs=() why=() d
    local home_fv='$HOME/uefi/Build/OvmfX64/DEBUG_GCC5/FV'
    DEBUG_OVMF_CODE=""
    DEBUG_OVMF_VARS=""
    DEBUG_OVMF_SEARCHED=""
    if [[ -n "${AXL_OVMF_DEBUG_DIR:-}" ]]; then
        dirs=("$AXL_OVMF_DEBUG_DIR")
    else
        [[ -n "${EDK2_DIR:-}" ]] && dirs+=("$EDK2_DIR/Build/OvmfX64/DEBUG_GCC5/FV")
        if [[ -n "${HOME:-}" ]]; then
            dirs+=("$HOME/uefi/Build/OvmfX64/DEBUG_GCC5/FV")
        fi
    fi
    for d in "${dirs[@]}"; do
        if [[ ! -e "$d" ]]; then
            why+=("$d: missing")
        elif [[ ! -r "$d" || ! -x "$d" ]]; then
            why+=("$d: unreadable")
        elif [[ ! -f "$d/OVMF_CODE.fd" ]]; then
            why+=("$d: no CODE file")
        elif [[ ! -f "$d/OVMF_VARS.fd" ]]; then
            why+=("$d: no VARS file")
        elif [[ ! -r "$d/OVMF_CODE.fd" || ! -r "$d/OVMF_VARS.fd" ]]; then
            why+=("$d: unreadable")
        else
            DEBUG_OVMF_CODE="$d/OVMF_CODE.fd"
            DEBUG_OVMF_VARS="$d/OVMF_VARS.fd"
            return 0
        fi
    done
    if [[ -z "${AXL_OVMF_DEBUG_DIR:-}" && -z "${HOME:-}" ]]; then
        why+=("$home_fv: not searched, HOME is unset")
    fi
    for d in "${why[@]}"; do
        DEBUG_OVMF_SEARCHED+="${DEBUG_OVMF_SEARCHED:+; }$d"
    done
    return 1
}

# --------------------------------------------------------------------------
# debug_ovmf_unavailable — end a test that found no DEBUG OVMF.
# A SKIP (exit 77) by default. With AXL_REQUIRE_DEBUG_OVMF=1 -- which
# scripts/release-suite.sh sets -- a FAILURE (exit 1): a skip passes a suite,
# and a release certified by a run that skipped such a test was never checked
# by it.
# --------------------------------------------------------------------------

debug_ovmf_unavailable() {
    local why="no DEBUG OVMF -- $DEBUG_OVMF_SEARCHED (set AXL_OVMF_DEBUG_DIR)"
    if [[ "${AXL_REQUIRE_DEBUG_OVMF:-}" == 1 ]]; then
        echo "FAIL: $why -- this run requires one"
        exit 1
    fi
    echo "SKIP: $why"
    exit 77
}

# --------------------------------------------------------------------------
# find_shell_efi — locate a UEFI Shell binary that MATCHES the active firmware
# Usage: SHELL_EFI=$(find_shell_efi <arch>)
#
# Priority:
#   1. Local EDK2 build (if EDK2_DIR is set)
#   2. Previously extracted Shell, cached next to the firmware .fd
#   3. Extract the firmware's OWN Shell from the active firmware .fd
#   4. System-packaged Shell.efi (distro edk2 / qemu packages) — LAST RESORT
#
# Extraction (3) is preferred over a distro package (4) because the extracted
# Shell is guaranteed ABI-compatible with the firmware in use. A mismatched
# package Shell — e.g. the distro's Shell.efi against a different OVMF/AAVMF
# build — starts but HANGS before its banner (regression from ab2b9762, which
# had preferred the package). The package tier is kept only as a fallback for a
# minimal host that can't extract (no native fwtool, python3, or uefiextract).
#
# Requires: find_firmware() must be called first (sets FW_CODE).
# --------------------------------------------------------------------------

# Shell.efi GUID (same across all EDK2 builds)
_SHELL_GUID="7C04A583-9E3E-4F1C-AD65-E05268D0B4D1"

find_shell_efi() {
    local arch="$1"

    # 1. Local EDK2 build (if EDK2_DIR is set)
    if [[ -n "${EDK2_DIR:-}" ]]; then
        local built="$EDK2_DIR/Build/Shell/DEBUG_GCC5/$arch/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi"
        if [[ -f "$built" ]]; then
            echo "$built"
            return 0
        fi
    fi

    # 2. Check for previously extracted Shell.efi next to firmware
    if [[ -z "${FW_CODE:-}" ]]; then
        log_warning "find_shell_efi: FW_CODE not set (call find_firmware first)"
        return 1
    fi

    local fw_dir
    fw_dir="$(dirname "$FW_CODE")"
    local cached="$fw_dir/Shell_${arch}.efi"

    if [[ -f "$cached" ]] && head -c2 "$cached" | grep -q "MZ"; then
        echo "$cached"
        return 0
    fi

    # 3. Extract the firmware's OWN Shell from the active firmware .fd. Preferred
    #    over a distro/system package (tier 4) because the extracted Shell is
    #    guaranteed to match the firmware in use — a mismatched package Shell
    #    starts but hangs before its banner. The shell ships inside every
    #    OVMF/AAVMF DXE firmware volume; pull it out with a dependency-light
    #    parser (native fwtool, else python, else uefiextract). If none of those
    #    is available, fall through to the packaged Shell (tier 4).
    #
    # The active FW_CODE may not contain an extractable Shell.efi (some
    # firmware builds use a format our parser / uefiextract can't decode), so
    # also try the QEMU-bundled firmware as a candidate. QEMU_DIR may be unset
    # if find_qemu hasn't run; the :- default keeps `set -u` safe.
    local qemu_dir="${QEMU_DIR:-}"
    local qemu_share="${qemu_dir%/bin}/share/qemu"
    local fw_candidates=("$FW_CODE") fw_candidate
    case "$arch" in
        X64)     [[ -f "$qemu_share/edk2-x86_64-code.fd" ]] && \
                     fw_candidates+=("$qemu_share/edk2-x86_64-code.fd") ;;
        AARCH64) [[ -f "$qemu_share/edk2-aarch64-code.fd" ]] && \
                     fw_candidates+=("$qemu_share/edk2-aarch64-code.fd") ;;
    esac

    local tmpdir
    tmpdir=$(mktemp -d)
    local pe_body=""

    # 3a. Native host fwtool — the C build of the AXL firmware parser
    #     (byte-identical to the Python tier below, but with no python3
    #     dependency). Preferred extractor; if the host toolchain can't build
    #     it, this falls through to the Python (3b) and uefiextract (3c) tiers.
    local scripts_dir project_root fwtool_host
    scripts_dir="$(dirname "${BASH_SOURCE[0]}")"
    project_root="$(cd "$scripts_dir/.." && pwd)"
    fwtool_host="$("$project_root/scripts/build-prefix.sh" --abs x64)/build/fwtool-host"
    if [[ ! -x "$fwtool_host" ]]; then
        "$project_root/scripts/axl-build.sh" ARCH=x64 fwtool-host >/dev/null 2>&1 || true
    fi
    if [[ -x "$fwtool_host" ]]; then
        local fv_shell="$tmpdir/Shell.efi"
        for fw_candidate in "${fw_candidates[@]}"; do
            if "$fwtool_host" extract "$fw_candidate" "$_SHELL_GUID" -o "$fv_shell" >/dev/null 2>&1 \
                && head -c2 "$fv_shell" | grep -q "MZ"; then
                pe_body="$fv_shell"
                log_info "Extracted Shell.efi for $arch via native fwtool"
                break
            fi
        done
    fi

    # 3b. Dependency-free Python extraction (fallback when fwtool is
    #     unavailable — e.g. a host with no C toolchain).
    local extractor="$(dirname "${BASH_SOURCE[0]}")/extract-fv-shell.py"
    if [[ -z "$pe_body" ]] && [[ -f "$extractor" ]] && command -v python3 &>/dev/null; then
        local fv_shell="$tmpdir/Shell.efi"
        for fw_candidate in "${fw_candidates[@]}"; do
            if python3 "$extractor" "$fw_candidate" -o "$fv_shell" 2>/dev/null \
                && head -c2 "$fv_shell" | grep -q "MZ"; then
                pe_body="$fv_shell"
                break
            fi
        done
    fi

    # 3c. uefiextract for firmware our parser couldn't decode (Tiano-compressed).
    if [[ -z "$pe_body" ]] && command -v uefiextract &>/dev/null; then
        local extract_dir="$tmpdir/out"
        for fw_candidate in "${fw_candidates[@]}"; do
            if uefiextract "$fw_candidate" "$_SHELL_GUID" -o "$extract_dir" >/dev/null 2>&1; then
                # -print -quit (not `| head -1`): a closed pipe would SIGPIPE
                # find and, under `set -o pipefail`, abort the whole function.
                local body
                body=$(find "$extract_dir" -name "body.bin" -path "*PE32*" -print -quit 2>/dev/null)
                if [[ -n "$body" ]] && head -c2 "$body" | grep -q "MZ"; then
                    pe_body="$body"
                    break
                fi
            fi
            rm -rf "$extract_dir"
        done
    fi

    # Extraction succeeded → cache the firmware-matching Shell and use it.
    if [[ -n "$pe_body" ]]; then
        if cp "$pe_body" "$cached" 2>/dev/null; then
            log_info "Extracted Shell.efi for $arch from firmware ($(du -h "$cached" | cut -f1))"
            rm -rf "$tmpdir"
            echo "$cached"
            return 0
        fi
        # Firmware dir not writable — try ~/.cache/axl. Both cp's are guarded so
        # a read-only cache location degrades to the package tier rather than
        # aborting the function under `set -e`.
        local user_cache="${XDG_CACHE_HOME:-$HOME/.cache}/axl"
        if mkdir -p "$user_cache" 2>/dev/null \
            && cp "$pe_body" "$user_cache/Shell_${arch}.efi" 2>/dev/null; then
            log_info "Extracted Shell.efi for $arch (cached in $user_cache)"
            rm -rf "$tmpdir"
            echo "$user_cache/Shell_${arch}.efi"
            return 0
        fi
        log_warning "find_shell_efi: extracted a Shell for $arch but could not cache it (firmware dir and $user_cache both unwritable); falling back to a packaged Shell"
    fi
    rm -rf "$tmpdir"

    # 4. System-packaged standalone Shell.efi (distro edk2 / qemu packages) —
    #    LAST RESORT, only when extraction wasn't possible (no native fwtool,
    #    python3, or uefiextract). It is NOT guaranteed to match the firmware in
    #    use, so it is tried only after extraction; a mismatch can hang on boot.
    local shell_paths=()
    case "$arch" in
        X64)
            shell_paths=(
                /usr/share/edk2/ovmf/Shell.efi
                /usr/share/OVMF/Shell.efi
                /usr/share/edk2/x64/Shell.efi
                /usr/share/edk2-shell/x64/Shell.efi
                /usr/share/qemu/edk2-x86_64-shell.efi
            ) ;;
        AARCH64)
            shell_paths=(
                /usr/share/edk2/aarch64/Shell.efi
                /usr/share/AAVMF/Shell.efi
                /usr/share/edk2-shell/aa64/Shell.efi
                /usr/share/qemu/edk2-aarch64-shell.efi
            ) ;;
    esac
    local sp
    for sp in "${shell_paths[@]}"; do
        if [[ -f "$sp" ]] && head -c2 "$sp" | grep -q "MZ"; then
            log_warning "find_shell_efi: could not extract a Shell from the firmware; falling back to packaged '$sp' (may not match the firmware in use)"
            echo "$sp"
            return 0
        fi
    done

    log_warning "Shell.efi not found for $arch (could not extract it from the firmware and no distro UEFI Shell package found — install edk2-shell / qemu's edk2-*-shell.efi, or uefiextract from LongSoft/UEFITool)"
    return 1
}

# --------------------------------------------------------------------------
# find_shell_launcher — locate (building on demand) the AXL shell launcher
# Usage: LAUNCHER=$(find_shell_launcher <arch>) || LAUNCHER=""
#
# The launcher (test/integration/axl-shell-launcher.c) is staged as the boot
# binary in place of the Shell itself; it sibling-loads Shell.efi with
# LoadOptions "-delay 0" so the EDK2 Shell skips its 5-second startup countdown
# (five gBS->Stall(1s) busy-waits) — reclaiming ~5 s of wall time per guest
# boot across the suite. Callers stage the launcher as BOOTX64.EFI and put
# Shell.efi beside it; if this returns non-zero, fall back to staging Shell.efi
# directly (the countdown returns, but the boot is unaffected).
#
# Built on demand (like the native fwtool-host in find_shell_efi) so a fresh
# tree / standalone integration test still gets it without a prior full build.
#
# AXL_SHELL_LAUNCHER_BIN overrides which binary is staged as the launcher,
# skipping both the build and the default path. Two uses: staging a launcher
# built elsewhere (a package layout ships no build definition, so the on-demand
# build cannot run), and FAULT INJECTION — the fallback test stages a binary that
# never chains the Shell, which is the only way to exercise the "firmware does
# not reach the Shell" path without the firmware that once did it. The MZ check
# applies to the override too: a non-PE override is no launcher at all, and
# reporting it as one would move the failure to the guest, where it is a hang
# instead of an error.
# --------------------------------------------------------------------------
find_shell_launcher() {
    local arch="$1" build="${2:-build}"
    local scripts_dir project_root native_arch launcher
    if [[ -n "${AXL_SHELL_LAUNCHER_BIN:-}" ]]; then
        if [[ -f "$AXL_SHELL_LAUNCHER_BIN" ]] &&
           head -c2 "$AXL_SHELL_LAUNCHER_BIN" | grep -q "MZ"; then
            echo "$AXL_SHELL_LAUNCHER_BIN"
            return 0
        fi
        return 1
    fi
    scripts_dir="$(dirname "${BASH_SOURCE[0]}")"
    project_root="$(cd "$scripts_dir/.." && pwd)"
    native_arch="$(arch_dir "$arch")"
    launcher="$("$project_root/scripts/build-prefix.sh" --abs "$native_arch")/axl-shell-launcher.efi"
    # Always invoke the build (not just when the .efi is absent): the launcher
    # depends on its source and libaxl.a, so a PRESENT-BUT-STALE binary — built
    # against an older library — must be rebuilt, not reused. ninja's
    # dependency check makes this a no-op when the binary is already fresh. (A
    # stale launcher was the cause of a hang mistaken for the launcher being
    # broken.)
    if [[ "$build" == "build" ]]; then
        "$project_root/scripts/axl-build.sh" ARCH="$native_arch" shell-launcher >/dev/null 2>&1 || true
    fi
    if [[ -f "$launcher" ]] && head -c2 "$launcher" | grep -q "MZ"; then
        echo "$launcher"
        return 0
    fi
    return 1
}

# --------------------------------------------------------------------------
# Shell-launcher negative memo
#
# The launcher is ON by default, but it was once observed to hang some
# firmware: the guest loads BOOTX64.EFI and never reaches the Shell. That
# firmware is gone from every machine here, so the default cannot be made safe
# by fixing a fault nobody can reproduce. Instead the default VERIFIES ITSELF
# on whatever firmware it actually meets: run-qemu.sh notices the Shell was
# never reached, records a memo here, and retries without the launcher. Later
# runs read the memo and skip straight to the direct Shell boot.
#
# ONLY FAILURES ARE RECORDED, which is what makes the cache safe to trust:
# working firmware needs no entry and pays nothing, and a stale, unwritable or
# corrupt memo can only cost the 4.6 s/boot saving — never correctness. (Same
# shape as glibc's syscall fallbacks, which memoize ENOSYS and never success.)
#
# The key covers arch + firmware identity + launcher identity, so a firmware
# upgrade or a rebuilt launcher earns a fresh attempt rather than inheriting an
# old verdict. Presence of the file IS the verdict; its content is diagnostics
# for whoever finds it.
# --------------------------------------------------------------------------

# shell_launcher_memo_path <arch> <launcher> — print the memo path, or return 1
# when there is nowhere to cache (no XDG_CACHE_HOME and no HOME). Returning 1
# disables memoization rather than failing the run: the cost is speed.
shell_launcher_memo_path() {
    local arch="$1" launcher="$2" cache_root digest
    cache_root="${XDG_CACHE_HOME:-}"
    if [[ -z "$cache_root" ]]; then
        [[ -n "${HOME:-}" ]] || return 1
        cache_root="$HOME/.cache"
    fi
    # size+mtime, not a content hash: the firmware is ~4 MB and this runs on
    # every boot. A non-GNU stat degrades to a path-only key (coarser, still
    # correct) instead of breaking the run.
    local fw_id launcher_id
    fw_id="${FW_CODE:-none}:$(stat -c '%s:%Y' "${FW_CODE:-/nonexistent}" 2>/dev/null || echo '?')"
    launcher_id="$launcher:$(stat -c '%s:%Y' "$launcher" 2>/dev/null || echo '?')"
    digest=$(printf '%s\n%s\n%s\n' "$arch" "$fw_id" "$launcher_id" \
             | sha256sum | cut -d' ' -f1)
    echo "$cache_root/axl-sdk/shell-launcher-blocklist/$digest"
}

# shell_launcher_memo_present <arch> <launcher> — 0 if this combination is
# already known not to reach the Shell.
shell_launcher_memo_present() {
    local memo
    memo=$(shell_launcher_memo_path "$1" "$2") || return 1
    [[ -f "$memo" ]]
}

# shell_launcher_memo_record <arch> <launcher> — record the negative verdict.
# Never fatal: a read-only or absent cache home just means the next run pays
# the same one-time timeout again.
shell_launcher_memo_record() {
    local arch="$1" launcher="$2" memo
    memo=$(shell_launcher_memo_path "$arch" "$launcher") || return 0
    mkdir -p "$(dirname "$memo")" 2>/dev/null || return 0
    {
        echo "# axl-sdk: this firmware did not reach the UEFI Shell through the"
        echo "# AXL shell launcher, so the launcher is skipped for it. Delete this"
        echo "# file to try again (a firmware or launcher rebuild retries anyway)."
        echo "arch=$arch"
        echo "firmware=${FW_CODE:-unknown}"
        echo "launcher=$launcher"
        echo "recorded=$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo unknown)"
    } > "$memo" 2>/dev/null || return 0
    return 0
}

# --------------------------------------------------------------------------
# stage_boot_shell <staging_dir> <arch> <boot_name> <shell_efi>
#
# Stage the boot binary that drops the guest into the UEFI Shell at
# \EFI\BOOT\<boot_name>. Prefers the AXL launcher (find_shell_launcher), which
# chainloads Shell.efi with "-delay 0" to skip the 5 s startup countdown; the
# Shell is staged beside it (\EFI\BOOT\Shell.efi) for the launcher's sibling
# resolution. Falls back to booting Shell.efi directly when the launcher can't
# be built (the countdown returns, boot otherwise unaffected). Shared by
# run-qemu.sh and the integration harness (common-test.sh) so they can't drift.
# --------------------------------------------------------------------------
stage_boot_shell() {
    local staging="$1" arch="$2" boot_name="$3" shell_efi="$4"
    # unset_default: what an unset AXL_SHELL_LAUNCHER means for THIS caller.
    #   auto — the caller can tell whether the boot reached the Shell and undo
    #          itself if it did not. Only run-qemu.sh can: it owns the serial
    #          log and the sentinel.
    #   off  — the caller has no such oracle, so it must not opt a user into a
    #          launcher that might not come back. Defaulting to `off` means a
    #          caller that forgets to pass anything gets the safe answer.
    local unset_default="${5:-off}"
    mkdir -p "$staging/EFI/BOOT"
    # The -delay 0 launcher skips the Shell's 5 s startup countdown, worth
    # ~4.6 s on every guest boot. Where it is the default it is safe because it
    # is SELF-VERIFYING rather than pre-verified: run-qemu.sh records a negative
    # memo for any firmware that does not reach the Shell through it, and this
    # consults that memo. See the memo block above for why only failures are
    # recorded.
    #
    #   unset -> $unset_default (see above)
    #   0     -> off, always (bisect a suspected launcher fault)
    #   1     -> on, always: the memo is IGNORED and no fallback happens, so a
    #            launcher regression stays a loud failure. run-integration.sh
    #            and test-shell-launcher-qemu.sh depend on that.
    SHELL_LAUNCHER_STAGED=false
    SHELL_LAUNCHER_PATH=""
    local mode="${AXL_SHELL_LAUNCHER:-$unset_default}"
    local launcher=""
    rm -f "$staging/EFI/BOOT/axl-shell.opt"
    # AXL_SHELL_OPTIONS reach the Shell only through the launcher, so asking
    # for them FORCES it: auto mode could otherwise fall back (a memo, a
    # failed boot's retry) to a Shell booted without them, and the caller
    # would rely on -nonest it never got. With the launcher switched off
    # they cannot apply at all, and that is an error, not a warning.
    if [[ -n "${AXL_SHELL_OPTIONS:-}" ]]; then
        if [[ "$mode" == "0" || "$mode" == "off" ]]; then
            echo "run-qemu: AXL_SHELL_OPTIONS='$AXL_SHELL_OPTIONS' needs the Shell" \
                 "launcher, which AXL_SHELL_LAUNCHER=$mode switches off" >&2
            return 1
        fi
        mode=1
    fi
    if [[ "$mode" != "0" && "$mode" != "off" ]]; then
        # Build the launcher only when it was explicitly ASKED for. In auto mode
        # this is an optimization, and a silent multi-minute build inside what
        # looks like a QEMU launch is not one — nor is racing another ad-hoc run
        # in the same tree. A stale prebuilt launcher is safe here precisely
        # because auto mode can undo itself.
        local build="build"
        [[ "$mode" == "1" ]] || build="nobuild"
        launcher=$(find_shell_launcher "$arch" "$build") || launcher=""
        if [[ -n "$launcher" && "$mode" != "1" ]] &&
           shell_launcher_memo_present "$arch" "$launcher"; then
            # SAY SO. Falling back is correct, but it costs ~4.6 s on EVERY
            # boot of this firmware, and until now nothing reported it: the
            # suite simply ran slower with no visible cause. On a 180-boot run
            # that is ~14 minutes attributable to a file in a cache directory.
            #
            # Printed unconditionally rather than behind --verbose, because the
            # whole defect is that the cost was INVISIBLE, and a diagnostic you
            # have to already suspect is one you will not switch on. It fires
            # only when a memo actually applies, which is rare -- and if it IS
            # printing on every boot, that repetition is the finding.
            local _memo
            _memo=$(shell_launcher_memo_path "$arch" "$launcher" 2>/dev/null || echo '?')
            echo "run-qemu: shell launcher SKIPPED for this firmware — this boot pays" >&2
            echo "          the Shell's ~4.6 s startup countdown." >&2
            echo "          firmware: ${FW_CODE:-unknown}" >&2
            echo "          memo:     $_memo" >&2
            [[ -f "$_memo" ]] && sed -n 's/^recorded=/          recorded: /p' "$_memo" >&2
            echo "          Delete that file to retry; a firmware or launcher rebuild" >&2
            echo "          retries anyway. AXL_SHELL_LAUNCHER=1 forces it on." >&2
            launcher=""
        fi
    fi
    if [[ -n "$launcher" ]]; then
        cp "$launcher" "$staging/EFI/BOOT/$boot_name"
        cp "$shell_efi" "$staging/EFI/BOOT/Shell.efi"
        SHELL_LAUNCHER_STAGED=true
        SHELL_LAUNCHER_PATH="$launcher"
        # AXL_SHELL_OPTIONS: more Shell options after "-delay 0" (e.g. -nonest,
        # which a guest needs for Execute to report whether a command ran).
        # The launcher reads them from this file beside it.
        if [[ -n "${AXL_SHELL_OPTIONS:-}" ]]; then
            printf '%s\n' "$AXL_SHELL_OPTIONS" > "$staging/EFI/BOOT/axl-shell.opt"
        fi
    else
        # Only the launcher passes options, and mode=1 above means a missing
        # one is a build failure: say so rather than boot without them.
        if [[ -n "${AXL_SHELL_OPTIONS:-}" ]]; then
            echo "run-qemu: AXL_SHELL_OPTIONS='$AXL_SHELL_OPTIONS' NOT applied:" \
                 "the Shell launcher could not be built for $arch" >&2
            return 1
        fi
        cp "$shell_efi" "$staging/EFI/BOOT/$boot_name"
    fi
}

# --------------------------------------------------------------------------
# Test-log section assertions
#
# UEFI nsh-driven QEMU tests typically run several commands per boot and
# pipe the entire serial console into one log. A whole-log grep can't tell
# whether the matched line came from the right command, which makes
# strict assertions fragile.
#
# The convention is to bracket each command's output with a marker line
# (`echo "=== <SECTION> ==="`) before invoking it. assert_in_section then
# slices the log between that marker and the next "===" marker — so the
# assertion only sees the output of the targeted command.
#
# Usage:
#   assert_in_section LABEL SECTION_MARKER PATTERN
#     LABEL          short tag printed in PASS/FAIL output
#     SECTION_MARKER text after "=== " on the marker line (e.g. "memspd list")
#     PATTERN        ERE pattern grepped against the slice
#
# The log path is taken from the global $LOG variable. Existing scripts
# in test/integration/ use $TEST_CLEAN_LOG (set by test_clean_log) for
# the same purpose; assert_in_section falls back to that if $LOG isn't
# set, so callers can use either convention without an explicit alias.
# Returns 0 on a hit, 1 on miss; prints PASS / FAIL line in the same
# shape the existing test-runner uses so a containing script's
# fail-counter increment works the same way.
# --------------------------------------------------------------------------

assert_in_section() {
    local label="$1"
    local section="$2"
    local pattern="$3"

    local log="${LOG:-${TEST_CLEAN_LOG:-}}"
    if [[ -z "$log" ]]; then
        echo "FAIL: $label  (\$LOG / \$TEST_CLEAN_LOG global not set; assert_in_section needs one)"
        return 1
    fi
    if [[ ! -f "$log" ]]; then
        echo "FAIL: $label  (log file '$log' not found)"
        return 1
    fi

    # Slice from the section's "=== <section> ===" line, exclusive of
    # the line itself, up to the next line starting with "===" (any
    # text after). awk emits everything strictly between the two
    # markers, so the slice is just that command's output.
    local slice
    slice=$(awk -v want="=== $section ===" '
        $0 == want                  { in_section = 1; next }
        in_section && /^=== /        { in_section = 0 }
        in_section                   { print }
    ' "$log")

    if [[ -z "$slice" ]]; then
        echo "FAIL: $label  (section '=== $section ===' not found in $log)"
        return 1
    fi

    if echo "$slice" | grep -qE -- "$pattern"; then
        echo "PASS: $label"
        return 0
    fi
    echo "FAIL: $label  (pattern not in section '$section': $pattern)"
    return 1
}

# --------------------------------------------------------------------------
# Host-port allocation — safe for multiple INDEPENDENT runners
#
# Why this exists: every harness in and around this repo used to derive its
# host ports from a formula (TEST_PORT_BASE + slot). A formula only keeps
# ONE invocation internally consistent — two developers, a consumer repo's
# suite running alongside ours, or two agents all start from the same
# constants and collide. The failure is nasty out of proportion to its
# cause: QEMU refuses the ENTIRE -netdev when any single hostfwd cannot
# bind, so a busy port presents as "the guest has no network", not "port
# busy". A stale host-server.py squatting on 18001 cost real debugging
# time exactly this way.
#
# The mechanism is two checks per candidate port, in this order:
#
#   1. CLAIM — take a non-blocking exclusive flock on
#      $AXL_PORT_LOCK_DIR/<port>.lock and keep the file descriptor OPEN.
#      This is what makes cooperating runners (anything that sources this
#      file) mutually exclusive, and it is race-free: flock(2) is atomic,
#      so of two runners reaching for the same port at the same instant
#      exactly one wins. The claim lives as long as the allocating shell,
#      and the kernel drops it if that shell dies — no stale state to
#      clean up, no reaper needed.
#
#   2. VERIFY — confirm nothing is ACTUALLY listening on the port right
#      now. The lock only binds processes that agreed to play; this catches
#      the ones that did not (a leftover Python server, an unrelated VM, a
#      consumer that hard-codes its ports).
#
# On the residual race: between VERIFY and QEMU's own bind, a foreign
# process could still take the port. No probe-based scheme can close that
# — the only way would be to hold the real socket, which is precisely what
# we must hand to QEMU. So we do not pretend it is closed. We make it
# LOUD instead: axl_report_hostfwd_failure turns QEMU's bind refusal into
# an explicit diagnosis at the point of failure, and run-integration.sh's
# existing retry re-enters the allocator and draws a different port. A
# collision therefore costs one clear error line or one retry, never a
# mystery network timeout.
#
# Range: 18000-19999 by default, deliberately below Linux's ephemeral
# range (32768-60999) so an outbound connection can never squat on us.
# --------------------------------------------------------------------------

# Deliberately a FIXED host-wide path, not ${TMPDIR}/... — a lock only means
# anything if every runner on the host reaches for the SAME file, and TMPDIR
# is routinely per-process (run-qemu.sh sets its own; CI sets one per job).
# Keyed by uid so a shared /tmp cannot produce cross-user permission errors.
# The files are empty and are left behind on purpose: the claim is the flock,
# which the kernel drops when the holder exits, so there is no stale state to
# reap and no window where a reaper could delete a file another runner holds.
AXL_PORT_LOCK_DIR="${AXL_PORT_LOCK_DIR:-/tmp/axl-port-locks-$(id -u)}"
AXL_PORT_LO="${AXL_PORT_LO:-18000}"
AXL_PORT_HI="${AXL_PORT_HI:-19999}"

# Descriptors for every port this shell has claimed. Held open on purpose:
# closing one releases the claim. Never close these by hand.
_AXL_PORT_HELD_FDS=()

# --------------------------------------------------------------------------
# axl_port_in_use — is anything listening on this host port right now?
# Usage: axl_port_in_use <port> [tcp|udp]     (0 = in use)
# --------------------------------------------------------------------------

axl_port_in_use() {
    local port="$1" proto="${2:-tcp}"
    if command -v ss >/dev/null 2>&1; then
        local flag="-Hltn"
        [[ "$proto" == "udp" ]] && flag="-Hlun"
        [[ -n "$(ss "$flag" "sport = :$port" 2>/dev/null)" ]] && return 0
        return 1
    fi
    # No iproute2: fall back to a connect probe. TCP only — a UDP port
    # cannot be probed this way, so treat it as free and let the claim
    # lock carry the weight.
    [[ "$proto" != "tcp" ]] && return 1
    # The subshell owns the descriptor, so it is closed when the probe ends —
    # nothing to clean up here.
    if ( exec 3<>"/dev/tcp/127.0.0.1/$port" ) 2>/dev/null; then
        return 0
    fi
    return 1
}

# --------------------------------------------------------------------------
# axl_alloc_host_port — claim host port(s) that are free RIGHT NOW
# Usage: axl_alloc_host_port <varname> [count] [tcp|udp] || exit 1
#
# Assigns the (first) claimed port to the named variable; with count > 1 the
# claimed ports are CONTIGUOUS from there, so a caller can keep addressing
# them as base+0, base+1, ... Prints nothing.
#
# The result comes back through a named variable rather than stdout on
# purpose. A claim lives exactly as long as the shell holding its file
# descriptor, and `PORT=$(...)` runs the function in a SUBSHELL that exits
# immediately — which would drop every claim the instant it was made and
# quietly reintroduce the collisions this exists to prevent. Assigning in
# the caller's own shell keeps the claim alive for the caller's lifetime,
# and child processes inherit it, which is what lets a test claim up front
# and hand the port to QEMU later.
# --------------------------------------------------------------------------

axl_alloc_host_port() {
    local __var="$1" count="${2:-1}" proto="${3:-tcp}"
    local lo="$AXL_PORT_LO" hi="$AXL_PORT_HI"
    local span=$(( hi - lo + 1 ))
    if [[ -z "$__var" ]]; then
        log_error "axl_alloc_host_port: needs a destination variable name"
        return 1
    fi
    if (( count < 1 || span < count )); then
        log_error "axl_alloc_host_port: cannot fit $count port(s) in $lo-$hi"
        return 1
    fi
    mkdir -p "$AXL_PORT_LOCK_DIR" 2>/dev/null || {
        log_error "axl_alloc_host_port: cannot create $AXL_PORT_LOCK_DIR"
        return 1
    }
    # Start the scan at a pseudo-random offset rather than at the bottom of
    # the range. Two runners starting at the same instant then contend on
    # different ports instead of walking the identical prefix and serialising
    # on the same locks.
    local start=$(( (RANDOM * 32768 + RANDOM + $$) % span ))
    local attempt base k port fd ok
    local -a taken
    for (( attempt = 0; attempt <= span - count; attempt++ )); do
        base=$(( lo + (start + attempt) % span ))
        (( base + count - 1 > hi )) && continue   # keep the run inside the range
        taken=(); ok=1
        for (( k = 0; k < count; k++ )); do
            port=$(( base + k ))
            # The 2>/dev/null is scoped to the group ON PURPOSE. `exec
            # {fd}>file 2>/dev/null` would apply the stderr redirect to the
            # SHELL, permanently — silencing every later diagnostic,
            # including the hostfwd-collision report this file exists to
            # print. A brace group keeps it to this one redirection.
            if ! { exec {fd}>"$AXL_PORT_LOCK_DIR/$port.lock"; } 2>/dev/null; then
                ok=0; break
            fi
            if ! flock -n "$fd"; then
                exec {fd}>&-        # claimed by another runner
                ok=0; break
            fi
            if axl_port_in_use "$port" "$proto"; then
                exec {fd}>&-        # lock free but a foreign process is on it
                ok=0; break
            fi
            taken+=("$fd")
        done
        if (( ok )); then
            _AXL_PORT_HELD_FDS+=("${taken[@]}")
            printf -v "$__var" '%s' "$base"
            return 0
        fi
        # Partial run — give the ports back before trying the next base.
        for fd in "${taken[@]}"; do
            exec {fd}>&-
        done
    done
    log_error "axl_alloc_host_port: no free run of $count $proto port(s) in $lo-$hi (raise AXL_PORT_HI or clear stale listeners)"
    return 1
}

# --------------------------------------------------------------------------
# axl_report_hostfwd_failure — turn QEMU's bind refusal into a diagnosis
# Usage: axl_report_hostfwd_failure <qemu-log> <context>   (1 = it happened)
#
# QEMU rejects the whole -netdev when any one hostfwd cannot bind, which
# otherwise reaches the caller as an empty serial log or a 60 s
# "server did not start" — the single most misleading failure this harness
# produces. Call this wherever a QEMU log is available before concluding
# anything about the guest.
# --------------------------------------------------------------------------

axl_report_hostfwd_failure() {
    local log="$1" context="${2:-qemu}"
    [[ -f "$log" ]] || return 0
    local line
    line=$(grep -m1 -a "Could not set up host forwarding rule" "$log" 2>/dev/null) || return 0
    [[ -n "$line" ]] || return 0
    local port
    # hostfwd=tcp::PORT- or, with --hostfwd-bind, hostfwd=tcp:ADDR:PORT-
    port=$(sed -n "s/.*hostfwd=tcp:[0-9.]*:\([0-9]\+\)-.*/\1/p" <<<"$line" | head -1)
    cat >&2 <<EOF

ERROR: $context — QEMU refused its network device: a host port is already in use.

  $line

  QEMU rejects the ENTIRE -netdev when any single hostfwd cannot bind, so the
  guest booted with NO network at all. Do not read this as a guest/firmware
  bug — nothing in the guest was ever reachable.
EOF
    if [[ -n "$port" ]]; then
        echo "  Host port $port is held by:" >&2
        ss -Hltnp "sport = :$port" 2>/dev/null | sed 's/^/    /' >&2 \
            || echo "    (unknown — 'ss -ltnp' unavailable)" >&2
    fi
    cat >&2 <<'EOF'
  Fix: let the harness pick the port instead of pinning it — unset
  TEST_PORT_BASE (test_port then claims a verified-free port), or use
  run-qemu.sh --hostfwd auto:<guest-port>.
EOF
    return 1
}

# --------------------------------------------------------------------------
# CPU-spike sampler — shared by run-qemu.sh and test/integration/common-test.sh
#
# This lived in run-qemu.sh, where it has been on by default for a long time.
# But run-qemu.sh and common-test.sh are SIBLINGS — both source this file, and
# common-test.sh builds its own TEST_QEMU_CMD rather than calling run-qemu.sh.
# So every integration suite ran with no sampling at all: a guest that
# busy-waits after boot burned its whole timeout with nothing flagged, which is
# exactly the defect the sampler exists to catch, on the path where most of our
# tests live. Moved here verbatim so both siblings get the same behaviour.
#
# Two globals the block used to close over are now parameters:
#   - the ARCH-dependent warm-up, via cpu_warmup_init <arch>
#   - the summary file's directory, via cpu_monitor_start <pid> [tmpdir]
#
# The knobs keep their names and defaults so run-qemu.sh's flags (--no-cpu-warn,
# --cpu-report, --cpu-threshold, --cpu-sustain) are unchanged; run-qemu.sh
# assigns them after sourcing this file, so its values still win.
# --------------------------------------------------------------------------

CPU_REPORT="${CPU_REPORT:-false}"
CPU_SUSTAIN="${CPU_SUSTAIN:-2}"         # seconds at threshold to count as a spike
CPU_SPIKE_EXIT="${CPU_SPIKE_EXIT:-8}"   # distinct exit code for a sustained spike
# CPU_WARN / CPU_THRESHOLD default to empty here so cpu_policy_init <arch> can
# apply the KVM/TCG-aware policy (below) only where a caller has not already set
# them via a flag or env override. A caller that samples MUST call cpu_policy_init.
CPU_WARN="${CPU_WARN:-}"
CPU_THRESHOLD="${CPU_THRESHOLD:-}"

# Wall-clock budget (seconds; empty = none) and its own exit code, kept
# DISTINCT from the spike code so a caller can tell the two apart.
#
# Why a second signal at all: the sampler asks "is the guest holding
# >=CPU_THRESHOLD cores past the warm-up". For an IDLE-shaped run -- an app that
# should be sitting at a prompt -- that is a real spike detector, and it has
# earned its keep. For a COMPUTE-shaped run it is not: a 1-vCPU KVM guest tops
# out ~1.05 cores and a compute-bound binary holds that from boot to exit, so
# the only free variable is how LONG the run is. It reports a spike while
# measuring duration, and it fires on axl-sdk's own unit suite every x64 run.
# So `--workload compute` swaps it for this: measure duration, say duration.
MAX_DURATION="${MAX_DURATION:-}"
DURATION_EXIT="${DURATION_EXIT:-9}"

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
# through OVMF boot than KVM-X64. Set it via cpu_warmup_init <arch>;
# run-qemu.sh passes $ARCH, common-test.sh passes $TEST_ARCH.
CPU_WARMUP=10
cpu_warmup_init() {
    CPU_WARMUP=10
    [[ "${1:-X64}" == "AARCH64" ]] && CPU_WARMUP=15
    return 0
}

# cpu_policy_init <arch> — set the CPU-spike detection policy for KVM vs TCG.
# threshold + warm-up + TCG carve-out are chosen TOGETHER; fixing one alone
# leaves either a check that can never fire (a 1-vCPU guest tops out ~1.05 cores,
# so a >=1.5 threshold is dead) or one that fires on every emulated boot.
#   - KVM (X64 with a usable /dev/kvm): an idle guest is ~0.05 cores and an AXL
#     spin pegs ~1.0, so a 0.5-core spike sustained past the boot warm-up is a
#     real signal -> warn on.
#   - TCG (AARCH64 on an x86 host, or no usable /dev/kvm): the guest legitimately
#     pegs a host core emulating, so the sampler cannot tell a spin from normal
#     work -> carve the warn out (an actionless false positive otherwise).
# Respects a CPU_WARN / CPU_THRESHOLD a caller already set (a run-qemu flag, a
# TEST_CPU_* env), applying the policy default only where still unset.
cpu_policy_init() {
    local arch="${1:-X64}"
    cpu_warmup_init "$arch"
    CPU_THRESHOLD="${CPU_THRESHOLD:-0.5}"
    if [[ "$arch" != "AARCH64" && -r /dev/kvm && -w /dev/kvm ]]; then
        CPU_WARN="${CPU_WARN:-true}"
    else
        CPU_WARN="${CPU_WARN:-false}"   # TCG: a pegged host core is expected
    fi
    return 0
}

# cpu_sampler PID OUT [UNTIL]: as above; with UNTIL, sampling also ends when
# that process does -- for a guest that outlives the run being measured (a
# leased pool guest: run-qemu.sh's pooled mode passes a sentinel it kills when
# the run ends).
cpu_sampler() {
    local qpid="$1" out="$2" until="${3:-$1}"
    local hz; hz=$(getconf CLK_TCK 2>/dev/null || echo 100)
    awk -v pid="$qpid" -v until_pid="$until" -v hz="$hz" -v interval=0.2 \
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
    function running() { return alive(pid) && (until_pid == pid || alive(until_pid)) }
    BEGIN {
        # Warm-up, but poll rather than one long sleep: a guest that exits
        # during the window (most integration tests finish in 10-20s) would
        # otherwise keep the sampler — and the caller waiting on it — alive
        # for the full warm-up with nothing left to measure. Same measurement,
        # no added latency.
        for (t = 0; t < warmup; t += interval) {
            if (!running()) { print "0.00 0.00 0.000"; exit 0 }
            system("sleep " interval)
        }
        prev = read_total(pid)
        if (prev < 0) { print "0.00 0.00 0.000"; exit 0 }
        peak = 0; streak = 0; streak_max = 0; sum = 0; n = 0
        while (running()) {
            system("sleep " interval)
            cur = read_total(pid)
            if (cur < 0) break
            d = cur - prev; prev = cur
            cores = d / (hz * interval)
            sum += cores; n++
            if (cores > peak) peak = cores
            if (cores >= thr) {
                streak += interval
                if (streak > streak_max) streak_max = streak
            } else {
                streak = 0
            }
        }
        # peak (max 0.2s reading) sustain_max (longest >=thr streak, s) mean (avg cores)
        printf "%.2f %.2f %.3f\n", peak, streak_max, (n > 0 ? sum / n : 0)
    }' > "$out"
}

# Print a CPU-spike summary if the sampler captured a sustained spike, and/or
# (with --cpu-report) an always-on report of the sampled host-CPU usage.
# Reads "<peak> <sustain_max> <mean>" from the file written by cpu_sampler
# (1.0 = one host core saturated; measured after the boot warm-up window).
# Returns CPU_SPIKE_EXIT when CPU_WARN is on and a sustained post-warm-up spike
# was detected (so the caller can fail the run); 0 otherwise. --no-cpu-warn
# (CPU_WARN=false) suppresses BOTH the WARN line and this non-zero return.
cpu_summary() {
    local summary_file="$1"
    [[ "$CPU_WARN" != "true" && "$CPU_REPORT" != "true" ]] && return 0
    [[ ! -s "$summary_file" ]] && return 0
    local peak sustain mean
    read -r peak sustain mean < "$summary_file" || return 0
    mean="${mean:-0.000}"

    # Always-on report — a machine-greppable line consumers (e.g. the
    # server-CPU regression test) parse. Goes to stdout, not stderr.
    if [[ "$CPU_REPORT" == "true" ]]; then
        printf "CPU-REPORT: mean %s cores, peak %s cores (after %ss warm-up)\n" \
            "$mean" "$peak" "$CPU_WARMUP"
    fi

    # awk for the comparison; $sustain and $CPU_SUSTAIN are floats.
    if [[ "$CPU_WARN" == "true" ]]; then
        local breached
        breached=$(awk -v s="$sustain" -v t="$CPU_SUSTAIN" \
            'BEGIN{print (s+0 >= t+0) ? "1" : "0"}')
        if [[ "$breached" == "1" ]]; then
            printf "WARN: CPU spike — peak %s cores, sustained ≥%s cores for %ss (threshold %ss)\n" \
                "$peak" "$CPU_THRESHOLD" "$sustain" "$CPU_SUSTAIN" >&2
            return "$CPU_SPIKE_EXIT"
        fi
    fi
    return 0
}

# --- CPU-monitor lifecycle (shared by the screenshot + default run branches) ---
# Both branches launch/kill QEMU differently, so only the monitor lifecycle is
# factored here (the launch code is deliberately left per-branch — it diverges too
# much to share without obscuring it). cpu_monitor_start records the summary path +
# sampler pid in globals; cpu_monitor_finish waits the sampler and summarizes,
# returning the spike status.
CPU_SUMMARY_FILE=""
CPU_SAMPLER_PID=""

# Resolve the QEMU child pid of a `( timeout ... qemu ) &` wrapper subshell.
# Empty if not found within ~1s (caller decides any fallback).
qemu_child_pid() {
    local wrapper="$1" pid=""
    local _
    for _ in 1 2 3 4 5; do
        pid=$(pgrep -P "$wrapper" 2>/dev/null | head -1)
        [[ -n "$pid" ]] && break
        sleep 0.2
    done
    printf '%s' "$pid"
}

# Resolve the REAL qemu pid behind the QEMU_PID that `run-qemu.sh --background`
# prints. That value is the `timeout` wrapper (see the note in run-qemu.sh's
# background branch), whose own utime+stime is ~0 for the guest's whole life, so
# /proc sampling it reports an idle guest however hard the guest spins. When
# run-qemu's `pgrep -P` misses it falls back to the SUBSHELL pid, whose only
# child is `timeout` -- so the child found here is checked by comm, and a
# `timeout` is descended through once more.
# Echoes the pid of a process whose comm is qemu-system*; returns 1 with a
# message on stderr otherwise -- never a fallback to a wrapper, which would read
# as "idle".
qemu_real_pid() {
    local wrapper="$1" pid="" comm="" depth
    pid="$wrapper"
    for depth in 1 2; do
        pid=$(qemu_child_pid "$pid")
        if [[ -z "$pid" ]]; then
            echo "qemu_real_pid: no child process under pid $wrapper (QEMU" \
                 "exited, or it is not the run-qemu wrapper); refusing to" \
                 "sample the wrapper" >&2
            return 1
        fi
        # setpriv (the pdeathsig prefix) execs into qemu in place, so for a
        # moment this pid's comm is still `setpriv`: poll it, bounded to ~2 s,
        # rather than refuse a guest that is a few ms from existing.
        local _t
        for ((_t = 0; _t < 20; _t++)); do
            comm=$(cat "/proc/$pid/comm" 2>/dev/null || true)
            [[ "$comm" == setpriv ]] || break
            sleep 0.1
        done
        case "$comm" in
            qemu-system*) printf '%s' "$pid"; return 0 ;;
            timeout) continue ;;
        esac
        break
    done
    echo "qemu_real_pid: pid $pid under $wrapper is '$comm', not qemu-system*;" \
         "refusing to sample it" >&2
    return 1
}

# proc_cpu_cores <pid> <window_secs> -- host cores (utime+stime delta) the pid
# used over the window. Returns 1 with a message, printing nothing, when the pid
# vanishes: "0.00" must only ever mean "measured, idle".
proc_cpu_cores() {
    local pid="$1" window="$2" hz st t1 t2 s1 s2
    hz="$(getconf CLK_TCK 2>/dev/null || echo 100)"
    st="$(cat "/proc/$pid/stat" 2>/dev/null)" \
        || { echo "proc_cpu_cores: /proc/$pid/stat unreadable" >&2; return 1; }
    # Fields after "(comm)": utime(#14),stime(#15) are idx 11,12 of the tail.
    read -r -a t1 <<<"${st##*) }"
    s1=$(( ${t1[11]:-0} + ${t1[12]:-0} ))
    sleep "$window"
    st="$(cat "/proc/$pid/stat" 2>/dev/null)" \
        || { echo "proc_cpu_cores: pid $pid exited during the window" >&2; return 1; }
    read -r -a t2 <<<"${st##*) }"
    s2=$(( ${t2[11]:-0} + ${t2[12]:-0} ))
    awk -v d="$(( s2 - s1 ))" -v hz="$hz" -v w="$window" \
        'BEGIN { printf "%.2f", (hz > 0 && w > 0) ? d / hz / w : 0 }'
}

# host_load_sampler <pid> <outfile> [interval] -- while <pid> lives, sample how
# many tasks the HOST has runnable (procs_running in /proc/stat) and keep
# "<sum> <samples>" in <outfile>. host_contention_factor turns that into a
# number. This is a measurement of the box the run shared, taken during the run.
#
# WHY NOT THE GUEST'S OWN SCHEDSTAT. Tried first: /proc/<pid>/schedstat's
# runqueue-wait. On this box (a hybrid-core laptop part) a guest sharing the
# machine with verify.sh's other jobs waited only ~7 s of a 119 s run, yet its
# ON-CPU time rose from 48 s (alone) to 90 s -- the same work took ~1.9x the
# CPU time, because contended threads land on slower cores and clocks. Time
# waiting for a CPU is the part that is small; the slowdown is in the part
# that looks like work. Host oversubscription is what moves with it.
host_load_sampler() {
    local pid="$1" out="$2" interval="${3:-0.5}" r sum=0 n=0
    while kill -0 "$pid" 2>/dev/null; do
        r=$(awk '$1 == "procs_running" { print $2 }' /proc/stat 2>/dev/null)
        if [[ "$r" =~ ^[0-9]+$ ]]; then
            sum=$((sum + r)); n=$((n + 1))
            printf '%s %s\n' "$sum" "$n" > "$out.tmp" && mv -f "$out.tmp" "$out"
        fi
        sleep "$interval"
    done
}

# host_steal_ns -- one reading, in ns, of the box's steal time (/proc/stat,
# summed over its CPUs: time a hypervisor below this kernel ran something
# else -- this box can itself be a VM). Only grows; a run's figure is the
# difference of two readings. Box-wide, so no thread of the guest can take
# it away by exiting. [hz] is CLK_TCK, for a caller reading it repeatedly
# (getconf is a fork). Returns 1, printing nothing, when it cannot be read:
# a 0 here would read as the box's whole lifetime of steal against the next.
host_steal_ns() {
    local steal hz="${1:-}"
    [[ -n "$hz" ]] || hz=$(getconf CLK_TCK)
    { read -r _ _ _ _ _ _ _ _ steal _ < /proc/stat; } 2>/dev/null || return 1
    [[ "$steal" =~ ^[0-9]+$ && "$hz" =~ ^[0-9]+$ && "$hz" -gt 0 ]] || return 1
    printf '%s\n' "$(( steal * (1000000000 / hz) ))"
}

# proc_start_ticks <pid> -- <pid>'s start time (field 22 of /proc/<pid>/stat,
# clock ticks since boot): the same pid with another start time is another
# process. Read after the last ") ", which ends the command name (a name
# may hold spaces and parentheses). Returns 1 when it cannot be read.
proc_start_ticks() {
    local st f
    { st=$(< "/proc/$1/stat"); } 2>/dev/null || return 1
    read -ra f <<<"${st##*) }"
    [[ "${f[19]:-}" =~ ^[0-9]+$ ]] || return 1
    printf '%s\n' "${f[19]}"
}

# thread_run_delays <pid> -- one "<tid> <run_delay_ns>" line per thread of
# <pid>: field 2 of /proc/<pid>/task/<tid>/schedstat, the time the thread
# was runnable but waiting for a CPU, on THIS kernel. Returns 1 with nothing
# printed when no thread could be read.
#
# Why run delay and not the load average: what voids a timing measurement is
# a vCPU that wanted the CPU and did not get it, and run delay is that,
# directly. Every thread, not the vCPU alone: QEMU names them all
# "qemu-system-*" unless started with debug-threads=on, and its main loop
# kept waiting delays the guest's timers just the same.
#
# PER THREAD, never summed into one number: a summed reading loses a thread
# that exits between two readings -- QEMU's I/O workers retire after ~10 s
# idle, about when a guest's first real work starts -- and its whole history
# with it, which read a starved window as idle or NEGATIVE (qloop's row on a
# leased guest read -59 ms). Compare two readings with run_delay_window_ns.
thread_run_delays() {
    local t d n=0
    for t in /proc/"$1"/task/*; do
        { read -r _ d _ < "$t/schedstat"; } 2>/dev/null || continue
        [[ "$d" =~ ^[0-9]+$ ]] || continue
        printf '%s %s\n' "${t##*/}" "$d"; n=$((n + 1))
    done
    (( n > 0 ))
}

# run_delay_window_ns <before> <after> -- the ns of run delay between two
# thread_run_delays readings: each thread's growth, summed. A thread only in
# <after> was born inside the window and counts from 0; so does one whose
# counter went DOWN (its tid was reused by a new thread). A thread only in
# <before> exited and adds nothing -- nor takes anything away. Never
# negative. Returns 1, printing nothing, when the two share no thread: not
# measured is never 0.
run_delay_window_ns() {
    awk 'NF != 2 { next }
         NR == FNR { b[$1] = $2; nb++; next }
         { d = ($1 in b && $2 + 0 >= b[$1] + 0) ? $2 - b[$1] : $2; s += d; if ($1 in b) n++ }
         END { if (!nb || !n) exit 1; printf "%.0f\n", s }' \
        <(printf '%s\n' "$1") <(printf '%s\n' "$2")
}

# run_delay_window_ms <before> <after> -- run_delay_window_ns in whole ms.
run_delay_window_ms() {
    local ns
    ns=$(run_delay_window_ns "$1" "$2") || return 1
    printf '%s\n' "$(( ns / 1000000 ))"
}

# AXL_STARVE_MAX_MS -- the ONE limit both timing witnesses judge against: a
# timing miss while the guest's QEMU waited longer than this for a CPU (run
# delay, read per thread: thread_run_delays) is load, not a verdict.
# scripts/qloop-verdict.sh reads it over a whole qloop run
# (vcpu_starve_sampler); test/integration/test-net-latency-qemu.sh over each
# guest's round-trip window (run_delay_window_ms). Measured (12 CPUs): a
# whole qloop run (~28 s) 6 ms quiet, 7014 ms with twelve busy loops on the
# twelve CPUs; a net-latency round-trip window (~0.1-0.5 s) under 1 ms in a
# spike and 0-34 ms over five full quiet runs (load ~5); beside busy loops
# 15-280 ms, and every one of the 13 p90 misses seen there (5.11-8.89 ms) in
# a window of 78 ms or more (2026-11-09).
AXL_STARVE_MAX_MS=50

# vcpu_starve_sampler <pid> <outfile> [interval] -- while <pid> lives, read
# its threads' run delay (thread_run_delays) every <interval> s and keep the
# run's figures so far in <outfile> as "<delay_ns> <steal_first_ns>
# <steal_last_ns> <readings>"; vcpu_starve_report turns them into the run's
# line. The delay is ACCUMULATED reading to reading (run_delay_window_ns),
# so a thread that exits mid-run keeps what it accrued up to the last reading
# it was in. One SUMMED first-against-last pair (the old witness) also
# subtracted that thread's whole earlier history, which drove the figure
# negative; one PER-THREAD pair would still drop the exiting thread's share
# of the run. A reading that fails (the process going) is skipped. The pid
# is pinned by its start time (proc_start_ticks): once it names another
# process, sampling stops and the figure so far -- the right process's --
# stands, as when it exits. Two consecutive readings that share no thread
# cannot come from one live process (its main thread's tid is the pid), so
# that case writes "unmeasured" and stops: a figure only where one was
# measured. Steal is box-wide (host_steal_ns), first reading against last.
vcpu_starve_sampler() {
    local pid="$1" outf="$2" interval="${3:-0.2}" prev="" cur w total=0 n=0 s0="" s
    local hz st st0=""
    hz=$(getconf CLK_TCK)
    while kill -0 "$pid" 2>/dev/null; do
        if cur=$(thread_run_delays "$pid") && st=$(proc_start_ticks "$pid") \
           && s=$(host_steal_ns "$hz"); then
            [[ -n "$st0" ]] || st0="$st"
            [[ "$st" == "$st0" ]] || return 0
            if [[ -n "$prev" ]]; then
                if ! w=$(run_delay_window_ns "$prev" "$cur"); then
                    echo unmeasured > "$outf.tmp" && mv -f "$outf.tmp" "$outf"
                    return 0
                fi
                total=$((total + w))
            fi
            [[ -n "$s0" ]] || s0="$s"
            prev="$cur"; n=$((n + 1))
            printf '%s %s %s %s\n' "$total" "$s0" "$s" "$n" > "$outf.tmp" && mv -f "$outf.tmp" "$outf"
        fi
        sleep "$interval"
    done
}

# vcpu_starve_report <outfile> -- "<delay_ms> <steal_ms>" over the sampled
# run, or nothing and return 1 when it was not measured: no file, fewer than
# two readings (a window of no length), or readings that could not be
# compared. Both figures are non-negative by construction, and anything else
# in the file reads as not measured, never as a number.
vcpu_starve_report() {
    local d s1 s2 n
    [[ -s "$1" ]] && read -r d s1 s2 n < "$1" || return 1
    [[ "$d" =~ ^[0-9]+$ && "$s1" =~ ^[0-9]+$ && "$s2" =~ ^[0-9]+$ && "$n" =~ ^[0-9]+$ ]] || return 1
    (( n >= 2 && s2 >= s1 )) || return 1
    printf '%s %s\n' $(( d / 1000000 )) $(( (s2 - s1) / 1000000 ))
}

# budget_scale_ok <value> -- a declared wall-clock budget scale is a decimal in
# [1, 3] with a nonzero leading digit: 0 or 0.5 would shrink a budget below its
# idle value (a false failure by construction), more than 3 (the cap on the
# measured factor too) would cancel it, and junk would abort arithmetic. Both
# budget checks use it: common-test.sh's one-boot run (TEST_BUDGET_SCALE) and
# test/integration/lib/unit-ratchet.sh over pooled runs (AXL_UNIT_BUDGET_SCALE).
budget_scale_ok() {
    [[ "$1" =~ ^[1-9][0-9]*(\.[0-9]+)?$ ]] && awk -v v="$1" 'BEGIN { exit !(v <= 3) }'
}

# host_contention_factor <outfile> [cpus] [cap] -- echoes
# min(cap, max(1, mean runnable / cpus)) to two decimals: how oversubscribed the
# box was while the run shared it. 1.00 means it was not. Returns 1, printing
# nothing, when there were no samples: the caller decides what unmeasured means
# -- it must not be "factor 1" silently.
#
# WHY THE RAW RATIO, which overstates the slowdown. Beside verify.sh's other
# jobs the x64 suite (75-89 s with nothing of ours beside it) took 107-134 s over
# five runs at mean runnable 1.4-2.4 per cpu: a slowdown of 1.4-1.8x, between
# 0.75x and 1.0x the ratio, and not steady -- the guest is partly asleep (24 of
# its 73 idle seconds are wall-clock waits), the other jobs run niced, and the
# box is shared with other people's work. A damped factor (K = 0.5-0.6 was
# tried) fell 17% short of a clean run in one of the five, and a gate that
# fails a clean run is worse than a loose one. So the budget scales by the raw
# ratio, and what that costs is margin: up to ~85% over a heavily contended
# run, against ~27% idle. The idle margin is the one that catches a regression
# -- run `verify.sh --only=x64` (no neighbours, factor 1.00) for that. The cap
# keeps a pathological box from cancelling the budget altogether. The mean
# includes the guest and the sampler's own wakeups (~1-2 runnable), far below
# the cpu count, so an idle box reads 1.00.
#
# CPUS IS THE HOST'S, not `nproc`: procs_running counts the whole host, and
# nproc shrinks under taskset or a cpuset cgroup, which would inflate the factor
# toward its cap with nothing wrong.
host_contention_factor() {
    local file="$1" cpus="${2:-$(getconf _NPROCESSORS_ONLN)}" cap="${3:-3}" sum n
    [[ -s "$file" ]] && read -r sum n < "$file" && (( n > 0 )) || return 1
    awk -v s="$sum" -v n="$n" -v c="$cpus" -v cap="$cap" \
        'BEGIN { f = (s / n) / c; if (f < 1) f = 1;
                 if (f > cap) f = cap; printf "%.2f", f }'
}

# tools_dir_stale <tools-dir> -- print (one per line) each *.efi in the dir that
# is OLDER than the library in the sibling lib/ (<prefix>/lib/libaxl.a). Every
# tool of this tree links libaxl.a, so a tool built before the current library
# is either a leftover from another build (a release's RTK image, an old
# checkout) or a missed rebuild -- and a test that enumerates tools/*.efi tests
# it as if it were current. Returns 1 with a message, printing nothing, when the
# library cannot be found: "nothing stale" must not be what an unchecked dir
# reports. Prints nothing and returns 0 when every tool is current.
tools_dir_stale() {
    local dir="$1" lib f
    lib="$(dirname "$dir")/lib/libaxl.a"
    if [[ ! -f "$lib" ]]; then
        echo "tools_dir_stale: no $lib to compare against" >&2
        return 1
    fi
    for f in "$dir"/*.efi; do
        [[ -e "$f" ]] || continue
        if [[ "$f" -ot "$lib" ]]; then basename "$f"; fi
    done
    return 0
}

# duration_summary <elapsed_ms> -- enforce MAX_DURATION, if one is set.
# Returns DURATION_EXIT on a breach so the caller can surface it as the run's
# status, exactly as cpu_summary does for a spike. No budget set = no-op.
duration_summary() {
    local elapsed_ms="${1:-0}"
    [[ -z "$MAX_DURATION" ]] && return 0
    local budget_ms=$((MAX_DURATION * 1000))
    if (( elapsed_ms > budget_ms )); then
        printf "FAIL: duration budget exceeded - ran %d.%03ds (budget %ss)\n" \
            $((elapsed_ms / 1000)) $((elapsed_ms % 1000)) "$MAX_DURATION" >&2
        return "$DURATION_EXIT"
    fi
    return 0
}

# Milliseconds since the epoch, for the duration budget.
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

# ---------------------------------------------------------------------------
# Wall-time accounting: the TIMING lines. Always on, CI included, so a run's
# logs alone say where each test's wall time went (guest boot, guest run, pool
# lease and release, host-side setup and checks). One line per event, every
# seconds value with three decimals, `-` where a value is unknowable:
#
#   TIMING: guest kind=<booted|pooled> via=<fg|bg|direct> guest=<s> boot=<s|-> rc=<n|-|unfinished>
#       common-test.sh, when one guest run ends. guest = the run's wall time:
#       booted, launch to QEMU's exit (firmware boot included); pooled, from
#       entering the lease's directory to the end of the exec (fg) or of the
#       stream (bg). boot = launch to the Shell's banner on the serial console
#       (AXL_TIMING_BOOT_MARK: firmware init + BDS + Shell start); `-` on a
#       pooled run (the pool booted it) or a guest that printed no banner.
#       rc = the guest's status (124: the harness's limit); `-` for a
#       background run (nothing waits for its status); `unfinished` for a run
#       still going when the test exited (killed, or failed mid-run).
#   TIMING: test=<name> total=<s> setup=<s|-> guest=<s> boot=<s> between=<s|-> post=<s|-> booted=<n> pooled=<n> start=<kind|-> lease=<ms|->
#       common-test.sh, once, as test_cleanup begins (before its teardown, so
#       a teardown that hangs cannot lose it). total = from sourcing
#       common-test.sh. setup = before the first guest run; post = after the
#       last one ended; between = the host time between the first run's start
#       and the last one's end that no run covered. `-`: no guest run, so no
#       split. guest/boot = the sums of the guest lines above (boot over the
#       runs that had a reading). start/lease: a pooled test's lease -- how
#       its guest came to be clean (booted|restored|cold) and the wait for it
#       in ms, as `axlpool run` measured it (AXL_POOL_START, AXL_POOL_LEASE_MS).
#   TIMING: run-qemu mode=<fg|bg|shot|interactive|window> guest=<s> boot=<s|-> rc=<n|unfinished>
#       scripts/run-qemu.sh, when its guest ends (for --background, from the
#       watchdog subshell, after this script has returned). guest/boot as above.
#   TIMING: run-qemu mode=pooled guest=<s> boot=- restore=<s> rc=<n|unfinished>
#       scripts/run-qemu.sh on a leased guest (its pooled mode): guest = from
#       the mid-lease restore's start to the end of the run, restore = that
#       restore's share of it. rc = exec's status (124: --timeout cut it off).
#       lib/timing-report.py counts it as a pooled run.
#   TIMING: runner test=<name> attempt=<n> via=<boot|pool> wall=<s> cpu=<s> rc=<n>
#       lib/run-one.sh, after each run of a test (a --background guest that
#       outlives its test may still append its own line after this one):
#       its whole wall time, the pool's lease and release included. cpu =
#       user + system of every process the attempt REAPED (the test, its
#       QEMUs, builds, tools), from the runner's cutime + cstime. The runner
#       is a child subreaper (lib/subreaper.py), so an orphan -- a
#       `run-qemu.sh --background` guest's watchdog -- is reaped by it too.
#       Not in it: a process still running when the line is written, one
#       killed with nothing left to reap it, and a pooled guest's QEMU (the
#       pool's).
#   axlpool run: leased <guest> <class>/<arch> (<start>, pool start <ms> ms) in <ms> ms
#   axlpool run: released <guest> (<outcome>) in <ms> ms
#       test/pool/axlpool/client.py, the pooled run's wrapper (stderr).
#
# WHERE THEY GO. To $AXL_TEST_TIMING_LOG when it is set -- lib/run-one.sh
# sets it to the test's own log, which it opens O_APPEND so an appended line
# lands between the test's output instead of being overwritten by it. That is
# what makes run-qemu.sh's line survive: 46 tests capture run-qemu.sh's
# stdout+stderr in a variable and parse it, so a line on stderr would be
# swallowed (and could break an exact match). Unset, every producer writes
# only to a TERMINAL, never to a stderr that is not one: a caller capturing a
# harness snippet's output with 2>&1 (test-duration-budget-selftest.sh reads
# its last line) must never find a TIMING line in it. Started from a terminal,
# common-test.sh points the variable at that terminal for its children.
#
# lib/timing-report.py reads a run's log directory and tabulates them.
# ---------------------------------------------------------------------------
# axl_strip_escapes FILE -- FILE with terminal escape sequences removed, to
# stdout (carriage returns kept: callers decide). The ONE definition, for
# run-qemu.sh's clean log and common-test.sh's: the parameter class is the
# full CSI parameter range (ECMA-48 0x30-0x3F), so DEC private modes like
# ESC[=3h and ESC[?25l strip, not only numeric CSI like ESC[2J; and the
# ESC( / ESC) charset designators UEFI consoles emit on init go too.
axl_strip_escapes() {
    sed -E '
        s/\x1b\[[0-9;:<=>?]*[a-zA-Z@`{|}~]//g
        s/\x1b[()][A-Za-z0-9]//g
    ' "$1"
}

AXL_TIMING_BOOT_MARK='UEFI Interactive Shell'

# axl_timing_now VAR -- microseconds since the epoch into VAR. No fork on
# bash 5 ($EPOCHREALTIME; its radix follows the locale, so every non-digit is
# dropped -- the fraction is always six digits).
axl_timing_now() {
    if [[ -n "${EPOCHREALTIME:-}" ]]; then
        printf -v "$1" '%s' "${EPOCHREALTIME//[!0-9]/}"
    else
        printf -v "$1" '%s' "$(date +%s%6N)"
    fi
}

# axl_timing_secs VAR US -- US microseconds as seconds, three decimals, into
# VAR ("12.345"); a negative span (clock steps) reads 0.000.
axl_timing_secs() {
    local ms=$(( $2 / 1000 ))
    (( ms < 0 )) && ms=0
    printf -v "$1" '%d.%03d' $(( ms / 1000 )) $(( ms % 1000 ))
}

# axl_timing_emit LINE -- LINE to $AXL_TEST_TIMING_LOG (one >> per line, so
# concurrent writers interleave whole lines), else to stderr only when it is a
# terminal, else nowhere.
axl_timing_emit() {
    if [[ -n "${AXL_TEST_TIMING_LOG:-}" ]]; then
        printf '%s\n' "$1" >> "$AXL_TEST_TIMING_LOG" 2>/dev/null || true
    elif [[ -t 2 ]]; then
        printf '%s\n' "$1" >&2
    fi
    return 0
}

# axl_timing_watch PID LOG OUT [noend] -- a background watcher (its pid in
# AXL_TIMING_WATCH_PID): OUT.boot gets the time LOG first shows the Shell
# banner, OUT.end the time PID ended (not with `noend`: then it writes
# nothing once PID is gone, and exits with it). Whoever removes OUT's
# directory stops it first with axl_timing_unwatch. Either file stays absent when it did not
# happen; the reader takes "now" for a missing end. LOG may be empty (no file
# to read: no boot reading). One subshell and two tails per guest, no loop.
#
# A `read` loop, not `grep -m1 -q`: grep held its match until the tail
# feeding it exited (measured: a banner at 0.5 s reported at 1.5 s, the
# writer's lifetime). Both tails take --pid, so they end with the guest even
# if this subshell is killed; an empty PID is refused, since `tail --pid=`
# with no pid follows forever. Output to /dev/null: a watcher must not hold
# a caller's $(...) open.
axl_timing_watch() {
    local pid="$1" log="$2" out="$3" noend="${4:-}"
    AXL_TIMING_WATCH_PID=""
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 0
    (
        if [[ -n "$log" ]]; then
            while IFS= read -r line; do
                if [[ "$line" == *"$AXL_TIMING_BOOT_MARK"* ]]; then
                    axl_timing_now now
                    printf '%s\n' "$now" > "$out.boot"
                    break
                fi
            done < <(exec tail -n +1 -s 0.1 --pid="$pid" -F -- "$log" 2>/dev/null)
        fi
        [[ "$noend" == noend ]] && exit 0
        tail --pid="$pid" -s 0.2 -f /dev/null 2>/dev/null || true
        axl_timing_now now
        # TEST HOOK (test-timing-lines.sh): hold the write, deaf to TERM, as a
        # watcher caught mid-write is; and record a write whose directory is
        # already gone -- the cleanup removed it while this could still write.
        if [[ -n "${AXL_TIMING_TEST_HOLD:-}" ]]; then
            trap '' TERM
            sleep "$AXL_TIMING_TEST_HOLD"
            [[ -d "${out%/*}" ]] || printf '%s\n' "$out" >> "${AXL_TIMING_TEST_LATE:-/dev/null}"
        fi
        printf '%s\n' "$now" > "$out.end"
    ) > /dev/null 2>&1 &
    AXL_TIMING_WATCH_PID=$!
    # Out of the job table: a caller's bare `wait` or `jobs -p` must not see
    # it, and killing it must print no "Terminated" job report.
    disown "$AXL_TIMING_WATCH_PID" 2>/dev/null || true
}

# axl_timing_unwatch PID -- stop a watcher and WAIT until it has exited, so
# a cleanup may then remove the directory it writes into: a file it created
# mid-`rm -rf` would make the final rmdir fail and leave the tree behind.
# TERM, polled up to 1 s; then KILL, polled up to 1 s more -- bounded, so it
# never hangs a cleanup. Polls `kill -0`: a disowned watcher is not ours to
# `wait` for, and bash still reaps it, so it does not linger as a zombie.
axl_timing_unwatch() {
    local pid="$1" i sig
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 0
    for sig in TERM KILL; do
        kill -s "$sig" "$pid" 2>/dev/null || return 0
        for (( i = 0; i < 20; i++ )); do
            kill -0 "$pid" 2>/dev/null || return 0
            sleep 0.05
        done
    done
    return 0
}

# axl_timing_read VAR FILE -- the stamp a watcher wrote to FILE into VAR, or
# empty when it wrote none.
axl_timing_read() {
    local v=""
    [[ -s "$2" ]] && read -r v < "$2" 2>/dev/null
    [[ "$v" =~ ^[0-9]+$ ]] || v=""
    printf -v "$1" '%s' "$v"
}

# Start the background CPU sampler against a running QEMU pid. No-op (leaves the
# globals empty) when both warn and report are off, or the pid is empty.
# Usage: cpu_monitor_start <qemu-pid> [tmpdir] [until-pid]   (tmpdir defaults
# to $TMPDIR; until-pid: see cpu_sampler)
cpu_monitor_start() {
    local qpid="$1" tmpdir="${2:-$TMPDIR}" until="${3:-}"
    CPU_SUMMARY_FILE=""
    CPU_SAMPLER_PID=""
    [[ "$CPU_WARN" != "true" && "$CPU_REPORT" != "true" ]] && return 0
    [[ -z "$qpid" ]] && return 0
    CPU_SUMMARY_FILE="$tmpdir/cpu-summary.txt"
    cpu_sampler "$qpid" "$CPU_SUMMARY_FILE" ${until:+"$until"} &
    CPU_SAMPLER_PID=$!
}

# Wait for the sampler (QEMU must already be dead/dying) and summarize. Returns
# cpu_summary's status (CPU_SPIKE_EXIT on a breach, else 0). Safe when the monitor
# never started. Callers must capture the status (`|| rc=$?`) under `set -e`.
cpu_monitor_finish() {
    if [[ -n "$CPU_SAMPLER_PID" ]]; then
        wait "$CPU_SAMPLER_PID" 2>/dev/null || true
    fi
    [[ -z "$CPU_SUMMARY_FILE" ]] && return 0
    cpu_summary "$CPU_SUMMARY_FILE"
}
