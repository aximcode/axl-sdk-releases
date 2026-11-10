# AxlFlags.cmake -- the flag set every TU is built with (the retired
# Makefile's CFLAGS_BASE + CFLAGS_BUILD + INCLUDES, and CXXFLAGS -- the names
# below), as interface targets.
#   axl_target_flags  what the code is built FOR (C, C++ and assembly)
#   axl_config_flags  the configuration (Debug, the default; Release)
#   axl_flags         C/C++: both, plus the language flags and include paths

# CMake's own per-configuration defaults (-g, -O3 -DNDEBUG, ...) are not ours:
# the configuration's flags are set on axl_flags below, as the Makefile's
# CFLAGS_BUILD set them.
foreach(_lang C CXX ASM)
    foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set(CMAKE_${_lang}_FLAGS_${_cfg} "")
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# The flag set (the Makefile's CFLAGS_BASE + CFLAGS_BUILD + INCLUDES)
# ---------------------------------------------------------------------------

if(AXL_ARCH STREQUAL "aa64")
    set(AXL_GCC_ARCH -mno-outline-atomics -ffixed-x18)
else()
    set(AXL_GCC_ARCH -mno-red-zone -march=x86-64)
endif()
set(AXL_STACK_PROTECTOR -fstack-protector-strong -mstack-protector-guard=global)

# The Makefile's CFLAGS_BASE as one list, for a command CMake does not
# generate itself: the pack wrap assembles each image's payload inside a
# script (cmake/AxlWrapEfi.cmake). Built from the same pieces as the targets.
set(AXL_CFLAGS_BASE -std=gnu2x -DAXL_ALLOW_UEFI -ffreestanding -fshort-wchar
    -fno-builtin -fno-math-errno -fno-trapping-math -fno-omit-frame-pointer
    -fpic ${AXL_GCC_ARCH} ${AXL_STACK_PROTECTOR} -Wall -DAXL_BACKEND_NATIVE)
if(NOT AXL_ARCH STREQUAL "aa64")
    list(APPEND AXL_CFLAGS_BASE -fno-asynchronous-unwind-tables)
endif()

# newlib's struct mallinfo: DOES THIS TOOLCHAIN'S MATCH ITS OWN HEADER?
# <malloc.h> declares ten size_t, but an x86_64-elf newlib built from the
# pre-axl3 recipe filled ten INT -- a property of the TOOLCHAIN BUILD, not of
# the arch, so it is probed: the SIZE of __malloc_current_mallinfo in libc.a,
# 40 bytes (ten ints, broken) or 80 (ten size_t, correct). Only on 40 does
# src/cxxrt/axl-cxxrt-alloc.c displace newlib's mallinfo
# (AXL_NEWLIB_MALLINFO_INT). The Makefile probed this; test-toolchain-variant.sh
# holds the CMake build to it with a stub nm.
# A probe that could not RUN is not a probe that found 80 bytes, so every way
# it can fail to see is fatal rather than read as "correct".
execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=libc.a
                OUTPUT_VARIABLE _axl_libc OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _axl_rc ERROR_VARIABLE _axl_err)
if(NOT _axl_rc EQUAL 0 OR NOT IS_ABSOLUTE "${_axl_libc}" OR NOT EXISTS "${_axl_libc}")
    message(FATAL_ERROR "newlib mallinfo probe: ${CMAKE_C_COMPILER} -print-file-name=libc.a "
        "gave '${_axl_libc}' (exit ${_axl_rc}) -- not an existing libc.a. ${_axl_err}")
endif()
# A toolchain rebuilt in place at the same path re-probes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_axl_libc}")
execute_process(COMMAND ${AXL_BINUTILS_PREFIX}nm -S --defined-only ${_axl_libc}
                OUTPUT_VARIABLE _axl_nm RESULT_VARIABLE _axl_rc ERROR_VARIABLE _axl_err)
if(NOT _axl_rc EQUAL 0)
    message(FATAL_ERROR "newlib mallinfo probe: ${AXL_BINUTILS_PREFIX}nm on ${_axl_libc} "
        "failed (exit ${_axl_rc}): ${_axl_err}")
endif()
string(REGEX MATCH "[0-9a-fA-F]+ ([0-9a-fA-F]+) [A-Za-z] __malloc_current_mallinfo(\n|$)" _ "${_axl_nm}")
if("${CMAKE_MATCH_1}" STREQUAL "")
    message(FATAL_ERROR "newlib mallinfo probe: ${_axl_libc} defines no "
        "__malloc_current_mallinfo with a size -- not the newlib this build displaces")
endif()
math(EXPR _axl_mi_size "0x${CMAKE_MATCH_1}")
set(AXL_NEWLIB_MALLINFO_DEF "")
if(_axl_mi_size EQUAL 40)
    set(AXL_NEWLIB_MALLINFO_DEF AXL_NEWLIB_MALLINFO_INT=1)
    list(APPEND AXL_CFLAGS_BASE -DAXL_NEWLIB_MALLINFO_INT=1)
