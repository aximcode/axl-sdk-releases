# src/pci -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-pci.c
    axl-pci-cap.c
    axl-pci-slot.c
    axl-pci-ids.c
    axl-pci-class.c)
