# axl-sdk -- the CMake build (docs/AXL-Build-System-Design.md).
#
# Shared machinery lives in cmake/ (toolchain, flags, helpers, images, lint);
# what is built is declared by each directory's own CMakeLists.txt. It is the
# only build since 5.0.0 retired the Makefile it was restated from.
#
#   cmake --preset x64-debug && cmake --build --preset x64-debug

# 3.29: an empty CTEST_PARALLEL_LEVEL (set by every test preset) means one
# test per core, so the presets need not name a core count.
cmake_minimum_required(VERSION 3.29)

# The `fuzz` preset: a HOST build of test/fuzz alone, with clang -- libFuzzer
# and ASan cannot use the cross toolchain, and one project has one C compiler.
option(AXL_FUZZ "Build only the host-side libFuzzer harnesses (test/fuzz)" OFF)
if(AXL_FUZZ)
    project(axl_fuzz LANGUAGES C)
    enable_testing()
    add_subdirectory(test/fuzz)
    return()
endif()

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AxlToolchain.cmake")
endif()

project(axl LANGUAGES C CXX ASM)

# VERSION and axl-version.h must agree (scripts/bump-version.sh writes both):
# the release names its tarballs from VERSION and its binaries report the
# header's string, so a tag cut without the bump would ship tarballs of one
# version holding binaries of another. The Makefile's `all` checked this on
# every build (check-version); this is that check, at configure time -- and both
# files reconfigure the tree when they change.
file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION _axl_ver LIMIT_COUNT 1)
file(STRINGS ${CMAKE_SOURCE_DIR}/include/axl/axl-version.h _axl_hver
     REGEX "^#define AXL_VERSION_STRING +\"[^\"]*\"")
string(REGEX REPLACE "^#define AXL_VERSION_STRING +\"([^\"]*)\".*" "\\1" _axl_hver "${_axl_hver}")
if(NOT _axl_ver OR NOT _axl_ver STREQUAL _axl_hver)
    message(FATAL_ERROR "VERSION (${_axl_ver}) != axl-version.h's AXL_VERSION_STRING "
        "(${_axl_hver}) -- run scripts/bump-version.sh X.Y.Z to update both")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/VERSION ${CMAKE_SOURCE_DIR}/include/axl/axl-version.h)

include(cmake/AxlCcache.cmake)
include(cmake/AxlFlags.cmake)
include(cmake/AxlHelpers.cmake)
include(cmake/AxlImages.cmake)

# Before any directory: an add_test() a directory makes before
# enable_testing() is silently not registered.
enable_testing()

# Before any directory too: test/integration configures its ctest-time script
# with this interpreter's path, and the tests below run it.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# What gets built, by directory. src/ first: it declares libaxl, which the
# vendored code in deps/ closes.
add_subdirectory(src)
add_subdirectory(deps)
add_subdirectory(drivers/crashhandler)
add_subdirectory(tools)
add_subdirectory(test/unit)
# The integration tests are CTest tests in the DEBUG trees (not the lint or RTK
# trees): test/integration registers them, and this tree binds the resource
# spec that bounds their pooled guests -- here, at the top, because ctest
# reads CTEST_RESOURCE_SPEC_FILE from the top-level directory. The spec's
# CONTENT is written at ctest time (cmake/AxlIntegrationTests.cmake.in).
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT AXL_LINT AND NOT AXL_RTK_USB_UNDI)
    set(AXL_INTEGRATION_TESTS ON)
    set(CTEST_RESOURCE_SPEC_FILE ${CMAKE_BINARY_DIR}/axl-resource-spec.json)
else()
    set(AXL_INTEGRATION_TESTS OFF)
endif()
add_subdirectory(test/integration)
add_subdirectory(sdk)
add_subdirectory(experiments/axl-kernel)

# The goal names callers build (scripts/axl-build.sh), resolved now that
# every directory has declared its images.
include(cmake/AxlGoals.cmake)

# What an SDK prefix holds: install() rules, read by scripts/install.sh
# (`cmake --install`) and by CPack for the release archives.
include(cmake/AxlInstall.cmake)
# ...and the release archives': the documentation payload, the UEFI-tools
# archive, and the CPack config scripts/make-release-assets.sh packs them with.
include(cmake/AxlPackage.cmake)

# ---------------------------------------------------------------------------
# Lint (the x64-lint preset): clang-tidy beside every compile
# ---------------------------------------------------------------------------

option(AXL_LINT "Run clang-tidy beside every compile (replaces scripts/lint.sh)" OFF)
if(AXL_LINT)
    include(cmake/AxlLint.cmake)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


if(AXL_LINT)
    # The clang COMPILER's own -Wall -Wextra diagnostics, which clang-tidy does
    # not report: clang -fsyntax-only over this build's compile database.
    add_test(NAME lint.clang-warnings
        COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/scripts/check-clang-warnings.py
                -p ${CMAKE_BINARY_DIR} --libc-include ${AXL_LIBC_INCLUDE}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(lint.clang-warnings PROPERTIES LABELS "gate;lint")
endif()

