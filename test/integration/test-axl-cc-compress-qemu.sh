#!/bin/bash
# test-meta: arch=x64 needs=cmake est=40 local-only=0 guest=standard
# test-axl-cc-compress-qemu.sh -- the downstream opt-in to packed images:
# `axl-cc --compress` and the CMake package's COMPRESS keyword, built against
# the STAGED SDK exactly as a consumer builds.
#
# --compress keeps the linked image as <out>.raw.efi and writes <out>.efi
# through the same decision the in-tree WRAP_EFI makes: axl-efi-pack exit 0
# links the axl-pack stub around the packed payload, exit 3 keeps a byte copy
# of the raw image, anything else fails the build. What is pinned here:
#
#   default OFF      a build without the flag is the raw image, byte for byte
#                    (the .raw.efi of a --compress build equals the default
#                    build's .efi, and two default builds agree -- the control
#                    that makes a cmp worth anything)
#   keep             hello.c is under the threshold: <out>.efi == .raw.efi
#   wrap             fetch.c (a real example, ~120 KB) wraps: its module name
#                    is axl-pack-stub, the raw image's is fetch.efi
#   --service        --compress reaches the launcher only, never the driver
#   failure          an axl-efi-pack failure fails axl-cc and leaves the
#                    previous <out>.efi untouched
#   CMake COMPRESS   axl_add_app / axl_add_driver accept it; the raw image
#                    equals a plain configure's output
#   boot             the kept hello, the wrapped fetch and the CMake-wrapped
#                    fetch run in ONE boot; their lines are asserted exactly
#
# Requires a staged SDK (scripts/install.sh --arch x64) and cmake.
#
# Usage: ./test/integration/test-axl-cc-compress-qemu.sh

source "$(dirname "$0")/common-test.sh"
# The CMake CI and the release use, run alone or under the suite
# (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
source "$PROJECT_DIR/scripts/axl-cmake-pin.sh"
axl_use_pinned_cmake || exit 1
set +e
set -uo pipefail

export TEST_SKIP_RATCHET=1
TEST_ARCH=X64

test_setup

SDK="$(test_sdk_dir)"
AXL_CC="$SDK/bin/axl-cc"
if [[ ! -x "$AXL_CC" || ! -d "$SDK/lib/axl/x64" ]]; then
    echo "ERROR: staged SDK missing -- run 'scripts/install.sh --arch x64' first" >&2
    exit 2
fi
EX="$PROJECT_DIR/sdk/examples"
W="$TEST_TMPDIR/cc"
mkdir -p "$W/d" "$W/d2" "$W/c" "$W/svc" "$W/fail"

check_eq() {
    local label="$1" got="$2" want="$3"
    if [[ "$got" == "$want" ]]; then
        test_host_pass "$label"
    else
        test_host_fail "$label"
        echo "      want: $(printf '%q' "$want")"
        echo "      got:  $(printf '%q' "$got")"
    fi
}
check_true() {  # check_true <label> <command...>
    local label="$1"; shift
    if "$@"; then test_host_pass "$label"; else test_host_fail "$label"; fi
}
cc() {  # cc <log> <axl-cc args...> -- the log keeps the failure readable
    local log="$1"; shift
    "$AXL_CC" --arch x64 "$@" > "$log" 2>&1
    local rc=$?
    [[ $rc -eq 0 ]] || sed 's/^/      /' "$log" | tail -8
    return $rc
}

echo "=== axl-cc --compress (x64, staged SDK) ==="

# -- what the SDK must ship for a consumer to wrap ---------------------------
for f in bin/axl-efi-pack lib/axl/x64/pack/axl-pack-stub.o \
         lib/axl/x64/pack/axl-pack-format.o lib/axl/x64/pack/LzmaDec.o \
         lib/axl/x64/pack/Bra.o lib/axl/x64/pack/Bra86.o; do
    check_true "the SDK ships $f" test -s "$SDK/$f"
done

# -- default OFF -------------------------------------------------------------
cc "$W/d/hello.log" "$EX/hello.c" -o "$W/d/hello.efi"
check_eq "default build of hello.c succeeds" "$?" 0
cc "$W/d2/hello.log" "$EX/hello.c" -o "$W/d2/hello.efi"
check_true "control: two default builds of hello.c are byte-identical" \
    cmp -s "$W/d/hello.efi" "$W/d2/hello.efi"
