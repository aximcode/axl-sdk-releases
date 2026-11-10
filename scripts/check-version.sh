#!/bin/bash
# check-version.sh -- VERSION and axl-version.h's AXL_VERSION_STRING agree.
#
# scripts/bump-version.sh writes both; a tag cut without it ships tarballs
# named from VERSION holding binaries that report the header's string. The
# CMake configure refuses the mismatch (CMakeLists.txt); this is the same
# check for scripts/cut-release.sh, which must not configure a tree to ask.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
file_ver=$(cat "$REPO/VERSION" 2>/dev/null) || { echo "ERROR: cannot read VERSION"; exit 1; }
header_ver=$(sed -n 's/^#define AXL_VERSION_STRING  *"\(.*\)".*/\1/p' "$REPO/include/axl/axl-version.h")
if [[ -z "$file_ver" || "$file_ver" != "$header_ver" ]]; then
    echo "ERROR: VERSION ($file_ver) != axl-version.h ($header_ver)"
    echo "       run scripts/bump-version.sh X.Y.Z to update both"
    exit 1
fi
echo "check-version: VERSION and axl-version.h agree ($file_ver)"
