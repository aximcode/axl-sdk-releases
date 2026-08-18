#!/bin/bash
# test-meta: arch=both needs= est=75 local-only=0
# test-cxx-streams-qemu.sh — axl::cout / axl::cin / axl::cerr and axl::string,
# built FREESTANDING by the staged SDK's axl-c++ and run under QEMU.
#
# Why this is a separate script from test-cxx-hosted-qemu.sh: axl::cin needs
# REAL standard input, and the only way to give a UEFI image that is a shell
# redirect (`< in.txt`) or a pipe. The hosted harness runs a bare .efi with no
# shell script at all, so it has no way to feed one. This script drives the
# fixture from startup.nsh, the same way test-driver-stdio-qemu.sh does.
#
# What each case pins, and why it is here rather than implied:
#
#   1. Builds with NO FLAG. This used to read "FREESTANDING -- if these needed
#      --hosted, std::string would have been the right answer and axl::string
#      should not exist", and that reasoning no longer applies: T3 retired the
#      freestanding C++ mode, so <string> is always available and this case
#      cannot speak to whether axl::string earns its place. It earns it on
#      recoverable OOM instead -- settled 2026-08-16, AXL-Cxx-Design.md 9c --
#      which case 5 below is what actually pins. What this case still pins is
#      that the fixture builds the way a consumer builds anything, which is
#      what cases 2+ run against.
#   2. The globals are CONSTANT-INITIALISED. axl::cout / cerr / cin must emit
#      no .init_array entry: a dynamic initialiser would reintroduce the
#      static-init-order question, and .init_array was being eaten by
#      --gc-sections until 8db522c1 -- so a stream that depended on it would
#      have been silently unconstructed.
#   3. `< in.txt` REDIRECT and the shell's DEFAULT `|` pipe both parse. The
#      default pipe transcodes to UCS-2; reading raw axl_stdin would make
#      `echo 42 | prog` parse as 4, which is a wrong answer rather than an
#      error. axl::cin reads through axl_stdin_text() for exactly this.
#   4. Exact output lines, via grep -Fxq. Substring matches would let the
#      formatting regress silently.
#   5. No memory leaked. axl::cin owns a line buffer and a decoding stream and
#      has no destructor (one would put an .init_array entry back), so the
#      axl_atexit hook is the only thing freeing them.
#
# Auxiliary single-binary test (opts out of the test-axl.sh ratchet).
# Requires a staged SDK: scripts/install.sh --arch all --cpp
#
# Usage: ./test/integration/test-cxx-streams-qemu.sh [--arch X64|AARCH64]

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
SRC="$PROJECT_DIR/test/integration/cxx-streams-selftest.cpp"
LIB_DIR="$(test_sdk_dir)/lib/axl/$_native_arch"
NATIVE_DIR="$(test_build_dir)"

WORK="$TEST_TMPDIR/cxx-streams"
mkdir -p "$WORK"

pass=0
fail=0
# Takes a COMMAND, not an exit code: common-test.sh sets `-e`, and a bare
# `[[ ... ]]; check "$?"` aborts the whole script the first time it is false.
# That is not theoretical -- it silently truncated this file's own output and
# hid a failing assertion behind a wall of passes.
check() {  # check <msg> <cmd...>
    local msg="$1"; shift
    if "$@"; then
        echo "  PASS: $msg"; pass=$((pass + 1))
    else
        echo "  FAIL: $msg"; fail=$((fail + 1))
    fi
}

# An ABSENCE assertion. Separate from check() because check() takes a command
# that must succeed, and inverting that inline reads backwards.
check_absent() {  # check_absent <msg> <pattern> <file>
    local msg="$1" pat="$2" file="$3"
    if grep -q "$pat" "$file"; then
        echo "  FAIL: $msg"; fail=$((fail + 1))
        grep "$pat" "$file" | sed 's/^/      /' | head -3
    else
        echo "  PASS: $msg"; pass=$((pass + 1))
    fi
}

