#!/bin/bash
# test-portalloc-selftest.sh — host-only checks for the host-port allocator in
# scripts/axl-common.sh and its test_port wiring in common-test.sh (no QEMU).
# Not a QEMU integration test; excluded from discovery by name.
set -euo pipefail
cd "$(dirname "$0")"
PROJECT_DIR="$(cd ../.. && pwd)"
COMMON="$PROJECT_DIR/scripts/axl-common.sh"

fail=0
check() {
    if [[ "$2" == "$3" ]]; then
        echo "  PASS: $1"
    else
        echo "  FAIL: $1 (got '$2' want '$3')"; fail=1
    fi
}
check_true() {
    if [[ "$2" == "true" ]]; then
        echo "  PASS: $1"
    else
        echo "  FAIL: $1"; fail=1
    fi
}

# Private lock dir + range per selftest run, so this never perturbs (or is
# perturbed by) a real suite running concurrently on the same host.
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
export AXL_PORT_LOCK_DIR="$tmp/locks"

# --- basic allocation ---------------------------------------------------
p=$(bash -c "source '$COMMON'; axl_alloc_host_port P; echo \$P")
check_true "allocates a port in the default range" \
  "$([[ "$p" -ge 18000 && "$p" -le 19999 ]] && echo true)"

# --- a block is contiguous and every port in it is claimed --------------
read -r b busy <<<"$(bash -c "source '$COMMON'
    axl_alloc_host_port B 4
    # Each port of the block must now be individually unclaimable by a
    # second runner — a block that only locked its base would collide on
    # base+1.
    n=0
    for k in 0 1 2 3; do
        bash -c \"source '$COMMON'; AXL_PORT_LO=\$((B+k)) AXL_PORT_HI=\$((B+k)) axl_alloc_host_port X\" \
            2>/dev/null && n=\$((n+1))
    done
    echo \"\$B \$n\"")"
check_true "block base is in range" \
  "$([[ "$b" -ge 18000 && "$b" -le 19999 ]] && echo true)"
check "all 4 ports of a block are individually claimed" "$busy" "0"

# --- distinctness across CONCURRENT independent processes ---------------
# The core requirement: N runners started at once must never agree on a port.
# Each holder sleeps so every claim overlaps every other claim in time.
out="$tmp/conc"; mkdir -p "$out"
for i in $(seq 1 12); do
    bash -c "source '$COMMON'; axl_alloc_host_port P; echo \$P > '$out/$i'; sleep 2" &
done
wait
got=$(cat "$out"/* | sort); uniq=$(cat "$out"/* | sort -u)
check "12 concurrent allocations are distinct" \
  "$(echo "$got" | wc -l)" "$(echo "$uniq" | wc -l)"

# --- a port held by a LIVE cooperating runner is not handed out again ---
# Pin the range to a single port so "skipped" is the only way to not return it.
one=$(bash -c "source '$COMMON'; axl_alloc_host_port P; echo \$P")
(
  bash -c "source '$COMMON'; AXL_PORT_LO=$one AXL_PORT_HI=$one axl_alloc_host_port P; sleep 3"
) &
holder=$!
sleep 0.5
rc=0
bash -c "source '$COMMON'; AXL_PORT_LO=$one AXL_PORT_HI=$one axl_alloc_host_port P" \
  >/dev/null 2>&1 || rc=$?
check_true "single-port range already claimed -> allocation fails, not double-issued" \
  "$([[ $rc -ne 0 ]] && echo true)"

# --- the claim is released when the holding process exits ---------------
wait "$holder" 2>/dev/null || true
rc=0
again=$(bash -c "source '$COMMON'; AXL_PORT_LO=$one AXL_PORT_HI=$one axl_alloc_host_port P; echo \$P" \
  2>/dev/null) || rc=$?
check "claim released on holder exit -> same port re-issued" "$again" "$one"

# --- a port a FOREIGN process is actually listening on is skipped -------
# This is the case a formula cannot catch (the stale host-server.py squatter).
squat=$(bash -c "source '$COMMON'; axl_alloc_host_port P; echo \$P")
python3 - "$squat" <<'PY' &
import socket, sys, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(1); time.sleep(4)
PY
squatter=$!
for _ in $(seq 1 40); do
    ss -Hltn "sport = :$squat" | grep -q LISTEN && break
    sleep 0.1
done
rc=0
bash -c "source '$COMMON'; AXL_PORT_LO=$squat AXL_PORT_HI=$squat axl_alloc_host_port P" \
  >/dev/null 2>&1 || rc=$?
check_true "port with a foreign listener is refused even though its lock is free" \
  "$([[ $rc -ne 0 ]] && echo true)"
kill "$squatter" 2>/dev/null || true; wait "$squatter" 2>/dev/null || true

# --- test_port: explicit TEST_PORT_BASE stays arithmetic ----------------
# (subshell-isolated: common-test.sh sets `set -euo pipefail` and sources
#  axl-common.sh at load; keep those out of this selftest)
check "test_port slot 0 honours explicit base" \
  "$(TEST_PORT_BASE=20400 bash -c 'source ./common-test.sh; test_port 0')" "20400"
check "test_port slot 5 honours explicit base" \
  "$(TEST_PORT_BASE=20400 bash -c 'source ./common-test.sh; test_port 5')" "20405"

# --- test_port: no explicit base -> allocated, in range, stable per slot -
# The stability check is the one that matters: call sites are all
# `PORT=$(test_port 0)`, so a claim taken inside a command substitution
# would be dropped on return and the next call would hand out a different
# port for the same slot — silently breaking the host/guest agreement.
read -r a0 a1 a0again <<<"$(bash -c '
    source ./common-test.sh
    echo "$(test_port 0) $(test_port 1) $(test_port 0)"')"
check_true "test_port standalone slot 0 is inside the allocator range" \
  "$([[ "$a0" -ge 18000 && "$a0" -le 19999 ]] && echo true)"
check_true "test_port standalone slots 0 and 1 differ" \
  "$([[ "$a0" -ne "$a1" ]] && echo true)"
check "test_port standalone is stable for a repeated slot" "$a0again" "$a0"

# --- test_port: two standalone runs do not collide ----------------------
r1=$(bash -c 'source ./common-test.sh; test_port 0; sleep 1' &
     bash -c 'source ./common-test.sh; test_port 0; sleep 1'
     wait) || true
check "two concurrent standalone test_port runs differ" \
  "$(echo "$r1" | sort -u | wc -l)" "2"

# --- the hostfwd-collision detector recognises QEMU's message -----------
log="$tmp/qemu.log"
printf "%s\n" \
  "qemu-system-x86_64: -netdev user,id=net0,hostfwd=tcp::18000-:7000: Could not set up host forwarding rule 'tcp::18000-:7000'" \
  > "$log"
rc=0
bash -c "source '$COMMON'; axl_report_hostfwd_failure '$log' 'selftest'" \
  >/dev/null 2>&1 || rc=$?
check_true "hostfwd-failure detector fires on QEMU's bind-refusal line" \
  "$([[ $rc -ne 0 ]] && echo true)"
printf "boot ok\nlistening on port 7000\n" > "$log"
rc=0
bash -c "source '$COMMON'; axl_report_hostfwd_failure '$log' 'selftest'" \
  >/dev/null 2>&1 || rc=$?
check "hostfwd-failure detector stays quiet on a healthy log" "$rc" "0"

[[ $fail -eq 0 ]] && echo "portalloc selftest: OK" || { echo "portalloc selftest: FAILED"; exit 1; }
