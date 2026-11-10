# AxlPackDeb.cmake -- CPACK_EXTERNAL_PACKAGE_SCRIPT for the .deb
# (AXL-Distribution-Design.md §17.5; Build-System-Design §10.18).
#
# WHY NOT CPack's DEB generator. Measured: its packages were byte-identical
# across two fresh ext4 filesystems but NOT between ext4 and XFS -- its
# control file's Installed-Size sums cmSystemTools::FileLength over every
# staged path, directories included, and a directory's st_size is the
# filesystem's (4096 on ext4, tens of bytes on XFS). So the same commit gave
# one .deb on a GitHub runner and another on this host. `cpack -G External`
# stages the components exactly as before, and this script builds the
# package with the distribution's own tool:
#
#   dpkg-deb --root-owner-group -Zgzip --build <staging> <file>.deb
#
# dpkg-deb writes the data in NAME order; every staged mtime is set to
# SOURCE_DATE_EPOCH first (make-release-assets.sh sets it from the commit);
# --root-owner-group (dpkg 1.19.0+) records root:root without fakeroot. The
# control file is written here from the CPACK_DEBIAN_* settings of
# packaging/system/CPackConfig-package.cmake.in, with an Installed-Size that
# depends on the files alone: the sum of each regular file's size in KiB,
# rounded up per file, plus one per directory -- dpkg-gencontrol's rule.

foreach(_v CPACK_TEMPORARY_DIRECTORY CPACK_TOPLEVEL_DIRECTORY CPACK_DEBIAN_FILE_NAME
           CPACK_DEBIAN_PACKAGE_NAME CPACK_PACKAGE_VERSION CPACK_DEBIAN_PACKAGE_RELEASE
           CPACK_DEBIAN_PACKAGE_ARCHITECTURE CPACK_PACKAGE_CONTACT)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "AxlPackDeb: ${_v} is not set (run scripts/make-release-assets.sh deb)")
    endif()
endforeach()
if("$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
    message(FATAL_ERROR "AxlPackDeb: SOURCE_DATE_EPOCH is not set -- the package's mtimes would be the build's")
endif()
find_program(_axl_dpkg_deb dpkg-deb)
if(NOT _axl_dpkg_deb)
    message(FATAL_ERROR "AxlPackDeb: dpkg-deb not found (apt install dpkg / dnf install dpkg)")
endif()

set(_stage "${CPACK_TEMPORARY_DIRECTORY}")
# Installed-Size, from the files alone.
file(GLOB_RECURSE _paths LIST_DIRECTORIES true RELATIVE "${_stage}" "${_stage}/*")
set(_kib 0)
foreach(_p IN LISTS _paths)
    if(_p MATCHES "^DEBIAN(/|$)")
        continue()
    elseif(IS_DIRECTORY "${_stage}/${_p}" AND NOT IS_SYMLINK "${_stage}/${_p}")
        math(EXPR _kib "${_kib} + 1")
    else()
        file(SIZE "${_stage}/${_p}" _sz)
        math(EXPR _kib "${_kib} + (${_sz} + 1023) / 1024")
    endif()
endforeach()

set(_ctl "Package: ${CPACK_DEBIAN_PACKAGE_NAME}\n")
string(APPEND _ctl "Version: ${CPACK_PACKAGE_VERSION}-${CPACK_DEBIAN_PACKAGE_RELEASE}\n")
string(APPEND _ctl "Architecture: ${CPACK_DEBIAN_PACKAGE_ARCHITECTURE}\n")
string(APPEND _ctl "Maintainer: ${CPACK_PACKAGE_CONTACT}\n")
string(APPEND _ctl "Installed-Size: ${_kib}\n")
foreach(_f DEPENDS:Depends RECOMMENDS:Recommends CONFLICTS:Conflicts REPLACES:Replaces
           SECTION:Section HOMEPAGE:Homepage)
    string(REPLACE ":" ";" _f "${_f}")
    list(GET _f 0 _var)
    list(GET _f 1 _field)
    if(NOT "${CPACK_DEBIAN_PACKAGE_${_var}}" STREQUAL "")
        string(APPEND _ctl "${_field}: ${CPACK_DEBIAN_PACKAGE_${_var}}\n")
    endif()
endforeach()
string(APPEND _ctl "Priority: optional\n")
string(APPEND _ctl "Description: ${CPACK_PACKAGE_DESCRIPTION_SUMMARY}\n")
string(APPEND _ctl " ${CPACK_PACKAGE_DESCRIPTION}\n")

file(REMOVE_RECURSE "${_stage}/DEBIAN")
file(MAKE_DIRECTORY "${_stage}/DEBIAN")
file(WRITE "${_stage}/DEBIAN/control" "${_ctl}")
foreach(_s IN LISTS CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA)
    file(INSTALL DESTINATION "${_stage}/DEBIAN" TYPE FILE
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                          WORLD_READ WORLD_EXECUTE
         FILES "${_s}")
endforeach()

# EVERY mtime the commit's. dpkg-deb only CLAMPS to SOURCE_DATE_EPOCH -- an
# older file keeps its own mtime, which is the checkout's, so two clones
# would give two packages (measured: VERSION kept the checkout's time).
execute_process(
    COMMAND find "${_stage}" -exec touch -h -d "@$ENV{SOURCE_DATE_EPOCH}" {} +
    RESULT_VARIABLE _rc ERROR_VARIABLE _e)
if(NOT _rc STREQUAL "0")
    message(FATAL_ERROR "AxlPackDeb: could not set the staged mtimes (${_rc}): ${_e}")
endif()

set(_out "${CPACK_TOPLEVEL_DIRECTORY}/${CPACK_DEBIAN_FILE_NAME}")
execute_process(
    COMMAND "${_axl_dpkg_deb}" --root-owner-group -Zgzip -z9 --build "${_stage}" "${_out}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _e)
if(NOT _rc STREQUAL "0")
    message(FATAL_ERROR "AxlPackDeb: dpkg-deb failed (${_rc}): ${_o}${_e}")
endif()
set(CPACK_EXTERNAL_BUILT_PACKAGES "${_out}")
