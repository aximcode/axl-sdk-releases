#!/bin/bash
# check-install-parity.sh -- does install.sh stage the same prefix it did
# before its copies became install() rules?
#
# WHY. install.sh staged the SDK by copying files itself until 2026-11-10;
# since then the prefix comes from `cmake --install` (cmake/AxlInstall.cmake)
# and install.sh only builds, installs and publishes
# (docs/AXL-Build-System-Design.md §10.16). The claim that the move changed
# nothing a consumer receives is checked here rather than asserted: the
# copy-based install.sh is taken from git at BASE, run against THIS tree's
# sources and build trees, and its prefix compared with the current
# install.sh's -- the same file set, the same bytes, the same modes
# (scripts/compare-trees.py; mtimes and owners are not compared).
#
# A MIGRATION CHECK, deliberately not a suite test: the old script carries its
# own copy of what the generated files say, so the first intended change to
# axl-config.cmake.in or axl.pc.in makes the two differ for a good reason.
# Run it when changing the install rules' SHAPE (a path, a mode, a component),
# with --base naming the last commit whose stage you trust.
#
# Usage: scripts/check-install-parity.sh [--base REV] [--arch all|x64|aa64]
#                                          [--no-cpp] [--keep DIR]
#   --base REV   the commit whose scripts/install.sh is the reference
#                (default: d50df81d, the last copy-based install.sh)
#   --no-cpp     install C-only (default: --cpp), the path that drops the
#                C++ glue component
#   --keep DIR   leave both prefixes under DIR for inspection
# The environment reaches both runs, so an override (AXL_AA64_GCC=...) or
# AXL_TOOLCHAIN=cross is compared by setting it on this script.
# Exit 0 identical, 1 different, 2 usage or a step that could not run.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$(dirname "$SCRIPT_DIR")"

BASE=d50df81d
ARCH=all
KEEP=""
CPP=--cpp
while [[ $# -gt 0 ]]; do
    case "$1" in
        --base) BASE="${2:?--base needs a revision}"; shift 2 ;;
        --arch) ARCH="${2:?--arch needs all, x64 or aa64}"; shift 2 ;;
        --keep) KEEP="${2:?--keep needs a directory}"; shift 2 ;;
        --no-cpp) CPP=--no-cpp; shift ;;
        -h|--help) sed -n '2,33p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "check-install-parity: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

# The reference script must sit IN scripts/: it finds axl-common.sh, the
# toolchain manifest and the build wrapper beside itself. A dotfile name no
# glob in the tree picks up, unique per run, removed on every exit.
OLD="$(mktemp "$SCRIPT_DIR/.install-parity-base.XXXXXX")"
if [[ -n "$KEEP" ]]; then
    mkdir -p "$KEEP"
    WORK="$(cd "$KEEP" && pwd)"
    rm -rf "${WORK:?}/old" "${WORK:?}/new"
else
    WORK="$(mktemp -d -t axl-instparity.XXXXXXXX)"
fi
cleanup() {
    rm -f "$OLD"
    [[ -n "$KEEP" ]] || rm -rf "$WORK"
}
trap cleanup EXIT

if ! git -C "$SDK_DIR" show "$BASE:scripts/install.sh" > "$OLD" 2>/dev/null \
        || [[ ! -s "$OLD" ]]; then
    echo "check-install-parity: no scripts/install.sh at $BASE" >&2
    exit 2
fi
chmod 755 "$OLD"
if grep -q 'cmake --install\|--component' "$OLD"; then
    echo "check-install-parity: $BASE's install.sh already installs with CMake;" \
         "name the last copy-based commit with --base" >&2
    exit 2
fi

echo "check-install-parity: reference $BASE (copy-based), arch $ARCH, $CPP"
if ! "$OLD" --arch "$ARCH" "$CPP" --prefix "$WORK/old" > "$WORK/old.log" 2>&1; then
    cat "$WORK/old.log" >&2
    echo "check-install-parity: the reference install.sh failed" >&2
    exit 2
fi
if ! "$SCRIPT_DIR/install.sh" --arch "$ARCH" "$CPP" --prefix "$WORK/new" \
        > "$WORK/new.log" 2>&1; then
    cat "$WORK/new.log" >&2
    echo "check-install-parity: the current install.sh failed" >&2
    exit 2
fi
rc=0
python3 "$SCRIPT_DIR/compare-trees.py" "$WORK/old" "$WORK/new" || rc=$?
case "$rc" in
    0) echo "check-install-parity: PASS -- the install-based prefix is the copy-based one" ;;
    1) echo "check-install-parity: FAIL -- the prefixes differ (above)" ;;
    *) echo "check-install-parity: the comparison could not run" >&2; exit 2 ;;
esac
exit "$rc"
