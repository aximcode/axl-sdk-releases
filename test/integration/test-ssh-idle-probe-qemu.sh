#!/bin/bash
# test-meta: arch=both needs=ssh est=80 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-idle-probe-qemu.sh --
# one part of the AxlSsh idle suite:
# client-alive probes: none while a hook holds the loop or the peer talks,
# and the drop of a silent peer, by channel and by global request.
#
# The body -- the boot, the servers, every scenario, and the helpers of
# lib/ssh-hostile-lib.sh, lib/ssh-channel-lib.sh and lib/ssh-idle-lib.sh --
# is lib/ssh-idle-cases.sh, shared by every part;
# this file only picks its scenarios. That file's header says how the
# suite is split and why.
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control.
#
# Usage: ./test/integration/test-ssh-idle-probe-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh idle probe"
HOSTILE_SCENARIOS="hook-gated-not-dropped peer-silent-channel-probe chatty-peer-not-probed peer-silent-dropped disconnect-control"

source "$(dirname "$0")/lib/ssh-idle-cases.sh"
