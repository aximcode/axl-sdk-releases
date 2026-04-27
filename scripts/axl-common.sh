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
# --------------------------------------------------------------------------

QEMU_DIR="${QEMU_DIR:-$HOME/projects/qemu/install/bin}"
MKIMAGE_DIR="${MKIMAGE_DIR:-$HOME/projects/mkimage}"

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
# build_qemu_base_cmd <arch> <qemu_bin> <mem> <vars_file>
# Outputs the base QEMU arguments (machine, CPU, memory, pflash) to stdout,
# NUL-separated for safe consumption via mapfile -d ''.
# Requires FW_CODE to be set by find_firmware() before calling.
# Usage: mapfile -d '' -t cmd < <(build_qemu_base_cmd X64 /path/qemu 512M vars.fd)
# --------------------------------------------------------------------------

build_qemu_base_cmd() {
    local arch="$1" qemu_bin="$2" mem="$3" vars_file="$4"

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
                printf '%s\0' "-enable-kvm" "-cpu" "host"
            fi
            ;;
        AARCH64)
            printf '%s\0' "-machine" "virt" "-cpu" "cortex-a57"
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
# find_qemu — locate QEMU binary for a given architecture
# Usage: QEMU_BIN=$(find_qemu <arch>) || exit 1
# Priority: $QEMU_DIR > system PATH > /usr/libexec
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

    # $QEMU_DIR (from env, defaults to ~/projects/qemu/install/bin)
    if [[ -n "${QEMU_DIR:-}" && -x "$QEMU_DIR/$binary" ]]; then
        echo "$QEMU_DIR/$binary"
        return 0
    fi

    # System install (CI runners, distro packages) — search PATH.
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

    log_error "$binary not found at \${QEMU_DIR}=${QEMU_DIR:-<unset>} or on \$PATH"
    log_error "Install QEMU (apt/dnf install qemu-system-x86) or set QEMU_DIR"
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
    local qemu_share="${QEMU_DIR%/bin}/share/qemu"

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
            log_error "OVMF firmware not found"
            log_error "Set OVMF_CODE or install: dnf install edk2-ovmf / apt install ovmf"
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
            log_error "AAVMF firmware not found"
            log_error "Set AAVMF_CODE or install: dnf install edk2-aarch64 / apt install qemu-efi-aarch64"
            return 1
            ;;
        *)
            log_error "find_firmware: unknown arch '$arch'"
            return 1
            ;;
    esac
}

# --------------------------------------------------------------------------
# find_shell_efi — locate standalone UEFI Shell binary
# Usage: SHELL_EFI=$(find_shell_efi <arch>)
#
# Priority:
#   1. Local EDK2 build (if EDK2_DIR is set)
#   2. Previously extracted (cached next to firmware .fd)
#   3. Extract from QEMU firmware .fd via uefiextract
#
# Requires: find_firmware() must be called first (sets FW_CODE).
# For extraction: uefiextract (from LongSoft/UEFITool) must be in $PATH.
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

    # 3. Extract from firmware .fd via uefiextract
    if ! command -v uefiextract &>/dev/null; then
        log_warning "Shell.efi not found for $arch (install uefiextract from LongSoft/UEFITool to extract from firmware)"
        return 1
    fi

    # Build list of firmware images to try extraction from.
    # The active FW_CODE may not contain an extractable Shell.efi (some
    # system-packaged builds use a format uefiextract can't parse), so
    # also try the QEMU-bundled firmware as a fallback.
    local qemu_share="${QEMU_DIR%/bin}/share/qemu"
    local fw_candidates=("$FW_CODE")
    case "$arch" in
        X64)     [[ -f "$qemu_share/edk2-x86_64-code.fd" ]] && \
                     fw_candidates+=("$qemu_share/edk2-x86_64-code.fd") ;;
        AARCH64) [[ -f "$qemu_share/edk2-aarch64-code.fd" ]] && \
                     fw_candidates+=("$qemu_share/edk2-aarch64-code.fd") ;;
    esac

    local tmpdir
    tmpdir=$(mktemp -d)
    local extract_dir="$tmpdir/out"
    local extracted=false
    for fw_candidate in "${fw_candidates[@]}"; do
        if uefiextract "$fw_candidate" "$_SHELL_GUID" -o "$extract_dir" >/dev/null 2>&1; then
            extracted=true
            break
        fi
        rm -rf "$extract_dir"
    done

    if [[ "$extracted" != "true" ]]; then
        rm -rf "$tmpdir"
        log_warning "Shell.efi extraction failed for $arch"
        return 1
    fi

    # Find the PE32 image section body (the actual Shell.efi binary)
    local pe_body
    pe_body=$(find "$extract_dir" -name "body.bin" -path "*PE32*" | head -1)

    if [[ -z "$pe_body" ]] || ! head -c2 "$pe_body" | grep -q "MZ"; then
        rm -rf "$tmpdir"
        log_warning "Shell.efi not found in firmware image for $arch"
        return 1
    fi

    # Cache next to firmware for future use
    if cp "$pe_body" "$cached" 2>/dev/null; then
        log_info "Extracted Shell.efi for $arch from firmware ($(du -h "$cached" | cut -f1))"
    else
        # Firmware dir not writable — cache in ~/.cache/axl instead
        local user_cache="${XDG_CACHE_HOME:-$HOME/.cache}/axl"
        mkdir -p "$user_cache"
        cached="$user_cache/Shell_${arch}.efi"
        cp "$pe_body" "$cached"
        log_info "Extracted Shell.efi for $arch (cached in $user_cache)"
    fi

    rm -rf "$tmpdir"
    echo "$cached"
    return 0
}
