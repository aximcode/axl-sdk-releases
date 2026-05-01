ACPI table discovery and typed readers.

Locates ACPI tables published by firmware in the EFI configuration
table, transparently walks RSDT (ACPI 1.0) or XSDT (ACPI 2.0+),
and serves up identity-mapped table-header pointers. Cursor-style
iteration matches AxlSmbios:

```c
AxlAcpiHeader *h = NULL;
while ((h = axl_acpi_find_next("APIC", h)) != NULL) {
    /* process each MADT — typically just one, but the API is
       correct for OEM platforms that publish more */
}
```

Header: `<axl/axl-acpi.h>`.

## Scope

Discovery and typed readers for the small set of tables the SDK
consumes directly. **AML interpretation is out of scope** — that's
ACPICA-sized work that diagnostic tools don't need. Other tables
stay raw; consumers walk them via the `AxlAcpiHeader` cursor and
the table's own definitions from the ACPI spec.

## Discovery

`axl_acpi_find()` and `axl_acpi_find_next()` look up tables by
their 4-byte signature (NOT nul-terminated — match the on-wire
field exactly):

```c
AxlAcpiHeader *mcfg = axl_acpi_find("MCFG");
AxlAcpiHeader *fadt = axl_acpi_find("FACP");   /* spec quirk */
```

`axl_acpi_next()` iterates every table regardless of signature.
`axl_acpi_revision()` returns the RSDP revision byte (0 = ACPI 1.0,
2+ = ACPI 2.0+) for callers that care; `_find()` handles
RSDT-vs-XSDT internally so consumers don't choose. Lazy on first
use — no explicit init.

## Checksums

Validate a table before trusting its contents. Returns true if the
unsigned-byte sum across the header's `length` field equals 0
modulo 256 (the ACPI invariant):

```c
if (!axl_acpi_checksum_ok(table)) {
    axl_warning("checksum invalid; skipping");
    return -1;
}
```

The typed readers below already invoke this internally.

## Typed Readers

### MCFG — PCIe ECAM

Decodes the per-segment configuration-space allocation entries.
Required for `axl_pci_*` (R+2):

```c
AxlAcpiMcfg mcfg;
if (axl_acpi_read_mcfg(&mcfg) == 0) {
    for (size_t i = 0; i < mcfg.count; i++) {
        AxlAcpiMcfgEntry *e = &mcfg.segments[i];
        axl_printf("seg %u  buses %02x..%02x  base 0x%llx\n",
                   e->segment, e->start_bus, e->end_bus,
                   (unsigned long long)e->base_addr);
    }
}
```

### MADT — Interrupt controllers

x86 firmware populates `ioapics`; AArch64 firmware populates
`gic_regions`. Consumers check whichever is non-empty for the
running arch:

```c
AxlAcpiMadt madt;
if (axl_acpi_read_madt(&madt) == 0) {
    for (size_t i = 0; i < madt.ioapic_count; i++) {
        axl_printf("IOAPIC %u  addr 0x%x  irq base %u\n",
                   madt.ioapics[i].id,
                   madt.ioapics[i].addr,
                   madt.ioapics[i].global_irq_base);
    }
}
```

### FACP/FADT — Fixed-feature pointers

A minimal subset: SMI command port + ACPI enable/disable for
power-state interaction, PM1 event/control block ports for SCI
handling, DSDT pointer for callers that walk AML themselves, and
the boot-arch flags. The full FADT has many more fields — read it
directly via `axl_acpi_find("FACP", NULL)` if you need them.

```c
AxlAcpiFacp facp;
if (axl_acpi_read_facp(&facp) == 0) {
    axl_printf("SMI cmd port: 0x%x  enable: 0x%x\n",
               facp.smi_cmd, facp.acpi_enable);
}
```