check_true "a default build writes no .raw.efi" test ! -e "$W/d/hello.raw.efi"
cc "$W/d/fetch.log" "$EX/fetch.c" -o "$W/d/fetch.efi"
check_eq "default build of fetch.c succeeds" "$?" 0
check_eq "control: a default image's module name is its file name" \
    "$(test_pe_module "$W/d/fetch.efi")" "fetch.efi"

# -- keep: hello.c is under the threshold ------------------------------------
cc "$W/c/hello.log" --compress "$EX/hello.c" -o "$W/c/hello.efi"
check_eq "--compress hello.c succeeds" "$?" 0
check_true "--compress keeps the linked image as hello.raw.efi, equal to the default build" \
    cmp -s "$W/c/hello.raw.efi" "$W/d/hello.efi"
check_true "hello.c under the threshold: hello.efi is a byte copy of the raw image" \
    cmp -s "$W/c/hello.efi" "$W/c/hello.raw.efi"
check_true "hello.so sits beside hello.efi" test -s "$W/c/hello.so"
check_true "no hello.raw.so is written" test ! -e "$W/c/hello.raw.so"

# -- wrap: fetch.c ------------------------------------------------------------
cc "$W/c/fetch.log" --compress "$EX/fetch.c" -o "$W/c/fetch.efi"
check_eq "--compress fetch.c succeeds" "$?" 0
check_true "fetch.raw.efi equals the default build" \
    cmp -s "$W/c/fetch.raw.efi" "$W/d/fetch.efi"
check_eq "fetch.raw.efi's module name is fetch.efi" \
    "$(test_pe_module "$W/c/fetch.raw.efi")" "fetch.efi"
check_eq "fetch.efi is the stub (module name axl-pack-stub)" \
    "$(test_pe_module "$W/c/fetch.efi")" "axl-pack-stub"
raw_sz=$(stat -c%s "$W/c/fetch.raw.efi" 2>/dev/null || echo 0)
fin_sz=$(stat -c%s "$W/c/fetch.efi" 2>/dev/null || echo 0)
check_true "the wrapped fetch.efi is smaller ($raw_sz -> $fin_sz)" \
    test "$fin_sz" -gt 0 -a "$fin_sz" -lt "$raw_sz"
check_true "fetch.so is the RAW image's symbols, beside fetch.efi" \
    cmp -s "$W/c/fetch.so" "$W/d/fetch.so"
check_true "no fetch.efi.tmp is left behind" test ! -e "$W/c/fetch.efi.tmp"

# -- -c ignores --compress (CMake's OPTIONS reach the compile step too) -------
cc "$W/c/obj.log" --compress -c "$EX/hello.c" -o "$W/c/hello.o"
check_eq "--compress with -c compiles (the flag is inert there)" "$?" 0

# -- --service: the launcher only ---------------------------------------------
( cd "$W/svc" && "$AXL_CC" --arch x64 --compress --service service_demo \
    "$EX/service-demo.c" > "$W/svc/svc.log" 2>&1 )
svc_rc=$?
check_eq "--compress --service builds" "$svc_rc" 0
[[ $svc_rc -eq 0 ]] || tail -8 "$W/svc/svc.log" | sed 's/^/      /'
check_true "the launcher's raw image is kept (service_demo.raw.efi)" \
    test -s "$W/svc/service_demo.raw.efi"
check_true "the embedded driver is never packed (no service_demo-dxe.raw.efi)" \
    test ! -e "$W/svc/service_demo-dxe.raw.efi"
check_eq "the driver's module name is its own" \
    "$(test_pe_module "$W/svc/service_demo-dxe.efi")" "service_demo-dxe.efi"
check_eq "the launcher is wrapped (module name axl-pack-stub)" \
    "$(test_pe_module "$W/svc/service_demo.efi")" "axl-pack-stub"

# -- failure: a packer error fails the build, FINAL untouched -----------------
# A copy of the SDK's bin/ with axl-efi-pack replaced; axl-cc finds the SDK
# from its own location, so lib/ include/ share/ are linked in as they are.
FAKE="$W/fakesdk"
mkdir -p "$FAKE/bin"
for d in lib include share; do ln -s "$SDK/$d" "$FAKE/$d"; done
cp "$AXL_CC" "$FAKE/bin/axl-cc"
ln -s "$SDK/bin/pe-set-debug" "$FAKE/bin/pe-set-debug"
printf '#!/bin/sh\necho "axl-efi-pack: injected failure" >&2\nexit 2\n' > "$FAKE/bin/axl-efi-pack"
chmod +x "$FAKE/bin/axl-efi-pack"
echo "previous" > "$W/fail/fetch.efi"
"$FAKE/bin/axl-cc" --arch x64 --compress "$EX/fetch.c" -o "$W/fail/fetch.efi" \
    > "$W/fail/fail.log" 2>&1
