# AxlImages.cmake -- linking UEFI images (the retired Makefile's LINK_EFI_*).
#
# "The Makefile's X" below names what the Makefile, retired in 5.0.0, called
# the same piece -- the port's provenance, not a second build.
#
# An image is three steps, each a custom command:
#   ld       the objects, in ORDER, then libaxl.a and the C library in one
#            --start-group, under the arch's EFI linker script -> an ELF .so
#            (kept unstripped, beside the image, for debuggers)
#   objcopy  the listed sections, stripped, as PE/COFF with the subsystem
#   pe-set-debug  the debug directory, module name and NX_COMPAT (a host tool)
#
# ORDER IS PART OF THE IMAGE: ld lays sections out in input order, which is
# why every runtime piece and every tool is an object library of its own
# (axl_object). Slice 2's measure was `cmp` against the Makefile's image.

if(AXL_ARCH STREQUAL "aa64")
    set(_ldsarch aarch64)
    set(AXL_PE_TARGET pei-aarch64-little)
else()
    set(_ldsarch x86_64)
    set(AXL_PE_TARGET pei-x86-64)
endif()
# A C++ image links under the exceptions script (_eh), which KEEPs .eh_frame:
# libstdc++ is compiled with exceptions whatever the image's own code was.
set(AXL_EFI_LDS ${CMAKE_SOURCE_DIR}/scripts/elf_${_ldsarch}_efi.lds)
set(AXL_EFI_LDS_EH ${CMAKE_SOURCE_DIR}/scripts/elf_${_ldsarch}_efi_eh.lds)
set(AXL_LD ${AXL_BINUTILS_PREFIX}ld)
set(AXL_OBJCOPY ${AXL_BINUTILS_PREFIX}objcopy)

# The Makefile's LDFLAGS_EFI. --version-script localizes every symbol (a UEFI
# image exports nothing), which is what lets --gc-sections drop the runtime an
# image does not call.
set(AXL_LDFLAGS_EFI -nostdlib -shared -Bsymbolic --no-warn-rwx-segments
    --no-undefined -z noexecstack --fatal-warnings --gc-sections
    --version-script=${CMAKE_SOURCE_DIR}/scripts/efi-localize.ver)
set(AXL_OBJCOPY_SECTIONS -j .text -j .sdata -j .data -j .bss -j .dynamic -j .dynsym
    -j .rel -j .rela -j .rela.dyn -j .reloc -j .rodata -j .dbgdir
    -j .eh_frame -j .gcc_except_table)
# What objcopy drops from every image's PE (symbols and debug info stay in
# the .so beside it).
set(AXL_OBJCOPY_STRIP --strip-all)

# The C library every in-tree link carries (the Makefile's LIBC_A / LIBM_A /
# LIBGCC_A): asked of the compiler, which knows its own multilib. A compiler
# that does not have one echoes the bare name back, so check it is a file.
foreach(_lib c m gcc)
    execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=lib${_lib}.a
                    OUTPUT_VARIABLE _p OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT IS_ABSOLUTE "${_p}" OR NOT EXISTS "${_p}")
        message(FATAL_ERROR "images: ${CMAKE_C_COMPILER} has no lib${_lib}.a ('${_p}')")
    endif()
    string(TOUPPER ${_lib} _L)
    set(AXL_LIB${_L}_A "${_p}")
endforeach()
# ...and the C++ runtime every C++ link carries, from the C++ driver: these
# must be its copies, or they disagree with the headers the objects used.
foreach(_lib stdc++ supc++)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=lib${_lib}.a
                    OUTPUT_VARIABLE _p OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT IS_ABSOLUTE "${_p}" OR NOT EXISTS "${_p}")
        message(FATAL_ERROR "images: ${CMAKE_CXX_COMPILER} has no lib${_lib}.a ('${_p}') -- "
                            "every C++ link needs it; install the bare-metal toolchain")
    endif()
    string(REPLACE "+" "X" _L ${_lib})
    string(TOUPPER ${_L} _L)
    set(AXL_LIB${_L}_A "${_p}")
endforeach()

# Host tools, built with the HOST compiler (the Makefile's $(HOSTCC)): they
# run on the build machine and never enter an image. In <tree>/build/<tool>,
# where the Makefile put them, so a test or the installer finds them where it
# always looked.
find_program(AXL_HOST_CC NAMES gcc cc REQUIRED)
set(AXL_HOST_DIR ${CMAKE_BINARY_DIR}/build)

