#!/bin/bash
# sdk-prefix.sh — where the STAGED SDK lives.
#
# Usage:
#   scripts/sdk-prefix.sh [--abs]
#
#   --abs  absolute path instead of the repo-relative one
#
# Honours $AXL_SDK_PREFIX, which is the same value `install.sh --prefix` takes.
#
# WHY THIS EXISTS, and why it is NOT build-prefix.sh with a flag. Two different
# things live under out/ and both were called "the prefix":
#
#   out/native-<arch>[-release][-tls]   the BUILD directory. Intermediate
#                                       objects and images; a function of
#                                       ARCH x BUILD x AXL_TLS. Ask
#                                       scripts/build-prefix.sh.
#   out/{bin,lib,include,share}         the STAGED SDK. What
#                                       `install.sh --prefix` produces, what a
#                                       consumer consumes, and what the .deb
#                                       and .rpm are built from. Ask here.
#
# They answer different questions with different inputs -- one varies with the
# build configuration, the other does not vary at all -- so folding them into
# one script with a mode flag would mean a single entry point whose arguments
# are only valid in half the cases. Two scripts, one question each.
#
# AXL-Distribution-Design.md §4 ("a build directory is not an install prefix")
# is the design; its P2 is the separation. That work looked like a sweep across
# the same ~149 make callers the CMake port's slice 3 touches, and both design
# docs recorded it that way. MEASURED 2026-08-16, that is wrong by a factor of
# twenty: 155 files invoke make and 139 of them already ask build-prefix.sh;
# only 23 reference an out/ path at all; and the genuine overlap between the
# two sweeps is SEVEN files. What P2 actually needs is this accessor plus the
# ~10 callers below -- which is why it exists now rather than being deferred
# behind the port.
#
# The flag spelling deliberately matches build-prefix.sh's. Two accessors that
# disagree about their own flags are worse than one accessor and a hardcoded
# path, because the reader then has to remember which is which.
#
# It is a SCRIPT rather than only a shell function for build-prefix.sh's
# reason: most callers here are standalone integration tests that source
# nothing at all and cannot use a helper from common-test.sh.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ABS=false
for arg in "$@"; do
    case "$arg" in
        --abs) ABS=true ;;
        *)     echo "ERROR: unknown argument '$arg' (usage: $0 [--abs])" >&2
               exit 2 ;;
    esac
done

# Default matches install.sh's own default, so nothing moves unless asked. A
# separation that relocated the staged SDK would be a breaking change wearing
# a refactor's clothes -- every consumer instruction, every package recipe and
# every test names this path today.
PREFIX="${AXL_SDK_PREFIX:-out}"

if $ABS; then
    case "$PREFIX" in
        /*) printf '%s\n' "$PREFIX" ;;                  # already absolute
        *)  printf '%s/%s\n' "$REPO_ROOT" "$PREFIX" ;;  # re-root, not cwd-relative
    esac
else
    printf '%s\n' "$PREFIX"
fi
