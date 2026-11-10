# AxlInstall.cmake -- what an SDK prefix holds, as install() rules
# (docs/AXL-Build-System-Design.md §10.16).
#
# `cmake --install` of a built tree stages the SDK: scripts/install.sh runs it
# for a prefix, and CPack (cmake/AxlPackage.cmake) for the release archives,
# so the two cannot disagree about a file. These rules were install.sh's own
# copy commands until 2026-11-10, moved with their modes and destinations
# unchanged; scripts/check-install-parity.sh compared the result with the
# copy-based script's, file for file.
#
# FOUR COMPONENTS, because an SDK prefix holds both arches and one tree builds
# one. Each file belongs to exactly one, so no file is installed twice when
# both trees install into one prefix:
#
#   sdk_arch    this arch's libraries, runtime and pack-stub objects and its
#               axl-<arch>.pc -- from EVERY arch's tree
#   sdk_cxxrt   this arch's C++ runtime glue objects -- from every arch's
#               tree, unless the install is C-only (install.sh --no-cpp)
#   sdk_host    bin/pe-set-debug, bin/axl-efi-pack and the unsuffixed axl.pc
#               -- from ONE tree, the last arch installed (install.sh's
#               "axl.pc reflects the last arch built")
#   sdk_common  everything arch-neutral: headers, linker scripts, the CMake
#               package, axl-cc, the `axl` dispatcher and its host tools,
#               metadata and sidecars -- from one tree
#
# Not here: the `current` link and the stale-prefix warning, which act on
# the DESTINATION rather than stage content, stay in install.sh.

include_guard(GLOBAL)

set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(_axl_libdir lib/axl/${AXL_ARCH})

# ---------------------------------------------------------------------------
# sdk_arch
# ---------------------------------------------------------------------------
install(FILES
        ${CMAKE_BINARY_DIR}/lib/libaxl.a
        # THE STAND-IN LIBC (AXL-Host-Toolchain-Design.md §5.3): weak leaf
        # functions that complete a link with no libc (AXL_TOOLCHAIN=host).
        ${CMAKE_BINARY_DIR}/lib/libaxl-standin.a
        ${AXL_HOST_DIR}/axl-crt0-native.o
        ${AXL_HOST_DIR}/axl-crt0-minimal.o
        ${AXL_HOST_DIR}/axl-crt0-gcc-${CMAKE_SYSTEM_PROCESSOR}.o
        ${AXL_HOST_DIR}/axl-reloc.o
        ${AXL_HOST_DIR}/axl-debug-info.o
    DESTINATION ${_axl_libdir} COMPONENT sdk_arch)
# THE AXL-PACK STUB for `axl-cc --compress`, prebuilt per arch (the stub-only
# format variant's build lives here, and a consumer compiling it would be a
# fourth copy of that recipe for check-flag-parity to police). Under its own
# pack/ so the vendored LZMA SDK's generic names (Bra.o) cannot be mistaken
# for AXL objects.
install(FILES
        ${AXL_HOST_DIR}/axl-pack-stub.o
        ${AXL_HOST_DIR}/axl-pack-format.o
        ${AXL_HOST_DIR}/axl-backend-native-lines.o
        ${AXL_HOST_DIR}/LzmaDec.o
        ${AXL_HOST_DIR}/Bra.o
        ${AXL_HOST_DIR}/Bra86.o
    DESTINATION ${_axl_libdir}/pack COMPONENT sdk_arch)

# pkg-config: relocatable through ${pcfiledir}.
set(AXL_VERSION "${_axl_ver}")
configure_file(${CMAKE_SOURCE_DIR}/cmake/axl.pc.in
    ${CMAKE_BINARY_DIR}/install/axl-${AXL_ARCH}.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/install/axl-${AXL_ARCH}.pc
    DESTINATION lib/pkgconfig COMPONENT sdk_arch)

# ---------------------------------------------------------------------------
# sdk_cxxrt -- THE C++ RUNTIME GLUE, as OBJECTS rather than an archive: the
# member each displaces (newlib's sbrk, libstdc++'s verbose terminate handler)
# would otherwise be pulled for another symbol and multiply-define. axl-cc
# names them individually on every C++ link; -nothrow is the --no-eh-frame
# alternative to -eh, so both ship and the consumer chooses.
# ---------------------------------------------------------------------------
install(FILES
        ${AXL_HOST_DIR}/axl-cxxrt-alloc.o
        ${AXL_HOST_DIR}/axl-cxxrt-eh.o
        ${AXL_HOST_DIR}/axl-cxxrt-nothrow.o
        ${AXL_HOST_DIR}/axl-cxxrt-stubs.o
        ${AXL_HOST_DIR}/axl-cxxrt-terminate.o
    DESTINATION ${_axl_libdir} COMPONENT sdk_cxxrt)

