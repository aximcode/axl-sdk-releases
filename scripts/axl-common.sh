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

# --------------------------------------------------------------------------
# build_qemu_base_cmd <arch> <qemu_bin> <mem> <vars_file> [cpu_override]
# Outputs the base QEMU arguments (machine, CPU, memory, pflash) to stdout,
# NUL-separated for safe consumption via mapfile -d ''.
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
            printf '%s\0' "-device" "virtio-rng-pci"
            ;;
    esac

    printf '%s\0' "-m" "$mem"
    printf '%s\0' "-drive" "if=pflash,format=raw,readonly=on,file=$FW_CODE"
    printf '%s\0' "-drive" "if=pflash,format=raw,file=$vars_file"
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
# on a missing tool. `find` prints top-level entries so mcopy -s auto-creates the
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
    dd if=/dev/zero of="$target" bs=1K count="$size_kb" 2>/dev/null
    mkfs.vfat -F 32 -n "$label" "$target" >/dev/null
    (
        cd "$staging" || exit 1
        local entry
        for entry in $(find . -maxdepth 1 -mindepth 1 -printf '%P\n'); do
            mcopy -s -i "$target" "$entry" "::/"
        done
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
    fwtool_host="$project_root/out/native-x64/build/fwtool-host"
    if [[ ! -x "$fwtool_host" ]]; then
        make -C "$project_root" ARCH=x64 fwtool-host >/dev/null 2>&1 || true
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
# tree / standalone integration test still gets it without a prior `make all`.
# --------------------------------------------------------------------------
find_shell_launcher() {
    local arch="$1"
    local scripts_dir project_root native_arch launcher
    scripts_dir="$(dirname "${BASH_SOURCE[0]}")"
    project_root="$(cd "$scripts_dir/.." && pwd)"
    native_arch="$(arch_dir "$arch")"
    launcher="$project_root/out/native-$native_arch/axl-shell-launcher.efi"
    # Always invoke make (not just when the .efi is absent): the launcher depends
    # on its source and libaxl.a, so a PRESENT-BUT-STALE binary — built against
    # an older library — must be rebuilt, not reused. make's own dependency check
    # makes this a no-op when the binary is already fresh. (A stale launcher was
    # the cause of a hang mistaken for the launcher being broken.)
    make -C "$project_root" ARCH="$native_arch" shell-launcher >/dev/null 2>&1 || true
    if [[ -f "$launcher" ]] && head -c2 "$launcher" | grep -q "MZ"; then
        echo "$launcher"
        return 0
    fi
    return 1
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
    mkdir -p "$staging/EFI/BOOT"
    # Booting the Shell DIRECTLY is the default — it is what every consumer's
    # ambient-Shell boot has always relied on. The -delay 0 launcher (which skips
    # the 5 s startup countdown) is OPT-IN via AXL_SHELL_LAUNCHER=1: it has been
    # observed to hang some firmware (the guest loads \EFI\BOOT\BOOTX64.EFI but
    # never chains to the Shell), so it must not be the default until that fault
    # is fixed. When enabled, the launcher is staged as the boot binary with
    # Shell.efi beside it for its sibling resolution.
    local launcher=""
    if [[ "${AXL_SHELL_LAUNCHER:-0}" == "1" ]]; then
        launcher=$(find_shell_launcher "$arch") || launcher=""
    fi
    if [[ -n "$launcher" ]]; then
        cp "$launcher" "$staging/EFI/BOOT/$boot_name"
        cp "$shell_efi" "$staging/EFI/BOOT/Shell.efi"
    else
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
    port=$(sed -n "s/.*hostfwd=tcp::\([0-9]\+\)-.*/\1/p" <<<"$line" | head -1)
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
        # Warm-up, but poll rather than one long sleep: a guest that exits
        # during the window (most integration tests finish in 10-20s) would
        # otherwise keep the sampler — and the caller waiting on it — alive
        # for the full warm-up with nothing left to measure. Same measurement,
        # no added latency.
        for (t = 0; t < warmup; t += interval) {
            if (!alive(pid)) { print "0.00 0.00 0.000"; exit 0 }
            system("sleep " interval)
        }
        prev = read_total(pid)
        if (prev < 0) { print "0.00 0.00 0.000"; exit 0 }
        peak = 0; streak = 0; streak_max = 0; sum = 0; n = 0
        while (alive(pid)) {
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

# Start the background CPU sampler against a running QEMU pid. No-op (leaves the
# globals empty) when both warn and report are off, or the pid is empty.
# Usage: cpu_monitor_start <qemu-pid> [tmpdir]   (tmpdir defaults to $TMPDIR)
cpu_monitor_start() {
    local qpid="$1" tmpdir="${2:-$TMPDIR}"
    CPU_SUMMARY_FILE=""
    CPU_SAMPLER_PID=""
    [[ "$CPU_WARN" != "true" && "$CPU_REPORT" != "true" ]] && return 0
    [[ -z "$qpid" ]] && return 0
    CPU_SUMMARY_FILE="$tmpdir/cpu-summary.txt"
    cpu_sampler "$qpid" "$CPU_SUMMARY_FILE" &
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
