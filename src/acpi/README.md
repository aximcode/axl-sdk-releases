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

Discovery, typed readers for the small set of tables the SDK
consumes directly, and a **non-evaluating AML namespace walker**.
Other tables stay raw; consumers walk them via the `AxlAcpiHeader`
cursor and the table's own definitions from the ACPI spec.

**AML execution is out of scope, permanently.** That means a
bytecode interpreter with `OperationRegion` access — ACPICA-sized
work that diagnostic tools don't need, and which would have to touch
SystemMemory, SystemIO and PCI config space to be useful.

Parsing is a different thing, and it is in scope. `axl_aml_walk_*`
reads static `Name()` declarations directly out of the table's byte
stream: a structural walk with no evaluation, no side effects and no
hardware access. The hard stop is easy to state and easy to review —
**it never evaluates.** A `Method` body is skipped by its length and
never entered, so anything whose value is not a literal in the byte
stream is reported as present-but-unreadable rather than guessed at.

That distinction is not academic. On real firmware a large fraction
of the interesting objects *are* Methods, and they differ wildly by
machine: one measured client had 34% of its `_ADR` behind Methods,
while a measured server had none — but every one of that server's
`_SEG` and `_BBN` objects was a Method, and one of them resolved to
a field inside an `OperationRegion`. A walker that silently reported
"absent" for those would be lying; reporting "present, unreadable"
is the whole point.

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

## AML namespace walker

`axl_aml_walk_*` finds `Device` declarations in a DSDT or SSDT and
reports the integer objects each one carries. It is a cursor, matching
the discovery API's shape:

```c
AxlAmlWalk walk;
AxlAmlNode node;

if (axl_aml_walk_begin(&walk, axl_acpi_find("DSDT")) == AXL_OK) {
    while (axl_aml_walk_next(&walk, &node)) {
        if (node.adr.kind == AXL_AML_VALUE_STATIC) {
            axl_printf("%s  _ADR=%llx%s\n", node.path,
                       (unsigned long long)node.adr.value,
                       node.conditional ? "  (conditional)" : "");
        }
    }
    if (axl_aml_walk_truncated(&walk)) {
        axl_printf("warning: table malformed, walk stopped early\n");
    }
}
```

`AxlAmlWalk` is stack-allocated and the walk owns no memory, so there
is nothing to free and nothing to leak.

### Four states, not two

Every object is reported as one of:

| kind | meaning |
|---|---|
| `AXL_AML_VALUE_STATIC` | a literal in the byte stream; `value` is valid |
| `AXL_AML_VALUE_METHOD` | declared as a Method — computed at runtime, unreadable here |
| `AXL_AML_VALUE_NON_INTEGER` | declared as a String, Buffer or Package — read perfectly well, just not a number |
| `AXL_AML_VALUE_ABSENT` | not declared on this device at all |

`NON_INTEGER` is not a theoretical case: `_UID` is often a string
(`Name (_UID, "IPMI Device")`), and 34 of one measured client DSDT's
155 `_UID` declarations are strings. Calling those `METHOD` would
claim the walker could not read something it read fine.

**Branch on `kind`, never on `value == 0`.** Zero is an ordinary
`_ADR`, `_UID` and `_SEG`.

Collapsing `METHOD` into `ABSENT` would be actively misleading, and
how much lands in each bucket is not predictable from one machine:

| object | a measured client | a measured server |
|---|---|---|
| `_ADR` | 183 static / **96 Method** | **434 static / 0 Method** |
| `_SUN` | **none at all** | **26 static** |
| `_SEG` / `_BBN` | 1 static / 2 Method | **0 static / 17 Method** |

On that server every `_BBN` was a Method, and one resolved to a field
inside an `OperationRegion` — a bus number that simply cannot be had
without executing AML and touching hardware. Reporting it as
"present, unreadable" is what lets a consumer go get the number from
PCI config space instead of assuming the firmware was silent.

### Conditional devices

A device declared inside an `If` or `Else` body is reported with
`conditional` set. Whether it exists at runtime depends on a
condition only an interpreter could evaluate, so the walker reports
the device *and* the doubt. On one measured machine ~23% of devices —
including a host bridge — were declared this way, so neither dropping
them nor asserting they exist is acceptable.

### Malformed tables

The table is untrusted firmware input. Every package length is
validated against the remaining table length before it is trusted,
and nesting is capped at `AXL_AML_DEPTH_MAX`. A bad table produces a
bounded walk that stops, which `axl_aml_walk_truncated()` reports —
never a runaway or a read past the end.
