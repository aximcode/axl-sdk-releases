#!/bin/bash
# test-meta: arch=both needs=ssh est=69 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-idle-unused-qemu.sh --
# one part of the AxlSsh idle suite:
# the unused-connection timeout (sshd's UnusedConnectionTimeout) against a
# real ssh(1) and ssh-peer.py, and the keepalive answers it ignores.
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
# Usage: ./test/integration/test-ssh-idle-unused-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh idle unused"
HOSTILE_SCENARIOS="answers-are-not-unimplemented unused-ssh-N unused-larval unused-with-session unused-after-close disconnect-control"

source "$(dirname "$0")/lib/ssh-idle-cases.sh"
