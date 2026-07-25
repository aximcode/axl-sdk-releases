#!/bin/bash
# test-runner-selftest.sh — host-only checks for run-integration.sh using stub
# "tests" (a pass, a fail, a hang). No QEMU. Not a real integration test;
# excluded from discovery by name (*-selftest.sh).
set -uo pipefail
cd "$(dirname "$0")"

stub=$(mktemp -d); trap 'rm -rf "$stub"' EXIT
printf '#!/bin/bash\nexit 0\n'   > "$stub/test-pass-qemu.sh"
printf '#!/bin/bash\nexit 1\n'   > "$stub/test-fail-qemu.sh"
printf '#!/bin/bash\nsleep 30\n' > "$stub/test-hang-qemu.sh"
chmod +x "$stub"/*.sh

fail=0
need() { grep -q "$1" <<<"$2" || { echo "  FAIL: expected /$1/ in output"; fail=1; }; }

# --- serial (-j1 explicitly; the no-j default now auto-sizes to nproc-2) ---
out=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh -j1 --timeout 2 2>&1); rc=$?
need "test-pass-qemu.sh PASS" "$out"
need "test-fail-qemu.sh FAIL" "$out"
need "test-hang-qemu.sh TIMEOUT" "$out"
[[ $rc -ne 0 ]] || { echo "  FAIL: runner exit 0 despite a failing test"; fail=1; }
[[ $fail -eq 0 ]] && echo "  PASS: serial verdicts + non-zero exit"

# --- parallel (-j4): same verdicts as serial, still non-zero exit ---
outp=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh -j4 --timeout 2 2>&1); rcp=$?
need "test-pass-qemu.sh PASS" "$outp"
need "test-fail-qemu.sh FAIL" "$outp"
need "test-hang-qemu.sh TIMEOUT" "$outp"
[[ $rcp -ne 0 ]] || { echo "  FAIL: -j4 exit 0 despite a failing test"; fail=1; }
[[ $fail -eq 0 ]] && echo "  PASS: -j4 verdicts + non-zero exit"

# --- retry-once: a test that fails on attempt 1 but passes on attempt 2 is
#     reported PASS (the parallel pool's transient-contention mitigation). ---
rstub=$(mktemp -d)
printf '#!/bin/bash\nm=%s/flaky.marker\n[[ -e "$m" ]] && exit 0\ntouch "$m"; exit 1\n' "$rstub" \
    > "$rstub/test-flaky-qemu.sh"
chmod +x "$rstub"/*.sh
rout=$(RUN_INTEGRATION_DIR="$rstub" ./run-integration.sh -j1 --timeout 5 2>&1); rrc=$?
rm -rf "$rstub"
need "test-flaky-qemu.sh PASS" "$rout"
need "(retry" "$rout"
[[ $rrc -eq 0 ]] || { echo "  FAIL: retry-recovered flaky test should exit 0"; fail=1; }
[[ $fail -eq 0 ]] && echo "  PASS: transient failure retried -> PASS, exit 0"

# --- concurrent workers get distinct host-port bases ---
# The runner no longer assigns TEST_PORT_BASE (a formula only keeps ONE
# invocation self-consistent — see run_one). It must leave the variable
# unset so each test claims its own base through common-test.sh, and those
# claims must not collide across workers running at the same time. The stubs
# source common-test.sh for real and hold their claim while the others run,
# so the overlap is genuine rather than sequential reuse of one port.
bstub=$(mktemp -d); bout=$(mktemp -d)
for n in a b c; do
    printf '#!/bin/bash\nsource "%s/common-test.sh"\necho "$TEST_PORT_BASE" > "%s/$$.base"\nsleep 2\nexit 0\n' \
        "$PWD" "$bout" > "$bstub/test-base-$n-qemu.sh"
done
chmod +x "$bstub"/*.sh
RUN_INTEGRATION_DIR="$bstub" ./run-integration.sh -j4 --timeout 20 >/dev/null 2>&1
nbases=$(cat "$bout"/*.base 2>/dev/null | wc -l)
ndistinct=$(cat "$bout"/*.base 2>/dev/null | sort -u | wc -l)
inrange=$(awk '$1 >= 18000 && $1 <= 19999' "$bout"/*.base 2>/dev/null | wc -l)
rm -rf "$bstub" "$bout"
if [[ "$nbases" -eq 3 && "$ndistinct" -eq 3 ]]; then
    echo "  PASS: 3 concurrent workers claimed 3 distinct port bases"
else
    echo "  FAIL: expected 3 distinct port bases, got $ndistinct of $nbases"; fail=1
fi
if [[ "$inrange" -eq 3 ]]; then
    echo "  PASS: runner left TEST_PORT_BASE to the allocator (all in 18000-19999)"
else
    echo "  FAIL: $inrange/3 bases came from the allocator range"; fail=1
fi

# --- --shard i/K partitions the set: union == full, no overlap ---
a=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --shard 0/2 --list | sort)
b=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --shard 1/2 --list | sort)
allset=$(RUN_INTEGRATION_DIR="$stub" ./run-integration.sh --list | sort)
union=$(printf '%s\n%s\n' "$a" "$b" | grep -v '^$' | sort -u)
overlap=$(comm -12 <(printf '%s\n' "$a" | grep -v '^$') <(printf '%s\n' "$b" | grep -v '^$'))
if [[ "$union" == "$allset" ]]; then echo "  PASS: shards cover the full set"; else echo "  FAIL: shard union != full set"; fail=1; fi
if [[ -z "$overlap" ]]; then echo "  PASS: shards do not overlap"; else echo "  FAIL: shard overlap: $overlap"; fail=1; fi

[[ $fail -eq 0 ]] && echo "runner selftest: OK" || { echo "runner selftest: FAILED"; exit 1; }
