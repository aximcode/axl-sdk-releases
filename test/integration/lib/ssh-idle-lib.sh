# shellcheck shell=bash
# ssh-idle-lib.sh -- the helpers shared by the AxlSsh idle suites:
# lib/ssh-idle-cases.sh (a real ssh(1)) and lib/ssh-idle-hostile-cases.sh
# (hostile byte-level peers), the bodies of the test-ssh-idle-*-qemu.sh parts. Sourced AFTER lib/ssh-hostile-lib.sh's
# hostile_boot and lib/ssh-channel-lib.sh, whose mark/since/since_re/
# wait_since, PEER, check and DUMP_LOGS these read. Never run on its own.
#
# EVERY DROP TIMING IS READ OFF THE SERVER'S OWN CLOCK: at each idle drop
# the server writes, at DEBUG, how long the silence or the unused stretch
# was by the monotonic clock that timed it (`idle: dropping, nothing heard
# for N ms`, `idle: dropping, no channel in use for N ms`; drop_ms below).
# The host's clock does not keep pace with the guest's under load, and the
# guest console's timestamps are an RTC second with a fraction from an
# unrelated counter (src/log/axl-log.c says so). Host-clock waits are loose
# sanity limits only, never a rule's bound.

took() { echo "  $1 took $((SECONDS - t0))s"; }

# Guest DEBUG lines: timestamp, level, `ssh:func:line: ` (measured).
DBG='[0-9:.]+ \[DEBUG\] ssh:[a-z_0-9]+:[0-9]+: '
PROBE_CHAN_RE="${DBG}idle: keepalive probe [0-9]+, as a channel request"
PROBE_GLOB_RE="${DBG}idle: keepalive probe [0-9]+, as a global request"
DISC_RE="${DBG}disconnect: .*"
RESTART_RE="${DBG}idle: last channel freed; unused clock restarts, [0-9]+ ms"
INACTIVE='terminating inactive connection'
SILENT='Timeout, client not responding'
UNUSED_FOR='no channel in use'
SILENT_FOR='nothing heard'

# The idle lines at EITHER level, for user $2 (an ERE fragment), after $1.
idle_count() {
    since "$1" | grep -cE -- "ssh(:[a-z_]+:[0-9]+)?: $3 from user $2 ${HOST_AS_SEEN//./\\.} port [0-9]+( \(and [0-9]+ more since the previous line, at debug level only\))?\$" || true
}

# The port the guest recorded for the last "Accepted <method> for <user>"
# after mark $1 (auth_succeed's INFO line).
accepted_port() {
    since "$1" | grep -oE "Accepted $2 for $3 from ${HOST_AS_SEEN//./\\.} port [0-9]+\$" \
        | tail -1 | grep -oE '[0-9]+$' || true
}

# peer_ts NAME [ssh-peer.py args...] -- one ssh-peer.py run, each output
# line prefixed with the host's clock ($EPOCHREALTIME) in NAME.ts and kept
# bare in ssh-NAME.log, where hostile_finish counts `peer: authenticated`.
# Sets PEER_RC.
peer_ts() {
    local name="$1"
    shift
    PEER_RC=0
    set +e
    timeout 60 python3 -u "$PEER" "$@" 2>&1 \
        | while IFS= read -r l; do printf '%s %s\n' "$EPOCHREALTIME" "$l"; done \
        > "$TEST_TMPDIR/$name.ts"
    PEER_RC=${PIPESTATUS[0]}
    set -e
    cut -d' ' -f2- "$TEST_TMPDIR/$name.ts" > "$TEST_TMPDIR/ssh-$name.log"
    DUMP_LOGS+=("$TEST_TMPDIR/ssh-$name.log")
}
# Seconds, to 0.1, from the line $2 to the line $3 in NAME.ts ($1).
ts_gap() {
    awk -v a="$2" -v b="$3" '{ t = $1; $1 = ""; sub(/^ /, "") }
        $0 == a && !ta { ta = t } $0 == b && !tb { tb = t }
        END { if (ta && tb) printf "%.1f\n", tb - ta; else print "none" }' \
        "$TEST_TMPDIR/$1.ts"
}
# Is "$1" a decimal in [$2, $3]?
within() { awk -v x="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(x ~ /^[0-9.]+$/ && x >= lo && x <= hi) }'; }
# Is "$1" a decimal in ($2, $3] -- strictly above $2? For "more than 3 s":
# sshd's rule is strict (now > expiry).
above_within() { awk -v x="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(x ~ /^[0-9.]+$/ && x > lo && x <= hi) }'; }
# How many lines of host file $1 are exactly $2.
lines_of() { local n; n=$(grep -cxF -- "$2" "$1" || true); echo "${n:-0}"; }
# The N of the server's one `idle: dropping, <$2> for N ms` DEBUG line after
# mark $1, or "none" when there is not exactly one.
drop_ms() {
    local v
    v=$(since "$1" | grep -oE -- "${DBG}idle: dropping, $2 for [0-9]+ ms\$" \
        | grep -oE '[0-9]+ ms$' | cut -d' ' -f1 || true)
    if [[ "$(printf '%s\n' "$v" | grep -c . || true)" == 1 ]]; then echo "$v"; else echo none; fi
}

# hostile_fill_slots COUNT WAIT_ERE LOGPREFIX SSH_ARGS... -- start COUNT
# real ssh(1) holders in the background, each `ssh -v -E <LOGPREFIX><i>.log
# SSH_ARGS...`, and leave their PIDs in HOLDERS. In waves of five, each
# waited for (up to 20 s) until the guest has logged WAIT_ERE once per
# holder so far: fifteen key exchanges at once would pass max_startups (10),
# and admission would EVICT some of them. A server's sixteenth slot is then
# the only one free -- an authenticated connection is never evicted -- so a
# later arrival gets in only if something freed a slot.
hostile_fill_slots() {
    local n="$1" ere="$2" prefix="$3" m i
    shift 3
    m=$(mark)
    HOLDERS=()
    for i in $(seq 1 "$n"); do
        timeout 60 ssh -v -E "$TEST_TMPDIR/$prefix$i.log" "$@" \
            > /dev/null 2>&1 < /dev/null &
        HOLDERS+=("$!")
        BG_PIDS+=("$!")
        if (( i % 5 == 0 )); then
            wait_since "$m" "$i" 20 "$ere" || true
        fi
    done
}

# wait_probe_quiet QUIET CAP -- until the guest has written no keepalive
# probe line, on any server, for QUIET seconds; at most CAP. Returns 1 at the
# cap. A probe line names no connection, so a scenario that COUNTS them
# needs the boot quiet first: holders hostile_release_slots has killed are
# still probed for a few seconds, until the guest has processed their
# closes (measured: two probed 2.7 s after the kill, X64). QUIET must exceed
# every running server's probe interval plus its reaper tick.
wait_probe_quiet() {
    local m i cur last=-1 still=0
    m=$(mark)
    for ((i = 0; i < $2 * 5; i++)); do
        cur=$(since_re "$m" "${DBG}idle: keepalive probe [0-9]+, as a (global|channel) request")
        if [[ "$cur" == "$last" ]]; then still=$((still + 1)); else still=0; last=$cur; fi
        if (( still >= $1 * 5 )); then return 0; fi
        sleep 0.2
    done
    return 1
}

# End every holder hostile_fill_slots started, and reap them.
hostile_release_slots() {
    local p
    for p in "${HOLDERS[@]}"; do kill "$p" 2>/dev/null || true; done
    for p in "${HOLDERS[@]}"; do wait "$p" 2>/dev/null || true; done
}
