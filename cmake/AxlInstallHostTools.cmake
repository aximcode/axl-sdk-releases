# AxlInstallHostTools.cmake -- axl_install_host_tools(<component>): the `axl`
# dispatcher, the host tools it fronts and the installer, in the shape both
# the SDK prefix and the host-tools archive ship (AXL-Distribution-Design.md
# §13, §20). ONE rule for both: the two staging paths of this file list were
# two hand-written lists, then one list with two mode rules, and each time
# they drifted.
#
# Included by cmake/AxlInstall.cmake (the SDK's sdk_common component) and by
# packaging/host-tools/CMakeLists.txt (a project with no compiler, so the
# host-tools archive needs no toolchain to build).

include_guard(GLOBAL)

function(axl_install_host_tools component)
    # HOST_TOOL_FILES (scripts/axl-lists.conf), through the one reader the
    # build, the gates and the scripts agree with.
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    execute_process(COMMAND bash ${_root}/scripts/axl-list.sh HOST_TOOL_FILES
        OUTPUT_VARIABLE _tools ERROR_VARIABLE _err RESULT_VARIABLE _rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0 OR _tools STREQUAL "")
        message(FATAL_ERROR "install: could not read HOST_TOOL_FILES (scripts/axl-list.sh, ${_rc}): ${_err}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${_root}/scripts/axl-lists.conf ${_root}/scripts/axl-list.sh)
    separate_arguments(_tools UNIX_COMMAND "${_tools}")

    install(PROGRAMS ${_root}/scripts/axl DESTINATION bin COMPONENT ${component})
    # REAL FILENAMES: the scripts find each other by name. A SHEBANG IS THE
    # MANIFEST: `axl` offers the executables in libexec/axl as commands, so a
    # file that can be run (#!) installs 755 and one that cannot -- axl-common.sh
    # is sourced, gdb-sample.py loaded inside gdb -- 644, decided at INSTALL
    # time from the file itself. v4.7.0's host-tools tarball, staged 0755 with
    # one hand-written exception, offered both as commands that cannot run.
    install(CODE "
        foreach(_axl_t ${_tools})
            set(_axl_src \"${_root}/scripts/\${_axl_t}\")
            if(NOT EXISTS \"\${_axl_src}\")
                message(FATAL_ERROR \"install: missing host tool: scripts/\${_axl_t}\")
            endif()
            # HEX: a text read of 2 bytes returns them with a newline appended.
            file(READ \"\${_axl_src}\" _axl_head LIMIT 2 HEX)
            if(_axl_head STREQUAL \"2321\")
                set(_axl_perm OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
            else()
                set(_axl_perm OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
            endif()
            file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/libexec/axl\" TYPE FILE
                 FILE_PERMISSIONS \${_axl_perm} FILES \"\${_axl_src}\")
        endforeach()
        " COMPONENT ${component})
    # The installer ships INSIDE the install (`axl update` / `axl uninstall`
    # run it; §20's M2), 0644 deliberately: a mode bit would list it as an
    # `axl install` command.
    install(FILES ${_root}/packaging/install.sh DESTINATION libexec/axl COMPONENT ${component})
endfunction()
