#!/bin/bash
# test-meta: arch=x64 needs=cmake,ninja est=40 local-only=0
# test-cmake-package.sh — the GENERATED CMake package must configure.
#
# WHY THIS EXISTS. scripts/install.sh generates an axl-config.cmake that
# RE-IMPLEMENTS axl-cc's whole pipeline in CMake -- its own compile line, its
# own ld, its own objcopy, its own pe-set-debug. That makes it the third of the
# three build paths `gate.check-flag-parity` polices, and until this file
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
# TWICE: under the pinned CMake AND under the consumer FLOOR. The package
# and sdk/examples build with the CONSUMER's CMake, and both promise
# CMake 3.20 (`cmake_minimum_required(VERSION 3.20)` in sdk/examples; a
# version check in the package). The pinned 4.2.8 cannot see a command newer
# than 3.20 (3.21's file(COPY_FILE), say) or a newer policy default slipping
# into what install.sh generates or sdk/examples, so the same cases run again
# under 3.20.6, the last 3.20
# (`ci-cmake.sh --floor-version`, fetched and checksum-verified like the pin,
# scripts/axl-cmake-pin.sh axl_floor_cmake_bin; docs/AXL-Build-System-Design.md
# §10.13). Each run is a CHILD of this script with that cmake first on PATH;
# the child first asserts `cmake --version` IS the one it was given, so a run
# that silently took another cmake fails instead of passing.
#
# And BELOW the floor: under 3.16.9 (`ci-cmake.sh --below-floor-version`, the
# floor 3.20 replaced) find_package(axl) must stop with the package's own
# exact error naming 3.20, not configure into something half-working.
#
# A CMake that cannot be INSTALLED is a FAIL naming it, never a skip: "could
# not run 3.20" and "3.20 configured fine" must not read alike. The one skip
# is a machine Kitware built no such release for (3.20: x86_64 and aarch64;
# 3.16: x86_64 only).
#
# Requires a staged SDK (scripts/install.sh) and cmake.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
source "$PROJECT_DIR/scripts/axl-cmake-pin.sh"

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

