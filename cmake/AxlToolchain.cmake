# AxlToolchain.cmake -- the bare-metal cross toolchain that builds the SDK.
#
# Reads scripts/axl-toolchains.conf, the file axl-cc and the installer
# share (its KEY=VALUE lines are valid sh and, parsed here, CMake), so the
# compiler is chosen in one place. AXL_ARCH (x64 | aa64)
# picks the arch; AXL_X64_GCC / AXL_X64_GXX / AXL_X64_BINUTILS_PREFIX
# (AXL_AA64_* on aa64), in the environment, override the conf's defaults as
# they do for axl-cc.
#
# CMAKE_SYSTEM_NAME Generic: there is no OS and no libc to link against at
# configure time. CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY is required:
# without it CMake's compiler check LINKS a test program, and a UEFI image has
# no default linker script to link one with.

if(NOT AXL_ARCH)
    set(AXL_ARCH x64)
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES AXL_ARCH)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../scripts/axl-toolchains.conf" _axl_conf
     REGEX "^AXL_[A-Z0-9_]+=")
foreach(_line IN LISTS _axl_conf)
    string(REGEX MATCH "^([A-Z0-9_]+)=(.*)$" _ "${_line}")
    set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()

if(AXL_ARCH STREQUAL "x64")
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
    set(_axl_pfx AXL_X64)
elseif(AXL_ARCH STREQUAL "aa64")
    set(CMAKE_SYSTEM_PROCESSOR aarch64)
    set(_axl_pfx AXL_AA64)
else()
    message(FATAL_ERROR "AXL_ARCH must be x64 or aa64, not '${AXL_ARCH}'")
endif()

# AXL_TOOLCHAIN (environment) -- WHICH toolchain, beside the locators: `axl`
# (the default, the toolchains scripts/install-toolchain.sh installs), `auto`
# (the same thing here: axl-cc's x64 default, so a consumer exporting it can
# still build the SDK), or `cross` (one you supply, named by every locator:
# the conf defaults are NOT consulted, so a forgotten one is refused by name
# rather than silently filled from /opt; for the host's own binutils name
# its directory, e.g. AXL_X64_BINUTILS_PREFIX=/usr/bin/). `host` is refused
# with the reason: it governs what a CONSUMER compiles with axl-cc. The same
# variant axl-cc and install.sh take (test-toolchain-variant.sh holds all
# three).
#
# The locators are spelled out, never built from a prefix: scripts/axl-build.sh
# fingerprints the environment by the $ENV{NAME}s it finds in cmake/, and a
# composed name is one it cannot see.
if(AXL_ARCH STREQUAL "x64")
    set(_axl_env_GCC "$ENV{AXL_X64_GCC}")
    set(_axl_env_GXX "$ENV{AXL_X64_GXX}")
    set(_axl_env_BINUTILS_PREFIX "$ENV{AXL_X64_BINUTILS_PREFIX}")
else()
    set(_axl_env_GCC "$ENV{AXL_AA64_GCC}")
    set(_axl_env_GXX "$ENV{AXL_AA64_GXX}")
    set(_axl_env_BINUTILS_PREFIX "$ENV{AXL_AA64_BINUTILS_PREFIX}")
endif()
set(_axl_env_variant "$ENV{AXL_TOOLCHAIN}")
if(_axl_env_variant STREQUAL "auto")
    set(_axl_env_variant axl)
endif()
if(_axl_env_variant STREQUAL "host")
    message(FATAL_ERROR "AXL_TOOLCHAIN=host does not build the SDK. It governs what a "
        "CONSUMER compiles with axl-cc; the SDK builds ITSELF with the bare-metal "
        "toolchain on both arches (docs/AXL-Host-Toolchain-Design.md section 8). "
        "Use `axl` here -- or `auto`, which means `axl` for this build -- and set "
        "AXL_TOOLCHAIN=host on axl-cc when you compile against the result.")
elseif(NOT _axl_env_variant MATCHES "^(axl|cross|)$")
    message(FATAL_ERROR "AXL_TOOLCHAIN=${_axl_env_variant} is not a toolchain variant. Use "
        "`axl` (the default: the toolchains scripts/install-toolchain.sh installs), "
        "`auto` (the same thing here, and axl-cc's x64 default) or `cross` (one you "
        "supply, named by ${_axl_pfx}_GCC, ${_axl_pfx}_GXX and ${_axl_pfx}_BINUTILS_PREFIX).")
endif()

