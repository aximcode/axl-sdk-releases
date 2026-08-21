#!/bin/bash
# test-meta: arch=none needs= est=12 local-only=0
# test-hermetic-toolchain.sh — a consumer build must take NO binary from the
# host toolchain.
#
# WHY THIS EXISTS. 4.0.0 claims "the SDK now takes no compiler, assembler or
# linker from the distro on either arch", and until now nothing enforced it: the
# claim rested on a one-time inventory. A consumer proved it the only way that
# actually proves anything -- by poisoning every host toolchain binary on PATH
# and rebuilding -- and found the one place it was not literally true.
#
# `axl-cc` shells out to `nm` in three places. Two named "${CROSS}nm"; the
# --minimal-runtime log-emitter check named "${NM_BIN:-nm}", the HOST's. On an
# ordinary machine there is no visible difference -- a host binutils `nm` reads
# both ELF targets fine -- which is exactly why it survived: the failure is
# invisible until the host toolchain is absent, and then it is a hard refusal.
#
# So this asserts the property rather than the spelling. A grep for
# unprefixed tool names would approximate it and drift; poisoning PATH and
# watching for a single invocation is the thing itself, and it caught a case
# no reading of the source had.
#
# THE PATH THAT NEEDS nm IS THE POINT. The log check only inspects when
# --minimal-runtime is given AND neither `log` nor `nolog` was named -- declare
# either and axl-cc never looks, which is how the reporting consumer worked
# around it. So the fixture deliberately leaves the log question OPEN. A test
# that passed `nolog` would exercise nothing and report green forever.
#
# NOT poisoned: coreutils, sed/grep/awk, mktemp, python3. Only the toolchain
# names, which is the claim under test.
#
# Host-only: no QEMU. Needs a staged SDK.
#
# Usage: ./test/integration/test-hermetic-toolchain.sh

set -uo pipefail
source "$(dirname "$0")/common-test.sh"
test_parse_args "$@"
set +e

AXL_CC="$(test_sdk_dir)/bin/axl-cc"
[[ -x "$AXL_CC" ]] || { echo "ERROR: staged SDK missing — run scripts/install.sh --arch x64"; exit 2; }
# Same staleness guard as test-axl-cc-flags.sh: nothing in the suite restages,
# so an edit to scripts/axl-cc that was never installed would leave this test
# reporting on the PREVIOUS driver.
if ! cmp -s "$PROJECT_DIR/scripts/axl-cc" "$AXL_CC"; then
    echo "ERROR: staged $AXL_CC is STALE — it differs from scripts/axl-cc." >&2
    echo "       Run 'scripts/install.sh --arch x64' first." >&2
    exit 2
fi

WORK="$(mktemp -d -t axl-hermetic.XXXXXXXX)"; trap 'rm -rf "$WORK"' EXIT
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# --- the poison ------------------------------------------------------------
# Every host toolchain name becomes a shim that RECORDS itself and exits 111.
# Recording matters more than the exit code: a build that succeeds while having
# called one of these is still not hermetic, and the exit code alone cannot
# tell you that.
SHIM="$WORK/poison"; mkdir -p "$SHIM"
CALLS="$WORK/calls.txt"; : > "$CALLS"
for t in gcc cc g++ c++ cpp ld as ar nm objcopy objdump ranlib strip readelf addr2line size; do
    cat > "$SHIM/$t" <<SHIMEOF
#!/bin/sh
echo "$t \$*" >> "$CALLS"
exit 111
SHIMEOF
    chmod +x "$SHIM/$t"
done

cat > "$WORK/app.c" <<'CEOF'
int main(void) { return 0; }
CEOF

echo "=== 1. the poison works (control) ==="
# Without a control, a green run below could mean "hermetic" or "the shims are
# not on PATH". This tree has paid for that ambiguity before.
if PATH="$SHIM:$PATH" nm --version >/dev/null 2>&1; then
    fail "the shim is not shadowing host nm — every assertion below is vacuous"
else
    pass "host nm on PATH is poisoned (exit 111)"
fi
# The control just CALLED a shim, which is recorded like any other call. Clear
# the log so section 3 measures the build and not this proof.
: > "$CALLS"

echo "=== 2. a --minimal-runtime build with the log question OPEN ==="
# The one shape that makes axl-cc inspect the objects. Left open on purpose.
out="$(PATH="$SHIM:$PATH" "$AXL_CC" --minimal-runtime "$WORK/app.c" -o "$WORK/app.efi" 2>&1)"
rc=$?
if [[ "$rc" -eq 0 ]]; then
    pass "builds with no host toolchain on PATH (rc=0)"
else
    fail "build FAILED without a host toolchain (rc=$rc)"
    printf '%s\n' "$out" | head -6 | sed 's/^/      /'
fi
[[ -s "$WORK/app.efi" ]] && pass "produced a non-empty .efi" \
                         || fail "no .efi was produced"

echo "=== 3. ...and reached for NOTHING on the host ==="
if [[ -s "$CALLS" ]]; then
    fail "$(wc -l < "$CALLS" | tr -d ' ') host toolchain invocation(s)"
    sort -u "$CALLS" | head -6 | sed 's/^/      /'
else
    pass "zero host toolchain invocations"
fi

# --- and the ordinary shape stays hermetic too ------------------------------
echo "=== 4. a plain (full-runtime) build is hermetic as well ==="
: > "$CALLS"
out="$(PATH="$SHIM:$PATH" "$AXL_CC" "$WORK/app.c" -o "$WORK/full.efi" 2>&1)"
rc=$?
[[ "$rc" -eq 0 ]] && pass "plain build succeeds with no host toolchain (rc=0)" \
                  || { fail "plain build FAILED (rc=$rc)"; printf '%s\n' "$out" | head -5 | sed 's/^/      /'; }
[[ -s "$CALLS" ]] && { fail "plain build touched the host toolchain"; sort -u "$CALLS" | head -4 | sed 's/^/      /'; } \
                  || pass "plain build made zero host toolchain invocations"

echo
echo "hermetic-toolchain: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
