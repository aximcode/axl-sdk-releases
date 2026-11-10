#!/bin/bash
# test-meta: arch=both needs= est=94 local-only=0 builds=ssh-server-selftest guest=standard
# test-ssh-hostile-qemu.sh --
# one part of the AxlSsh hostile-peer suite: malformed input, a
# connection flood, the auth phase driven hostilely -- auth-cap
# (MAX_AUTH_REQUESTS, src/net/axl-ssh-server.c) and auth-forge -- and the
# per-source penalties on a second server, from a peer that never
# authenticates. auth-stall is test-ssh-hostile-stall-qemu.sh's.
#
# The body -- the budgets, the boot, every scenario and the finish -- is
# lib/ssh-hostile-cases.sh, shared by every part; this file only picks its
# scenarios. That file's header says how the suite is split and why.
#
# Environment: SSH_HOSTILE_ONLY=<scenario> runs one; SSH_HOSTILE_ABUSE=0 runs
# the boot-and-deadline control; SSH_HOSTILE_KEEP=<dir> keeps every ssh(1)
# transcript.
#
# Usage: ./test/integration/test-ssh-hostile-qemu.sh [--arch X64|AARCH64]

HOSTILE_SUMMARY="ssh hostile"
HOSTILE_SCENARIOS="bad-ident bad-length bad-padding flood auth-cap auth-forge penalty"

source "$(dirname "$0")/lib/ssh-hostile-cases.sh"
