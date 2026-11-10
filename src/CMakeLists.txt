# src -- libaxl.a, and the runtime objects every image links beside it.
#
# Each module adds its own sources (and the vendored code it uses) to libaxl,
# in this order -- the archive's member order (the retired Makefile's
# LIB_SOURCES order), which is part of every image: ld extracts members in
# archive order. src/gfx/fonts follows src/math for the same reason.

add_library(axl STATIC)
target_link_libraries(axl PRIVATE axl_flags)
set_target_properties(axl PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set_property(GLOBAL APPEND PROPERTY AXL_LINT_SRC axl)

foreach(_m
    backend/native
    mem
    format
    log
    data
    stream
    fs
    util
    smbios
    acpi
    pci
    usb
    block
    nvme
    ata
    scsi
    smart
    serial
    fv
    fw
    pack
    tpm
    hii
    ramdisk
    loop
    service
    event
    task
    9p
    net
    gfx
    math
    gfx/fonts
    input
    smbus
    ipmi
    spd
    posix
    runtime
    vterm)
    add_subdirectory(${_m})
endforeach()

add_subdirectory(crt0)
add_subdirectory(cxxrt)

# libaxl-standin.a -- the stand-in C library (AXL-Host-Toolchain-Design.md
# §5.3): weak leaf functions that complete a link with no libc at all
# (AXL_TOOLCHAIN=host). Linked by no image in this tree, so nothing else
# builds it; the SDK ships it beside libaxl.a (scripts/install.sh), so it is
# part of `all`. The retired Makefile's STANDIN_SOURCES, in its order.
add_library(axl_standin STATIC
    $<TARGET_OBJECTS:axl_obj_src_data_axl_str_compat>
    $<TARGET_OBJECTS:axl_obj_src_mem_axl_intrinsics>)
set_target_properties(axl_standin PROPERTIES
    OUTPUT_NAME axl-standin
    LINKER_LANGUAGE C
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
