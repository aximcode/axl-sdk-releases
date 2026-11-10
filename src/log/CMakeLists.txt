# src/log -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-log.c
    axl-log-emit.c
    axl-log-ring.c
    axl-log-line.c
    axl-log-file.c
    axl-log-serial.c)
