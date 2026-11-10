# AxlRtkOut.cmake -- the end of the sshd-rtk / sshd-full-rtk goal (cmake -P;
# -DIMG, -DLABEL). The image is copied into $ENV{SSHD_RTK_OUT} when the caller
# set one (a test builds into its own directory, the release script into the
# release tree), and the goal reports where it is in the Makefile's words --
# the line test-sshd-rtk-qemu.sh matches.

get_filename_component(_name "${IMG}" NAME)
get_filename_component(_dir "${IMG}" DIRECTORY)
set(_out "$ENV{SSHD_RTK_OUT}")
if(_out AND NOT _out STREQUAL _dir)
    file(MAKE_DIRECTORY "${_out}")
    file(COPY_FILE "${IMG}" "${_out}/${_name}" ONLY_IF_DIFFERENT)
    set(_dir "${_out}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "  Built: ${_dir}/${_name} (${LABEL})")
