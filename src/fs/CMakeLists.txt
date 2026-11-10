# src/fs -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-fs.c
    axl-fs-replace.c
    axl-file-writer.c
    axl-file-view.c
    axl-file-gen.c
    axl-fs-provider.c
    axl-device-path.c)
