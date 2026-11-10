# src/9p -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-9p-codec.c
    axl-9p-client.c
    axl-9p-mount.c
    axl-9p-server.c
    axl-9p-server-ops.c
    axl-9p-server-io-ops.c
    axl-9p-server-ns-ops.c
    axl-9p-server-fid.c)