echo "=== C++ Streams Test ($TEST_ARCH) ==="

# ---------------------------------------------------------------------------
# The image that RUNS is the Makefile's, built DEBUG so AXL_MEM_DEBUG is on
# and the leak assertions below have something to read. scripts/install.sh
# stages RELEASE only.
# ---------------------------------------------------------------------------
echo "--- building the DEBUG fixture (freestanding, AXL_MEM_DEBUG on) ---"
make -C "$PROJECT_DIR" ARCH="$_native_arch" AXL_CPP=1 \
    ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} cxx-streams-selftest 2>&1 | tail -3

EFI="$NATIVE_DIR/cxx-streams-selftest.efi"
if [[ ! -f "$EFI" ]]; then
    echo "WARN: cxx-streams-selftest.efi not built on this box; skipping."
    echo "C++ streams test: SKIP"
    exit 0
fi
test_add_efi "$EFI"

# ---------------------------------------------------------------------------
# The globals must be CONSTANT-INITIALISED (header note 2). This was claimed
# in the comment above and asserted NOWHERE, which is the same "gate that
# cannot see" shape as the leak check below -- a regression to dynamic
# initialisation would have passed the entire suite.
#
# Checked on the .o, not the .efi: objcopy's -j list does not carry
# .init_array through to the PE image, so its absence there proves nothing.
# ---------------------------------------------------------------------------
FIXTURE_OBJ="$(test_build_dir "$_native_arch")/build/cxx-streams-selftest.o"
READELF=readelf
NM_BIN=nm
if [[ "$TEST_ARCH" == "AARCH64" ]]; then
    READELF=aarch64-linux-gnu-readelf
    NM_BIN=aarch64-linux-gnu-nm
fi

if [[ -f "$FIXTURE_OBJ" ]]; then
    "$READELF" -SW "$FIXTURE_OBJ" > "$WORK/sections.txt" 2>&1
    "$NM_BIN" -C "$FIXTURE_OBJ"   > "$WORK/syms.txt"     2>&1

    check_absent "no .init_array: the stream globals are constant-initialised" \
        '\.init_array' "$WORK/sections.txt"
    check_absent "no _GLOBAL__sub_I: no dynamic initialiser was emitted" \
        'GLOBAL__sub_I' "$WORK/syms.txt"

    # Positive controls. Without these, a readelf/nm that failed outright
    # would report both absences as PASS -- "nothing was watching" again.
    check "readelf actually read the section table" \
        grep -q '\.text' "$WORK/sections.txt"
    check "nm actually read the symbol table" \
        grep -q 'axl::cout' "$WORK/syms.txt"
else
    echo "  NOTE: $FIXTURE_OBJ absent; skipping the constant-init assertions."
fi

# ---------------------------------------------------------------------------
# Separately, the CONSUMER toolchain must build the same source freestanding.
# This is what proves axl-c++ and the staged headers work for someone outside
# the tree; it is a compile+link assertion, not the image that runs.
# ---------------------------------------------------------------------------
if [[ -x "$AXL_CXX" && -f "$LIB_DIR/axl-cxxrt-terminate.o" ]]; then
    # The staged SDK is a SECOND TREE: axl-c++ compiles against
    # out/include/axl-sdk, so an un-restaged edit to include/axl means this
    # exercises the PREVIOUS build. Compared by content -- install.sh
    # deliberately avoids mtime churn, so an mtime test reports false drift.
    check "staged headers match include/axl (else: install.sh --arch all --cpp)" \
        diff -rq "$PROJECT_DIR/include/axl" "$(test_sdk_dir)/include/axl-sdk/axl"

    check "axl-c++ builds the fixture with no mode flag" \
        "$AXL_CXX" --arch "$_native_arch" --release "$SRC" -o "$WORK/consumer.efi"
