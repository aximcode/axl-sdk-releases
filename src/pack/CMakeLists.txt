# src/pack -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-pack-format.c)

axl_source_includes_after(TARGET axl DIRS deps/lzma SOURCES axl-pack-format.c)

# The pack stub: the loader a packed image starts in (an image of its own).
axl_objects(SRC axl-pack-stub.c)
# ...and the TEST stub (AXL_PACK_STUB_TEST_OBJS), whose first decode sees a
# damaged payload: it must re-read its file and still run the image.
axl_object_variant(axl_obj_src_pack_axl_pack_stub_retrytest SRC axl-pack-stub.c
    AXL_PACK_STUB_TEST_CORRUPT_FIRST)

# The stub with an EMPTY payload, one per variant: what the wrap tells
# axl-efi-pack the stub costs (--stub-size) -- measured, not assumed -- and the
# image the stub's gates read. Both link the one empty payload object.
# file(CONFIGURE), not file(WRITE): it writes only when the content differs.
# An unconditional write gave the payload a new mtime on EVERY configure, so
# any re-run of CMake -- ninja's own, after a glob or a listed file changed,
# or a test configuring the shared tree -- made the stub and every wrapped
# image stale while nothing had changed (test-cmake-reconfigure-noop.sh).
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/empty.pack CONTENT "")
axl_embed_blob(pack ${CMAKE_CURRENT_BINARY_DIR}/empty.pack OBJECT
    TARGET axl_pack_empty_blob SECTION ".axlpack,\"a\"" ASM empty.s)
foreach(_v app drv)
    if(_v STREQUAL "app")
        set(_sub 10)
    else()
        set(_sub 11)
    endif()
    axl_efi_image(axl_image_pack_stub_${_v}_empty
        EFI build/axl-pack-stub-${_v}-empty.efi SUBSYSTEM ${_sub}
        NO_LIBS MODULE axl-pack-stub EXTRA_SECTIONS .axlpack
        OBJECTS ${AXL_PACK_STUB_OBJS} axl_pack_empty_blob
        LINK_FLAGS --defsym=_AxlEntry=axl_pack_stub_entry)
endforeach()

