# AxlWrapEfi.cmake -- the pack wrap (the retired Makefile's WRAP_EFI), as a
# script (cmake -P).
#
# OUT becomes RAW compressed behind the pack stub when axl-efi-pack judges
# that worth it, else a byte copy of RAW (src/pack/README.md). The decision is
# the packer's and per image -- its exit code -- which is why this is a script
# run by axl_wrap_efi (cmake/AxlImages.cmake) and not a set of fixed rules:
#
#   exit 0  wrap: embed the .pack in .axlpack, link the stub variant RAW's PE
#           subsystem picks (10 -> app, 11/12 -> driver)
#   exit 3  keep: OUT is a copy of RAW
#   else    the build fails
#
# OUT is only ever replaced by a rename, so a step that fails leaves the
# previous OUT (still out of date) rather than a half-made one.
#
# Inputs (-D), lists '|'-separated: RAW OUT STATE MODE PACK_TOOL PACK_FLAGS
# STUB_APP STUB_DRV CC CFLAGS LD LDFLAGS LDS STUB_OBJS OBJCOPY SECTIONS
# PE_TARGET PE_SET_DEBUG.

foreach(_v CFLAGS LDFLAGS STUB_OBJS SECTIONS PACK_FLAGS)
    string(REPLACE "|" ";" ${_v} "${${_v}}")
endforeach()

file(REMOVE "${OUT}.tmp")
if(MODE STREQUAL "off")
    file(COPY_FILE "${RAW}" "${OUT}.tmp")
    file(RENAME "${OUT}.tmp" "${OUT}")
    message(STATUS "WRAP ${OUT}: AXL_PACK off, raw copy")
    return()
endif()

# _le(<var> <hex-bytes>) -- a little-endian integer from file(READ ... HEX).
function(_le var hex)
    string(LENGTH "${hex}" _n)
    set(_be "")
    while(_n GREATER 0)
        math(EXPR _n "${_n} - 2")
        string(SUBSTRING "${hex}" ${_n} 2 _byte)
        string(APPEND _be "${_byte}")
    endwhile()
    math(EXPR _v "0x${_be}")
    set(${var} ${_v} PARENT_SCOPE)
endfunction()

# The PE subsystem: e_lfanew at 0x3C, the optional header's Subsystem 92
# bytes past the PE signature.
file(READ "${RAW}" _hex OFFSET 60 LIMIT 4 HEX)
_le(_pe "${_hex}")
math(EXPR _at "${_pe} + 92")
file(READ "${RAW}" _hex OFFSET ${_at} LIMIT 2 HEX)
_le(_sub "${_hex}")
if(_sub EQUAL 10)
    set(_stub "${STUB_APP}")
    set(_outsub 10)
elseif(_sub EQUAL 11 OR _sub EQUAL 12)
    set(_stub "${STUB_DRV}")
    set(_outsub 11)
else()
    message(FATAL_ERROR "WRAP_EFI: ${RAW}: PE subsystem '${_sub}' is neither an app nor a driver")
endif()

get_filename_component(_dir "${STATE}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
file(SIZE "${_stub}" _stub_size)
execute_process(COMMAND ${PACK_TOOL} ${PACK_FLAGS} --stub-size ${_stub_size}
                        -o "${STATE}.pack" "${RAW}"
                RESULT_VARIABLE _rc)
if(_rc EQUAL 3)
    file(COPY_FILE "${RAW}" "${OUT}.tmp")
    file(RENAME "${OUT}.tmp" "${OUT}")
    return()
elseif(NOT _rc EQUAL 0)
    message(FATAL_ERROR "WRAP_EFI: axl-efi-pack failed on ${RAW} (exit ${_rc})")
endif()

# The payload, in its own allocated read-only section (AXL_PACK_SECTION), as
# the Makefile's EMBED_ASM wrote it.
file(WRITE "${STATE}.s"
"\t.section .axlpack,\"a\"\n\t.balign 8\n\t.globl axl_embedded_pack\n\t.globl axl_embedded_pack_end\naxl_embedded_pack:\n\t.incbin \"${STATE}.pack\"\naxl_embedded_pack_end:\n\n\t.section .note.GNU-stack, \"\", %progbits\n")
execute_process(COMMAND ${CC} ${CFLAGS} -c "${STATE}.s" -o "${STATE}.blob.o"
                COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND ${LD} ${LDFLAGS} -T ${LDS} --defsym=_AxlEntry=axl_pack_stub_entry
                        -o "${STATE}.stub.so" ${STUB_OBJS} "${STATE}.blob.o"
                COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND ${OBJCOPY} ${SECTIONS} -j .axlpack --strip-all
                        --output-target=${PE_TARGET} --subsystem=${_outsub}
                        "${STATE}.stub.so" "${OUT}.tmp"
                COMMAND_ERROR_IS_FATAL ANY)
# The module name is the stub's, not the file's: a crash dump names the stub
# and the image it loaded differently.
execute_process(COMMAND ${PE_SET_DEBUG} "${OUT}.tmp" axl-pack-stub
                COMMAND_ERROR_IS_FATAL ANY)
file(RENAME "${OUT}.tmp" "${OUT}")
