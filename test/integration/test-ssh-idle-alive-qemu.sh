#!/bin/bash
# test-meta: arch=both needs=ssh est=70 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-idle-alive-qemu.sh --
# one part of the AxlSsh idle suite:
# client-alive against a real ssh(1) that answers its probes, quiet or
# under bulk output.
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
# Usage: ./test/integration/test-ssh-idle-alive-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh idle alive"
HOSTILE_SCENARIOS="alive-ssh-answers alive-ssh-bulk-quiet-client"

source "$(dirname "$0")/lib/ssh-idle-cases.sh"