# ONE TOOLCHAIN PER TREE. CMake fixes the compiler at a tree's first
# configure, while this file runs again at every reconfigure -- including the
# one Ninja starts by itself after a CMakeLists.txt edit, in whatever
# environment the build was started from. Re-reading the locators there gave
# a tree two toolchains: a `cross` tree regenerated without its variables
# kept its compiler but linked with the /opt binutils. So what the first
# configure resolved is kept in the cache (AXL_TC_*: the variant, and each
# locator with where it came from, `env` or `default`) and used from then on.
# A later configure is refused when its environment NAMES a different value,
# or when a locator that came from the conf default no longer matches it (a
# toolchain bump): the tree must be configured afresh (`cmake --fresh`, or
# delete it), as the retired Makefile's build-state signature wiped its
# objects. The entries are try_compile platform variables, so a check project
# compiles with the tree's toolchain, not whatever the environment says then.
set(_axl_kinds GCC GXX BINUTILS_PREFIX)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES AXL_TC_VARIANT
     AXL_TC_GCC AXL_TC_GXX AXL_TC_BINUTILS_PREFIX
     AXL_TC_GCC_FROM AXL_TC_GXX_FROM AXL_TC_BINUTILS_PREFIX_FROM)
if(DEFINED AXL_TC_VARIANT)
    set(_axl_variant "${AXL_TC_VARIANT}")
    set(_axl_conflicts "")
    if(NOT _axl_env_variant STREQUAL "" AND NOT _axl_env_variant STREQUAL _axl_variant)
        list(APPEND _axl_conflicts "AXL_TOOLCHAIN=${_axl_env_variant}, but this tree was configured as ${_axl_variant}")
    endif()
    foreach(_kind IN LISTS _axl_kinds)
        set(_axl_${_kind} "${AXL_TC_${_kind}}")
        if(NOT _axl_env_${_kind} STREQUAL "" AND NOT _axl_env_${_kind} STREQUAL _axl_${_kind})
            list(APPEND _axl_conflicts "${_axl_pfx}_${_kind}=${_axl_env_${_kind}}, but this tree was configured with ${_axl_${_kind}}")
        elseif(_axl_env_${_kind} STREQUAL "" AND AXL_TC_${_kind}_FROM STREQUAL "default"
               AND NOT "${${_axl_pfx}_${_kind}_DEFAULT}" STREQUAL _axl_${_kind})
            list(APPEND _axl_conflicts "scripts/axl-toolchains.conf now names ${${_axl_pfx}_${_kind}_DEFAULT} for ${_axl_pfx}_${_kind}, but this tree was configured with ${_axl_${_kind}}")
        endif()
    endforeach()
    if(_axl_conflicts)
        list(JOIN _axl_conflicts "; " _axl_conflicts)
        message(FATAL_ERROR "${_axl_conflicts}. A tree keeps the toolchain it was "
            "configured with; to change it, configure it afresh: cmake --fresh "
            "--preset <preset> (or delete ${CMAKE_BINARY_DIR}).")
    endif()
else()
    set(_axl_variant "${_axl_env_variant}")
    if(_axl_variant STREQUAL "")
        set(_axl_variant axl)
    endif()
    foreach(_kind IN LISTS _axl_kinds)
        if(NOT _axl_env_${_kind} STREQUAL "")
            set(_axl_${_kind} "${_axl_env_${_kind}}")
            set(_axl_from env)
        elseif(_axl_variant STREQUAL "cross")
            message(FATAL_ERROR "AXL_TOOLCHAIN=cross needs ${_axl_pfx}_${_kind} set to your own "
                "toolchain's: this variant never falls back to scripts/axl-toolchains.conf. "
                "Set ${_axl_pfx}_GCC, ${_axl_pfx}_GXX and ${_axl_pfx}_BINUTILS_PREFIX, or unset "
                "AXL_TOOLCHAIN for the default toolchains.")
        else()
            set(_axl_${_kind} "${${_axl_pfx}_${_kind}_DEFAULT}")
            set(_axl_from default)
        endif()
        set(AXL_TC_${_kind} "${_axl_${_kind}}" CACHE INTERNAL "cmake/AxlToolchain.cmake")
        set(AXL_TC_${_kind}_FROM "${_axl_from}" CACHE INTERNAL "cmake/AxlToolchain.cmake")
    endforeach()
    set(AXL_TC_VARIANT "${_axl_variant}" CACHE INTERNAL "cmake/AxlToolchain.cmake")
endif()

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER   "${_axl_GCC}")
set(CMAKE_CXX_COMPILER "${_axl_GXX}")
set(CMAKE_ASM_COMPILER "${_axl_GCC}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# ld and objcopy for the image pipeline (cmake/AxlImages.cmake), from the same
# conf entry axl-cc's $CROSS comes from.
set(AXL_BINUTILS_PREFIX "${_axl_BINUTILS_PREFIX}")
