#!/bin/bash
# ci-cmake.sh -- the ONE CMake every CI and release workflow builds with.
#
# WHY. The tree builds with CMake + Ninja only (the Makefile went in 5.0.0),
# and the workflows took whatever CMake their image had: ci.yml's containers
# apt's 4.2.3 (ubuntu:26.04), its integration job the self-hosted host's, and
# release.yml the hosted runner image's 3.31.6. So CI's release-parity check
# certified tarballs a DIFFERENT CMake from the release's built. One version,
# pinned here with its checksums, is installed by every job that configures
# the tree (docs/AXL-Build-System-Design.md §10.12, owner decision 4).
#
# Kitware's own release tarball, not a distro package (apt cannot pin a
# version across 24.04 runners and 26.04 containers) and not an action
# (a third-party action would be one more thing that runs with the job's
# token). Only curl, tar and sha256sum, which every job already has.
#
# Into $RUNNER_TEMP, so every job downloads it (~50 MB, ~2 s): a persistent
# directory would be shared by the root container jobs and the host job, and
# a version bump would then have to replace a tree another user extracted.
#
# THE FLOOR. What SHIPS -- the CMake package install.sh generates and
# sdk/examples -- builds with the CONSUMER's CMake, at the minimum those
# declare (`cmake_minimum_required(VERSION 3.20)`; the package refuses older
# with a FATAL_ERROR). The same installer fetches the last release of that
# minor, 3.20.6, so test-cmake-package.sh can build the package under it, and
# one release BELOW it, 3.16.9 (the floor until 3.20 replaced it), so the
# test can watch the package refuse it (§10.13). Kitware built 3.20+ for
# x86_64 and aarch64 and named it `linux`, like the pin; 3.16 exists for
# x86_64 only and says `Linux`.
#
# Usage:
#   scripts/ci-cmake.sh --version          the pinned version
#   scripts/ci-cmake.sh --floor-version    the consumer floor version
#   scripts/ci-cmake.sh --below-floor-version
#                                          the version the package must refuse
#   scripts/ci-cmake.sh name [--floor|--below-floor]
#                                          the directory that version's
#                                          tarball extracts to, on this
#                                          machine (exit 3 if none)
#   scripts/ci-cmake.sh install [--floor|--below-floor] DIR
#                                          install the pinned (floor, below-
#                                          floor) CMake under DIR (reused when
#                                          already there and verified), and
#                                          print its bin/ directory -- for
#                                          `>> "$GITHUB_PATH"`
#
# Exit 0 on success; 1 on a download or checksum failure, or a cmake that
# does not report the version asked for; 2 on a usage error; 3 on a machine
# that version has no build for (its own status, so a caller can tell "not
# built for this machine" from "this script did not understand me").

set -euo pipefail

AXL_CI_CMAKE_VERSION=4.3.6
# From Kitware's cmake-4.3.6-SHA-256.txt (github.com/Kitware/CMake releases;
# cmake.org/files/v4.3/ publishes the same two lines).
AXL_CI_CMAKE_SHA256_x86_64=fba967b44c7b40f155f9492dacb5477aab8333699986e1e07a7d90803aba7219
AXL_CI_CMAKE_SHA256_aarch64=310df7369f818779e269d900ae5a3ef02c6089fb7b1a49fc1a2aaf7e7275a2dc

AXL_FLOOR_CMAKE_VERSION=3.20.6
# From Kitware's cmake-3.20.6-SHA-256.txt (cmake.org/files/v3.20/).
AXL_FLOOR_CMAKE_SHA256_x86_64=458777097903b0f35a0452266b923f0a2f5b62fe331e636e2dcc4b636b768e36
AXL_FLOOR_CMAKE_SHA256_aarch64=9ac2035174d3c62827c85fb052372d70cf35fb565e8d47b8241fbe451c29b2f2

AXL_BELOW_FLOOR_CMAKE_VERSION=3.16.9
# From Kitware's cmake-3.16.9-SHA-256.txt (github.com/Kitware/CMake releases).
AXL_BELOW_FLOOR_CMAKE_SHA256_x86_64=96d79367dcafb47d606573a4687021d3cb18dd53d83458fc1d6d9c89bf31b553

