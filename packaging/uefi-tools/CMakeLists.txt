# packaging/uefi-tools -- the UEFI-tools archive's install rules
# (axl-sdk-uefi-tools-<v>-<arch>.tar.gz; docs/AXL-Build-System-Design.md §10.16).
#
# A project of its own with NO languages, configured by
# scripts/make-release-assets.sh with
#   -DAXL_ARCH=x64|aa64  -DAXL_IMAGE_TREE=<the arch's built RELEASE tree>
# The archive ships IMAGES another tree built; reading them from a named tree
# (rather than this project's own) is what lets the packing run against any
# tree the build wrapper names -- test-make-test-release.sh's stub tree
# included -- and needs no toolchain.
#
# The version in VERSION and README.txt is $ENV{AXL_RELEASE_VERSION} when set
# (a test pre-release names its own), else VERSION's; sshd-rtk.efi and
# sshd-full-rtk.efi come from $ENV{AXL_RTK_OUT} when set (--with-rtk). Every
# `uefi-tools: ` message is relayed by make-release-assets.sh.
cmake_minimum_required(VERSION 3.29)
project(axl_uefi_tools NONE)

get_filename_component(AXL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
foreach(_v AXL_ARCH AXL_IMAGE_TREE)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "uefi-tools: ${_v} is required (scripts/make-release-assets.sh sets it)")
    endif()
endforeach()
if(NOT AXL_ARCH MATCHES "^(x64|aa64)$")
    message(FATAL_ERROR "uefi-tools: AXL_ARCH must be x64 or aa64, not '${AXL_ARCH}'")
endif()
file(STRINGS ${AXL_ROOT}/VERSION AXL_VERSION LIMIT_COUNT 1)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# SHIPPED_TOOL_NAMES (scripts/axl-lists.conf), through the one reader.
execute_process(COMMAND bash ${AXL_ROOT}/scripts/axl-list.sh SHIPPED_TOOL_NAMES
    OUTPUT_VARIABLE _axl_shipped_tools ERROR_VARIABLE _err RESULT_VARIABLE _rc
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _rc EQUAL 0 OR _axl_shipped_tools STREQUAL "")
    message(FATAL_ERROR "uefi-tools: could not read SHIPPED_TOOL_NAMES (${_rc}): ${_err}")
endif()
separate_arguments(_axl_shipped_tools UNIX_COMMAND "${_axl_shipped_tools}")

