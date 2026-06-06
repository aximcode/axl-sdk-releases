#!/bin/bash
# Render a UEFI GOP / axl_gfx app live on the user's machine over reverse
# VNC — the "show it to me" path. Thin wrapper over run-qemu.sh that
# fills in the two things that are easy to get wrong by hand:
#
#   1. The viewer host. Defaults to field 1 of $SSH_CONNECTION (the SSH
#      client = the machine running the TigerVNC listen loop). Override
#      with SHOW_IT_HOST=<ip>. Port defaults to 9999 (SHOW_IT_PORT).
#   2. Clean teardown. When launched non-interactively (e.g. by an agent
#      in the background), re-execs under setsid to become its own
#      process-group leader, so the whole QEMU tree dies with a single
#      `kill -- -<pgid>` and NEVER a pattern-matching `pkill -f` (which
#      self-matches the caller's shell and can kill a parallel run).
#
# The viewer side is the user's responsibility: a persistent TigerVNC
# listen loop, e.g.
#   while true; do vncviewer -listen 9999 -SecurityTypes None; done
# (QEMU offers unencrypted "None"; modern TigerVNC rejects it by default.)
#
# The app MUST block until a key — draw-then-exit loses the frame to the
# shell repaint. Press a key in the VNC window to exit the app.
#
# Usage: ./scripts/show-it.sh [run-qemu OPTIONS] <file.efi> [app args...]
# Env:   SHOW_IT_HOST  viewer IP (default: $SSH_CONNECTION field 1)
#        SHOW_IT_PORT  viewer port (default: 9999)
#        RUN_QEMU      path to run-qemu.sh (default: alongside this script)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_QEMU="${RUN_QEMU:-$ROOT_DIR/scripts/run-qemu.sh}"
PORT="${SHOW_IT_PORT:-9999}"

HOST="${SHOW_IT_HOST:-}"
if [[ -z "$HOST" ]]; then
    HOST="$(printf '%s' "${SSH_CONNECTION:-}" | awk '{print $1}')"
fi

[[ $# -ge 1 ]] || {
    echo "usage: show-it.sh [run-qemu OPTIONS] <file.efi> [app args...]" >&2
    exit 2
}
[[ -n "$HOST" ]] || {
    echo "show-it: cannot derive viewer host — set SHOW_IT_HOST=<ip> (no \$SSH_CONNECTION)" >&2
    exit 1
}
[[ -x "$RUN_QEMU" ]] || {
    echo "show-it: run-qemu.sh not found/executable at $RUN_QEMU (set RUN_QEMU=...)" >&2
    exit 1
}

# Non-interactive launch (no tty on stdin) → isolate into our own process
# group so a caller can tear the run down by pgid alone. The guard stops
# the re-exec from recursing.
if [[ ! -t 0 && -z "${SHOW_IT_REEXEC:-}" ]]; then
    export SHOW_IT_REEXEC=1
    exec setsid "$0" "$@"
fi

PGID="$(ps -o pgid= -p $$ | tr -d ' ')"
echo "show-it: reverse VNC -> $HOST:$PORT  (your viewer must be listening there)"
if [[ -t 0 ]]; then
    echo "show-it: press a key in the VNC window to exit the app; Ctrl-C here to quit QEMU."
else
    echo "show-it: stop this run with:  kill -- -$PGID   (NEVER pkill -f <pattern>)"
fi

exec "$RUN_QEMU" --vnc-reverse "$HOST:$PORT" "$@"
