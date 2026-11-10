#!/bin/bash

# AXL Build Script
# Builds the AXL library, tests, tools and fixtures for one or both arches,
# through scripts/axl-build.sh (CMake + Ninja).

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

# Map arch names to the build's spelling
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

    # The CMake tree for ARCH x BUILD (build/<preset>), named by the wrapper
    # every build goes through.
    tree="$("$PROJECT_ROOT/scripts/axl-build.sh" --print-prefix --abs \
            ARCH="$native_arch" BUILD="$native_build")" || exit 1
    if $BUILD_CLEAN; then
        log_info "Removing $tree"
        # Under the tree's build lock (scripts/axl-build.sh's _lock_tree:
        # <tree>.axl-build.lock), so a build running in it finishes before its
        # tree goes. Without flock, as axl-build.sh does: say so and go on.
        mkdir -p "$(dirname "$tree")"
        if command -v flock >/dev/null 2>&1; then
            ( flock 9 && rm -rf -- "${tree:?}" ) 9>"$tree.axl-build.lock"
        else
            log_warning "flock not found -- removing $tree without its build lock"
            rm -rf -- "${tree:?}"
        fi
    fi

    log_info "Building AXL for $arch ($native_build)..."
    # `all` is everything: the library, the unit tests, every tool, demo and
    # fixture. Ninja runs one job per core already, so -n no longer changes
    # anything (it is accepted for old command lines).
    "$PROJECT_ROOT/scripts/axl-build.sh" ARCH="$native_arch" BUILD="$native_build" all || exit 1
    log_success "$arch: build complete"
done
