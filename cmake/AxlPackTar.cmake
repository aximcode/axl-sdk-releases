# AxlPackTar.cmake -- CPACK_EXTERNAL_PACKAGE_SCRIPT for the release archives
# (docs/AXL-Build-System-Design.md §10.18).
#
# Run by `cpack -G External` after it has staged the components into
# CPACK_TEMPORARY_DIRECTORY. Packs the ONE versioned root there
# (CPACK_PACKAGING_INSTALL_PREFIX) into CPACK_PACKAGE_FILE_NAME.tar.gz with
# the line the release's archives have always been made with, and reports it
# through CPACK_EXTERNAL_BUILT_PACKAGES so CPack copies it out as its own.
#
#   tar --sort=name --format=gnu --owner=0 --group=0 --numeric-owner
#       --mtime=@$SOURCE_DATE_EPOCH -cf - <root> | gzip -n
#
# Every property that makes two builds one archive is in that line, none in
# the builder: entries in NAME order (not the filesystem's readdir order,
# which on ext4 is a per-filesystem hash), numeric owner 0/0 with no names
# (`tar x` as root then cannot hand the files to a local user), the commit's
# mtime, and a gzip header without a file name or a timestamp.

foreach(_v CPACK_TEMPORARY_DIRECTORY CPACK_TOPLEVEL_DIRECTORY CPACK_PACKAGE_FILE_NAME
           CPACK_PACKAGING_INSTALL_PREFIX)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "AxlPackTar: ${_v} is not set (run scripts/make-release-assets.sh)")
    endif()
endforeach()
if("$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
    message(FATAL_ERROR "AxlPackTar: SOURCE_DATE_EPOCH is not set -- the archive's mtimes would be the build's")
endif()

string(REGEX REPLACE "^/+" "" _root "${CPACK_PACKAGING_INSTALL_PREFIX}")
if(_root STREQUAL "" OR _root MATCHES "/")
    message(FATAL_ERROR "AxlPackTar: the versioned root must be one directory, not '${CPACK_PACKAGING_INSTALL_PREFIX}'")
endif()
if(NOT IS_DIRECTORY "${CPACK_TEMPORARY_DIRECTORY}/${_root}")
    message(FATAL_ERROR "AxlPackTar: nothing was staged at ${CPACK_TEMPORARY_DIRECTORY}/${_root}")
endif()

set(_out "${CPACK_TOPLEVEL_DIRECTORY}/${CPACK_PACKAGE_FILE_NAME}.tar.gz")
execute_process(
    COMMAND tar --sort=name --format=gnu --owner=0 --group=0 --numeric-owner
                "--mtime=@$ENV{SOURCE_DATE_EPOCH}"
                -C "${CPACK_TEMPORARY_DIRECTORY}" -cf - "${_root}"
    COMMAND gzip -n -9
    OUTPUT_FILE "${_out}"
    RESULT_VARIABLE _rc
    ERROR_VARIABLE _err)
if(NOT _rc STREQUAL "0")
    message(FATAL_ERROR "AxlPackTar: tar | gzip failed (${_rc}): ${_err}")
endif()
set(CPACK_EXTERNAL_BUILT_PACKAGES "${_out}")
