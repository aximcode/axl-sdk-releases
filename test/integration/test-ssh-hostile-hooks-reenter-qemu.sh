#!/bin/bash
# test-meta: arch=both needs= est=111 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-hostile-hooks-reenter-qemu.sh --
# one part of the AxlSsh password-hook hazards: auth-hook-reenter,
# a hook that runs the server's loop from inside itself while the login
# grace runs out, and the whole hook watchdog (NEW-1) it carries.
#
# The body -- the budgets, the boot, every scenario and the finish -- is
# lib/ssh-hostile-hooks-cases.sh, shared by every part; this file only picks its
# scenarios. That file's header says how the suite is split and why.
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control; SSH_HOSTILE_KEEP=<dir> keeps every ssh(1)
# transcript.
#
# Usage: ./test/integration/test-ssh-hostile-hooks-reenter-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh hostile hooks reenter"
HOSTILE_SCENARIOS="auth-hook-reenter"

source "$(dirname "$0")/lib/ssh-hostile-hooks-cases.sh"