usage() {
    echo "usage: ci-cmake.sh --version | --floor-version | --below-floor-version | name [--floor|--below-floor] | install [--floor|--below-floor] DIR" >&2
    exit 2
}

kind=pinned
case "${1:-}" in
    --version)             [[ $# -eq 1 ]] || usage; echo "$AXL_CI_CMAKE_VERSION"; exit 0 ;;
    --floor-version)       [[ $# -eq 1 ]] || usage; echo "$AXL_FLOOR_CMAKE_VERSION"; exit 0 ;;
    --below-floor-version) [[ $# -eq 1 ]] || usage; echo "$AXL_BELOW_FLOOR_CMAKE_VERSION"; exit 0 ;;
    name|install)
        cmd="$1"; shift
        case "${1:-}" in
            --floor)       kind=floor; shift ;;
            --below-floor) kind=below-floor; shift ;;
        esac
        if [[ "$cmd" == name ]]; then
            [[ $# -eq 0 ]] || usage
        else
            [[ $# -eq 1 && -n "$1" && "$1" != -* ]] || usage
            dir="$1"
        fi ;;
    *)                     usage ;;
esac

machine="$(uname -m)"
# The machines each version is built for, spelled out rather than inferred
# from which checksum variables exist (the environment could supply one).
why=""
case "$kind" in
    pinned)      version="$AXL_CI_CMAKE_VERSION" os=linux prefix=AXL_CI_CMAKE
                 machines="x86_64 aarch64" ;;
    floor)       version="$AXL_FLOOR_CMAKE_VERSION" os=linux prefix=AXL_FLOOR_CMAKE
                 machines="x86_64 aarch64" ;;
    below-floor) version="$AXL_BELOW_FLOOR_CMAKE_VERSION" os=Linux prefix=AXL_BELOW_FLOOR_CMAKE
                 machines="x86_64" why=" (Kitware built 3.16 for x86_64 only)" ;;
esac
if [[ " $machines " != *" $machine "* ]]; then
    echo "ci-cmake.sh: no $kind CMake $version for machine '$machine'$why" >&2
    exit 3
fi
sha_var="${prefix}_SHA256_$machine"
sha="${!sha_var}"
name="cmake-$version-$os-$machine"
if [[ "$cmd" == name ]]; then
    echo "$name"
    exit 0
fi

mkdir -p "$dir"
root="$(cd "$dir" && pwd)"
bin="$root/$name/bin"

# reports_version BIN -- the cmake there says it is the version asked for.
# A tree with the right NAME is not proof (an interrupted extraction leaves
# one), so the version is asked of the binary itself.
reports_version() {
    [[ -x "$1/cmake" ]] \
        && [[ "$("$1/cmake" --version 2>/dev/null | head -1)" == "cmake version $version" ]]
}

if ! reports_version "$bin"; then
    tmp="$(mktemp -d "$root/.ci-cmake.XXXXXX")"
    trap 'rm -rf "$tmp"' EXIT
    url="https://github.com/Kitware/CMake/releases/download/v$version/$name.tar.gz"
    echo "ci-cmake.sh: fetching $url" >&2
    if ! curl -fsSL --retry 3 -o "$tmp/$name.tar.gz" "$url"; then
        echo "ci-cmake.sh: download failed: $url" >&2
        exit 1
    fi
    if ! echo "$sha  $tmp/$name.tar.gz" | sha256sum -c --status -; then
        echo "ci-cmake.sh: $name.tar.gz does not match its $kind SHA-256 ($sha)" >&2
        exit 1
    fi
    tar -xzf "$tmp/$name.tar.gz" -C "$tmp" || {
        echo "ci-cmake.sh: could not extract $name.tar.gz" >&2; exit 1; }
    rm -rf "${root:?}/$name"
    mv "$tmp/$name" "$root/$name"
    if ! reports_version "$bin"; then
        echo "ci-cmake.sh: $bin/cmake does not report version $version" >&2
        exit 1
    fi
fi
echo "$bin"
