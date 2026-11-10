# AxlHelpers.cmake -- how a directory's CMakeLists.txt declares what it builds.

# Extra include directories, searched AFTER the common ones -- as the
# Makefile appended them. Order is semantic: deps/lzma carries an errno.h that
# must not shadow newlib's for anything the common directories resolve.
#
# axl_includes_after(<target> <dir>...) for a whole group: an interface
# library linked after axl_flags, whose directories therefore follow its
# (inherited include directories keep link order; a target's OWN would go
# first). <dir> is relative to the source root.
function(axl_includes_after target)
    add_library(${target}_includes_after INTERFACE)
    list(TRANSFORM ARGN PREPEND "${CMAKE_SOURCE_DIR}/")
    target_include_directories(${target}_includes_after INTERFACE ${ARGN})
    target_link_libraries(${target} PRIVATE ${target}_includes_after)
endfunction()

# axl_source_includes_after(TARGET <target> DIRS <dir>... SOURCES <src>...)
# for single files: a source's INCLUDE_DIRECTORIES property would also go
# first, so these are -I compile options, which CMake emits after every
# include directory. Source properties are DIRECTORY-scoped, so TARGET names
# the target the sources are compiled by: libaxl is declared in src/ and its
# sources added from each module's directory, and a property set without it
# would silently not apply. <dir> is relative to the source root; <src> to
# the calling directory.
function(axl_source_includes_after)
    cmake_parse_arguments(PARSE_ARGV 0 A "" "TARGET" "DIRS;SOURCES")
    list(TRANSFORM A_DIRS PREPEND "-I${CMAKE_SOURCE_DIR}/")
    set_property(SOURCE ${A_SOURCES} TARGET_DIRECTORY ${A_TARGET}
                 APPEND PROPERTY COMPILE_OPTIONS ${A_DIRS})
endfunction()

# axl_object(<name> <lint-pass> <source>...) -- an object library with the
# full C/C++ flag set. <lint-pass> files it for cmake/AxlLint.cmake: SRC (the
# full .clang-tidy), BUGPRONE (tests, tools, fixtures), CXX, or NONE.
# ONE SOURCE PER LIBRARY wherever an image links the object on its own: the
# link line's ORDER is part of the image (ld lays sections out in input
# order), and $<TARGET_OBJECTS> cannot pick one object out of a group.
function(axl_object name pass)
    add_library(${name} OBJECT ${ARGN})
    target_link_libraries(${name} PRIVATE axl_flags)
    set_property(GLOBAL APPEND PROPERTY AXL_LINT_${pass} ${name})
endfunction()

# axl_asm_object(<name> <source>) -- an entry stub: assembled (as the Makefile
# did) with CFLAGS_BASE alone (no configuration flags, no include paths).
function(axl_asm_object name src)
    add_library(${name} OBJECT ${src})
    target_link_libraries(${name} PRIVATE axl_target_flags)
    target_compile_options(${name} PRIVATE
        -std=gnu2x -ffreestanding -fno-math-errno -fno-trapping-math
        $<$<NOT:$<STREQUAL:${AXL_ARCH},aa64>>:-fno-asynchronous-unwind-tables>)
    target_compile_definitions(${name} PRIVATE AXL_ALLOW_UEFI)
endfunction()

# axl_obj_name(<var> <source>) -- the object library one source gets:
# axl_obj_<path from the source root>, dashes, dots and slashes folded to
# underscores (tools/hexdump.c -> axl_obj_tools_hexdump). <source> is relative
# to the calling directory.
function(axl_obj_name var src)
    file(RELATIVE_PATH _n ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/${src})
    string(REGEX REPLACE "\\.[^.]*$" "" _n "${_n}")
    string(REGEX REPLACE "[/.-]" "_" _n "${_n}")
    set(${var} axl_obj_${_n} PARENT_SCOPE)
endfunction()

# axl_objects(<lint-pass> <source>...) -- one object library per source,
# named by axl_obj_name.
function(axl_objects pass)
    foreach(_src IN LISTS ARGN)
        axl_obj_name(_n ${_src})
        axl_object(${_n} ${pass} ${_src})
    endforeach()
endfunction()

# axl_object_variant(<name> <lint-pass> <source> <definition>...) -- one
# source compiled again with the definitions that select a variant of it (the
# Makefile's per-variant rules). Lint EVERY variant of linted code: the
# definitions switch code in and out, so one variant does not see the others.
function(axl_object_variant name pass src)
    axl_object(${name} ${pass} ${src})
    target_compile_definitions(${name} PRIVATE ${ARGN})
endfunction()

# axl_list(<var> <NAME>) -- one of the build's named lists
# (scripts/axl-lists.conf), as a CMake list, read through scripts/axl-list.sh:
# the one reader the packaging scripts and the gates also agree with. A list it
# cannot answer fails the configure -- an empty tool list would build, and
# ship, nothing. Editing the file reconfigures.
function(axl_list var name)
    execute_process(
        COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/axl-list.sh ${name}
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0 OR _out STREQUAL "")
        message(FATAL_ERROR "axl_list(${name}): scripts/axl-list.sh failed (${_rc}): ${_err}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/axl-lists.conf ${CMAKE_SOURCE_DIR}/scripts/axl-list.sh)
    separate_arguments(_out UNIX_COMMAND "${_out}")
    set(${var} ${_out} PARENT_SCOPE)
endfunction()
