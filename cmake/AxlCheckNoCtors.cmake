# AxlCheckNoCtors.cmake -- an object with a NON-EMPTY .init_array fails
# (cmake -P; -DOBJDUMP, -DOBJECTS, ';'-separated).
#
# For an image with no runtime to walk .init_array (hello-minimal): a global
# constructor there is registered and never run, silently -- the shape the
# driver .init_array defect had. Asserted rather than trusted to the source.

if(NOT OBJECTS)
    message(FATAL_ERROR "no-ctors: no object to check -- the check would see nothing")
endif()
foreach(_o IN LISTS OBJECTS)
    execute_process(COMMAND ${OBJDUMP} -h ${_o} OUTPUT_VARIABLE _h RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "no-ctors: ${OBJDUMP} could not read ${_o} (exit ${_rc})")
    endif()
    # Every .init_array section, the prioritised .init_array.NNNNN of
    # __attribute__((constructor(N))) as well as the plain one.
    string(REGEX MATCHALL "\\.init_array(\\.[0-9]+)? +[0-9a-f]+" _secs "${_h}")
    foreach(_s IN LISTS _secs)
        string(REGEX MATCH "^([^ ]+) +([0-9a-f]+)$" _ "${_s}")
        math(EXPR _n "0x${CMAKE_MATCH_2}")
        if(_n GREATER 0)
            message(FATAL_ERROR "no-ctors: ${_o} registered a global constructor "
                "(${CMAKE_MATCH_1} is ${_n} bytes), and its image has NO runtime to run "
                "it. Use constant-initialised globals only.")
        endif()
    endforeach()
    message(STATUS "no-ctors: ${_o}: no global constructor")
endforeach()