# axl_host_tool(<name> SOURCES <src>... FLAGS <flag>... [DEPENDS <file>...])
# -- build/<name> with the host compiler: `FLAGS -o build/<name> SOURCES`,
# path in AXL_HOST_TOOL_<name> (the caller's scope). SOURCES and DEPENDS are
# relative to the source root; the headers the sources include are tracked
# by a depfile (cmake/AxlHostDeps.cmake), as every other compile's are.
# Recorded for the lint
# preset (cmake/AxlLint.cmake), which runs clang-tidy over each non-vendored
# source with the same FLAGS: a host tool is not a CMake target of this
# project (one project, one C compiler -- the cross one), so without that it
# would be in no compile database and no lint pass.
function(axl_host_tool name)
    cmake_parse_arguments(PARSE_ARGV 1 A "" "" "SOURCES;FLAGS;DEPENDS")
    set(_out ${AXL_HOST_DIR}/${name})
    list(TRANSFORM A_SOURCES PREPEND ${CMAKE_SOURCE_DIR}/ OUTPUT_VARIABLE _srcs)
    list(TRANSFORM A_DEPENDS PREPEND ${CMAKE_SOURCE_DIR}/ OUTPUT_VARIABLE _deps)
    add_custom_command(OUTPUT ${_out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${AXL_HOST_DIR}
        COMMAND ${CMAKE_COMMAND} -DCC=${AXL_HOST_CC} "-DFLAGS=${A_FLAGS}" "-DSRCS=${_srcs}"
                -DTARGET=${_out} -DDEPFILE=${_out}.d -P ${CMAKE_SOURCE_DIR}/cmake/AxlHostDeps.cmake
        COMMAND ${AXL_HOST_CC} ${A_FLAGS} -o ${_out} ${_srcs}
        DEPENDS ${_srcs} ${_deps} ${CMAKE_SOURCE_DIR}/cmake/AxlHostDeps.cmake
        DEPFILE ${_out}.d
        COMMENT "Building host tool ${name}" VERBATIM)
    set(AXL_HOST_TOOL_${name} ${_out} PARENT_SCOPE)
    set_property(GLOBAL APPEND PROPERTY AXL_HOST_TOOLS ${name})
    set_property(GLOBAL PROPERTY "AXL_HOST_TOOL_SRCS:${name}" "${_srcs}")
    set_property(GLOBAL PROPERTY "AXL_HOST_TOOL_FLAGS:${name}" "${A_FLAGS}")
endfunction()

axl_host_tool(pe-set-debug SOURCES scripts/pe-set-debug.c FLAGS -Wall -O2)
set(AXL_PE_SET_DEBUG ${AXL_HOST_TOOL_pe-set-debug})
# axl-efi-pack, the packer WRAP_EFI runs: the shared format module and the
# LZMA encoder and decoder, every branch converter (it packs either arch).
axl_host_tool(axl-efi-pack
    SOURCES scripts/axl-efi-pack.c src/pack/axl-pack-format.c deps/lzma/LzmaDec.c
            deps/lzma/LzmaEnc.c deps/lzma/LzFind.c deps/lzma/CpuArch.c deps/lzma/Bra.c
            deps/lzma/Bra86.c
    FLAGS -Wall -Wextra -O2 -DZ7_ST -DAXL_PACK_ALL_DECODERS -I${CMAKE_SOURCE_DIR}/deps/lzma)
set(AXL_EFI_PACK ${AXL_HOST_TOOL_axl-efi-pack})

# A target to depend on: images are declared in other directories, and a
# custom command's output is only known by target to targets in its own.
add_custom_target(axl_host_tools DEPENDS ${AXL_PE_SET_DEBUG} ${AXL_EFI_PACK})

# The runtime every application and driver links: entry stub, relocator,
# debug-directory section; an application adds the AXL CRT0. The C library's
# porting objects go into the --start-group beside libaxl.a.
set(AXL_CRT0_DRIVER axl_obj_crt0_gcc axl_obj_src_crt0_axl_reloc axl_obj_debug_info)
set(AXL_CRT0_APP ${AXL_CRT0_DRIVER} axl_obj_src_crt0_axl_crt0_native)
# --minimal-runtime's: the minimal CRT0 instead of the full one.
set(AXL_CRT0_MINIMAL ${AXL_CRT0_DRIVER} axl_obj_src_crt0_axl_crt0_minimal)
set(AXL_PORTING axl_obj_src_cxxrt_axl_cxxrt_alloc axl_obj_src_cxxrt_axl_cxxrt_stubs)
# A C++ image's: the porting objects, then the exception glue and the
# terminate handler (the Makefile's CXX_PORTING_OBJS).
set(AXL_PORTING_CXX ${AXL_PORTING} axl_obj_src_cxxrt_axl_cxxrt_eh
    axl_obj_src_cxxrt_axl_cxxrt_terminate)

# The runtime objects, PUBLISHED where the Makefile kept them -- <tree>/build/,
# under its names -- for a link made outside this build (a test linking its
# own consumer, the SDK installer). Copies of the objects every image links;
# `<target>=<file name>` each.
set(AXL_RUNTIME_OBJECTS
    axl_obj_crt0_gcc=axl-crt0-gcc-${CMAKE_SYSTEM_PROCESSOR}.o
    axl_obj_src_crt0_axl_reloc=axl-reloc.o
    axl_obj_debug_info=axl-debug-info.o
    axl_obj_src_crt0_axl_crt0_native=axl-crt0-native.o
    axl_obj_src_crt0_axl_crt0_minimal=axl-crt0-minimal.o
    axl_obj_src_cxxrt_axl_cxxrt_alloc=axl-cxxrt-alloc.o
    axl_obj_src_cxxrt_axl_cxxrt_stubs=axl-cxxrt-stubs.o
    axl_obj_src_cxxrt_axl_cxxrt_eh=axl-cxxrt-eh.o
    axl_obj_src_cxxrt_axl_cxxrt_terminate=axl-cxxrt-terminate.o
    axl_obj_src_cxxrt_axl_cxxrt_nothrow=axl-cxxrt-nothrow.o)
set(_published "")
foreach(_pair IN LISTS AXL_RUNTIME_OBJECTS)
    string(REPLACE "=" ";" _pair ${_pair})
    list(GET _pair 0 _t)
    list(GET _pair 1 _n)
    add_custom_command(OUTPUT ${AXL_HOST_DIR}/${_n}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${AXL_HOST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_OBJECTS:${_t}> ${AXL_HOST_DIR}/${_n}
        DEPENDS $<TARGET_OBJECTS:${_t}> VERBATIM)
    list(APPEND _published ${AXL_HOST_DIR}/${_n})
endforeach()
add_custom_target(axl_runtime_objects ALL DEPENDS ${_published})

# The pack stub's objects (the Makefile's AXL_PACK_STUB_OBJS), in its order:
# no libaxl runtime, only the PE bootstrap, the stub, and the libaxl members it
# needs -- the format module, the console line writer, the LZMA decoder and
# both x86 branch converters (gc keeps the one in use). `axl@<source>` picks
# one object out of a target (see axl_efi_image).
set(AXL_PACK_STUB_OBJS axl_obj_crt0_gcc axl_obj_src_crt0_axl_reloc axl_obj_debug_info
    axl_obj_src_pack_axl_pack_stub
    axl@src/pack/axl-pack-format.c axl@src/backend/native/axl-backend-native-lines.c
    axl@deps/lzma/LzmaDec.c axl@deps/lzma/Bra.c axl@deps/lzma/Bra86.c)
# The test stub's (AXL_PACK_STUB_TEST_OBJS): the same, its stub the retry-test
# build (src/pack).
string(REPLACE axl_obj_src_pack_axl_pack_stub axl_obj_src_pack_axl_pack_stub_retrytest
       AXL_PACK_STUB_TEST_OBJS "${AXL_PACK_STUB_OBJS}")

# _axl_object_expr(<out-var> <out-targets-var> <item>) -- what an OBJECTS item
# links: an object library's objects, or, for `<target>@<source>`, the ONE
# object <target> compiled from <source> (a $<FILTER> over its objects by
# file name --
# libaxl's members are compiled once, and the pack stub links five of them).
function(_axl_object_expr var tvar item)
    if(item MATCHES "^([A-Za-z0-9_]+)@(.+)$")
        set(_t ${CMAKE_MATCH_1})
        # By FILE NAME: CMake names an object after its source's path relative
        # to the TARGET's directory (src/pack's axl-pack-format.c is
        # pack/axl-pack-format.c.obj in libaxl, deps/ code __/deps/...), and
        # libaxl's file names are unique, so /<name>.obj picks exactly one.
        get_filename_component(_name "${CMAKE_MATCH_2}" NAME)
        string(REPLACE "." "\\." _re "/${_name}.obj")
        set(${var} "$<FILTER:$<TARGET_OBJECTS:${_t}>,INCLUDE,${_re}$>" PARENT_SCOPE)
        set(${tvar} ${_t} PARENT_SCOPE)
    else()
        set(${var} "$<TARGET_OBJECTS:${item}>" PARENT_SCOPE)
        set(${tvar} ${item} PARENT_SCOPE)
    endif()
endfunction()

# The pack stub's own objects, PUBLISHED as the runtime objects are (above):
# the SDK ships them prebuilt under lib/axl/<arch>/pack/ for `axl-cc
# --compress` (scripts/install.sh), which links exactly these beside the
# runtime objects. Five are libaxl members, one object each out of the
# archive's target. `<item>=<file name>`; part of axl_runtime_objects.
set(_pack_published "")
foreach(_pair
        axl_obj_src_pack_axl_pack_stub=axl-pack-stub.o
        axl@src/pack/axl-pack-format.c=axl-pack-format.o
        axl@src/backend/native/axl-backend-native-lines.c=axl-backend-native-lines.o
        axl@deps/lzma/LzmaDec.c=LzmaDec.o
        axl@deps/lzma/Bra.c=Bra.o
        axl@deps/lzma/Bra86.c=Bra86.o)
    string(REPLACE "=" ";" _pair ${_pair})
    list(GET _pair 0 _item)
    list(GET _pair 1 _n)
    _axl_object_expr(_obj _t ${_item})
    add_custom_command(OUTPUT ${AXL_HOST_DIR}/${_n}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${AXL_HOST_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${_obj} ${AXL_HOST_DIR}/${_n}
        DEPENDS ${_obj} ${_t} VERBATIM)
    list(APPEND _pack_published ${AXL_HOST_DIR}/${_n})
endforeach()
add_custom_target(axl_pack_stub_objects ALL DEPENDS ${_pack_published})

# The pull that makes a link take the log engine: libaxl's trampolines
# reach it only through a WEAK reference, so an image that logs must ask
# (the Makefile's LOG_ENGINE_PULL; axl-cc's --minimal-runtime=log).
set(AXL_LOG_ENGINE_PULL -u _axl_log_vdispatch)

# _axl_link_libs(<libs-var> <porting-var> <targets-var> <cxx>) -- the library
# group every image links after its objects (the Makefile's LINK_LIBS):
# the C library's porting objects (and, for a C++ image, the exception glue
# and terminate handler), libaxl.a, libstdc++/libsupc++ for C++, and the C
# library, in one --start-group. <porting-var> gets the porting objects alone
# (a link's file dependencies), <targets-var> the targets that make them.
function(_axl_link_libs libs_var porting_var targets_var cxx)
    if(cxx)
        set(port_targets ${AXL_PORTING_CXX})
        set(cxx_libs ${AXL_LIBSTDCXX_A} ${AXL_LIBSUPCXX_A})
    else()
        set(port_targets ${AXL_PORTING})
        set(cxx_libs "")
    endif()
    set(porting "")
    foreach(_t IN LISTS port_targets)
        list(APPEND porting $<TARGET_OBJECTS:${_t}>)
    endforeach()
    set(${libs_var} --start-group ${porting} $<TARGET_FILE:axl> ${cxx_libs}
        ${AXL_LIBC_A} ${AXL_LIBM_A} ${AXL_LIBGCC_A} --end-group PARENT_SCOPE)
    set(${porting_var} ${porting} PARENT_SCOPE)
    set(${targets_var} axl ${port_targets} PARENT_SCOPE)
endfunction()

# axl_efi_image(<target> EFI <path> [SO <path>] SUBSYSTEM <10|11|12>
#               OBJECTS <object-library>... [LINK_FLAGS <flag>...]
#               [NO_LIBS | CXX] [MODULE <name> | NO_PE_PATCH]
#               [EXTRA_SECTIONS <section>...])
# Links the OBJECTS in the order given, then libaxl.a and the C library --
# or nothing more with NO_LIBS (an image embedded IN libaxl, which links none
# of it). CXX links a C++ image: the C++ runtime glue and libstdc++ /
# libsupc++ join the group, under the exceptions linker script. EFI and SO are relative to the build tree; SO defaults to EFI's .so.
# OBJECTS are object libraries or `<target>@<source>` (one object of a
# target). EXTRA_SECTIONS are copied into the PE beside the standard list (the
# pack stub's .axlpack). MODULE names the module pe-set-debug writes, and then the PE is made under
# <efi>.tmp and renamed into place once patched, as the Makefile's recipes
# that named one did; without it pe-set-debug patches in place and takes the
# file's own name; NO_PE_PATCH skips pe-set-debug altogether (hello-minimal,
# whose recipe never ran it: no debug directory, no NX_COMPAT). A goal names
# the image by EFI (axl_image_target).
function(axl_efi_image target)
    cmake_parse_arguments(PARSE_ARGV 1 A "NO_LIBS;CXX;NO_PE_PATCH" "EFI;SO;SUBSYSTEM;MODULE"
                          "OBJECTS;LINK_FLAGS;EXTRA_SECTIONS")
    axl_image_target(${A_EFI} ${target})
    set(efi ${CMAKE_BINARY_DIR}/${A_EFI})
    if(A_SO)
        set(so ${CMAKE_BINARY_DIR}/${A_SO})
    else()
        string(REGEX REPLACE "\\.efi$" ".so" so "${efi}")
    endif()
    set(objs "")
    set(obj_targets "")
    foreach(_item IN LISTS A_OBJECTS)
        _axl_object_expr(_e _t ${_item})
        list(APPEND objs "${_e}")
        list(APPEND obj_targets ${_t})
    endforeach()
    list(REMOVE_DUPLICATES obj_targets)
    set(extra_j "")
    foreach(_sec IN LISTS A_EXTRA_SECTIONS)
        list(APPEND extra_j -j ${_sec})
    endforeach()
    set(porting "")
    set(libs "")
    set(lib_targets "")
    set(lds ${AXL_EFI_LDS})
    if(A_CXX)
        set(lds ${AXL_EFI_LDS_EH})
    endif()
    if(NOT A_NO_LIBS)
        _axl_link_libs(libs porting lib_targets "${A_CXX}")
    endif()
    get_filename_component(sodir ${so} DIRECTORY)
    get_filename_component(efidir ${efi} DIRECTORY)
    add_custom_command(OUTPUT ${so}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${sodir}
        COMMAND ${AXL_LD} ${AXL_LDFLAGS_EFI} -T ${lds} ${A_LINK_FLAGS}
                -o ${so} ${objs} ${libs}
        DEPENDS ${objs} ${porting} ${lib_targets} ${lds}
                ${CMAKE_SOURCE_DIR}/scripts/efi-localize.ver
        COMMENT "Linking ${A_EFI}" COMMAND_EXPAND_LISTS VERBATIM)
    if(A_MODULE)
        set(pe ${efi}.tmp)
        set(patch COMMAND ${AXL_PE_SET_DEBUG} ${pe} ${A_MODULE}
                  COMMAND ${CMAKE_COMMAND} -E rename ${pe} ${efi})
    elseif(A_NO_PE_PATCH)
        set(pe ${efi})
        set(patch "")
    else()
        set(pe ${efi})
        set(patch COMMAND ${AXL_PE_SET_DEBUG} ${efi})
    endif()
    add_custom_command(OUTPUT ${efi}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${efidir}
        COMMAND ${AXL_OBJCOPY} ${AXL_OBJCOPY_SECTIONS} ${extra_j} ${AXL_OBJCOPY_STRIP}
                --output-target=${AXL_PE_TARGET} --subsystem=${A_SUBSYSTEM} ${so} ${pe}
        ${patch}
        DEPENDS ${so} ${AXL_PE_SET_DEBUG}
        COMMENT "PE ${A_EFI}" COMMAND_EXPAND_LISTS VERBATIM)
    add_custom_target(${target} ALL DEPENDS ${efi})
    add_dependencies(${target} axl_host_tools ${obj_targets} ${lib_targets})
endfunction()

# axl_app_image(<path> [CXX] <object>...) and axl_driver_image(...) -- the
# Makefile's LINK_EFI_APP / LINK_EFI_DRIVER (with CXX, their _CXX forms): an
# image at <path>.efi, the same place under the build tree as under the
# Makefile's prefix, linked from the application or driver CRT0 and <object>s.
# The target is axl_image_<path>, its slashes and dashes folded to underscores.
function(_axl_simple_image kind path)
    cmake_parse_arguments(PARSE_ARGV 2 A "CXX" "" "")
    string(REGEX REPLACE "[/.-]" "_" _t "${path}")
    if(kind STREQUAL "app")
        set(crt0 ${AXL_CRT0_APP})
        set(sub 10)
        set(link ${AXL_LOG_ENGINE_PULL})
    else()
        set(crt0 ${AXL_CRT0_DRIVER})
        set(sub 11)
        set(link ${AXL_LOG_ENGINE_PULL} --defsym=_AxlEntry=DriverEntry)
    endif()
    if(A_CXX)
        set(cxx CXX)
    else()
        set(cxx "")
    endif()
    axl_efi_image(axl_image_${_t} EFI ${path}.efi SUBSYSTEM ${sub} ${cxx}
        OBJECTS ${crt0} ${A_UNPARSED_ARGUMENTS} LINK_FLAGS ${link})
endfunction()
function(axl_app_image path)
    _axl_simple_image(app ${path} ${ARGN})
endfunction()
function(axl_driver_image path)
    _axl_simple_image(driver ${path} ${ARGN})
endfunction()

# axl-link-pieces.sh -- the pieces of an image link, for a link made OUTSIDE
# this build (test-log-link-granularity.sh re-links a probe three ways, the
# way every image links): sourceable shell assignments, generated from the
# SAME variables and objects axl_efi_image links, so a probe cannot drift from
# the shipped link. Replaced the Makefile's `print-LINK_LIBS` and friends.
#   AXL_LP_LD AXL_LP_LDFLAGS AXL_LP_LDS     ld, its flags, the linker script
#   AXL_LP_CRT0_APP AXL_LP_CRT0_MINIMAL    an application's runtime objects,
#                                          full and --minimal-runtime
#   AXL_LP_LIBS                            the --start-group ... group
#   AXL_LP_LOG_PULL                        what makes a link take the log engine
#   AXL_LP_NM AXL_LP_OBJCOPY AXL_LP_OBJCOPY_SECTIONS AXL_LP_OBJCOPY_STRIP
#   AXL_LP_PE_TARGET
# The compile is the tree's own: compile_commands.json has every TU's.
set(_lp_objs_app "")
foreach(_t IN LISTS AXL_CRT0_APP)
    list(APPEND _lp_objs_app "$<TARGET_OBJECTS:${_t}>")
endforeach()
set(_lp_objs_min "")
foreach(_t IN LISTS AXL_CRT0_MINIMAL)
    list(APPEND _lp_objs_min "$<TARGET_OBJECTS:${_t}>")
endforeach()
_axl_link_libs(_lp_libs _lp_porting _lp_lib_targets "")
# Single-quoted below: a quote in any path would end the value early.
foreach(_p ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR} ${AXL_LD} ${AXL_OBJCOPY} ${AXL_LIBC_A})
    if(_p MATCHES "'")
        message(FATAL_ERROR "axl-link-pieces.sh: a path holds a single quote: ${_p}")
    endif()
endforeach()
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/axl-link-pieces.sh CONTENT
"# Generated by cmake/AxlImages.cmake -- the pieces of an image link (see there).
AXL_LP_LD='${AXL_LD}'
AXL_LP_LDFLAGS='$<JOIN:${AXL_LDFLAGS_EFI}, >'
AXL_LP_LDS='${AXL_EFI_LDS}'
AXL_LP_CRT0_APP='$<JOIN:${_lp_objs_app}, >'
AXL_LP_CRT0_MINIMAL='$<JOIN:${_lp_objs_min}, >'
AXL_LP_LIBS='$<JOIN:${_lp_libs}, >'
AXL_LP_LOG_PULL='$<JOIN:${AXL_LOG_ENGINE_PULL}, >'
AXL_LP_NM='${AXL_BINUTILS_PREFIX}nm'
AXL_LP_OBJCOPY='${AXL_OBJCOPY}'
AXL_LP_OBJCOPY_SECTIONS='$<JOIN:${AXL_OBJCOPY_SECTIONS}, >'
AXL_LP_OBJCOPY_STRIP='$<JOIN:${AXL_OBJCOPY_STRIP}, >'
AXL_LP_PE_TARGET='${AXL_PE_TARGET}'
")

# axl_embed_blob(<name> <file> (OBJECT [TARGET <t>] | INTO <target>)
#                [SECTION <asm-section>] [ASM <file-name>] [DEPENDS <target>...])
# -- the Makefile's EMBED_BLOB: <file>'s bytes as axl_embedded_<name> ..
# axl_embedded_<name>_end in .rodata, 8-byte aligned. The .s is generated with
# the Makefile's EMBED_ASM text and assembled as the Makefile assembled it
# (CFLAGS_BASE: axl_flags gives assembly exactly that). OBJECT makes it the
# object library axl_blob_<name>, for an image to link in its place; INTO
# adds it to <target>'s own sources at this point -- for libaxl, whose member
# order is the order sources are added (CMake would put an object library's
# objects FIRST). TARGET names the object library (default axl_blob_<name>);
# SECTION the assembler section (default .rodata; a pack payload goes in
# .axlpack,"a"); ASM the generated source's file name (default
# embed-blob-<name>.s, as the Makefile named it). DEPENDS names the target
# that makes <file>.
function(axl_embed_blob name file)
    cmake_parse_arguments(PARSE_ARGV 2 A "OBJECT" "INTO;TARGET;SECTION;ASM" "DEPENDS")
    if(NOT A_TARGET)
        set(A_TARGET axl_blob_${name})
    endif()
    if(NOT A_SECTION)
        set(A_SECTION .rodata)
    endif()
    if(NOT A_ASM)
        set(A_ASM embed-blob-${name}.s)
    endif()
    set(asm ${CMAKE_CURRENT_BINARY_DIR}/${A_ASM})
    file(CONFIGURE OUTPUT ${asm} CONTENT
"\t.section ${A_SECTION}\n\t.balign 8\n\t.globl axl_embedded_${name}\n\t.globl axl_embedded_${name}_end\naxl_embedded_${name}:\n\t.incbin \"${file}\"\naxl_embedded_${name}_end:\n\n\t.section .note.GNU-stack, \"\", %progbits\n")
    if(A_OBJECT)
        axl_object(${A_TARGET} NONE ${asm})
        set(owner ${A_TARGET})
    else()
        target_sources(${A_INTO} PRIVATE ${asm})
        set(owner ${A_INTO})
    endif()
    set_property(SOURCE ${asm} TARGET_DIRECTORY ${owner}
                 APPEND PROPERTY OBJECT_DEPENDS ${file})
    if(A_DEPENDS)
        add_dependencies(${owner} ${A_DEPENDS})
    endif()
endfunction()

# axl_wrap_efi(<target> RAW <path> OUT <path> [FORCE] [STUB <object>...])
# -- OUT becomes RAW compressed behind the pack stub when
# axl-efi-pack judges that worth it, else a byte copy of RAW. The decision is
# the packer's and per image (its exit code), so the steps run in a script,
# cmake/AxlWrapEfi.cmake, not as fixed build rules. FORCE wraps whatever the
# saving (test images). RAW is relative to the build tree unless absolute (a
# vendored binary). STUB replaces the stub's objects (default
# AXL_PACK_STUB_OBJS) -- the retry-test stub, which damages its first decode.
# AXL_PACK=OFF copies every image raw.
option(AXL_PACK "Ship images packed when the packer says it pays" ON)
function(axl_wrap_efi target)
    cmake_parse_arguments(PARSE_ARGV 1 A "FORCE" "RAW;OUT" "STUB")
    if(NOT A_STUB)
        set(A_STUB ${AXL_PACK_STUB_OBJS})
    endif()
    if(IS_ABSOLUTE "${A_RAW}")
        set(raw ${A_RAW})
    else()
        set(raw ${CMAKE_BINARY_DIR}/${A_RAW})
    endif()
    set(out ${CMAKE_BINARY_DIR}/${A_OUT})
    string(REPLACE "/" "__" stem "${A_OUT}")
    string(REGEX REPLACE "\\.efi$" "" stem "${stem}")
    set(stub_objs "")
    set(stub_targets "")
    foreach(_item IN LISTS A_STUB)
        _axl_object_expr(_e _t ${_item})
        list(APPEND stub_objs "${_e}")
        list(APPEND stub_targets ${_t})
    endforeach()
    list(REMOVE_DUPLICATES stub_targets)
    set(app_stub ${CMAKE_BINARY_DIR}/build/axl-pack-stub-app-empty.efi)
    set(drv_stub ${CMAKE_BINARY_DIR}/build/axl-pack-stub-drv-empty.efi)
    # Lists cross into the script joined by '|': a ';' would split the
    # COMMAND into separate arguments.
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND}
            -DRAW=${raw} -DOUT=${out}
            -DSTATE=${CMAKE_BINARY_DIR}/pack/${stem}
            -DMODE=$<IF:$<BOOL:${AXL_PACK}>,on,off>
            -DPACK_TOOL=${AXL_EFI_PACK} -DPACK_FLAGS=$<$<BOOL:${A_FORCE}>:--force>
            -DSTUB_APP=${app_stub} -DSTUB_DRV=${drv_stub}
            -DCC=${CMAKE_C_COMPILER} "-DCFLAGS=$<JOIN:${AXL_CFLAGS_BASE},|>"
            -DLD=${AXL_LD} "-DLDFLAGS=$<JOIN:${AXL_LDFLAGS_EFI},|>" -DLDS=${AXL_EFI_LDS}
            "-DSTUB_OBJS=$<JOIN:${stub_objs},|>"
            -DOBJCOPY=${AXL_OBJCOPY} "-DSECTIONS=$<JOIN:${AXL_OBJCOPY_SECTIONS},|>"
            -DPE_TARGET=${AXL_PE_TARGET} -DPE_SET_DEBUG=${AXL_PE_SET_DEBUG}
            -P ${CMAKE_SOURCE_DIR}/cmake/AxlWrapEfi.cmake
        DEPENDS ${raw} ${app_stub} ${drv_stub} ${AXL_EFI_PACK} ${AXL_PE_SET_DEBUG}
                ${CMAKE_SOURCE_DIR}/cmake/AxlWrapEfi.cmake ${stub_objs}
        COMMENT "Wrap ${A_OUT}" VERBATIM)
    add_custom_target(${target} ALL DEPENDS ${out})
    add_dependencies(${target} axl_host_tools axl_image_pack_stub_app_empty
                     axl_image_pack_stub_drv_empty ${stub_targets})
    axl_image_target(${A_OUT} ${target})
