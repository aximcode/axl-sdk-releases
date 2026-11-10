#!/bin/bash
# test-meta: arch=both needs= est=70 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-hostile-hooks-qemu.sh --
# one part of the AxlSsh password-hook hazards: a verdict that
# outlives its client, hooks that would nest, and the server freed from
# inside one. auth-hook-reenter (and the hook watchdog it carries) is
# test-ssh-hostile-hooks-reenter-qemu.sh's.
#
# The body -- the budgets, the boot, every scenario and the finish -- is
# lib/ssh-hostile-hooks-cases.sh, shared by every part; this file only picks its
# scenarios. That file's header says how the suite is split and why.
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control; SSH_HOSTILE_KEEP=<dir> keeps every ssh(1)
# transcript.
#
# Usage: ./test/integration/test-ssh-hostile-hooks-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh hostile hooks"
HOSTILE_SCENARIOS="auth-pending-drop auth-hook-nest auth-hook-free"

source "$(dirname "$0")/lib/ssh-hostile-hooks-cases.sh"
