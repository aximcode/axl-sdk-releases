#!/bin/bash
# bump-version.sh — atomically update VERSION and axl-version.h.
#
# Usage: scripts/bump-version.sh X.Y.Z
#
# After running, review the diff and commit both files together.
# The Makefile's check-version target will refuse to build if the
# two files disagree, so using this script is the safe way to bump.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 X.Y.Z" >&2
    exit 1
fi

NEW="$1"
if [[ ! "$NEW" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "ERROR: version must be X.Y.Z (got '$NEW')" >&2
    exit 1
fi

IFS='.' read -r MAJ MIN PAT <<< "$NEW"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION_FILE="$REPO_ROOT/VERSION"
HEADER_FILE="$REPO_ROOT/include/axl/axl-version.h"

if [[ ! -f "$VERSION_FILE" ]]; then
    echo "ERROR: $VERSION_FILE not found" >&2
    exit 1
fi
if [[ ! -f "$HEADER_FILE" ]]; then
    echo "ERROR: $HEADER_FILE not found" >&2
    exit 1
fi

echo "$NEW" > "$VERSION_FILE"

sed -i.bak \
    -e "s/^#define AXL_VERSION_MAJOR  *[0-9][0-9]*/#define AXL_VERSION_MAJOR   $MAJ/" \
    -e "s/^#define AXL_VERSION_MINOR  *[0-9][0-9]*/#define AXL_VERSION_MINOR   $MIN/" \
    -e "s/^#define AXL_VERSION_PATCH  *[0-9][0-9]*/#define AXL_VERSION_PATCH   $PAT/" \
    -e "s/^#define AXL_VERSION_STRING .*/#define AXL_VERSION_STRING  \"$NEW\"/" \
    "$HEADER_FILE"
rm "${HEADER_FILE}.bak"

echo "bumped to $NEW — review and commit:"
git --no-pager -C "$REPO_ROOT" diff VERSION include/axl/axl-version.h
