# shellcheck shell=bash
# sshd-tmux-lib.sh -- ssh sessions that TYPE like a terminal, and a keyboard
# that types like a keyboard. Sourced after sshd-lib.sh (it uses ssh_argv,
# TEST_TMPDIR and _sshd_cleanup). Never run on its own.
#
# A FIFO session (ssh_open) sends the bytes a test writes. A tmux session
# sends what a TERMINAL sends for a named key (`C-End` is `ESC[1;5F`), which is
# what a modifier test has to prove: the bytes are tmux's, not the test's.
# Each session is its own tmux server on a socket in the test's own scratch
# directory (`-S $TEST_TMPDIR/tmux-NAME.sock`), so nothing here can reach --
# or be reached by -- a tmux the user is running, and nothing is left behind
# in /tmp/tmux-UID when the scratch directory goes.
#
# hmp_sendkey is the other side: QEMU's monitor `sendkey`, a key pressed on the
# guest's keyboard device. The suite boots with
#   -monitor unix:$TEST_TMPDIR/mon.sock,server,nowait
# and ONE socat holds the connection for the whole run (a socat per key waits
# out its own -t after EOF, which costs seconds per key).

declare -A TMUX_SOCK=()
MON_FD=""
MON_PID=""

# Every tmux call is bounded: a tmux that hangs must fail the step (or the
# cleanup) that called it, not hang the run.
_tmux() {
    local name="$1"
    shift
    timeout 5 env -u TMUX tmux -S "${TMUX_SOCK[$name]}" "$@"
}

# tmux_ssh_open NAME [tmux conf lines...] -- a 132x60 tmux window running
# ssh_argv's `ssh -tt` session (stderr in NAME.err). The terminal is
# xterm-256color with no escape delay and no status line; each extra
# argument is one more line of the server's configuration.
tmux_ssh_open() {
    local name="$1"
    shift
    tmux_ssh_open_sized "$name" 132 60 "$@"
}

# tmux_ssh_open_sized NAME COLS ROWS [tmux conf lines...] -- tmux_ssh_open on
# a COLSxROWS window.
tmux_ssh_open_sized() {
    local name="$1" cols="$2" rows="$3" conf
    shift 3
    TMUX_SOCK[$name]="$TEST_TMPDIR/tmux-$name.sock"
    trap _sshd_tmux_cleanup EXIT
    conf="$TEST_TMPDIR/tmux-$name.conf"
    {
        echo "set -g default-terminal xterm-256color"
        echo "set -s escape-time 0"
        echo "set -g status off"
        [[ $# -gt 0 ]] && printf '%s\n' "$@"
    } > "$conf"
    ssh_argv "$name"
    _tmux "$name" -f "$conf" new-session -d -s "$name" -x "$cols" -y "$rows" \
        "exec ${SSH_ARGV[*]@Q} 2> ${TEST_TMPDIR@Q}/$name.err"
}

# tmux_probe -- does the host tmux work? Starts a throwaway server (options
# from a file, never a `set` before the server exists), captures the pane
# once, and stops it. Returns 1 if any step fails, hangs (each call is
# bounded) or the capture is not the text the pane printed. Callable before
# test_setup.
tmux_probe() {
    local d rc=1 i screen
    d=$(mktemp -d) || return 1
    printf 'set -g status off\n' > "$d/conf"
    if timeout 10 env -u TMUX tmux -S "$d/s" -f "$d/conf" new-session -d -x 40 -y 5 \
            'echo axl-tmux-probe; sleep 30' 2>/dev/null; then
        for ((i = 0; i < 25; i++)); do
            # Captured first, then matched: `tmux | grep -q` under pipefail
            # fails ON a match when grep closes the pipe early.
            screen=$(timeout 5 env -u TMUX tmux -S "$d/s" capture-pane -p 2>/dev/null || true)
            if grep -qx 'axl-tmux-probe' <<< "$screen"; then
                rc=0
                break
            fi
            sleep 0.2
        done
        timeout 5 env -u TMUX tmux -S "$d/s" kill-server 2>/dev/null || true
    fi
    rm -rf "$d"
    return "$rc"
}

# tmux_keys NAME KEY... -- press tmux-named keys (`C-End`, `M-x`, `Enter`).
tmux_keys() {
    local name="$1"
    shift
    _tmux "$name" send-keys -t "$name" "$@"
}

# tmux_hex NAME HEX -- send the bytes HEX spells (`1b5b41`) as one key, for
# what tmux has no key name for.
tmux_hex() {
    local -a bytes=()
    local hex="$2"
    while [[ -n "$hex" ]]; do bytes+=("${hex:0:2}"); hex=${hex:2}; done
    _tmux "$1" send-keys -t "$1" -H "${bytes[@]}"
}

# tmux_text NAME TEXT -- type TEXT literally.
tmux_text() { _tmux "$1" send-keys -t "$1" -l -- "$2"; }

# tmux_screen NAME -- the visible screen, one line per row.
tmux_screen() { _tmux "$1" capture-pane -p -t "$1"; }

# tmux_wait_screen NAME FIXED SECONDS -- wait for a screen row containing the
# fixed string FIXED. A wait, not an assertion: callers assert exact rows.
tmux_wait_screen() {
    local i screen
    for ((i = 0; i < $3 * 5; i++)); do
        screen=$(tmux_screen "$1" 2>/dev/null || true)
        grep -qF -- "$2" <<< "$screen" && return 0
        sleep 0.2
    done
    return 1
}

tmux_close() {
    [[ -n "${TMUX_SOCK[$1]:-}" ]] || return 0
    _tmux "$1" kill-server 2>/dev/null || true
    rm -f "${TMUX_SOCK[$1]}"
    TMUX_SOCK[$1]=""
}

# Open the persistent monitor connection (after QEMU created the socket).
hmp_open() {
    local i fifo="$TEST_TMPDIR/mon.in"
    for ((i = 0; i < 50; i++)); do
        [[ -S "$TEST_TMPDIR/mon.sock" ]] && break
        sleep 0.1
    done
    [[ -S "$TEST_TMPDIR/mon.sock" ]] || return 1
    rm -f "$fifo"
    mkfifo "$fifo"
    exec {MON_FD}<>"$fifo"
    socat - "UNIX-CONNECT:$TEST_TMPDIR/mon.sock" <&"$MON_FD" \
        > "$TEST_TMPDIR/mon.log" 2>&1 &
    MON_PID=$!
    trap _sshd_tmux_cleanup EXIT
}

# hmp_sendkey COMBO -- press COMBO (`ctrl-end`) on the guest's keyboard.
hmp_sendkey() { printf 'sendkey %s\n' "$1" >&"$MON_FD"; }

_hmp_close() {
    if [[ -n "$MON_PID" ]]; then
        kill "$MON_PID" 2>/dev/null || true
        wait "$MON_PID" 2>/dev/null || true
        MON_PID=""
    fi
    if [[ -n "$MON_FD" ]]; then
        exec {MON_FD}>&-
        MON_FD=""
    fi
}

_sshd_tmux_cleanup() {
    local n
    for n in "${!TMUX_SOCK[@]}"; do tmux_close "$n"; done
    _hmp_close
    _sshd_cleanup
}
