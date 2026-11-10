# shellcheck shell=bash
# qmp-lib.sh -- one QMP command from a shell test, through `axlpool qmp`
# (test/pool/axlpool/__main__.py): the pool's deadline-guarded QMP session
# over qemu.qmp, QEMU's own client. It replaces hand-rolled
# `{ qmp_capabilities; command; } | socat` pipelines, which had no deadline
# and no way to tell a refused command from no QEMU at all. Never run on its
# own; sourced.

_QMP_LIB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

# axl_qmp [--timeout S] SOCK COMMAND [ARGS-JSON] -- the command's return on
# stdout (JSON); exit 0, 1 when QEMU refused it (its error said), 3 when no
# QEMU answered (no socket, past the deadline).
axl_qmp() {
    PYTHONPATH="$_QMP_LIB_ROOT/test/pool${PYTHONPATH:+:$PYTHONPATH}" \
        python3 -m axlpool qmp "$@"
}

# axl_qmp_available -- whether axl_qmp can run here: the CLI itself starts
# (python3 >= 3.11 with qemu.qmp), not merely the one module importing --
# a probe weaker than the call let link-down tests run with no link down.
axl_qmp_available() {
    axl_qmp --help >/dev/null 2>&1
}
