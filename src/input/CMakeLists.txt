# src/input -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-input.c
    axl-input-gesture.c
    axl-input-debounce.c
    axl-virtual-pointer.c)
