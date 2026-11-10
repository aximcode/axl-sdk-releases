# src/crt0 -- the entry stubs and CRT0s every image links ahead of its own code.
axl_objects(SRC axl-crt0-native.c axl-crt0-minimal.c axl-reloc.c)
axl_asm_object(axl_obj_crt0_gcc axl-crt0-gcc-${CMAKE_SYSTEM_PROCESSOR}.S)
axl_asm_object(axl_obj_debug_info axl-debug-info.S)
