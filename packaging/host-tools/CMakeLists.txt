# packaging/host-tools -- the host-tools archive's install rules
# (axl-sdk-host-tools-<v>.tar.gz; docs/AXL-Build-System-Design.md §10.16).
#
# A project of its own with NO languages: the archive is shell and Python, so
# building it must not need the bare-metal toolchain the main project's
# configure checks for. scripts/make-release-assets.sh configures it into a
# scratch directory of its own and packs component host_tools.
#
# WHO IT IS FOR: a CI job that only needs run-qemu should not pull 182
# headers to get it (AXL-Distribution-Design.md §14.2). It is the MANAGER
# (§20): it carries the installer its verbs exec, and `axl prune` never
# removes the current one.
cmake_minimum_required(VERSION 3.29)
project(axl_host_tools NONE)

get_filename_component(AXL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(STRINGS ${AXL_ROOT}/VERSION AXL_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${AXL_ROOT}/VERSION)
set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

include(${AXL_ROOT}/cmake/AxlInstallHostTools.cmake)
include(${AXL_ROOT}/cmake/AxlQemuDeps.cmake)

# bin/axl + libexec/axl/: the SAME shape as the SDK prefix, so `axl <cmd>`
# works from an extracted archive as from an installed SDK.
axl_install_host_tools(host_tools)

# `axl --print-version`, axl_version.py and install.sh's already_installed
# check read share/axl/version; VERSION at the root is for a human.
file(WRITE ${CMAKE_BINARY_DIR}/version "${AXL_VERSION}\n")
install(FILES ${CMAKE_BINARY_DIR}/version DESTINATION share/axl COMPONENT host_tools)
install(FILES ${CMAKE_BINARY_DIR}/version DESTINATION . RENAME VERSION COMPONENT host_tools)
install(FILES ${AXL_ROOT}/LICENSE ${AXL_ROOT}/NOTICE ${AXL_ROOT}/CHANGELOG.md
    DESTINATION . COMPONENT host_tools)
configure_file(${AXL_ROOT}/cmake/host-tools-README.md.in ${CMAKE_BINARY_DIR}/README.md @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/README.md DESTINATION . COMPONENT host_tools)

set(_axl_ver "${AXL_VERSION}")
set(AXL_CMAKE_DIR "${AXL_ROOT}/cmake")
configure_file(${AXL_ROOT}/cmake/CPackConfig.cmake.in
    ${CMAKE_BINARY_DIR}/CPackConfig-release.cmake @ONLY)