fail_rc=$?
check_eq "axl-efi-pack exit 2 fails axl-cc" "$fail_rc" 1
check_eq "the failure names the packer and its exit status" \
    "$(grep -m1 '^ERROR: --compress' "$W/fail/fail.log")" \
    "ERROR: --compress: axl-efi-pack failed on $W/fail/fetch.raw.efi (exit 2)"
check_eq "a failed wrap leaves the previous fetch.efi untouched" \
    "$(cat "$W/fail/fetch.efi")" "previous"

# -- CMake COMPRESS -------------------------------------------------------------
cmake_build() {  # cmake_build <dir> <keyword-or-empty>
    local dir="$1" kw="$2"
    mkdir -p "$dir/src"
    cat > "$dir/src/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.20)
project(axlcompress C CXX)
find_package(axl REQUIRED)
axl_add_app(fetchc $EX/fetch.c $kw)
axl_add_driver(drvc $EX/driver.c $kw)
EOF
    cmake -S "$dir/src" -B "$dir/b" -DCMAKE_PREFIX_PATH="$SDK" > "$dir/cfg.log" 2>&1 \
        && cmake --build "$dir/b" > "$dir/bld.log" 2>&1
    local rc=$?
    [[ $rc -eq 0 ]] || tail -8 "$dir/cfg.log" "$dir/bld.log" 2>/dev/null | sed 's/^/      /'
    return $rc
}
if command -v cmake > /dev/null 2>&1; then
    cmake_build "$W/cm-plain" ""
    check_eq "CMake: a plain configure builds" "$?" 0
    cmake_build "$W/cm-comp" "COMPRESS"
    check_eq "CMake: axl_add_app / axl_add_driver accept COMPRESS and build" "$?" 0
    check_true "CMake: a plain build writes no fetchc.raw.efi" \
        test ! -e "$W/cm-plain/b/fetchc.raw.efi"
    check_true "CMake COMPRESS: fetchc.raw.efi equals the plain build's fetchc.efi" \
        cmp -s "$W/cm-comp/b/fetchc.raw.efi" "$W/cm-plain/b/fetchc.efi"
    check_eq "CMake COMPRESS: fetchc.efi is the stub (module name axl-pack-stub)" \
        "$(test_pe_module "$W/cm-comp/b/fetchc.efi")" "axl-pack-stub"
    check_true "CMake COMPRESS on a driver: drvc.raw.efi equals the plain build" \
        cmp -s "$W/cm-comp/b/drvc.raw.efi" "$W/cm-plain/b/drvc.efi"
else
    test_host_fail "cmake is not installed (needs=cmake)"
fi

# -- one boot: kept hello, wrapped fetch, CMake-wrapped fetchc ------------------
test_add_efi "$W/c/hello.efi" "hello.efi"
test_add_efi "$W/d/fetch.efi" "fetch-raw.efi"
test_add_efi "$W/c/fetch.efi" "fetch.efi"
[[ -f "$W/cm-comp/b/fetchc.efi" ]] && test_add_efi "$W/cm-comp/b/fetchc.efi" "fetchc.efi"
test_use_nsh

test_build_image
test_build_qemu_cmd
test_add_no_network
test_run_foreground 60
test_clean_log

echo "--- serial log (HELLO_BEGIN .. CC_DONE) ---"
sed -n '/^HELLO_BEGIN/,/^CC_DONE/p' "$TEST_CLEAN_LOG" | sed 's/^/  /'

win() {
    if ! test_slice_log "$1" "$2" "$TEST_TMPDIR/slice"; then
        echo "<marker $1 or $2 missing>"
        return 0
    fi
    grep -v '^$' "$TEST_TMPDIR/slice" || true
}
check_eq "boot: --compress hello.efi (kept raw)" "$(win HELLO_BEGIN HELLO_END)" \
'Hello, world!'
check_eq "boot: control, the raw fetch" "$(win RAW_BEGIN RAW_END)" \
'usage: fetch <url>
RAW_EXIT=0x1'
check_eq "boot: --compress fetch.efi (wrapped)" "$(win FETCH_BEGIN FETCH_END)" \
'usage: fetch <url>
FETCH_EXIT=0x1'
check_eq "boot: CMake COMPRESS fetchc.efi (wrapped)" "$(win CMAKE_BEGIN CMAKE_END)" \
'usage: fetch <url>
CMAKE_EXIT=0x1'

test_host_summary "axl-cc-compress (x64)"
