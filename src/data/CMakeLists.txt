# src/data -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-str.c
    axl-str-bmh.c
    axl-str-base64.c
    axl-str-scan.c
    axl-find.c
    axl-regex.c
    axl-string.c
    axl-str-wide.c
    axl-hash-table.c
    axl-sidecar.c
    axl-class-fmt.c
    axl-array.c
    axl-list.c
    axl-slist.c
    axl-queue.c
    axl-json-parse.c
    axl-json-lex.c
    axl-json-build.c
    axl-json-io.c
    axl-json-print.c
    axl-xml-writer.c
    axl-xml-parse.c
    axl-cache.c
    axl-page-cache.c
    axl-text-buffer.c
    axl-rb-tree.c
    axl-piece-tree.c
    axl-radix-tree.c
    axl-ntree.c
    axl-tree.c
    axl-ring-buf.c
    axl-digest.c
    axl-digest-md5.c
    axl-digest-sha1.c
    axl-digest-sha256.c
    axl-digest-sha512.c
    axl-digest-crc.c
    axl-compress.c
    axl-compress-lzma.c
    ${CMAKE_SOURCE_DIR}/deps/lzma/LzmaDec.c
    ${CMAKE_SOURCE_DIR}/deps/lzma/LzmaEnc.c
    ${CMAKE_SOURCE_DIR}/deps/lzma/LzFind.c
    ${CMAKE_SOURCE_DIR}/deps/lzma/CpuArch.c
    ${CMAKE_SOURCE_DIR}/deps/lzma/Bra.c
    ${CMAKE_SOURCE_DIR}/deps/lzma/Bra86.c
    axl-hmac.c
    axl-pbkdf2.c
    axl-bytes.c)

# lzma is vendored for this module's compressor: its own flags, and its
# include directory after the common ones (its errno.h must not shadow newlib's).
set(_lzma Bra.c Bra86.c CpuArch.c LzFind.c LzmaDec.c LzmaEnc.c)
list(TRANSFORM _lzma PREPEND ${CMAKE_SOURCE_DIR}/deps/lzma/)
axl_source_includes_after(TARGET axl DIRS deps/lzma SOURCES ${_lzma} axl-compress-lzma.c)
set_property(SOURCE ${_lzma} TARGET_DIRECTORY axl APPEND PROPERTY COMPILE_DEFINITIONS Z7_ST)

# Linked beside libaxl, not in it.
axl_objects(SRC axl-str-compat.c)
