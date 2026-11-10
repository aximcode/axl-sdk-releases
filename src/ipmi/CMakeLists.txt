# src/ipmi -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-ipmi.c
    axl-ipmi-kcs.c
    axl-ipmi-ssif.c
    axl-ipmi-edkii.c
    axl-ipmi-dell.c
    axl-ipmi-cmd.c
    axl-ipmi-format.c)
