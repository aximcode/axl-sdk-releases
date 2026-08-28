/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * lsacpi — list the ACPI tables the firmware published, and decode them.
 *
 * The obvious half is a table browser: what is there, how big, whose
 * OEM strings, and whether each checksum is valid. Most tables have no
 * typed reader and never will, so anything undecoded falls back to a
 * hexdump automatically rather than behind a flag -- a tool that says
 * "DSDT, 19404 bytes" and stops is useless in exactly the situation
 * where you already know something is wrong.
 *
 * Views:
 *   (default)   the inventory, one row per table
 *   <SIG>       decode one table through its typed reader, or hexdump it
 * Modifiers: -v verbose, -j JSON.
 *
 * FACS is the one row that needs special handling everywhere. It is not
 * a System Description Table: its first 8 bytes are signature and
 * length, and offset 8 onward is HardwareSignature, not
 * revision/checksum/OEM. Rendering it through AxlAcpiHeader prints four
 * columns of garbage and a meaningless checksum verdict.
 */

#include <axl.h>

static const AxlArgDesc flags[] = {
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose: extra per-table detail" },
    { .name = "json",      .short_name = 'j', .type = AXL_ARG_BOOL,
      .help = "JSON output" },
    { .name = "namespace", .short_name = 'n', .type = AXL_ARG_BOOL,
      .help = "List devices in the ACPI namespace (DSDT + SSDTs)" },
    { .name = "slots",     .short_name = 's', .type = AXL_ARG_BOOL,
      .help = "Correlate all four slot sources and mark disagreements" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "signature", .type = AXL_ARG_STRING,
      .help = "Decode one table by its 4-char signature (e.g. MCFG)" },
    {0}
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/* ACPI's fixed-size character fields are NOT nul-terminated. Copy into
   a buffer and trim trailing spaces so columns line up. */
static void
acpi_str(char *dst, size_t dstlen, const char *src, size_t n)
{
    size_t i = 0;
    /* ACPI's fields are fixed-size and NOT nul-terminated, so this
       copies exactly n bytes -- but the same helper is handed argv,
       where a short string ("AB") would be read past its NUL. Stop at
       a NUL as well as at n. */
    for (; i < n && i + 1 < dstlen && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    while (i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\0')) {
        i--;
    }
    dst[i] = '\0';
}

/* FACS has no standard header. Everything past signature+length means
   something else entirely, so no other column may be rendered from it. */
static bool
is_facs(const AxlAcpiHeader *h)
{
    return h->signature[0] == 'F' && h->signature[1] == 'A'
        && h->signature[2] == 'C' && h->signature[3] == 'S';
}

/* A FACS length lives in the same place as an SDT length, and that is
   the ONLY field the two share. */
static uint32_t
table_length(const AxlAcpiHeader *h)
{
    return h->length;
}

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------

static void
print_row_text(const AxlAcpiHeader *h)
{
    char sig[5], oem[7], oemtab[9];
    acpi_str(sig, sizeof(sig), h->signature, 4);

    if (is_facs(h)) {
        /* Dashes, not zeroes: FACS has no revision, no OEM strings
           and no whole-table checksum to report, and a zero would
           read as a value. */
        axl_printf("%-4s %6u %4s %-8s %-12s %s\n",
                   sig, table_length(h), "-", "-", "-", "-");
        return;
    }
    acpi_str(oem, sizeof(oem), h->oem_id, 6);
    acpi_str(oemtab, sizeof(oemtab), h->oem_table_id, 8);
    axl_printf("%-4s %6u %4u %-8s %-12s %s\n",
               sig, h->length, h->revision, oem, oemtab,
               axl_acpi_checksum_ok(h) ? "OK" : "BAD");
}

/* Emitted through AxlJsonWriter rather than printf: every string here
   is firmware-controlled, and an OEM ID carrying a quote, a backslash
   or a control byte would otherwise produce unparseable output. */
static void
print_row_json(AxlJsonWriter *jw, const AxlAcpiHeader *h)
{
    char sig[5], oem[7], oemtab[9];
    acpi_str(sig, sizeof(sig), h->signature, 4);

    axl_json_obj_begin(jw);
    axl_json_key(jw, "signature");
    axl_json_str(jw, sig);
    axl_json_key(jw, "length");
    axl_json_uint(jw, table_length(h));

    if (is_facs(h)) {
        axl_json_key(jw, "revision");    axl_json_null(jw);
        axl_json_key(jw, "oem_id");      axl_json_null(jw);
        axl_json_key(jw, "oem_table_id"); axl_json_null(jw);
        axl_json_key(jw, "checksum");    axl_json_null(jw);
        axl_json_obj_end(jw);
        return;
    }

    acpi_str(oem, sizeof(oem), h->oem_id, 6);
    acpi_str(oemtab, sizeof(oemtab), h->oem_table_id, 8);
    axl_json_key(jw, "revision");     axl_json_uint(jw, h->revision);
    axl_json_key(jw, "oem_id");       axl_json_str(jw, oem);
    axl_json_key(jw, "oem_table_id"); axl_json_str(jw, oemtab);
    axl_json_key(jw, "checksum");
    axl_json_str(jw, axl_acpi_checksum_ok(h) ? "OK" : "BAD");
    axl_json_obj_end(jw);
}

static int
render_inventory(bool json)
{
    AxlAcpiHeader *h = NULL;
    size_t         n = 0;

    AxlString     *js = NULL;
    AxlJsonWriter  jw;

    if (json) {
        js = axl_string_new("");
        axl_json_writer_init(&jw, js, 0);
        axl_json_obj_begin(&jw);
        axl_json_key(&jw, "tables");
        axl_json_arr_begin(&jw);
    } else {
        axl_printf("SIG  LENGTH  REV OEM ID   OEM TABLE ID CHECKSUM\n");
    }

    while ((h = axl_acpi_next(h)) != NULL) {
        if (json) {
            print_row_json(&jw, h);
        } else {
            print_row_text(h);
        }
        n++;
    }

    if (json) {
        axl_json_arr_end(&jw);
        axl_json_key(&jw, "count");
        axl_json_uint(&jw, n);
        axl_json_obj_end(&jw);
        axl_json_writer_finish(&jw);
        axl_printf("%s\n", axl_string_str(js));
        axl_string_free(js);
        return 0;
    }
    if (n == 0) {
        axl_printf("no ACPI tables found\n");
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Single-table decode
// ---------------------------------------------------------------------------

static int
render_mcfg(void)
{
    AxlAcpiMcfg m;
    if (axl_acpi_read_mcfg(&m) != AXL_OK) {
        axl_printerr("lsacpi: MCFG present but could not be decoded\n");
        return 1;
    }
    axl_printf("MCFG: %zu ECAM window(s)\n", m.count);
    for (size_t i = 0; i < m.count; i++) {
        axl_printf("  segment %u  buses %02x..%02x  base 0x%llx\n",
                   m.segments[i].segment, m.segments[i].start_bus,
                   m.segments[i].end_bus,
                   (unsigned long long)m.segments[i].base_addr);
    }
    return 0;
}

static int
render_hexdump(const AxlAcpiHeader *h, const char *sig)
{
    /* No typed reader for this one. Show the bytes rather than a
       one-line shrug -- and do it without a flag, because needing to
       ask twice is friction in exactly the case where you already
       suspect something. */
    /* axl_hexdump prints its own header with the signature, address
       and length, so this line carries only what that one cannot: WHY
       there are bytes instead of a decode. */
    axl_printf("%s: no typed reader, raw contents follow\n", sig);
    axl_hexdump(sig, h, table_length(h), 0, 0);
    return 0;
}

static int
render_one(const char *want, bool verbose)
{
    char sig[5];
    acpi_str(sig, sizeof(sig), want, 4);
    if (axl_strlen(sig) != 4) {
        axl_printerr("lsacpi: signature must be exactly 4 characters: '%s'\n",
                     want);
        return 1;
    }

    AxlAcpiHeader *h = axl_acpi_find(sig);
    if (h == NULL) {
        axl_printerr("lsacpi: no table with signature '%s'\n", sig);
        return 1;
    }

    if (verbose) {
        char oem[7];
        acpi_str(oem, sizeof(oem), h->oem_id, 6);
        axl_printf("%s: %u bytes%s\n", sig, table_length(h),
                   is_facs(h) ? " (FACS: no standard header)" : "");
        if (!is_facs(h)) {
            axl_printf("  revision %u  OEM %s  checksum %s\n",
                       h->revision, oem,
                       axl_acpi_checksum_ok(h) ? "OK" : "BAD");
        }
    }

    if (axl_strcmp(sig, "MCFG") == 0) {
        return render_mcfg();
    }
    return render_hexdump(h, sig);
}


// ---------------------------------------------------------------------------
// Namespace view
// ---------------------------------------------------------------------------

/* Every namespace object renders as its value, <method> or <absent>.
   Collapsing the middle case into "absent" would report "the firmware
   did not publish one" for something that is right there but computed
   at runtime -- and on real firmware that is not a rare case: one
   measured server has every _SEG and _BBN behind a Method. */
static void
put_value(const AxlAmlValue *v)
{
    switch (v->kind) {
    case AXL_AML_VALUE_STATIC:
        axl_printf(" %-10llx", (unsigned long long)v->value);
        break;
    case AXL_AML_VALUE_METHOD:
        axl_printf(" %-10s", "<method>");
        break;
    case AXL_AML_VALUE_NON_INTEGER:
        axl_printf(" %-10s", "<string>");
        break;
    case AXL_AML_VALUE_ABSENT:
    default:
        axl_printf(" %-10s", "-");
        break;
    }
}

static size_t
walk_one_table(AxlAcpiHeader *t, bool verbose, bool *any_incomplete)
{
    AxlAmlWalk walk;
    AxlAmlNode n;
    size_t     count = 0;

    if (axl_aml_walk_begin(&walk, t) != AXL_OK) {
        return 0;
    }
    while (axl_aml_walk_next(&walk, &n)) {
        count++;
        /* Without -v, only devices that carry something worth
           correlating; a bare Device with no _ADR is noise here. */
        if (!verbose && n.adr.kind == AXL_AML_VALUE_ABSENT
            && n.sun.kind == AXL_AML_VALUE_ABSENT) {
            continue;
        }
        axl_printf("%-32s", n.path);
        put_value(&n.adr);
        put_value(&n.sun);
        put_value(&n.uid);
        put_value(&n.seg);
        put_value(&n.bbn);
        axl_printf(" %s%s\n",
                   n.pld_kind == AXL_AML_VALUE_ABSENT ? "-" : "y",
                   n.conditional ? "  (conditional)" : "");
    }
    if (axl_aml_walk_truncated(&walk)) {
        *any_incomplete = true;
    }
    return count;
}

static int
render_namespace(bool verbose)
{
    AxlAcpiHeader *t;
    size_t         devices = 0, tables = 0;
    bool           incomplete = false;

    axl_printf("%-32s %-10s %-10s %-10s %-10s %-10s %s\n",
               "PATH", "_ADR", "_SUN", "_UID", "_SEG", "_BBN", "_PLD");

    t = axl_acpi_find("DSDT");
    if (t != NULL) {
        devices += walk_one_table(t, verbose, &incomplete);
        tables++;
    }
    t = NULL;
    while ((t = axl_acpi_find_next("SSDT", t)) != NULL) {
        devices += walk_one_table(t, verbose, &incomplete);
        tables++;
    }

    if (tables == 0) {
        axl_printerr("lsacpi: no DSDT or SSDT to walk\n");
        return 1;
    }
    axl_printf("\n%zu device(s) across %zu table(s)%s\n", devices, tables,
               incomplete ? " -- INCOMPLETE: part of a table could not be"
                            " parsed and was skipped" : "");
    return 0;
}


// ---------------------------------------------------------------------------
// Slot correlation — the reason this tool exists
// ---------------------------------------------------------------------------

/* Four sources describe the same slots and disagree on real hardware.
   No single one is authoritative, and no single JOIN works either:
   measured on a Dell PowerEdge, 25 of 34 Type 9 records carry a slot ID
   and NO bus address while 9 carry an address and no ID, so the two
   halves needed to join are almost never in the same record. Hence two
   join paths, neither privileged, with each row recording which one
   produced it. */

#define MAX_SLOT_ROWS 128

typedef enum { JOIN_NONE = 0, JOIN_ADDR, JOIN_SLOTNUM } JoinKind;

typedef struct {
    bool            have_addr;
    AxlPciAddr      addr;
    bool            have_slotnum;
    uint16_t        slotnum;

    bool            have_caps;
    AxlPciSlotCaps  caps;
    bool            dev_responds;      /* something answers at addr */

    bool            have_t9;
    /* Type 9's OWN claimed slot number, kept separate from the row's
       slotnum. The row's is set from the silicon's Physical Slot
       Number where a slot cap exists, so comparing SMBIOS against
       `slotnum` compares a value with itself -- which made the
       slot-number disagreement permanently unreachable. */
    bool            have_t9_slot_id;
    uint16_t        t9_slot_id;
    const char     *t9_designation;
    uint8_t         t9_usage;
    bool            t9_published_addr;

    bool            have_t41;
    const char     *t41_designation;

    bool            have_ns;
    uint64_t        ns_sun;

    JoinKind        join;
} SlotRow;

static SlotRow  g_rows[MAX_SLOT_ROWS];
static size_t   g_nrows;

static bool
addr_eq(AxlPciAddr a, AxlPciAddr b)
{
    return a.seg == b.seg && a.bus == b.bus
        && a.dev == b.dev && a.func == b.func;
}

static SlotRow *
row_by_addr(AxlPciAddr a)
{
    for (size_t i = 0; i < g_nrows; i++) {
        if (g_rows[i].have_addr && addr_eq(g_rows[i].addr, a)) {
            return &g_rows[i];
        }
    }
    return NULL;
}

static SlotRow *
row_by_slotnum(uint16_t n)
{
    for (size_t i = 0; i < g_nrows; i++) {
        if (g_rows[i].have_slotnum && g_rows[i].slotnum == n) {
            return &g_rows[i];
        }
    }
    return NULL;
}

static SlotRow *
row_new(void)
{
    if (g_nrows >= MAX_SLOT_ROWS) {
        return NULL;
    }
    SlotRow *r = &g_rows[g_nrows++];
    for (size_t i = 0; i < sizeof(*r); i++) {
        ((uint8_t *)r)[i] = 0;
    }
    return r;
}

/* Does anything answer at this address? A Type 9 record naming a bus
   address with nothing behind it is one of the disagreements this tool
   exists to surface, so "absent" has to be a fact we establish rather
   than assume. */
static bool
pci_responds(AxlPciAddr a)
{
    uint16_t vid = 0;
    return axl_pci_read_config_16(a, 0x00, &vid) == AXL_OK && vid != 0xFFFF;
}

/* Anchor: every downstream port that implements a slot. This is the
   hardware's own account, and the only source that is not a build-time
   assertion. */
static void
collect_slot_caps(void)
{
    AxlPciAddr *it = NULL;
    while ((it = axl_pci_next(it)) != NULL) {
        AxlPciSlotCaps c;
        if (axl_pci_read_slot_caps(*it, &c) != AXL_OK) {
            continue;
        }
        SlotRow *r = row_new();
        if (r == NULL) {
            return;
        }
        r->have_addr    = true;
        r->addr         = *it;
        r->have_caps    = true;
        r->caps         = c;
        r->dev_responds = true;   /* we just read its config space */
        r->have_slotnum = true;
        r->slotnum      = c.physical_slot_number;
    }
}

static void
collect_type9(void)
{
    AxlSmbiosHeader *h = NULL;
    while ((h = axl_smbios_find_next(9, h)) != NULL) {
        AxlSmbiosSystemSlot s;
        if (axl_smbios_read_system_slot(h, &s) != AXL_OK) {
            continue;
        }

        /* 0xFFFF / 0xFF are "not published", never a real address.
           Rendering them as 0xFFFF:0xFF:1F.7 would invent a device. */
        bool published = (s.segment_group != 0xFFFF) && (s.bus != 0xFF)
                         && (s.device_function != 0xFF);

        SlotRow *r = NULL;
        JoinKind how = JOIN_NONE;
        AxlPciAddr a = {0};

        if (published) {
            a.seg  = s.segment_group;
            a.bus  = s.bus;
            a.dev  = (uint8_t)((s.device_function >> 3) & 0x1F);
            a.func = (uint8_t)(s.device_function & 0x07);
            r = row_by_addr(a);
            how = JOIN_ADDR;
        }
        if (r == NULL && s.slot_id != 0) {
            r = row_by_slotnum(s.slot_id);
            if (r != NULL) {
                how = JOIN_SLOTNUM;
            }
        }
        if (r == NULL) {
            r = row_new();
            if (r == NULL) {
                return;
            }
            how = JOIN_NONE;
        }

        if (published && !r->have_addr) {
            r->have_addr = true;
            r->addr      = a;
        }
        if (!r->have_slotnum && s.slot_id != 0) {
            r->have_slotnum = true;
            r->slotnum      = s.slot_id;
        }
        if (s.slot_id != 0) {
            r->have_t9_slot_id = true;
            r->t9_slot_id      = s.slot_id;
        }
        r->have_t9           = true;
        r->t9_designation    = s.designation;
        r->t9_usage          = s.current_usage;
        r->t9_published_addr = published;
        if (r->join == JOIN_NONE) {
            r->join = how;
        }
        if (published && !r->dev_responds) {
            r->dev_responds = pci_responds(a);
        }
    }
}

static void
collect_type41(void)
{
    AxlSmbiosHeader *h = NULL;
    while ((h = axl_smbios_find_next(41, h)) != NULL) {
        AxlSmbiosOnboardDeviceExt d;
        if (axl_smbios_read_onboard_device_ext(h, &d) != AXL_OK) {
            continue;
        }
        if (d.segment_group == 0xFFFF || d.bus == 0xFF
            || d.device_function == 0xFF) {
            continue;
        }
        AxlPciAddr a;
        a.seg  = d.segment_group;
        a.bus  = d.bus;
        a.dev  = (uint8_t)((d.device_function >> 3) & 0x1F);
        a.func = (uint8_t)(d.device_function & 0x07);

        SlotRow *r = row_by_addr(a);
        if (r == NULL) {
            r = row_new();
            if (r == NULL) {
                return;
            }
            r->have_addr = true;
            r->addr      = a;
            r->join      = JOIN_ADDR;
        }
        r->have_t41        = true;
        r->t41_designation = d.reference_designation;
    }
}

/* The namespace joins by slot number, not by address: resolving a
   namespace device to a bus needs _BBN, which is a Method on every
   measured machine and on one resolves to a field inside an
   OperationRegion. _SUN needs no bus number at all. */
static void
collect_namespace(void)
{
    AxlAcpiHeader *t = axl_acpi_find("DSDT");
    for (int pass = 0; pass < 2; pass++) {
        while (t != NULL) {
            AxlAmlWalk walk;
            AxlAmlNode n;
            if (axl_aml_walk_begin(&walk, t) == AXL_OK) {
                while (axl_aml_walk_next(&walk, &n)) {
                    if (n.sun.kind != AXL_AML_VALUE_STATIC) {
                        continue;
                    }
                    SlotRow *r = row_by_slotnum((uint16_t)n.sun.value);
                    if (r == NULL) {
                        r = row_new();
                        if (r == NULL) {
                            return;
                        }
                        r->have_slotnum = true;
                        r->slotnum      = (uint16_t)n.sun.value;
                        r->join         = JOIN_SLOTNUM;
                    }
                    r->have_ns = true;
                    r->ns_sun  = n.sun.value;
                }
            }
            t = (pass == 0) ? NULL : axl_acpi_find_next("SSDT", t);
        }
        if (pass == 0) {
            t = axl_acpi_find_next("SSDT", NULL);
        }
    }
}


/* SMBIOS Type 9 current_usage values (SMBIOS spec Table 11). */
#define T9_USAGE_AVAILABLE  0x03
#define T9_USAGE_IN_USE     0x04

static int
render_slots(bool verbose)
{
    g_nrows = 0;
    collect_slot_caps();
    collect_type9();
    collect_type41();
    collect_namespace();

    if (g_nrows == 0) {
        axl_printf("no slots described by any source\n");
        return 0;
    }

    axl_printf("%-16s %-6s %-6s %-5s %-24s %s\n",
               "ADDRESS", "SLOT#", "PRESENT", "JOIN", "DESIGNATION", "NOTES");

    size_t disagreements = 0;
    for (size_t i = 0; i < g_nrows; i++) {
        SlotRow *r = &g_rows[i];
        char addr[AXL_PCI_ADDR_STR_MAX];

        if (r->have_addr) {
            axl_pci_addr_format(r->addr, addr, sizeof(addr));
        } else {
            addr[0] = '-'; addr[1] = '\0';
        }

        char slot[8];
        if (r->have_slotnum) {
            axl_snprintf(slot, sizeof(slot), "%u", r->slotnum);
        } else {
            slot[0] = '-'; slot[1] = '\0';
        }

        /* Presence Detect is the hardware's own live answer and the
           only column here that is not a build-time assertion. */
        const char *present = r->have_caps
                              ? (r->caps.presence_detect ? "yes" : "no")
                              : "?";

        const char *join = (r->join == JOIN_ADDR)    ? "addr"
                         : (r->join == JOIN_SLOTNUM) ? "slot"
                                                     : "-";

        const char *desig = r->t9_designation  ? r->t9_designation
                          : r->t41_designation ? r->t41_designation
                                               : "-";

        axl_printf("%-16s %-6s %-7s %-5s %-24s", addr, slot, present, join,
                   desig);

        /* --- the disagreements --------------------------------------
           Reported, never adjudicated: which source is right is a
           per-platform question the operator answers. */
        if (r->have_t9 && r->t9_published_addr && !r->dev_responds) {
            axl_printf(" [SMBIOS names an address with no device]");
            disagreements++;
        }
        if (r->have_t9 && r->have_caps && r->have_t9_slot_id
            && r->caps.physical_slot_number != r->t9_slot_id) {
            axl_printf(" [slot# SMBIOS %u vs silicon %u]",
                       r->t9_slot_id, r->caps.physical_slot_number);
            disagreements++;
        }
        if (r->have_t9 && r->have_caps
            && r->t9_usage == T9_USAGE_IN_USE && !r->caps.presence_detect) {
            axl_printf(" [SMBIOS says In Use, Presence Detect says empty]");
            disagreements++;
        }
        if (r->have_caps && !r->have_t9 && verbose) {
            axl_printf(" [slot in silicon, absent from SMBIOS]");
        }
        axl_printf("\n");
    }

    axl_printf("\n%zu row(s), %zu disagreement(s)\n", g_nrows, disagreements);
    return 0;
}

// ---------------------------------------------------------------------------

static int
run_lsacpi(AxlArgs *a)
{
    const char *sig     = axl_args_get_string(a, "signature");
    bool        verbose = axl_args_get_bool(a, "verbose");
    bool        json    = axl_args_get_bool(a, "json");
    bool        ns      = axl_args_get_bool(a, "namespace");

    if (axl_args_get_bool(a, "slots")) {
        return render_slots(verbose);
    }
    if (ns) {
        return render_namespace(verbose);
    }
    if (sig != NULL) {
        return render_one(sig, verbose);
    }
    return render_inventory(json);
}

AXL_TOOL_MAIN(lsacpi)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "lsacpi",
        .help        = "List and decode the ACPI tables the firmware published",
        .flags       = flags,
        .positionals = positional,
        .handler     = run_lsacpi,
    });
}
