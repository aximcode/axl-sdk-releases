#!/bin/bash
# test-meta: arch=x64 needs=cmake est=15 local-only=0
# test-cmake-package.sh — the GENERATED CMake package must configure.
#
# WHY THIS EXISTS. scripts/install.sh generates an axl-config.cmake that
# RE-IMPLEMENTS axl-cc's whole pipeline in CMake -- its own compile line, its
# own ld, its own objcopy, its own pe-set-debug. That makes it the third of the
# three build paths `make check-flag-parity` polices, and until this file
# existed **nothing executed it**: no test and no CI step ever called
# find_package(axl). check-flag-parity is a whole-file substring check, so it
# confirms a flag is MENTIONED, not that the package works.
#
# Configure AND build. Configuring exercises the package's own logic -- the
# find_package search, the arch dispatch, the option parsing, the FATAL_ERROR
# paths. Building is what proves the pipeline it drives actually produces a
# PE32+, and it is the safety net for changing that pipeline: this file was
# extended to build on the day the package stopped re-implementing axl-cc and
# started calling it, because a refactor of a shipped consumer path needs a
# test that runs the path.
#
# Not run under QEMU. The IMAGES are covered end to end by the axl-cc suites;
# what is unique here is the CMake layer on top, and a PE machine word is
# enough to prove it produced the right kind of artefact for the right arch.
#
# Requires a staged SDK (scripts/install.sh) and cmake.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

export TEST_SKIP_RATCHET=1

# Via sdk-prefix.sh, not a hard-coded path. This read $PROJECT_DIR/out, which
# stopped existing when the staged SDK moved to stage/ (v4.1.0, 2026-08-16)
# -- so this ENTIRE suite skipped from that day on, silently and with a
# green "PASS 0s" in the runner. The CMake package is the third build path
# check-flag-parity exists for, and it was the one with no coverage at all.
STAGE="${AXL_STAGE_DIR:-$("$PROJECT_DIR/scripts/sdk-prefix.sh" --abs)}"

pass=0
fail=0
check() {  # check <ok:0/1> <msg>
    if [[ "$1" == "0" ]]; then echo "  PASS: $2"; pass=$((pass + 1))
    else echo "  FAIL: $2"; fail=$((fail + 1)); fi
}

echo "=== cmake-package ==="
if ! command -v cmake >/dev/null 2>&1; then
    echo "  SKIP: cmake not installed"
    exit 0
fi
if [[ ! -f "$STAGE/lib/cmake/axl/axl-config.cmake" ]]; then
    echo "  SKIP: no staged CMake package at $STAGE/lib/cmake/axl/"
    echo "        run: scripts/install.sh --arch x64"
    exit 0
fi

WORK="$(mktemp -d -t axl-cmake-pkg.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/src"
printf '#include <axl.h>\nint main(void) { axl_print("hi\\n"); return 0; }\n' \
    > "$WORK/src/app.c"
printf '#include <vector>\nint main() { std::vector<int> v; v.push_back(1); return (int)v.size() - 1; }\n' \
    > "$WORK/src/app.cpp"

# configure <name> <extra axl_add_app args> -> writes $WORK/<name>.log, returns rc
configure() {
    local name="$1" extra="$2"
    cat > "$WORK/src/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.16)
project(axlpkgtest C CXX)
find_package(axl REQUIRED)
axl_add_app(demo app.c $extra)
EOF
    rm -rf "$WORK/build-$name"
    cmake -S "$WORK/src" -B "$WORK/build-$name" \
        -DCMAKE_PREFIX_PATH="$STAGE" \
        > "$WORK/$name.log" 2>&1
}

# 1. The package is FINDABLE and configures. Everything below is worthless if
#    this fails, so it is asserted rather than assumed.
configure plain ""
rc=$?
check "$rc" "find_package(axl) + axl_add_app configures (rc=$rc)"
[[ "$rc" -ne 0 ]] && tail -6 "$WORK/plain.log" | sed 's/^/      /'

# 2. HOSTED is REJECTED, and the message names it.
#
#    This is the assertion the keyword's continued presence in
#    _axl_build_efi's cmake_parse_arguments exists for. Sabotage-measured:
#    delete it there and HOSTED lands in UNPARSED_ARGUMENTS (the caller passes
#    it ahead of the SOURCES keyword), so it is **silently ignored** and
#    configure SUCCEEDS -- no error at all. That is worse than a confusing
#    error, and until this file existed the reasoning was enforced by a
#    comment alone.
#
#    Note the OUTER parse list in axl_add_app is not load-bearing for this:
#    sabotaging it leaves HOSTED in ARGN, where _axl_build_efi still catches
#    it. Measured too, so the redundancy is known rather than assumed.
configure hosted "HOSTED"
rc=$?
[[ "$rc" -ne 0 ]] && grep -q "HOSTED was removed" "$WORK/hosted.log"
check "$?" "axl_add_app(... HOSTED) fails, naming the removal (rc=$rc)"
[[ "$rc" -eq 0 ]] && echo "      configure SUCCEEDED; it must not"