# ---------------------------------------------------------------------
# THE PARENT: pick each CMake, run the cases under it as a child.
# ---------------------------------------------------------------------
if [[ -z "${AXL_PKG_RUN:-}" ]]; then
    # The CMake CI and the release use, run alone or under the suite
    # (scripts/axl-cmake-pin.sh): a local pass must be the one CI would see.
    axl_use_pinned_cmake || exit 1

    echo "=== cmake-package ==="
    if [[ ! -f "$STAGE/lib/cmake/axl/axl-config.cmake" ]]; then
        echo "  SKIP: no staged CMake package at $STAGE/lib/cmake/axl/"
        echo "        run: scripts/install.sh --arch x64"
        exit 0
    fi

    # cmake_error LOG -- the package's FATAL_ERROR in LOG as one line: CMake
    # re-wraps a message's text, and ends it at a blank line or at its
    # "Call Stack" trailer.
    cmake_error() {
        awk '/^CMake Error at .*axl-config\.cmake:[0-9]+ \(message\):$/ { on = 1; next }
             on && (/^$/ || /^Call Stack/) { exit }
             on' "$1" | tr -s ' \n' '  ' | sed 's/^ //; s/ $//'
    }

    # find_reason LOG -- the "Reason given by package:" find_package prints
    # for a config that set <pkg>_FOUND false, as one line.
    find_reason() {
        awk '/Reason given by package:$/ { on = 1; next }
             on && !started && /^$/ { next }
             on { started = 1 }
             on && (/^$/ || /^Call Stack/) { exit }
             on' "$1" | tr -s ' \n' '  ' | sed 's/^ //; s/ $//'
    }

    # below_case NAME FIND-LINE -- configure, under the below-floor cmake, a
    # project whose own minimum is deliberately LOWER (3.16), so an error can
    # only be the package's (sdk/examples would stop at its own
    # cmake_minimum_required). Leaves $bw/NAME.log and the rc in $rc.
    below_case() {
        mkdir -p "$bw/$1"
        printf 'cmake_minimum_required(VERSION 3.16)\nproject(axlold NONE)\n%s\nmessage(STATUS "axl: past find_package, axl_FOUND=[${axl_FOUND}]")\n' "$2" \
            > "$bw/$1/CMakeLists.txt"
        "$AXL_BELOW_FLOOR_CMAKE_BIN/cmake" -S "$bw/$1" -B "$bw/$1/b" -DCMAKE_PREFIX_PATH="$STAGE" \
            > "$bw/$1.log" 2>&1
        rc=$?
    }
    # The message names the FLOOR (ci-cmake.sh's), so moving the floor
    # without moving the package's check fails here.
    below_cases() {
        local got want="axl: the AXL SDK's CMake package requires CMake ${floor_v%.*} or newer (this is CMake $below_v)."
        got="$("$AXL_BELOW_FLOOR_CMAKE_BIN/cmake" --version 2>&1 | head -1)"
        [[ "$got" == "cmake version $below_v" ]]
        check "$?" "[below-floor] its cmake runs and is $below_v (got: ${got:-nothing})"
        bw="$(mktemp -d -t axl-cmake-old.XXXXXX)"

        # REQUIRED: configure stops, with the package's reason.
        below_case required 'find_package(axl REQUIRED)'
        check "$([[ "$rc" -ne 0 ]]; echo $?)" "[below-floor] find_package(axl REQUIRED) fails configure under $below_v (rc=$rc)"
        got="$(find_reason "$bw/required.log")"
        [[ "$got" == "$want" ]]
        check "$?" "[below-floor] ...giving the package's reason, naming CMake ${floor_v%.*}"
        [[ "$got" == "$want" ]] || { printf '      got:  %s\n      want: %s\n' "$got" "$want"; tail -8 "$bw/required.log" | sed 's/^/      /'; }
        check "$(grep -c '^CMake Error' "$bw/required.log" | grep -qx 1; echo $?)" "[below-floor] ...and it is the ONLY error"

        # Optional: not found, configure goes on -- the consumer can fall back.
        below_case optional 'find_package(axl)'
        check "$([[ "$rc" -eq 0 ]] && grep -qx -- '-- axl: past find_package, axl_FOUND=\[0\]' "$bw/optional.log"; echo $?)" \
            "[below-floor] an optional find_package(axl) is NOT FOUND and configure goes on (rc=$rc)"

        # include(): no not-found channel, so the package's own FATAL_ERROR.
        below_case include "include(\"$STAGE/lib/cmake/axl/axl-config.cmake\")"
        got="$(cmake_error "$bw/include.log")"
        [[ "$rc" -ne 0 && "$got" == "$want" ]]
        check "$?" "[below-floor] include() of the package stops with the same message as a FATAL_ERROR (rc=$rc)"
        [[ "$got" == "$want" ]] || printf '      got:  %s\n      want: %s\n' "$got" "$want"

        # include()d from ANOTHER package's config: CMAKE_FIND_PACKAGE_NAME
        # is that package's, and marking IT not-found while returning only
        # from axl-config.cmake would let Foo report found and the consumer
        # fail later on a missing axl_add_app. It must be the FATAL_ERROR.
        mkdir -p "$bw/foo"
        printf 'include("%s/lib/cmake/axl/axl-config.cmake")\n' "$STAGE" > "$bw/foo/FooConfig.cmake"
        below_case viafoo "find_package(Foo REQUIRED PATHS \"$bw/foo\" NO_DEFAULT_PATH)"
        got="$(cmake_error "$bw/viafoo.log")"
        [[ "$rc" -ne 0 && "$got" == "$want" ]]
        check "$?" "[below-floor] included by another package's config, it is the FATAL_ERROR, not that package's not-found (rc=$rc)"
        [[ "$got" == "$want" ]] || { printf '      got:  %s\n      want: %s\n' "$got" "$want"; tail -6 "$bw/viafoo.log" | sed 's/^/      /'; }
        rm -rf "$bw"
    }

    # run_child LABEL VERSION BINDIR -- the cases under BINDIR's cmake.
    run_child() {
        echo ""
        echo "--- under CMake $2 ($1) ---"
        PATH="$3:$PATH" AXL_PKG_RUN="$1" AXL_PKG_CMAKE_VERSION="$2" "${BASH_SOURCE[0]}"
        local rc=$?
        check "$rc" "the package and sdk/examples configure and build under the $1 CMake $2 (rc=$rc)"
    }

    pinned_v="$("$PROJECT_DIR/scripts/ci-cmake.sh" --version)"
    floor_skipped=0
    floor_v="$("$PROJECT_DIR/scripts/ci-cmake.sh" --floor-version)"

    # The floor tested is the floor DECLARED: sdk/examples is the CMakeLists
    # a consumer copies. Raising its minimum without moving the floor (or the
    # reverse) would test a promise nobody makes.
    # Every CMakeLists sdk/examples ships declares it.
    for ex in "$PROJECT_DIR"/sdk/examples/CMakeLists.txt "$PROJECT_DIR"/sdk/examples/*/CMakeLists.txt; do
        declared="$(sed -n 's/^cmake_minimum_required(VERSION \([0-9.]*\)).*/\1/p' "$ex")"
        [[ -n "$declared" && "${floor_v%.*}" == "$declared" ]]
        check "$?" "the floor CMake $floor_v is the minor ${ex#"$PROJECT_DIR"/} declares (VERSION ${declared:-<none found>})"
    done

    run_child pinned "$pinned_v" "$(dirname "$(command -v cmake)")"

    axl_floor_cmake_bin
    frc=$?
    case "$frc" in
        0) run_child floor "$floor_v" "$AXL_FLOOR_CMAKE_BIN" ;;
        2) echo ""
           echo "  SKIP: no floor CMake $floor_v for $(uname -m) -- the floor is UNTESTED here"
           floor_skipped=1 ;;
        *) check 1 "the floor CMake $floor_v could not be installed (above) -- the floor is UNTESTED, not passed" ;;
    esac

    # BELOW the floor: the package refuses, by name (below_cases).
    echo ""
    below_v="$("$PROJECT_DIR/scripts/ci-cmake.sh" --below-floor-version)"
    echo "--- under CMake $below_v (below the floor) ---"
    axl_below_floor_cmake_bin
    brc=$?
    case "$brc" in
        0) below_cases ;;
        2) echo "  SKIP: no CMake $below_v for $(uname -m) -- the refusal below the floor is UNTESTED here"
           floor_skipped=1 ;;
        *) check 1 "the below-floor CMake $below_v could not be installed (above) -- the refusal is UNTESTED, not passed" ;;
    esac

    echo ""
    echo "cmake-package: $pass passed, $fail failed"
    (( fail == 0 )) || exit 1
    # Part of the test did not run: the runner shows SKIP (77), never PASS.
    (( floor_skipped )) && exit 77
    exit 0