endfunction()

# axl_image_target(<path> <target>) -- records that <target> builds the
# build-tree file <path>, for a goal that names the image by that path
# (cmake/AxlGoals.cmake). The image functions call it for every image they
# declare; a file made any other way (a copy) calls it directly.
function(axl_image_target path target)
    get_property(_was GLOBAL PROPERTY "AXL_IMAGE_TARGET:${path}")
    if(_was)
        message(FATAL_ERROR "${path}: declared by ${_was} and again by ${target} -- "
            "one image, one target, or a goal names whichever came last")
    endif()
    set_property(GLOBAL PROPERTY "AXL_IMAGE_TARGET:${path}" ${target})
endfunction()

# axl_goal(<name> (IMAGES <prefix-path>... | TARGETS <target>... |
#                  DIRECTORY <source-dir>)) -- a goal a caller builds by name
# (cmake/AxlGoals.cmake). Recorded here and created by axl_goals_finalize(),
# after every directory, so a goal may name an image declared anywhere.
function(axl_goal name)
    cmake_parse_arguments(PARSE_ARGV 1 A "" "DIRECTORY" "IMAGES;TARGETS")
    set_property(GLOBAL APPEND PROPERTY AXL_GOALS ${name})
    set_property(GLOBAL PROPERTY "AXL_GOAL_IMAGES:${name}" "${A_IMAGES}")
    set_property(GLOBAL PROPERTY "AXL_GOAL_TARGETS:${name}" "${A_TARGETS}")
    set_property(GLOBAL PROPERTY "AXL_GOAL_DIR:${name}" "${A_DIRECTORY}")