install(CODE "
    # What the install has to SAY (a tool not shipped, one missing) goes to a
    # notes file make-release-assets.sh relays: CPack does not print an
    # install script's messages.
    set(_axl_notes \"${CMAKE_BINARY_DIR}/uefi-tools-notes.txt\")
    file(WRITE \"\${_axl_notes}\" \"\")
    set(_axl_v \"\$ENV{AXL_RELEASE_VERSION}\")
    if(_axl_v STREQUAL \"\")
        set(_axl_v \"${AXL_VERSION}\")
    endif()
    set(_axl_dst \"\${CMAKE_INSTALL_PREFIX}\")
    # The tools ship 0755, as the linker wrote them and every release has
    # carried them; the data beside them 0644.
    set(_axl_exe OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(_axl_ship ${_axl_shipped_tools})
    # A guard that checks nothing passes forever: a suspiciously short list
    # is refused, as a tool missing from the tree is.
    list(LENGTH _axl_ship _axl_n)
    if(_axl_n LESS 30)
        file(APPEND \"\${_axl_notes}\" \"SHIPPED_TOOL_NAMES named only \${_axl_n} tools; refusing to certify the archive against that\\n\")
        message(FATAL_ERROR \"uefi-tools: SHIPPED_TOOL_NAMES named only \${_axl_n} tools; refusing to certify the archive against that\")
    endif()
    # BY NAME from SHIPPED_TOOL_NAMES, never a glob over tools/: a stale or
    # hand-copied .efi in the tree must not ship. Each unlisted one is named.
    foreach(_axl_t IN LISTS _axl_ship)
        if(NOT EXISTS \"${AXL_IMAGE_TREE}/tools/\${_axl_t}.efi\")
            file(APPEND \"\${_axl_notes}\" \"MISSING: \${_axl_t}.efi\\n\")
            message(FATAL_ERROR \"uefi-tools: MISSING: \${_axl_t}.efi\")
        endif()
        file(INSTALL DESTINATION \"\${_axl_dst}\" TYPE FILE FILE_PERMISSIONS \${_axl_exe}
             FILES \"${AXL_IMAGE_TREE}/tools/\${_axl_t}.efi\")
    endforeach()
    file(GLOB _axl_all \"${AXL_IMAGE_TREE}/tools/*.efi\")
    list(SORT _axl_all)
    foreach(_axl_f IN LISTS _axl_all)
        get_filename_component(_axl_t \"\${_axl_f}\" NAME_WE)
        if(_axl_t MATCHES \"^sshd(-full)?-rtk\$\")
            if(\"\$ENV{AXL_RTK_OUT}\" STREQUAL \"\")
                file(APPEND \"\${_axl_notes}\" \"not shipping \${_axl_t}.efi: it embeds Realtek's driver (give --with-rtk)\\n\")
            endif()
        elseif(NOT _axl_t IN_LIST _axl_ship)
            file(APPEND \"\${_axl_notes}\" \"not shipping \${_axl_t}.efi: it is not in SHIPPED_TOOL_NAMES (a stale build output?)\\n\")
        endif()
    endforeach()
    # sshd-rtk.efi / sshd-full-rtk.efi only when asked, from the directory
    # that build wrote: each embeds Realtek's driver, which this tree does
    # not carry.
    if(NOT \"\$ENV{AXL_RTK_OUT}\" STREQUAL \"\")
        foreach(_axl_t sshd-rtk sshd-full-rtk)
            file(INSTALL DESTINATION \"\${_axl_dst}\" TYPE FILE FILE_PERMISSIONS \${_axl_exe}
                 FILES \"\$ENV{AXL_RTK_OUT}/\${_axl_t}.efi\")
        endforeach()
    endif()
    # EVERY sidecar in share/, BY GLOB (share/*.json5) -- the list that stood in release.yml
    # shipped lsusb and memspd without their databases through v4.6.0. At
    # least three, or share/ losing two of them would pass.
    file(GLOB _axl_sidecars \"${AXL_ROOT}/share/*.json5\")
    list(LENGTH _axl_sidecars _axl_n)
    if(_axl_n LESS 3)
        file(APPEND \"\${_axl_notes}\" \"share/ yielded only \${_axl_n} *.json5 sidecar(s); refusing\\n\")
        message(FATAL_ERROR \"uefi-tools: share/ yielded only \${_axl_n} *.json5 sidecar(s); refusing\")
    endif()
    file(INSTALL DESTINATION \"\${_axl_dst}\" TYPE FILE FILES \${_axl_sidecars})
    # sshd.conf beside sshd.efi, where sshd reads it.
    file(INSTALL DESTINATION \"\${_axl_dst}\" TYPE FILE
         FILES \"${AXL_ROOT}/tools/sshd.conf\")
    # drivers/<arch>/: the layout axl_driver_locate searches.
    foreach(_axl_d NetworkCommon UsbCdcEcm UsbCdcNcm UsbRndis RamDiskDxe)
        file(INSTALL DESTINATION \"\${_axl_dst}/drivers/${AXL_ARCH}\" TYPE FILE
             FILES \"${AXL_IMAGE_TREE}/release-drivers/${AXL_ARCH}/\${_axl_d}.efi\")
    endforeach()
    file(INSTALL DESTINATION \"\${_axl_dst}/third_party/mbedtls\" TYPE FILE
         FILES \"${AXL_ROOT}/deps/mbedtls/LICENSE\")
    file(INSTALL DESTINATION \"\${_axl_dst}/third_party/edk2\" TYPE FILE
         FILES \"${AXL_ROOT}/third_party/edk2/LICENSE\"
               \"${AXL_ROOT}/third_party/edk2/README.md\")
    # VERSION and the GENERATED README: scripts/make-uefi-tools-readme.py,
    # the tool list from TOOL_NAMES and devkit.conf's desc: lines
    # (test-uefi-tools-readme.sh runs the same generator; check-tool-docs
    # holds this call in place).
    set(_axl_gen \"${CMAKE_BINARY_DIR}/install/uefi-tools\")
    file(MAKE_DIRECTORY \"\${_axl_gen}\")
    file(WRITE \"\${_axl_gen}/VERSION\" \"\${_axl_v}\\n\")
    execute_process(
        COMMAND \"${Python3_EXECUTABLE}\" \"${AXL_ROOT}/scripts/make-uefi-tools-readme.py\"
                --arch ${AXL_ARCH} --version \"\${_axl_v}\"
        OUTPUT_FILE \"\${_axl_gen}/README.txt\" RESULT_VARIABLE _axl_rc)
    if(NOT _axl_rc EQUAL 0)
        file(APPEND \"\${_axl_notes}\" \"make-uefi-tools-readme.py failed (\${_axl_rc})\\n\")
        message(FATAL_ERROR \"uefi-tools: make-uefi-tools-readme.py failed (\${_axl_rc})\")
    endif()
    if(NOT \"\$ENV{AXL_RTK_OUT}\" STREQUAL \"\")
        file(READ \"${AXL_ROOT}/cmake/uefi-tools-README-rtk.txt\" _axl_rtk)
        file(APPEND \"\${_axl_gen}/README.txt\" \"\${_axl_rtk}\")
    endif()
    file(INSTALL DESTINATION \"\${_axl_dst}\" TYPE FILE
         FILES \"\${_axl_gen}/VERSION\" \"\${_axl_gen}/README.txt\")
    " COMPONENT uefi_tools)


set(_axl_ver "${AXL_VERSION}")
set(AXL_CMAKE_DIR "${AXL_ROOT}/cmake")
configure_file(${AXL_ROOT}/cmake/CPackConfig.cmake.in
    ${CMAKE_BINARY_DIR}/CPackConfig-release.cmake @ONLY)