fi

# ---------------------------------------------------------------------
# THE CHILD: every case, under the cmake first on PATH.
# ---------------------------------------------------------------------
# First prove WHICH cmake that is: everything below is worthless under the
# wrong one, and a cmake that cannot even run is not a configure failure.
got="$(cmake --version 2>&1 | head -1)"
if [[ "$got" != "cmake version $AXL_PKG_CMAKE_VERSION" ]]; then
    check 1 "[$AXL_PKG_RUN] cmake on PATH runs and is $AXL_PKG_CMAKE_VERSION (got: ${got:-nothing})"
    exit 1
fi
check 0 "[$AXL_PKG_RUN] cmake on PATH runs and is $AXL_PKG_CMAKE_VERSION"

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
cmake_minimum_required(VERSION 3.20)
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
cmake_minimum_required(VERSION 3.20)
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
# followed by the machine. Same trick test-host-deps-minimal.sh uses.
pe_machine() {
    local off
    off=$(od -An -tu4 -j60 -N4 "$1" | tr -d " ")
    od -An -tx2 -j $((off + 4)) -N2 "$1" | tr -d " "
}

build_case() {  # build_case <name> <cmakelists-body> <target> <expect-machine>
    local name="$1" body="$2" target="$3" want="$4"
    cat > "$WORK/src/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.20)
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
#
# Under BOTH generators a consumer is likely to use, at the floor as well as
# the pin. CMake gives a custom command a DEPFILE under Makefiles only from
# 3.20 -- the reason that is the package's minimum (it was 3.16, where a
# Makefile build tracked no headers at all).
#
# Two apps, each in an add_subdirectory(): CMP0116 resolves a depfile's
# relative paths against the subdirectory's binary dir, so where that dir is
# matters. `depapp` builds under the top build dir; `depext` in a binary dir
# OUTSIDE it.
#
# dep_case TAG GENERATOR
dep_case() {
    local tag="$1" gen="$2" d="$WORK/dep-$1"
    mkdir -p "$d/src/sub" "$d/src/ext"
    printf '#include "myhdr.h"\n#include <axl.h>\nint main(void){ return MYVAL; }\n' \
        > "$d/src/sub/app.c"
    cp "$d/src/sub/app.c" "$d/src/ext/app.c"
    printf '#define MYVAL 0\n' > "$d/src/sub/myhdr.h"
    cat > "$d/src/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(axldeptest C)
find_package(axl REQUIRED)
add_subdirectory(sub)
add_subdirectory(ext "${CMAKE_BINARY_DIR}/../ext-bin")
EOF
    echo 'axl_add_app(depapp app.c OPTIONS -I${CMAKE_CURRENT_SOURCE_DIR})' \
        > "$d/src/sub/CMakeLists.txt"
    echo 'axl_add_app(depext app.c OPTIONS -I${CMAKE_CURRENT_SOURCE_DIR}/../sub)' \
        > "$d/src/ext/CMakeLists.txt"
    if ! { cmake -G "$gen" -S "$d/src" -B "$d/b" -DCMAKE_PREFIX_PATH="$STAGE" \
                > "$d/cfg.log" 2>&1 \
            && cmake --build "$d/b" > "$d/b1.log" 2>&1; }; then
        check 1 "dep[$tag]: the tracking fixture builds (see $d/cfg.log)"
        tail -5 "$d/cfg.log" "$d/b1.log" 2>/dev/null | sed 's/^/      /'
        return
    fi
    check 0 "dep[$tag]: the tracking fixture builds"

    # A no-op rebuild must do NOTHING. Without this a package that rebuilt
    # unconditionally would pass the two assertions below for the wrong reason.
    cmake --build "$d/b" > "$d/noop.log" 2>&1
    ! grep -q "axl-cc:" "$d/noop.log"
    check "$?" "dep[$tag]: a no-op rebuild recompiles nothing"

    sleep 1.1; printf '#define MYVAL 1\n' > "$d/src/sub/myhdr.h"
    cmake --build "$d/b" > "$d/b2.log" 2>&1
    grep -q "axl-cc: depapp.app.o" "$d/b2.log" && grep -q "axl-cc: depext.app.o" "$d/b2.log"
    check "$?" "dep[$tag]: editing a PROJECT header rebuilds both objects"

    # RESTORE the mtime. $STAGE is the SHARED staged SDK that every other
    # suite and every consumer tree builds against, so bumping a header there
    # permanently would force rebuilds well outside this test.
    local sdk_hdr="$STAGE/include/axl-sdk/axl/axl-mem.h" sdk_ref="$d/mtime-ref" sdk_rc
    touch -r "$sdk_hdr" "$sdk_ref"
    sleep 1.1; touch "$sdk_hdr"
    cmake --build "$d/b" > "$d/b3.log" 2>&1
    sdk_rc=$?
    touch -r "$sdk_ref" "$sdk_hdr"
    [[ "$sdk_rc" -eq 0 ]] && grep -q "axl-cc: depapp.app.o" "$d/b3.log" \
        && grep -q "axl-cc: depext.app.o" "$d/b3.log"
    check "$?" "dep[$tag]: editing an SDK header rebuilds both objects (needs -MD, not -MMD)"

    # A header DELETED (and its #include dropped) must not break the build:
    # the case gcc's -MP exists for, which the package does not pass (CMake
    # 3.20 mangles its phony targets), so this is what proves each generator
    # copes without it. A REGRESSION GUARD: nothing in the package can be
    # sabotaged to produce a stale-rule error today (that would take CMake
    # dropping its own phony entries). What was measured is that the rc
    # check fires: keeping the #include of the deleted header fails it under
    # both generators and both CMakes.
    sleep 1.1
    printf '#include <axl.h>\nint main(void){ return 0; }\n' > "$d/src/sub/app.c"
    cp "$d/src/sub/app.c" "$d/src/ext/app.c"
    rm "$d/src/sub/myhdr.h"
    cmake --build "$d/b" > "$d/b4.log" 2>&1
    sdk_rc=$?
    [[ "$sdk_rc" -eq 0 ]] && grep -q "axl-cc: depapp.app.o" "$d/b4.log" \
        && grep -q "axl-cc: depext.app.o" "$d/b4.log"
    check "$?" "dep[$tag]: deleting a header (and its #include) rebuilds both objects, no stale-rule error (rc=$sdk_rc)"
    [[ "$sdk_rc" -eq 0 ]] || tail -4 "$d/b4.log" | sed 's/^/      /'
}
dep_case makefiles "Unix Makefiles"
dep_case ninja Ninja

# The gap this file was written for: an exceptions image through CMake. It
# used to CONFIGURE and then die at the first throw, because the package
# re-implemented axl-cc's pipeline without the _eh linker script, the glue
# objects or the toolchain libraries.
# The C++ cases need the staged cxxrt glue; a C-only SDK (install.sh
# --no-cpp) is a legitimate configuration, and FAILING there would report a
# defect that is really a missing optional component. The C, driver, embeds
# and multi cases above run either way.
if [[ ! -f "$STAGE/lib/axl/x64/axl-cxxrt-terminate.o" ]]; then
    echo "  SKIP: no staged C++ glue — C++, exceptions and sdk/examples cases not run"
    echo "        (install.sh --cpp to include them)"
    echo ""
    echo "cmake-package [$AXL_PKG_RUN]: $pass passed, $fail failed"
    [[ "$fail" -eq 0 ]]
    exit $?
fi
build_case cxx-app 'axl_add_app(cxxapp app.cpp)' cxxapp 8664

printf '#include <stdexcept>\nint main() { try { throw std::runtime_error("x"); } catch (const std::exception &) { return 0; } return 1; }\n' \
    > "$WORK/src/eh.cpp"
build_case eh 'axl_add_app(ehapp eh.cpp OPTIONS -fexceptions)' ehapp 8664

# sdk/examples AS SHIPPED: the CMakeLists a consumer copies, configured and
# built as-is (C and C++ apps, so it sits behind the C++ glue check). Every
# axl_add_app it declares must produce its .efi; the declared list is read
# from the file, so a new example is covered without editing this test.
ex_want="$(sed -n 's/^axl_add_app(\([^ )]*\).*/\1/p' "$PROJECT_DIR/sdk/examples/CMakeLists.txt" | sort)"
[[ -n "$ex_want" ]]
check "$?" "examples: control -- the declared axl_add_app targets are found ($(wc -w <<< "$ex_want"))"
rm -rf "$WORK/bld-examples"
if ! cmake -S "$PROJECT_DIR/sdk/examples" -B "$WORK/bld-examples" -DCMAKE_PREFIX_PATH="$STAGE" \
        > "$WORK/examples-cfg.log" 2>&1; then
    check 1 "examples: configure"
    tail -6 "$WORK/examples-cfg.log" | sed 's/^/      /'
elif ! cmake --build "$WORK/bld-examples" -j "$(nproc)" > "$WORK/examples-bld.log" 2>&1; then
    check 1 "examples: build"
    tail -6 "$WORK/examples-bld.log" | sed 's/^/      /'
else
    ex_got="$(find "$WORK/bld-examples" -maxdepth 1 -name '*.efi' ! -name '*.raw.efi' -printf '%f\n' \
        | sed 's/\.efi$//' | sort)"
    [[ "$ex_got" == "$ex_want" ]]
    check "$?" "examples: every declared app built its .efi"
    [[ "$ex_got" == "$ex_want" ]] || diff <(echo "$ex_want") <(echo "$ex_got") | sed 's/^/      /'
fi

echo ""
echo "cmake-package [$AXL_PKG_RUN]: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]]
