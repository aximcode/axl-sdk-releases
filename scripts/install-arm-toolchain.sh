#!/bin/bash
# DEPRECATED -- kept as a compatibility shim.
#
# The AArch64 and x86_64 bare-metal toolchains are now installed by one
# script with one version table (scripts/install-toolchain.sh, reading
# scripts/axl-toolchains.conf). This wrapper stays because the old name is
# referenced from README.md, several design docs, and the release workflow;
# renaming those in a sweep would have edited prose that describes history
# correctly.
#
# New callers should use:  ./scripts/install-toolchain.sh aa64
#
# Usage: ./scripts/install-arm-toolchain.sh

set -euo pipefail

HERE="$(cd -P "$(dirname "$0")" && pwd)"

echo "[install-arm-toolchain] NOTE: superseded by install-toolchain.sh;" >&2
echo "                        forwarding to: install-toolchain.sh aa64" >&2

exec "$HERE/install-toolchain.sh" aa64