# 3. The option-parsing machinery is INTACT, not merely absent. Emptying the
#    keyword list entirely would make case 2 pass for the wrong reason -- an
#    ignored HOSTED and a broken ALLOW_UEFI look identical from there -- so
#    assert a keyword that must still WORK.
configure allowuefi "ALLOW_UEFI"
rc=$?
check "$rc" "axl_add_app(... ALLOW_UEFI) still configures (rc=$rc)"
[[ "$rc" -ne 0 ]] && tail -6 "$WORK/allowuefi.log" | sed 's/^/      /'

# 4. C++ sources configure too. The package dispatches per extension to a
#    different compiler and flag set, and that arm was likewise unexercised.
cat > "$WORK/src/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(axlpkgtest C CXX)
find_package(axl REQUIRED)
axl_add_app(demo-cxx app.cpp)
EOF
rm -rf "$WORK/build-cxx"
cmake -S "$WORK/src" -B "$WORK/build-cxx" -DCMAKE_PREFIX_PATH="$STAGE" \
    > "$WORK/cxx.log" 2>&1
rc=$?
check "$rc" "a C++ axl_add_app configures with no mode flag (rc=$rc)"
[[ "$rc" -ne 0 ]] && tail -6 "$WORK/cxx.log" | sed 's/^/      /'

# ---------------------------------------------------------------------
# 5. It actually BUILDS -- C, a driver, EMBEDS, multi-source, C++, exceptions.
# ---------------------------------------------------------------------
# The PE machine word without `file`: e_lfanew at 0x3c points at "PE\0\0"
# followed by the machine. Same trick test-pkg-deps-minimal.sh uses.
pe_machine() {
    local off
    off=$(od -An -tu4 -j60 -N4 "$1" | tr -d " ")
    od -An -tx2 -j $((off + 4)) -N2 "$1" | tr -d " "
}

build_case() {  # build_case <name> <cmakelists-body> <target> <expect-machine>
    local name="$1" body="$2" target="$3" want="$4"
    cat > "$WORK/src/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.16)
project(axlpkgtest C CXX)
find_package(axl REQUIRED)
$body
EOF
    rm -rf "$WORK/bld-$name"
    if ! cmake -S "$WORK/src" -B "$WORK/bld-$name" -DCMAKE_PREFIX_PATH="$STAGE" \
            > "$WORK/$name-cfg.log" 2>&1; then
        check 1 "$name: configure (see $WORK/$name-cfg.log)"
        tail -5 "$WORK/$name-cfg.log" | sed 's/^/      /'
        return
    fi
    if ! cmake --build "$WORK/bld-$name" > "$WORK/$name-bld.log" 2>&1; then
        check 1 "$name: build"
        tail -6 "$WORK/$name-bld.log" | sed 's/^/      /'
        return
    fi
    local efi; efi="$(find "$WORK/bld-$name" -name "$target.efi" | head -1)"
    if [[ ! -f "$efi" ]]; then
        check 1 "$name: produced $target.efi"
        return
    fi
    local m; m="$(pe_machine "$efi")"
    [[ "$m" == "$want" ]]
    check "$?" "$name: built $target.efi (machine 0x$m, wanted 0x$want)"
}

printf 'int main(void) { return 0; }\n' > "$WORK/src/plain.c"
# A real second TU: it must DEFINE something the first one uses, or the link
# is a one-object link wearing two names. (First draft gave both a main() and
# the link correctly refused.)
printf 'int axl_pkgtest_helper(void);\nint axl_pkgtest_helper(void) { return 7; }\n' \
    > "$WORK/src/second.c"
printf 'int axl_pkgtest_helper(void);\nint main(void) { return axl_pkgtest_helper() == 7 ? 0 : 1; }\n' \
    > "$WORK/src/multimain.c"
build_case c-app   'axl_add_app(capp plain.c)'                 capp   8664

# A DRIVER and an EMBEDS case, because those are two of the paths this
# rewrite replaced and nothing else exercises them: check-examples never runs
# CMake. The driver is subsystem 11 with a DriverEntry, and axl_add_app's
# re-exported ${TARGET}_EFI_PATH feeding EMBEDS is the documented shared-driver
# shape -- a launcher embedding the driver .efi it just built.
printf '#include <axl.h>\nAXL_ALLOW_UEFI_NOTE\nunsigned long DriverEntry(void *h, void *st) { (void)h; (void)st; return 0; }\n' \
    | sed '/AXL_ALLOW_UEFI_NOTE/d' > "$WORK/src/drv.c"
build_case driver 'axl_add_driver(mydrv drv.c)' mydrv 8664
build_case embeds 'axl_add_driver(mydrv drv.c)
axl_add_app(launcher plain.c EMBEDS ${mydrv_EFI_PATH}=my_driver)
add_dependencies(launcher mydrv)' launcher 8664