else
    echo "  NOTE: no staged C++ SDK at $LIB_DIR;"
    echo "        skipping the consumer-toolchain assertions only."
    echo "        run: scripts/install.sh --arch all --cpp"
fi

# Stage the redirect inputs. Content is load-bearing -- see the expectations
# below, which are derived from these bytes.
printf '42 hello 3.5\ntrue 7\nff\nZ\ntoolongtoken\n' > "$TEST_STAGING/in.txt"
printf 'first line\nsecond\n\nlast-no-newline'    > "$TEST_STAGING/lines.txt"
printf 'notanumber alsobad\n'                     > "$TEST_STAGING/bad.txt"

{
    echo "@echo -off"
    echo "fs0:"
    echo "cd \\"
    echo "echo STREAMS_BEGIN"
    echo "cxx-streams-selftest.efi out"
    echo "cxx-streams-selftest.efi str"
    echo "cxx-streams-selftest.efi edge"
    echo "cxx-streams-selftest.efi in < in.txt"
    echo "cxx-streams-selftest.efi getline < lines.txt"
    echo "cxx-streams-selftest.efi fail < bad.txt"
    # The shell's DEFAULT pipe, which transcodes to UCS-2. No `|a` here on
    # purpose: reading raw axl_stdin would yield 4, not 42, and that is the
    # whole reason axl::cin goes through axl_stdin_text().
    echo "echo 42 | cxx-streams-selftest.efi pipe"
    echo "echo STREAMS_DONE"
    echo "reset -s"
} | test_set_startup

test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 120
test_clean_log

# ---------------------------------------------------------------------------
# Expectations. Every one is an EXACT line (grep -Fxq): a substring match
# would let "42" satisfy an assertion about "425".
# ---------------------------------------------------------------------------
EXPECTED=(
    # --- axl::cout / axl::cerr formatting -------------------------------
    "out: text 42 -7 3.5 true false"
    "out: widths 4000000000 -9000000000 18000000000"
    "out: null (null)"
    "out: from-a-string"
    "out: endl-once"
    "err: to stderr"

    # --- axl::string surface --------------------------------------------
    "str: empty 0 true ''"
    "str: built hello world! 12"
    "str: find 6 -1 true true true"
    "str: substr world"
    "str: erase hello! 6"
    "str: insert hello,!"
    "str: replace HELLO,!"
    "str: move HELLO,! 0"
    "str: swap bbb aaa"
    "str: compare true true false"
    "str: resize zzzqq 5"
    "str: shrink zz 2"
    "str: iterate 294 ac b c"
    "str: reserve keep true 4"
    "str: shrink_to_fit keep 23 true"
    "str: self 160 true true false"
    "str: moved-from moved reused"
    "str: steal stolen refilled"

    # --- aliasing, overflow, OOM, and the operators nothing else calls ---
    # Each of these was a CONFIRMED bug before the pre-commit review:
    # selfassign gave "\0ello", selfreplace gave "cdef\0fcdef", overflow
    # silently truncated to 4 bytes, concat did not COMPILE, ident was false,
    # and hexout printed "true255" because axl::hex bound operator<<(bool).
    "edge: selfassign hello 5 hello false"
    "edge: selfreplace abcdefcdef 10"
    "edge: overflow abcdef 6 true"
    "edge: concat foobar foo! foo? <foo"
    "edge: ident true true 0"
    "edge: compare false false true true true"
    "edge: assign two moved 0"
    "edge: oom-lazy '' true"
    "edge: oom-grow keep 4 true"
    "edge: sticky true false fresh"
    "edge: hexout ff beef 255"
    "edge: nullptr nullptr"
    "edge: search 3 1 4 2 0"
    "edge: reverse cba true true"
    "edge: bounds '' '' true 0"
    # Small-string optimisation -- the point of the refactor. AXL-Cxx-Design
    # section 4.5 measured a skin over AxlString at 9.2x the cost of this on
    # short-string construction; `false` for bad() under a forced
    # allocation failure is what proves no allocation happened.
    "edge: sso \\EFI\\BOOT\\BOOTX64.EFI 21 true false 23"
    "edge: ssocopy \\EFI\\BOOT\\BOOTX64.EFI true false"
    # `true true true`, not `true false true`. The middle field is
    # now_heap, and it was baselined to FALSE because an undisarmed
    # axl_mem_fail_next_alloc from the probe above was eaten by this
    # push_back -- so the case asserted that the SSO->heap transition does
    # NOT happen, and could not have failed if growth broke.
    "edge: ssogrow true true true 23 xxxx"
    "edge: ssoassign \\EFI\\BOOT\\BOOTX64.EFI true false"
    # The heap branches: adopt()'s `m_cap = other.m_cap` (the one place the
    # union write matters on a move) had no test at all -- dropping it left
    # the suite green.
    "edge: heapmove true true true true"
    "edge: shrinkheap true true true 300 true"
    "edge: ssosteal inline-bytes 0 true"
    "edge: float 1.5 (nil)"

    # --- >> extraction, from `< in.txt` ---------------------------------
    "in: first 42 hello 3.5 true"
    "in: crossed true true"
    "in: read true 7"
    "in: hex 255 true"
    "in: char Z true"
    "in: buf 'too' fail=true status=-8"
    "in: done"

    # --- getline, from `< lines.txt` ------------------------------------
    "gl: 1 'first line' true"
    "gl: 2 'second' true"
    "gl: 3 '' true"
    "gl: 4 'last-no-newline' true"
    "gl: 5 fail=true eof=true status=-5"
    "gl: done"

    # --- the failure model, from `< bad.txt` ----------------------------
    "fail: parse true 999 -4"
    "fail: pushback notanumber"
    "fail: sticky 111 222 true"
    "fail: read false -4"
    "fail: recheck true"
    "fail: rest alsobad"
    "fail: eof true true -5"
    "fail: done"

    # --- the shell's default (UCS-2) pipe -------------------------------
    "pipe: 42 true"
)

