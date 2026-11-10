# AxlCcache.cmake -- compile through ccache when it is there.
#
# AXL_CCACHE picks the policy:
#   AUTO (default)  use ccache if AXL_CCACHE_PROGRAM resolves, else build
#                   without it -- a host without ccache is not broken
#   ON              REQUIRE it; a configure that cannot find it fails, because
#                   asked for and silently absent is a slower build nobody
#                   chose and nobody can see
#   OFF             never
#
# It goes in as CMAKE_<LANG>_COMPILER_LAUNCHER, which Ninja prepends to the
# compile RULE only. compile_commands.json keeps naming the compiler itself,
# which matters: clang-tidy and scripts/compile-like.py read that database,
# and both would take `ccache` for the compiler.
#
# Included before any target exists: the variables seed each target's
# <LANG>_COMPILER_LAUNCHER property when the target is CREATED, so a target
# declared earlier would compile without it.
#
# C and C++ only. The few assembly sources are not worth a cache lookup each.
#
# Pinned by test/integration/test-cmake-ccache.sh, including that a cached
# build's images are byte-identical to a cold one's.

set(AXL_CCACHE AUTO CACHE STRING "Compile through ccache: AUTO, ON or OFF")
set_property(CACHE AXL_CCACHE PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${AXL_CCACHE}" _axl_ccache)
if(NOT _axl_ccache MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "AXL_CCACHE must be AUTO, ON or OFF, not '${AXL_CCACHE}'")
endif()

# A launcher already chosen -- on the command line, or seeded by CMake (3.17+)
# from an exported CMAKE_<LANG>_COMPILER_LAUNCHER. OFF clears it: "never"
# must hold on a host whose environment says otherwise. AUTO leaves a non-ccache
# launcher (sccache, distcc) in place; ON cannot have both, and refuses.
set(_axl_user_launcher "")
foreach(_lang C CXX)
    if(CMAKE_${_lang}_COMPILER_LAUNCHER)
        get_filename_component(_axl_n "${CMAKE_${_lang}_COMPILER_LAUNCHER}" NAME)
        if(_axl_n STREQUAL "ccache")
            # A ccache chosen by path is THE ccache, wherever it lives.
            set(AXL_CCACHE_PROGRAM "${CMAKE_${_lang}_COMPILER_LAUNCHER}")
        else()
            set(_axl_user_launcher "CMAKE_${_lang}_COMPILER_LAUNCHER=${CMAKE_${_lang}_COMPILER_LAUNCHER}")
        endif()
    endif()
endforeach()

if(_axl_ccache STREQUAL "OFF")
    set(CMAKE_C_COMPILER_LAUNCHER "")
    set(CMAKE_CXX_COMPILER_LAUNCHER "")
    return()
endif()
if(_axl_user_launcher)
    if(_axl_ccache STREQUAL "ON")
        message(FATAL_ERROR "AXL_CCACHE=ON, but another launcher is set "
            "(${_axl_user_launcher}). Drop one of them.")
    endif()
    message(STATUS "Compiling through ${_axl_user_launcher}, not ccache (AXL_CCACHE=AUTO)")
    return()
endif()

# find_program searches only while the cached result is -NOTFOUND, so a path
# it found once and that has since gone (ccache moved) would read as "none
# found" forever. _AXL_CCACHE_FOUND records what the SEARCH found, so only that
# is searched for again; a path the user gave is taken as given.
if(AXL_CCACHE_PROGRAM AND NOT EXISTS "${AXL_CCACHE_PROGRAM}"
   AND AXL_CCACHE_PROGRAM STREQUAL "${_AXL_CCACHE_FOUND}")
    unset(AXL_CCACHE_PROGRAM CACHE)
endif()
if(NOT AXL_CCACHE_PROGRAM)
    find_program(AXL_CCACHE_PROGRAM ccache)
    set(_AXL_CCACHE_FOUND "${AXL_CCACHE_PROGRAM}" CACHE INTERNAL "")
endif()

if(AXL_CCACHE_PROGRAM AND EXISTS "${AXL_CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${AXL_CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${AXL_CCACHE_PROGRAM}")
    message(STATUS "Compiling through ${AXL_CCACHE_PROGRAM} (AXL_CCACHE=${_axl_ccache})")
elseif(_axl_ccache STREQUAL "ON")
    message(FATAL_ERROR "AXL_CCACHE=ON, but no ccache was found "
        "(AXL_CCACHE_PROGRAM=${AXL_CCACHE_PROGRAM}). Install it, point "
        "AXL_CCACHE_PROGRAM at it, or configure with AXL_CCACHE=AUTO.")
else()
    message(STATUS "Compiling without ccache: none found (AXL_CCACHE=AUTO)")
endif()