# Multi-source, which the per-source compile step made possible to get wrong
# (object-name collisions are avoided by an index, not by the basename).
build_case multi 'axl_add_app(multiapp multimain.c second.c)' multiapp 8664

# The gap this file was written for: an exceptions image through CMake. It
# used to CONFIGURE and then die at the first throw, because the package
# re-implemented axl-cc's pipeline without the _eh linker script, the glue
# objects or the toolchain libraries.
# The C++ cases need the staged cxxrt glue; a C-only SDK (install.sh
# --no-cpp) is a legitimate configuration, and FAILING there would report a
# defect that is really a missing optional component. The C, driver, embeds
# and multi cases above run either way.
if [[ ! -f "$STAGE/lib/axl/x64/axl-cxxrt-terminate.o" ]]; then
    echo "  SKIP: no staged C++ glue — C++ and exceptions cases not run"
    echo "        (install.sh --cpp to include them)"
    echo ""
    echo "cmake-package: $pass passed, $fail failed"
    [[ "$fail" -eq 0 ]]
    exit $?
fi
build_case cxx-app 'axl_add_app(cxxapp app.cpp)' cxxapp 8664

printf '#include <stdexcept>\nint main() { try { throw std::runtime_error("x"); } catch (const std::exception &) { return 0; } return 1; }\n' \
    > "$WORK/src/eh.cpp"
build_case eh 'axl_add_app(ehapp eh.cpp OPTIONS -fexceptions)' ehapp 8664

# ---------------------------------------------------------------------
# 6. HEADER DEPENDENCY TRACKING, which is the whole reason the package
#    generates a depfile at all.
# ---------------------------------------------------------------------
# Editing a header must rebuild the object. The previous re-implementation had
# NO tracking (its DEPENDS named only the source), so this is new ground -- and
# it is the assertion that protected the dependency mechanism through a change
# of spelling: it was written while the package still used axl-cc's --depfile,
# went RED on the SDK-header case (that flag used -MMD internally, which omits
# -isystem headers), and went green when the package moved to gcc's own -MD.
# That is the swap it exists for, and the bug it caught on the way.
#
# Both a PROJECT header and a deep SDK header: the two arrive by different
# routes (-I vs the -isystem axl-cc adds itself), and only -MD lists the
# second -- -MMD omits system headers by definition.
mkdir -p "$WORK/dep/src"
printf '#include "myhdr.h"\n#include <axl.h>\nint main(void){ return MYVAL; }\n' \
    > "$WORK/dep/src/app.c"
printf '#define MYVAL 0\n' > "$WORK/dep/src/myhdr.h"
cat > "$WORK/dep/src/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(axldeptest C)
find_package(axl REQUIRED)
axl_add_app(depapp app.c OPTIONS -I${CMAKE_CURRENT_SOURCE_DIR})
EOF
if cmake -S "$WORK/dep/src" -B "$WORK/dep/b" -DCMAKE_PREFIX_PATH="$STAGE"         > "$WORK/dep-cfg.log" 2>&1    && cmake --build "$WORK/dep/b" > "$WORK/dep-b1.log" 2>&1; then
    check 0 "dep: the tracking fixture builds"

    # A no-op rebuild must do NOTHING. Without this a package that rebuilt
    # unconditionally would pass the two assertions below for the wrong reason.
    cmake --build "$WORK/dep/b" > "$WORK/dep-noop.log" 2>&1
    ! grep -q "axl-cc:" "$WORK/dep-noop.log"
    check "$?" "dep: a no-op rebuild recompiles nothing"

    sleep 1.1; printf '#define MYVAL 1\n' > "$WORK/dep/src/myhdr.h"
    cmake --build "$WORK/dep/b" > "$WORK/dep-b2.log" 2>&1
    grep -q "axl-cc:" "$WORK/dep-b2.log"
    check "$?" "dep: editing a PROJECT header rebuilds the object"

    # RESTORE the mtime. $STAGE is the SHARED staged SDK that every other
    # suite and every consumer tree builds against, so bumping a header there
    # permanently would force rebuilds well outside this test.
    _sdk_hdr="$STAGE/include/axl-sdk/axl/axl-mem.h"
    _sdk_ref="$WORK/dep/mtime-ref"; touch -r "$_sdk_hdr" "$_sdk_ref"
    sleep 1.1; touch "$_sdk_hdr"
    cmake --build "$WORK/dep/b" > "$WORK/dep-b3.log" 2>&1
    _sdk_rc=$?
    touch -r "$_sdk_ref" "$_sdk_hdr"
    [[ "$_sdk_rc" -eq 0 ]] && grep -q "axl-cc:" "$WORK/dep-b3.log"
    check "$?" "dep: editing an SDK header rebuilds the object (needs -MD, not -MMD)"
else
    check 1 "dep: the tracking fixture builds (see $WORK/dep-cfg.log)"
    tail -5 "$WORK/dep-cfg.log" "$WORK/dep-b1.log" 2>/dev/null | sed 's/^/      /'
fi

echo ""
echo "cmake-package: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]]
