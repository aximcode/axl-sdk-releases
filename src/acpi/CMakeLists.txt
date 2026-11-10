# src/acpi -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-acpi.c
    axl-acpi-mcfg.c
    axl-acpi-madt.c
    axl-acpi-fadt.c
    axl-acpi-aml.c)
