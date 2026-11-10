#!/bin/bash
# test-meta: arch=both needs=ssh est=84 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-idle-hostile-pause-qemu.sh --
# one part of the AxlSsh idle suite against hostile peers:
# the idle rules against a peer the input pause holds -- half-closed, or
# mid key exchange -- and a keepalive held inside a server rekey.
#
# The body -- the boot, the servers, every scenario, and the helpers of
# lib/ssh-hostile-lib.sh, lib/ssh-channel-lib.sh and lib/ssh-idle-lib.sh --
# is lib/ssh-idle-hostile-cases.sh, shared by every part;
# this file only picks its scenarios. That file's header says how the
# suite is split and why.
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control.
#
# Usage: ./test/integration/test-ssh-idle-hostile-pause-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh idle hostile pause"
HOSTILE_SCENARIOS="paused-half-close keepalive-held-in-rekey pause-mid-rekey"

source "$(dirname "$0")/lib/ssh-idle-hostile-cases.sh"
