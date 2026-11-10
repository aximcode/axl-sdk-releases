# src/vterm -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-vterm.c
    axl-console-screen.c
    ${CMAKE_SOURCE_DIR}/deps/libvterm/src/vterm.c
    ${CMAKE_SOURCE_DIR}/deps/libvterm/src/parser.c
    ${CMAKE_SOURCE_DIR}/deps/libvterm/src/state.c
    ${CMAKE_SOURCE_DIR}/deps/libvterm/src/pen.c
    ${CMAKE_SOURCE_DIR}/deps/libvterm/src/encoding.c
    ${CMAKE_SOURCE_DIR}/deps/libvterm/src/unicode.c
    ${CMAKE_SOURCE_DIR}/deps/libvterm/src/keyboard.c
    ${CMAKE_SOURCE_DIR}/deps/libvterm/src/mouse.c)

# libvterm is vendored for this module.
set(_vterm encoding.c keyboard.c mouse.c parser.c pen.c state.c unicode.c vterm.c)
list(TRANSFORM _vterm PREPEND ${CMAKE_SOURCE_DIR}/deps/libvterm/src/)
axl_source_includes_after(TARGET axl DIRS deps/libvterm deps/libvterm/src SOURCES ${_vterm})
