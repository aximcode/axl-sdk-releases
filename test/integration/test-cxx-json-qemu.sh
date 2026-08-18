#!/bin/bash
# test-meta: arch=both needs= est=75 local-only=0
# test-cxx-json-qemu.sh — the C++ JSON API (C6), run under QEMU.
#
# Covers AXL-Cxx-Design.md §9 phase C6 and the C additions it required:
#
#   axl::json_document / json_value   chaining navigation, typed extraction
#   json_array_range / object_range   range-for, with OWNED decoded keys
#   axl::json_writer + json_scope     RAII containers, templated add
#   json_writer::splice               the reader->writer bridge
#   axl::json_scanner                 the streaming read face
#
# Every expected value below was DERIVED from the fixture's document by hand,
# not copied from a passing run — the counts in `scan: counts` in particular
# are a hand census of kDoc, which is the only way that line can fail for the
# right reason.
#
# Auxiliary single-binary test (opts out of the test-axl.sh ratchet).
# Requires a staged SDK for the consumer-toolchain case:
#   scripts/install.sh --arch all --cpp
#
# Usage: ./test/integration/test-cxx-json-qemu.sh [--arch X64|AARCH64]

source "$(dirname "$0")/common-test.sh"

export TEST_SKIP_RATCHET=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) TEST_ARCH="$2"; shift 2 ;;
        *)      echo "Usage: $0 [--arch X64|AARCH64]"; exit 1 ;;
    esac
done

test_setup

declare -A _NATIVE_ARCH_MAP=([X64]=x64 [AARCH64]=aa64)
_native_arch="${_NATIVE_ARCH_MAP[$TEST_ARCH]:-x64}"

AXL_CXX="$(test_sdk_dir)/bin/axl-c++"
SRC="$PROJECT_DIR/test/integration/cxx-json-selftest.cpp"
LIB_DIR="$(test_sdk_dir)/lib/axl/$_native_arch"
NATIVE_DIR="$(test_build_dir)"

WORK="$TEST_TMPDIR/cxx-json"
mkdir -p "$WORK"

pass=0
fail=0
check() {  # check <msg> <cmd...>
    local msg="$1"; shift
    if "$@"; then
        echo "  PASS: $msg"; pass=$((pass + 1))
    else
        echo "  FAIL: $msg"; fail=$((fail + 1))
    fi
}

echo "=== C++ JSON Test ($TEST_ARCH) ==="

echo "--- building the DEBUG fixture (AXL_MEM_DEBUG on) ---"
make -C "$PROJECT_DIR" ARCH="$_native_arch" AXL_CPP=1 \
    ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} cxx-json-selftest 2>&1 | tail -3

EFI="$NATIVE_DIR/cxx-json-selftest.efi"
if [[ ! -f "$EFI" ]]; then
    echo "WARN: cxx-json-selftest.efi not built on this box; skipping."
    echo "C++ JSON test: SKIP"
    exit 0
fi
test_add_efi "$EFI"

if [[ -x "$AXL_CXX" && -f "$LIB_DIR/axl-cxxrt-terminate.o" ]]; then
    check "staged headers match include/axl (else: install.sh --arch all --cpp)" \
        diff -rq "$PROJECT_DIR/include/axl" "$(test_sdk_dir)/include/axl-sdk/axl"
    check "axl-c++ builds the fixture from the staged SDK" \
        "$AXL_CXX" --arch "$_native_arch" --release "$SRC" -o "$WORK/consumer.efi"
else
    echo "  NOTE: no staged C++ SDK at $LIB_DIR;"
    echo "        skipping the consumer-toolchain assertions only."
    # BALANCED SKIP (feedback_balancer_count), so the total below is a constant.
    check "SKIP balancer: staged SDK absent, so no header-drift check" true
    check "SKIP balancer: staged SDK absent, so no consumer-toolchain build" true
fi

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo JSON_BEGIN"
    echo "cxx-json-selftest.efi read"
    echo "cxx-json-selftest.efi chain"
    echo "cxx-json-selftest.efi range"
    echo "cxx-json-selftest.efi write"
    echo "cxx-json-selftest.efi splice"
    echo "cxx-json-selftest.efi scan"
    echo "cxx-json-selftest.efi own"
    echo "echo JSON_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 120
test_clean_log

