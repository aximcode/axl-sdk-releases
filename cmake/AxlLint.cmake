# AxlLint.cmake -- clang-tidy beside every compile (the x64-lint preset).
#
# Replaces scripts/lint.sh (design section 10.1, item 2). Ninja runs
# clang-tidy alongside each compile, in parallel, and only for a TU whose
# source or headers changed -- the compiler's depfiles say which. .clang-tidy
# sets WarningsAsErrors '*', so a finding FAILS that TU's compile: its object
# is not written and the next build checks it again. Without that, a finding
# would print once and the next build would skip the TU as up to date.
#
# Three passes, as lint.sh ran them:
#   src/             the full .clang-tidy (bugprone-* and clang-analyzer-*)
#   tests, tools,    bugprone-* only
#   fixtures
#   C++ TUs          the full config, against the toolchain's libstdc++
# Not linted, as before: vendored code (deps/), src/backend/,
# src/net/axl-mbedtls-platform.c, drivers/.

set(AXL_CLANG_TIDY_VERSION 21)    # keep in sync with ci.yml's clang-tidy-NN
find_program(AXL_CLANG_TIDY NAMES clang-tidy-${AXL_CLANG_TIDY_VERSION} clang-tidy REQUIRED)
execute_process(COMMAND ${AXL_CLANG_TIDY} --version OUTPUT_VARIABLE _ct_ver)
string(REGEX MATCH "version ([0-9]+)" _ "${_ct_ver}")
set(_ct_major "${CMAKE_MATCH_1}")
if(NOT _ct_major STREQUAL AXL_CLANG_TIDY_VERSION)
    message(WARNING "${AXL_CLANG_TIDY} is LLVM ${_ct_major}, not CI's pinned "
        "${AXL_CLANG_TIDY_VERSION}: a clean lint here does not guarantee a clean CI lint.")
endif()

# The toolchain's libc and libstdc++ headers. clang-tidy replays the compile
# with its own driver, and those directories are IMPLICIT in the cross
# compiler -- nothing on the command line names them -- so left alone it
# resolves <string.h> against the HOST's /usr/include. CMake recorded the
# compiler's own search list when it detected it; take them from there, and
# refuse to configure without them rather than lint against the wrong libc.
set(AXL_LIBC_INCLUDE "")
foreach(_d IN LISTS CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES)
    if(EXISTS "${_d}/string.h")
        set(AXL_LIBC_INCLUDE "${_d}")
    endif()
endforeach()
if(NOT AXL_LIBC_INCLUDE)
    message(FATAL_ERROR "lint: no directory holding string.h among the C compiler's "
        "implicit includes (${CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES}); clang-tidy would "
        "analyse against the host libc.")
endif()
set(AXL_CXX_INCLUDES "${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}")
list(FILTER AXL_CXX_INCLUDES INCLUDE REGEX "/c\\+\\+/")
set(_have_vector FALSE)
foreach(_d IN LISTS AXL_CXX_INCLUDES)
    if(EXISTS "${_d}/vector")
        set(_have_vector TRUE)
    endif()
endforeach()
if(NOT _have_vector)
    message(FATAL_ERROR "lint: no libstdc++ directory holding <vector> among the C++ "
        "compiler's implicit includes (${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}).")
endif()

set(_ct ${AXL_CLANG_TIDY} --quiet --extra-arg=-nostdlibinc
        --extra-arg=-idirafter${AXL_LIBC_INCLUDE})
set(AXL_TIDY_FULL ${_ct})
set(AXL_TIDY_BUGPRONE ${_ct} --checks=-clang-analyzer-*)
set(AXL_TIDY_CXX ${_ct} --extra-arg=-nostdinc++)
foreach(_d IN LISTS AXL_CXX_INCLUDES)
    list(APPEND AXL_TIDY_CXX --extra-arg=-isystem${_d})
endforeach()

