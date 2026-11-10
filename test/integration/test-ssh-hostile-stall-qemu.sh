#!/bin/bash
# test-meta: arch=both needs= est=95 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-hostile-stall-qemu.sh --
# one part of the AxlSsh hostile-peer suite: auth-stall, key
# exchange completed and then silence -- the only check that the
# pre-auth deadline (the login grace, src/net/axl-ssh-server.c) covers the
# window after NEWKEYS.
#
# The body -- the budgets, the boot, every scenario and the finish -- is
# lib/ssh-hostile-cases.sh, shared by every part; this file only picks its
# scenarios. That file's header says how the suite is split and why.
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control; SSH_HOSTILE_KEEP=<dir> keeps every ssh(1)
# transcript.
#
# Usage: ./test/integration/test-ssh-hostile-stall-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh hostile stall"
HOSTILE_SCENARIOS="auth-stall"

source "$(dirname "$0")/lib/ssh-hostile-cases.sh"
