#!/bin/bash
# test-meta: arch=both needs=ssh est=55 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-idle-hostile-churn-qemu.sh --
# one part of the AxlSsh idle suite against hostile peers:
# the unused clock across channel churn: it restarts only when the LAST
# channel in use is freed, and a half-closed session is still in use.
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
# Usage: ./test/integration/test-ssh-idle-hostile-churn-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh idle hostile churn"
HOSTILE_SCENARIOS="channel-churn-unused"

source "$(dirname "$0")/lib/ssh-idle-hostile-cases.sh"