# THE HOST TOOLS (axl_host_tool, cmake/AxlImages.cmake): pe-set-debug, which
# ships in the SDK and patches every image, the packer and fwtool-host. They
# are built with the HOST compiler by custom commands, so they are no target's
# sources and clang-tidy never saw them. Each non-vendored source gets the
# bugprone pass tools/ gets (the src/ ones among them keep their full pass in
# the cross build too: this is a second preprocessor configuration, e.g.
# fwtool-host's -DAXL_HOSTED), against the HOST libc (the build's own flags; no
# -nostdlibinc), as a step of its own: a stamp, rebuilt when the source, a
# header it includes (the host compiler's depfile), .clang-tidy or the lint
# stamp changes -- and a finding fails the step, as it fails a compile.
set(AXL_TIDY_HOST ${AXL_CLANG_TIDY} --quiet --checks=-clang-analyzer-*)
set(AXL_LINT_STAMP "${CMAKE_BINARY_DIR}/axl-lint.stamp")   # written below
get_property(_host_tools GLOBAL PROPERTY AXL_HOST_TOOLS)
set(_host_stamps "")
set(AXL_LINT_HOST_SOURCES "")
foreach(_tool IN LISTS _host_tools)
    get_property(_srcs GLOBAL PROPERTY "AXL_HOST_TOOL_SRCS:${_tool}")
    get_property(_flags GLOBAL PROPERTY "AXL_HOST_TOOL_FLAGS:${_tool}")
    # The compile flags clang-tidy replays: the build's, minus link-only ones.
    list(FILTER _flags EXCLUDE REGEX "^-Wl,")
    foreach(_src IN LISTS _srcs)
        file(RELATIVE_PATH _rel ${CMAKE_SOURCE_DIR} ${_src})
        if(_rel MATCHES "^deps/")
            continue()      # vendored code is never linted
        endif()
        string(MAKE_C_IDENTIFIER "${_tool}-${_rel}" _id)
        set(_stamp ${CMAKE_BINARY_DIR}/lint-host/${_id}.stamp)
        add_custom_command(OUTPUT ${_stamp}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/lint-host
            COMMAND ${CMAKE_COMMAND} -DCC=${AXL_HOST_CC} "-DFLAGS=${_flags}" -DSRCS=${_src}
                    -DTARGET=${_stamp} -DDEPFILE=${_stamp}.d -P ${CMAKE_SOURCE_DIR}/cmake/AxlHostDeps.cmake
            COMMAND ${AXL_TIDY_HOST} ${_src} -- ${_flags}
            COMMAND ${CMAKE_COMMAND} -E touch ${_stamp}
            DEPENDS ${_src} ${CMAKE_SOURCE_DIR}/.clang-tidy ${AXL_LINT_STAMP}
                    ${CMAKE_SOURCE_DIR}/cmake/AxlHostDeps.cmake
            DEPFILE ${_stamp}.d
            COMMENT "clang-tidy (host tool ${_tool}) ${_rel}" VERBATIM)
        list(APPEND _host_stamps ${_stamp})
        list(APPEND AXL_LINT_HOST_SOURCES ${_src})
    endforeach()
endforeach()
add_custom_target(axl_lint_host_tools ALL DEPENDS ${_host_stamps})

# Which targets take which pass: filed by axl_object() as each is declared.
get_property(AXL_LINT_SRC_TARGETS GLOBAL PROPERTY AXL_LINT_SRC)
get_property(AXL_LINT_BUGPRONE_TARGETS GLOBAL PROPERTY AXL_LINT_BUGPRONE)
get_property(AXL_LINT_CXX_TARGETS GLOBAL PROPERTY AXL_LINT_CXX)
set_target_properties(${AXL_LINT_SRC_TARGETS} PROPERTIES C_CLANG_TIDY "${AXL_TIDY_FULL}")
set_target_properties(${AXL_LINT_BUGPRONE_TARGETS} PROPERTIES C_CLANG_TIDY "${AXL_TIDY_BUGPRONE}")
set_target_properties(${AXL_LINT_CXX_TARGETS} PROPERTIES CXX_CLANG_TIDY "${AXL_TIDY_CXX}")

# axl_target_sources_abs(<var> <target>) -- a target's sources as absolute
# paths. add_library() keeps the spelling it was given, relative to the
# target's own directory; target_sources() from another directory stores them
# absolute. Both are normalised here.
function(axl_target_sources_abs var target)
    get_target_property(_srcs ${target} SOURCES)
    get_target_property(_dir ${target} SOURCE_DIR)
    set(_out "")
    foreach(_s IN LISTS _srcs)
        get_filename_component(_a "${_s}" ABSOLUTE BASE_DIR "${_dir}")
        list(APPEND _out "${_a}")
    endforeach()
    set(${var} "${_out}" PARENT_SCOPE)
endfunction()