EXPECTED=(
    # --- read ------------------------------------------------------------
    "read: parsed 1"
    "read: name axl 1"
    # "aAb" is 7 SOURCE bytes and 3 decoded -- the whole reason
    # axl_json_get_string_len had to exist.
    "read: esc aAb 3"
    # A key no fixed buffer would hold, read whole.
    "read: longkey long"
    "read: port 8080 1"
    "read: scale 150 1"
    "read: bool 1"
    # A JSON null EXISTS and an absent key does not -- the distinction the C
    # reader keeps and the C++ layer must not collapse.
    "read: null 1 1 0"
    "read: types 1 1 1 1"
    "read: done"

    # --- chain: C6's central decision ------------------------------------
    "chain: missing 0 1"
    "chain: notobject 1"
    "chain: badtype 0 1"
    # The FIRST failure survives two more lookups; a later step must not
    # overwrite AXL_NOT_FOUND with AXL_INVALID.
    "chain: first 1"
    "chain: default 0 1"
    "chain: valueor 8080 -1"
    "chain: done"

    # --- range -----------------------------------------------------------
    "range: array 3 6"
    "range: filtered 4"
    # Keys are DECODED and OWNED: "X-A" iterates as "X-A".
    "range: object 2 Accept;X-A;"
    "range: longest 54 a-very-long-key-that-no-fixed-buffer-would-hold-intact"
    "range: notarray 0 0"
    "range: missing 0"
    "range: done"

    # --- write -----------------------------------------------------------
    "write: ok 1 1"
    "write: doc {\"name\":\"axl\",\"port\":8080,\"scale\":1.5,\"on\":true,\"off\":false,\"nil\":null,\"items\":[1,2,\"three\"],\"nested\":{\"deep\":1}}"
    "write: earlyexit {\"a\":1}"
    "write: unclosed 0 1"
    "write: strings [\"str\",\"view\",null]"
    "write: done"

    # --- splice ----------------------------------------------------------
    "splice: doc {\"items\":[{\"id\":1},{\"id\":2},{\"id\":3}],\"added\":1}"
    # Reparsed, not just string-compared: an exact match proves the bytes,
    # this proves they are still JSON with the same values.
    "splice: round 1 6"
    "splice: missing {\"kept\":2} 1"
    "splice: done"

    # --- scan ------------------------------------------------------------
    # A hand census of kDoc: 17 keys, 6 strings, 5 numbers, 7 objects,
    # 2 arrays, and "port" seen.
    "scan: counts 17 6 5 7 2 1"
    "scan: clean 1"
    "scan: escapedkey 1"
    "scan: copied kept"
    # A malformed document ends the range exactly like a complete one, so
    # failed() is the only thing that can tell them apart.
    "scan: malformed 1"
    "scan: done"

    # --- own -------------------------------------------------------------
    "own: owning kept-alive"
    "own: before 7"
    "own: moved 7 0"
    "own: assigned 9"
    "own: badparse 0 1"
    "own: done"
)

echo "--- serial log (JSON_BEGIN .. JSON_DONE) ---"
sed -n '/JSON_BEGIN/,/JSON_DONE/p' "$TEST_CLEAN_LOG" | sed 's/^/  /'
echo "--- assertions ---"

for line in "${EXPECTED[@]}"; do
    check "$line" grep -aFxq "$line" "$TEST_CLEAN_LOG"
done

check "startup.nsh ran every verb to completion" \
    grep -aq '^JSON_DONE' "$TEST_CLEAN_LOG"

# ---------------------------------------------------------------------------
# Leaks. EXACTLY seven -- one per verb -- for the reason spelled out in
# test-cxx-seam-qemu.sh: a `-ge` floor lets a verb lose its verdict entirely
# and stay green, which is the failure the gate exists to prevent.
#
# json_document owns a token array and (for parse_owning) a std::string, and
# the `own` verb move-assigns one over another, so a destructor that forgot to
# free the destination shows up here and nowhere else.
# ---------------------------------------------------------------------------
n_leak=$(grep -acF '=== AxlMem leak report:' "$TEST_CLEAN_LOG" || true)
n_ok=$(grep -acF 'mem: no leaks detected' "$TEST_CLEAN_LOG" || true)

check "no leak report (found $n_leak)" test "$n_leak" -eq 0
check "leak accounting was ON: $n_ok clean verdicts (need exactly 7)" \
    test "$n_ok" -eq 7

# The run's own ratchet: TEST_SKIP_RATCHET=1 opts this script out of the
# suite-wide count check, so without this, deleting an EXPECTED entry is
# invisible. Counts itself, so the constant equals the total printed below.
EXPECT_TOTAL=52
check "assertion count is $EXPECT_TOTAL (else EXPECTED lost an entry)" \
    test $((pass + fail + 1)) -eq "$EXPECT_TOTAL"

echo ""
echo "cxx-json ($TEST_ARCH): $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
