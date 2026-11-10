# src/runtime -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-atexit.c
    axl-cxxabi.c
    axl-registry.c
    axl-runtime.c
    axl-signal.c
    axl-stack-guard.c)