endfunction()

# axl_goals_finalize() -- one target per recorded goal, depending on the
# targets that make its images. An image path no declaration pairs is a
# configure ERROR, not an empty goal: a goal that builds nothing would let a
# test boot a stale image.
function(axl_goals_finalize)
    get_property(_goals GLOBAL PROPERTY AXL_GOALS)
    foreach(_g IN LISTS _goals)
        get_property(_imgs GLOBAL PROPERTY "AXL_GOAL_IMAGES:${_g}")
        get_property(_tgts GLOBAL PROPERTY "AXL_GOAL_TARGETS:${_g}")
        get_property(_dir GLOBAL PROPERTY "AXL_GOAL_DIR:${_g}")
        foreach(_i IN LISTS _imgs)
            get_property(_t GLOBAL PROPERTY "AXL_IMAGE_TARGET:${_i}")
            if(NOT _t)
                message(FATAL_ERROR "goal ${_g}: no declaration builds ${_i}")
            endif()
            list(APPEND _tgts ${_t})
        endforeach()
        if(_dir)
            get_property(_dt DIRECTORY ${CMAKE_SOURCE_DIR}/${_dir} PROPERTY BUILDSYSTEM_TARGETS)
            list(APPEND _tgts ${_dt})
        endif()
        if(NOT _tgts)
            message(FATAL_ERROR "goal ${_g}: builds nothing")
        endif()
        add_custom_target(${_g})
        # ...and what every goal needs beside its images: the published
        # runtime and pack-stub objects, the stand-in libc and the host
        # tools. A caller that builds `tools` and then links against
        # <tree>/build/*.o must not get stale ones.
        add_dependencies(${_g} ${_tgts} axl_runtime_objects axl_pack_stub_objects
            axl_standin axl_host_tools)
    endforeach()
endfunction()
