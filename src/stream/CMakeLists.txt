# src/stream -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-stream.c
    axl-stream-buf.c
    axl-stream-file.c
    axl-stream-text.c
    axl-console-decode.c
    axl-compress-stream.c)