# ---------------------------------------------------------------------------
# sdk_host
# ---------------------------------------------------------------------------
install(PROGRAMS ${AXL_PE_SET_DEBUG} ${AXL_EFI_PACK} DESTINATION bin COMPONENT sdk_host)
# The unsuffixed axl.pc, so `pkg-config axl` works without an arch suffix.
install(FILES ${CMAKE_BINARY_DIR}/install/axl-${AXL_ARCH}.pc
    DESTINATION lib/pkgconfig RENAME axl.pc COMPONENT sdk_host)

# ---------------------------------------------------------------------------
# sdk_common
# ---------------------------------------------------------------------------
# Headers under include/axl-sdk/, never include/ directly: on a PREFIX=/usr
# install `-isystem /usr/include` would shadow the cross gcc's freestanding
# <stdint.h> with glibc's. The C++ headers ship unconditionally: header-only,
# and an `#error` under C, so a C consumer pays nothing.
install(FILES ${CMAKE_SOURCE_DIR}/include/axl.h
    DESTINATION include/axl-sdk COMPONENT sdk_common)
# Recursive, where the copies were one level (`include/axl/*.h`): the same
# set while include/axl/ has no subdirectory (include/uefi/'s one,
# generated/, was copied explicitly).
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/axl/ DESTINATION include/axl-sdk/axl
    COMPONENT sdk_common FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/uefi/ DESTINATION include/axl-sdk/uefi
    COMPONENT sdk_common FILES_MATCHING PATTERN "*.h")

# Linker scripts (the _eh variants are what axl-cc picks under -fexceptions:
# their KEEP(*(.eh_frame)) costs a C-only image +16.8%) and the version script
# that localizes symbols so --gc-sections can shrink the image.
install(FILES
        ${CMAKE_SOURCE_DIR}/scripts/elf_x86_64_efi.lds
        ${CMAKE_SOURCE_DIR}/scripts/elf_aarch64_efi.lds
        ${CMAKE_SOURCE_DIR}/scripts/elf_x86_64_efi_eh.lds
        ${CMAKE_SOURCE_DIR}/scripts/elf_aarch64_efi_eh.lds
        ${CMAKE_SOURCE_DIR}/scripts/efi-localize.ver
    DESTINATION lib/axl COMPONENT sdk_common)

# Metadata axl-cc reads. build-date is the install's UTC day -- string(TIMESTAMP)
# takes SOURCE_DATE_EPOCH when it is set, so a release archive records its
# commit's day and two builds of one commit agree.
file(WRITE ${CMAKE_BINARY_DIR}/install/backend "native\n")
configure_file(${CMAKE_SOURCE_DIR}/VERSION ${CMAKE_BINARY_DIR}/install/version COPYONLY)
install(FILES ${CMAKE_BINARY_DIR}/install/backend ${CMAKE_BINARY_DIR}/install/version
    DESTINATION share/axl COMPONENT sdk_common)
