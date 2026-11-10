#!/bin/bash
# test-meta: arch=none needs= est=10 local-only=0
# test-pool-units.sh -- `python3 -m axlpool units`: the systemd user units
# that run the guest pool as a socket-activated service (service.py's
# PROCESS). Pins their exact text, has systemd-analyze verify them, and
# checks the refusals (a missing agent, a path systemd cannot quote). Boots
# nothing.
#
# Usage: ./test/integration/test-pool-units.sh

set -uo pipefail

SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # physical: the generator resolves
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
POOL_DIR="$PROJECT_DIR/test/pool"
export TEST_SKIP_RATCHET=1

WORK="$(cd -P "$(mktemp -d -t axl-poolu.XXXXXXXX)" && pwd)"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
check() {   # check NAME GOT WANT
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"; echo "    got:  $2"; echo "    want: $3"; FAIL=$((FAIL + 1))
    fi
}
text() {    # text FILE -> its content, the final newline made visible
    if [[ -f "$1" ]]; then cat "$1"; echo "<EOF>"; fi
}
units() {   # units ARGS... -> runs the generator from the pool directory
    (cd "$POOL_DIR" && python3 -m axlpool units "$@")
}

echo "=== guest pool: systemd units ==="
PY="$(python3 -c 'import sys; print(sys.executable)')"

units --tree "$PROJECT_DIR" --out "$WORK/u" > "$WORK/out.txt" 2>&1
rc=$?
check "units: exit 0, both files written" "$rc $(cd "$WORK/u" 2>/dev/null && ls | tr '\n' ' ')" \
    "0 axlpool.service axlpool.socket "

check "axlpool.socket: exact text" "$(text "$WORK/u/axlpool.socket")" "[Unit]
Description=AXL test guest pool (socket)

[Socket]
ListenStream=%t/axlpool/pool.sock
SocketMode=0600
DirectoryMode=0700
Accept=no

[Install]
WantedBy=sockets.target
<EOF>"

check "axlpool.service: exact text" "$(text "$WORK/u/axlpool.service")" "[Unit]
Description=AXL test guest pool
Requires=axlpool.socket
After=axlpool.socket
StartLimitIntervalSec=300
StartLimitBurst=5

[Service]
Type=simple
WorkingDirectory=$POOL_DIR
ExecStart=$PY -m axlpool serve --tree $PROJECT_DIR --state %S/axlpool --socket %t/axlpool/pool.sock --exit-idle 900
KillMode=mixed
TimeoutStopSec=240
Restart=on-failure
RestartPreventExitStatus=2 3
RestartSec=2
<EOF>"

check "units: says where they went and how to install them" "$(cat "$WORK/out.txt")" \
"wrote $WORK/u/axlpool.socket
wrote $WORK/u/axlpool.service
install: cp $WORK/u/axlpool.socket $WORK/u/axlpool.service ~/.config/systemd/user/ && systemctl --user daemon-reload && systemctl --user enable --now axlpool.socket"

if command -v systemd-analyze > /dev/null; then
    # Its own runtime directory: `--user verify` builds a user manager, which
    # needs one, and a CI runner (a system service) has no login session --
    # "Failed to lookup RuntimeDirectory path" was the first dispatched run.
    mkdir -m 700 "$WORK/xdg"
    uverify() { env -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR="$WORK/xdg" \
                    systemd-analyze --user verify "$@" 2>&1; }
    verify="$(uverify "$WORK/u/axlpool.socket" "$WORK/u/axlpool.service")"
    check "systemd-analyze --user verify: clean (exit 0, nothing said)" "$?:$verify" "0:"
    # CONTROL: the same verify, given a unit it must reject (its ExecStart
    # names no command that exists), says so -- a
    # verify that cannot fail would pass anything.
    mkdir -p "$WORK/bad"
    cp "$WORK/u/axlpool.socket" "$WORK/bad/axlpool.socket"
    sed 's/^ExecStart=.*/ExecStart=relative-path-is-refused/' "$WORK/u/axlpool.service" > "$WORK/bad/axlpool.service"
    verify="$(uverify "$WORK/bad/axlpool.socket" "$WORK/bad/axlpool.service")"; vrc=$?
    check "control: verify rejects a unit whose ExecStart command cannot be found" \
        "$([[ $vrc -ne 0 ]] && grep -qF 'axlpool.service: Command relative-path-is-refused is not executable' <<< "$verify" && echo rejected || echo "accepted ($vrc): $verify")" "rejected"
else
    echo "  SKIP: systemd-analyze not installed"
fi

# Every character a unit line would split, quote or expand is refused.
for bad in "a b" 'a"b' "a'b" 'a\b' 'a%b' 'a$b'; do
    mkdir -p "$WORK/$bad/test/pool"
    : > "$WORK/$bad/test/pool/classes.toml"
    rm -rf "$WORK/u3"
    units --tree "$WORK/$bad" --out "$WORK/u3" > "$WORK/out3.txt" 2>&1
    rc=$?
    check "units: a tree path with [${bad:1:1}] is refused, nothing written" \
        "$rc $(cat "$WORK/out3.txt") $([[ -e "$WORK/u3" ]] && echo written || echo none)" \
        "1 axlpool units: $WORK/$bad: a path with whitespace, quotes, backslashes, % or \$ cannot go in a unit unescaped none"
done

mkdir -p "$WORK/not-a-tree"
units --tree "$WORK/not-a-tree" --out "$WORK/u4" > "$WORK/out4.txt" 2>&1
rc=$?
check "units: a tree with no test/pool/classes.toml is refused, nothing written" \
    "$rc $(cat "$WORK/out4.txt") $([[ -e "$WORK/u4" ]] && echo written || echo none)" \
    "1 axlpool units: $WORK/not-a-tree has no test/pool/classes.toml: not an axl-sdk tree none"

# serve's memory cap (serve-cases.py, no guest): its own counts, folded in.
mkdir -p "$WORK/serve"
sc="$(python3 "$POOL_DIR/serve-cases.py" "$WORK/serve" 2>&1)"; src=$?
printf '%s\n' "$sc" | grep -E '^  (PASS|FAIL):'
check "serve-cases: ran to its summary, every case passed" \
    "$src $(grep -E '^serve-cases: ' <<< "$sc")" "0 serve-cases: 10 passed, 0 failed"

echo "pool-units: $PASS passed, $FAIL failed"
(( FAIL == 0 && PASS > 0 ))
