# AxlToolSizes.cmake -- the `tool-sizes` report (cmake -P; -DPREFIX, -DARCH,
# -DTOOLS ';'-separated). Each shipped tool's raw (linked) and shipped
# (wrapped or copied) size, sorted by shipped size, one
# `<name> raw <n> shipped <m>` line each -- test-efi-pack-tools.sh pins the
# format -- then the totals. The raw column is selective linking's benefit; the
# shipped column is what a user copies.

set(_rows "")
set(_raw_sum 0)
set(_ship_sum 0)
foreach(_t IN LISTS TOOLS)
    file(SIZE "${PREFIX}/tools/raw/${_t}.efi" _raw)
    file(SIZE "${PREFIX}/tools/${_t}.efi" _ship)
    math(EXPR _raw_sum "${_raw_sum} + ${_raw}")
    math(EXPR _ship_sum "${_ship_sum} + ${_ship}")
    # A zero-padded key sorts numerically as a string (the Makefile's
    # `sort -n -k5`).
    string(LENGTH "${_ship}" _n)
    math(EXPR _pad "12 - ${_n}")
    string(REPEAT "0" ${_pad} _z)
    list(APPEND _rows "${_z}${_ship}|${_t} raw ${_raw} shipped ${_ship}")
endforeach()
list(SORT _rows)
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "")
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "Tool .efi sizes (${ARCH}, bytes):")
foreach(_r IN LISTS _rows)
    string(REGEX REPLACE "^[0-9]+\\|" "" _r "${_r}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E echo "${_r}")
endforeach()
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "total raw ${_raw_sum} shipped ${_ship_sum}")
file(SIZE "${PREFIX}/lib/libaxl.a" _lib)
execute_process(COMMAND ${CMAKE_COMMAND} -E echo
    "(libaxl.a archive: ${_lib} B -- selective linking pulls only referenced symbols)")