echo "--- serial log (STREAMS_BEGIN .. STREAMS_DONE) ---"
sed -n '/STREAMS_BEGIN/,/STREAMS_DONE/p' "$TEST_CLEAN_LOG" | sed 's/^/  /'
echo "--- assertions ---"

# -a on every grep: a stray NUL in the serial capture makes GNU grep treat the
# whole file as binary, which silently truncates listings while leaving counts
# correct. AARCH64 emits one reliably.
for line in "${EXPECTED[@]}"; do
    check "$line" grep -aFxq "$line" "$TEST_CLEAN_LOG"
done

check "startup.nsh ran every verb to completion" \
    grep -aq '^STREAMS_DONE' "$TEST_CLEAN_LOG"

# ---------------------------------------------------------------------------
# Leaks. axl::cin has no destructor BY DESIGN -- one would force a
# __cxa_atexit registration at static-init time and put back the .init_array
# entry that assertion 2 exists to keep out. So its line buffer and its
# decoding stream are freed by the axl_atexit hook and nothing else.
#
# The positive control matters as much as the check: if the staged library
# were built without AXL_MEM_DEBUG there would be no leak accounting at all,
# and "no leak report" would mean "nothing was watching" rather than "nothing
# leaked". Six verbs run, so six clean verdicts are expected.
# ---------------------------------------------------------------------------
n_leak=$(grep -acF '=== AxlMem leak report:' "$TEST_CLEAN_LOG" || true)
n_ok=$(grep -acF 'mem: no leaks detected' "$TEST_CLEAN_LOG" || true)

check "no leak report (found $n_leak)" test "$n_leak" -eq 0
check "leak accounting was ON: $n_ok clean verdicts (need >= 7)" \
    test "$n_ok" -ge 7

echo ""
echo "cxx-streams ($TEST_ARCH): $pass passed, $fail failed"
[[ "$fail" -eq 0 ]] && exit 0 || exit 1