elseif(NOT _axl_mi_size EQUAL 80)
    message(FATAL_ERROR "newlib mallinfo probe: __malloc_current_mallinfo is "
        "${_axl_mi_size} bytes in ${_axl_libc}; only 40 (ten int) and 80 (ten size_t) are known")
endif()
message(STATUS "newlib mallinfo: ${_axl_mi_size} bytes (${_axl_libc})")

# Shared by C, C++ and assembly: what the code is built FOR.
add_library(axl_target_flags INTERFACE)
target_compile_options(axl_target_flags INTERFACE
    -fshort-wchar -fno-builtin -fno-omit-frame-pointer -fpic
    ${AXL_GCC_ARCH} ${AXL_STACK_PROTECTOR} -Wall
    # CMake names sources by ABSOLUTE path, and __FILE__ is in every image
    # (axl_malloc records its caller's): mapped, the strings are repo-relative
    # (as the Makefile's were), and no builder's directory leaks into a binary.
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}/=)
target_compile_definitions(axl_target_flags INTERFACE AXL_BACKEND_NATIVE
    ${AXL_NEWLIB_MALLINFO_DEF})

# The configuration (Debug = the default, Release = BUILD=RELEASE).
add_library(axl_config_flags INTERFACE)
# C and C++ only: assembly (.S/.s) gets CFLAGS_BASE alone, which carries
# none of these.
target_compile_options(axl_config_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C,CXX>:$<IF:$<CONFIG:Release>,-Os,-Og>
                                -g -gdwarf -ffunction-sections -fdata-sections>)
target_compile_definitions(axl_config_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C,CXX>:$<IF:$<CONFIG:Release>,NDEBUG,AXL_MEM_DEBUG>>)

# C: CFLAGS + INCLUDES, mbedTLS included (it is unconditional). Assembly gets
# CFLAGS_BASE exactly -- the C base options without the configuration, the
# TLS block or any include path -- so an assembly source in any target
# compiles as the Makefile compiled one (a generated blob in libaxl, for one).
add_library(axl_flags INTERFACE)
target_link_libraries(axl_flags INTERFACE axl_target_flags axl_config_flags)
target_compile_options(axl_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C,ASM>:-std=gnu2x -ffreestanding -fno-math-errno -fno-trapping-math>
    $<$<AND:$<COMPILE_LANGUAGE:C,ASM>,$<NOT:$<STREQUAL:${AXL_ARCH},aa64>>>:-fno-asynchronous-unwind-tables>
    $<$<COMPILE_LANGUAGE:C>:-U_WIN32 -U_WIN64>)
target_compile_definitions(axl_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C,ASM>:AXL_ALLOW_UEFI>
    $<$<COMPILE_LANGUAGE:C>:AXL_HAVE_TLS MBEDTLS_CONFIG_FILE="axl-mbedtls-config.h">)
# -isystem for the vendored mbedTLS headers: third-party code lint must not
# treat as ours (-isystem silences its diagnostics). -Ideps/lzma is NOT here:
# its errno.h shadows newlib's, so it is added per source below.
target_include_directories(axl_flags SYSTEM INTERFACE
    $<$<COMPILE_LANGUAGE:C>:${CMAKE_SOURCE_DIR}/deps/mbedtls/include>)
target_include_directories(axl_flags INTERFACE
    $<$<COMPILE_LANGUAGE:C>:${CMAKE_SOURCE_DIR}/src/net>
    $<$<COMPILE_LANGUAGE:C,CXX>:${CMAKE_SOURCE_DIR}/include>
    $<$<COMPILE_LANGUAGE:C,CXX>:${CMAKE_SOURCE_DIR}/src/backend>
    $<$<COMPILE_LANGUAGE:C,CXX>:${CMAKE_SOURCE_DIR}/deps/libvterm/include>)
# C++: CXXFLAGS -- exceptions and RTTI OFF by default, as the Makefile's were.
# A target that implements them (the terminate handler) sets AXL_CXX_EH, which
# REMOVES the pair rather than adding -fexceptions after it, as the Makefile's
# CXXFLAGS_EH filtered them out. Its own target so that a TU the build
# compiles with CXXFLAGS and NO include path (hello-minimal.cpp) can have it
# without axl_flags' includes.
add_library(axl_cxx_flags INTERFACE)
target_link_libraries(axl_flags INTERFACE axl_cxx_flags)
target_compile_options(axl_cxx_flags INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++23 -fno-threadsafe-statics>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<BOOL:$<TARGET_PROPERTY:AXL_CXX_EH>>>>:-fno-exceptions -fno-rtti>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<BOOL:$<TARGET_PROPERTY:AXL_CXX_EH>>>:-fexceptions -frtti>)