# Not linted, as before: vendored code, src/backend/ and the mbedTLS platform
# glue -- all of it in libaxl. Source properties are DIRECTORY-scoped and
# libaxl is declared in src/, so TARGET_DIRECTORY names it: set without that,
# from here, these would silently not apply.
axl_target_sources_abs(_axl_srcs axl)
set(_skip "")
foreach(_a IN LISTS _axl_srcs)
    if(_a MATCHES "^${CMAKE_SOURCE_DIR}/(deps/|src/backend/)"
       OR _a STREQUAL "${CMAKE_SOURCE_DIR}/src/net/axl-mbedtls-platform.c")
        list(APPEND _skip "${_a}")
    endif()
endforeach()
set_property(SOURCE ${_skip} TARGET_DIRECTORY axl PROPERTY SKIP_LINTING ON)

# COVERAGE, checked here so the lint preset cannot even configure with a file
# it would not lint: every .c lint.sh linted (src/ minus the exclusions,
# test/unit/, tools/, test/integration/), every host tool's C under scripts/,
# and every C++ TU under src/ must be a source of a target (or host tool)
# above. So a new scripts/*.c that is not a host tool's fails this configure. A file in no such target is never analysed and
# would report clean.
file(GLOB_RECURSE _want RELATIVE ${CMAKE_SOURCE_DIR} CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.cpp
     ${CMAKE_SOURCE_DIR}/test/unit/*.c ${CMAKE_SOURCE_DIR}/tools/*.c
     ${CMAKE_SOURCE_DIR}/test/integration/*.c ${CMAKE_SOURCE_DIR}/scripts/*.c)
list(FILTER _want EXCLUDE REGEX "^src/backend/")
list(REMOVE_ITEM _want src/net/axl-mbedtls-platform.c)
set(_have "")
foreach(_t IN LISTS AXL_LINT_SRC_TARGETS AXL_LINT_BUGPRONE_TARGETS AXL_LINT_CXX_TARGETS)
    axl_target_sources_abs(_srcs ${_t})
    foreach(_a IN LISTS _srcs)
        file(RELATIVE_PATH _r ${CMAKE_SOURCE_DIR} ${_a})
        list(APPEND _have ${_r})
    endforeach()
endforeach()
foreach(_a IN LISTS AXL_LINT_HOST_SOURCES)
    file(RELATIVE_PATH _r ${CMAKE_SOURCE_DIR} ${_a})
    list(APPEND _have ${_r})
endforeach()
set(_unlinted "")
foreach(_f IN LISTS _want)
    if(NOT _f IN_LIST _have)
        list(APPEND _unlinted ${_f})
    endif()
endforeach()
if(_unlinted)
    list(JOIN _unlinted "\n  " _unlinted)
    message(FATAL_ERROR "lint: these sources are in no linted target, so clang-tidy "
        "would never see them:\n  ${_unlinted}\nAdd each to the target that builds it "
        "(its directory's CMakeLists.txt), or exclude it in cmake/AxlLint.cmake with a reason.")
endif()
# WHAT A VERDICT DEPENDS ON beyond the source and its headers: the check
# configuration and the clang-tidy that ran it. Ninja knows neither, so a TU
# linted clean would stay "up to date" after either changed -- a stale green.
# Every linted source's object depends on .clang-tidy and on a stamp holding
# the clang-tidy version and the three passes' full arguments; file(CONFIGURE)
# rewrites the stamp only when that text changes, so an unrelated re-configure
# does not relint the tree.
file(CONFIGURE OUTPUT "${AXL_LINT_STAMP}" CONTENT
"${_ct_ver}
full: ${AXL_TIDY_FULL}
bugprone: ${AXL_TIDY_BUGPRONE}
host: ${AXL_TIDY_HOST}
cxx: ${AXL_TIDY_CXX}
")
foreach(_t IN LISTS AXL_LINT_SRC_TARGETS AXL_LINT_BUGPRONE_TARGETS AXL_LINT_CXX_TARGETS)
    axl_target_sources_abs(_srcs ${_t})
    list(REMOVE_ITEM _srcs ${_skip})     # vendored code is never linted
    set_property(SOURCE ${_srcs} TARGET_DIRECTORY ${_t} APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_SOURCE_DIR}/.clang-tidy" "${AXL_LINT_STAMP}")
endforeach()

list(LENGTH _want _n)
list(LENGTH AXL_LINT_SRC_TARGETS _ns)
list(LENGTH AXL_LINT_BUGPRONE_TARGETS _nb)
list(LENGTH AXL_LINT_CXX_TARGETS _nc)
message(STATUS "lint: clang-tidy ${_ct_major} over ${_n} sources in ${_ns} full, "
    "${_nb} bugprone-only and ${_nc} C++ target(s)")