install(CODE "
    string(TIMESTAMP _axl_day \"%Y-%m-%d\" UTC)
    file(WRITE \"${CMAKE_BINARY_DIR}/install/build-date\" \"\${_axl_day}\\n\")
    file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/share/axl\" TYPE FILE
         FILES \"${CMAKE_BINARY_DIR}/install/build-date\")
    " COMPONENT sdk_common)

# The JSON5 sidecars, BY GLOB at install time and never a list: a list in the
# tarball's staging once named the PCI files only, so v4.6.0 shipped lsusb and
# memspd without their databases (`gate.check-tools-sidecars`).
install(DIRECTORY ${CMAKE_SOURCE_DIR}/share/ DESTINATION share/axl
    COMPONENT sdk_common FILES_MATCHING PATTERN "*.json5")
# The bulk converters for the full pci.ids / usb.ids databases.
install(PROGRAMS
        ${CMAKE_SOURCE_DIR}/scripts/_ids_parser.py
        ${CMAKE_SOURCE_DIR}/scripts/pci-ids-to-json5.py
        ${CMAKE_SOURCE_DIR}/scripts/usb-ids-to-json5.py
    DESTINATION share/axl/scripts COMPONENT sdk_common)

# The CMake package: one template (cmake/axl-config.cmake.in) with the
# compilers substituted -- at INSTALL time, as install.sh's sed always did, so
# a conf bump or an override in the installing environment reaches it rather
# than a value frozen at the tree's first configure. This tree's own arch is
# what its configure recorded (AXL_TC_*: the compiler its libraries were
# built with); the other arch is resolved as the build resolves a locator --
# the environment's AXL_<ARCH>_* first, then scripts/axl-toolchains.conf's
# default, and under AXL_TOOLCHAIN=cross no default at all
# (cmake/AxlToolchain.cmake). The $ENV{} names are spelled out: axl-build.sh
# fingerprints the environment by the names it finds in cmake/.
if(AXL_ARCH STREQUAL "x64")
    set(_axl_own X64)
else()
    set(_axl_own AA64)
endif()
install(CODE "
    set(_axl_variant \"${AXL_TC_VARIANT}\")
    set(_axl_env_X64_GCC \"\$ENV{AXL_X64_GCC}\")
    set(_axl_env_X64_GXX \"\$ENV{AXL_X64_GXX}\")
    set(_axl_env_X64_BINUTILS_PREFIX \"\$ENV{AXL_X64_BINUTILS_PREFIX}\")
    set(_axl_env_AA64_GCC \"\$ENV{AXL_AA64_GCC}\")
    set(_axl_env_AA64_GXX \"\$ENV{AXL_AA64_GXX}\")
    set(_axl_env_AA64_BINUTILS_PREFIX \"\$ENV{AXL_AA64_BINUTILS_PREFIX}\")
    file(STRINGS \"${CMAKE_SOURCE_DIR}/scripts/axl-toolchains.conf\" _axl_conf
         REGEX \"^AXL_[A-Z0-9_]+=\")
    foreach(_axl_line IN LISTS _axl_conf)
        string(REGEX MATCH \"^([A-Z0-9_]+)=(.*)\$\" _ \"\${_axl_line}\")
        set(_axl_conf_\${CMAKE_MATCH_1} \"\${CMAKE_MATCH_2}\")
    endforeach()
    set(AXL_${_axl_own}_GCC \"${AXL_TC_GCC}\")
    set(AXL_${_axl_own}_GXX \"${AXL_TC_GXX}\")
    set(AXL_${_axl_own}_BINUTILS_PREFIX \"${AXL_TC_BINUTILS_PREFIX}\")
    foreach(_axl_a X64 AA64)
        if(_axl_a STREQUAL \"${_axl_own}\")
            continue()
        endif()
        foreach(_axl_k GCC GXX BINUTILS_PREFIX)
            if(NOT _axl_env_\${_axl_a}_\${_axl_k} STREQUAL \"\")
                set(AXL_\${_axl_a}_\${_axl_k} \"\${_axl_env_\${_axl_a}_\${_axl_k}}\")
            elseif(_axl_variant STREQUAL \"cross\")
                set(AXL_\${_axl_a}_\${_axl_k} \"\")
            else()
                set(AXL_\${_axl_a}_\${_axl_k} \"\${_axl_conf_AXL_\${_axl_a}_\${_axl_k}_DEFAULT}\")
            endif()
        endforeach()
    endforeach()
    configure_file(\"${CMAKE_SOURCE_DIR}/cmake/axl-config.cmake.in\"
        \"${CMAKE_BINARY_DIR}/install/axl-config.cmake\" @ONLY)
    file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/lib/cmake/axl\" TYPE FILE
         FILES \"${CMAKE_BINARY_DIR}/install/axl-config.cmake\")
    " COMPONENT sdk_common)
# The conf is read at configure time too (cmake/AxlToolchain.cmake): a bump
# must reconfigure, so the tree's toolchain-conflict check sees it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/scripts/axl-toolchains.conf)

# The compiler drivers. axl-c++ ships unconditionally: a 9-line wrapper whose
# absence would turn axl-cc's precise "C++ runtime missing" diagnostic into a
# shell's "command not found".
install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/axl-cc ${CMAKE_SOURCE_DIR}/scripts/axl-c++
    DESTINATION bin COMPONENT sdk_common)
# The toolchain manifest axl-cc reads, and the installer its "toolchain
# missing" diagnostic names -- shipped, or that remedy is a dead end outside a
# checkout.
install(FILES ${CMAKE_SOURCE_DIR}/scripts/axl-toolchains.conf
    DESTINATION share/axl COMPONENT sdk_common)
install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/install-toolchain.sh
    DESTINATION bin RENAME axl-install-toolchain COMPONENT sdk_common)

# The `axl` dispatcher, the host tools it fronts and the installer: the rule
# the host-tools archive shares (cmake/AxlInstallHostTools.cmake).
include(${CMAKE_SOURCE_DIR}/cmake/AxlInstallHostTools.cmake)
axl_install_host_tools(sdk_common)
