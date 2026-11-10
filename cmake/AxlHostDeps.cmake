# AxlHostDeps.cmake -- one depfile for a host tool built from several sources
# in ONE compiler call (cmake -P; -DCC -DFLAGS -DSRCS -DTARGET -DDEPFILE, the
# lists `;`-separated).
#
# gcc's -MD/-MM write one TU's dependencies per file, so a multi-source
# `gcc ... -o tool a.c b.c` with -MF keeps only the last TU's: a header only
# a.c includes would be missed and the tool left stale (axl-efi-pack.c and
# src/pack/axl-pack-format.h were). Here each source is asked on its own and
# the rules are concatenated under one target, which Ninja's DEPFILE reads.

foreach(_v CC FLAGS SRCS TARGET DEPFILE)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "AxlHostDeps.cmake: -D${_v} is required")
    endif()
endforeach()
set(_all "")
foreach(_src IN LISTS SRCS)
    execute_process(COMMAND ${CC} ${FLAGS} -MM -MT ${TARGET} ${_src}
        OUTPUT_VARIABLE _rule RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "AxlHostDeps.cmake: ${CC} -MM ${_src} failed:\n${_err}")
    endif()
    string(APPEND _all "${_rule}")
endforeach()
file(WRITE ${DEPFILE} "${_all}")
