#!/bin/bash
# pool-check.sh -- refuse a run that ASKED for the pool when the pool cannot
# serve it, rather than quietly booting everything: that run would not be
# what was asked for.
#
# One copy, two callers: run-integration.sh --pool, before any test starts,
# and the CTest registration's integration.pool fixture (every guest= test
# requires it in pool mode; cmake/AxlIntegrationTests.cmake.in).
#
# Usage: lib/pool-check.sh SOCK ARCH PROJECT_DIR [PREFIX [auto]]
#   ARCH    X64 | AARCH64 -- the service must have an agent for it
#   PREFIX  what the refusal names as the asker (default "pool")
#   auto    the pool was NOT asked for, only defaulted to (AXL_IT_POOL=auto,
#           the runner's default): an unusable service is SAID, on stdout, and
#           is not a failure -- the guest= tests then miss and boot.
#
# Exit: 0 the service answers and has an agent for ARCH (its status line is
# printed), or auto and it does not ("pool: unavailable (...) -- guest= tests
# boot"); 2 it does not, asked for (the reason is on stderr).
set -uo pipefail

sock="${1:?usage: pool-check.sh SOCK ARCH PROJECT_DIR [PREFIX]}"
arch="${2:?usage: pool-check.sh SOCK ARCH PROJECT_DIR [PREFIX]}"
project="${3:?usage: pool-check.sh SOCK ARCH PROJECT_DIR [PREFIX]}"
prefix="${4:-pool}"
mode="${5:-}"
refuse() {   # refuse REASON
    if [[ "$mode" == auto ]]; then
        echo "pool: unavailable ($1) -- guest= tests boot"
        exit 0
    fi
    echo "$prefix, but $1" >&2
    exit 2
}

# 2>&1 on the whole group: a failed `cd` is a reason too, not an empty one.
if ! status=$( { cd "$project/test/pool" && python3 -m axlpool status --socket "$sock"; } 2>&1); then
    refuse "$status"
fi
# A service with no agent for this arch would miss every lease: every
# guest= test run twice and the run never pooled. Refused like no service.
if ! grep -qw "$arch" <<< "${status#*agents:}"; then
    refuse "the service has no agent for $arch ($status)"
fi
printf '%s\n' "$status"
