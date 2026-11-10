# src/mem -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-mem.c
    axl-arena.c)

# Linked beside libaxl, not in it.
axl_objects(SRC axl-intrinsics.c)
