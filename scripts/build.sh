#!/bin/bash

# AXL Build Script
# Builds AXL library and test applications using gcc.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
PARALLEL_JOBS=$(nproc)

# Shared library (logging, QEMU)
source "$SCRIPT_DIR/axl-common.sh"

# Parse arguments
BUILD_ARCHS=()
BUILD_TARGET="DEBUG"
BUILD_CLEAN=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --arch)
            BUILD_ARCHS+=("$2")
            shift 2
            ;;
        --release)
            BUILD_TARGET="RELEASE"
            shift
            ;;
        --clean)
            BUILD_CLEAN=true
            shift
            ;;
        -n|-j)
            PARALLEL_JOBS="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [--arch ARCH]... [--release] [--clean] [-n JOBS]"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Check UEFI manifest coverage
# ---------------------------------------------------------------------------

if [[ -f "$PROJECT_ROOT/scripts/uefi-manifest.json5" && \
      -d "$PROJECT_ROOT/deps/uefi-spec" ]]; then
    python3 "$PROJECT_ROOT/scripts/generate-uefi-headers.py" \
        --check "$PROJECT_ROOT/src" "$PROJECT_ROOT/include/axl" \
        --extra-header "$PROJECT_ROOT/include/uefi/axl-uefi-extra.h" \
        --manifest "$PROJECT_ROOT/scripts/uefi-manifest.json5" \
        --input "$PROJECT_ROOT/deps/uefi-spec" \
        2>/dev/null || {
        log_warning "UEFI manifest check failed — run with --verbose for details"
    }
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

[[ ${#BUILD_ARCHS[@]} -eq 0 ]] && BUILD_ARCHS=(X64 AARCH64)

# Map arch names to Makefile values
declare -A NATIVE_ARCH_MAP=(
    [X64]=x64
    [AARCH64]=aa64
)

for arch in "${BUILD_ARCHS[@]}"; do
    native_arch="${NATIVE_ARCH_MAP[$arch]}"
    if [[ -z "$native_arch" ]]; then
        log_error "Unsupported architecture: $arch"
        exit 1
    fi

    native_build="DEBUG"
    [[ "$BUILD_TARGET" == "RELEASE" ]] && native_build="RELEASE"

    if $BUILD_CLEAN; then
        make -C "$PROJECT_ROOT" \
            PREFIX="out/native-$native_arch" clean
    fi

    log_info "Building AXL for $arch ($native_build)..."
    make -C "$PROJECT_ROOT" \
        ARCH="$native_arch" \
        PREFIX="out/native-$native_arch" \
        BUILD="$native_build" \
        all tests hello gfx-demo runtime-demo \
        echo-server tcp-echo-server echo-client echo-server-sync \
        -j "$PARALLEL_JOBS" || exit 1
    log_success "$arch: build complete"
done
