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
 *   (default)   the inventory, one row per table, every header field
 *   <SIG>       decode EVERY table with that signature -- a server can
 *               publish nineteen SSDTs, ten of them identical in every
 *               header field, so picking the first would be a silent
 *               wrong answer
 *   --at ADDR   decode the one table at a physical address, which is the
 *               only key guaranteed to name a single table
 * Modifiers: -v also dump raw bytes, -j JSON.
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
      .help = "With <SIG>: also dump the table's raw bytes" },
    { .name = "at",        .type = AXL_ARG_STRING,
      .help = "Decode the table at this physical address (from the ADDRESS "
              "column) -- the only key that is unique when several tables "
              "share a signature" },
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
/* The creator ID rendered for a TEXT column. A zeroed creator becomes
   "-" rather than a blank, which is indistinguishable from padding --
   QEMU's BGRT zeroes it. JSON says the same thing with null instead,
   which is why this is text-only. */
static void
creator_text(char *dst, size_t dstlen, const AxlAcpiHeader *h)
{
    acpi_str(dst, dstlen, h->creator_id, 4);
    if (dst[0] == '\0' && dstlen > 1) {
        dst[0] = '-';
        dst[1] = '\0';
    }
}

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

/* Every --json mode emits ONE object on ONE line. Opening, closing,
   freeing and error-checking the document lives here so the four modes
   cannot drift in how they do it -- and so the truncation check is not
   four chances to forget it. */
typedef struct {
    AxlString     *buf;
    AxlJsonWriter  jw;
} JsonDoc;

static bool
json_doc_begin(JsonDoc *d)
{
    d->buf = axl_string_new("");
    if (d->buf == NULL) {
        axl_printerr("lsacpi: out of memory building JSON\n");
        return false;
    }
    axl_json_writer_init(&d->jw, d->buf, 0);
    axl_json_obj_begin(&d->jw);
    return true;
}

static int
json_doc_end(JsonDoc *d)
{
    axl_json_obj_end(&d->jw);
    axl_json_writer_finish(&d->jw);

    /* A writer that hit an OOM or a structural misuse goes quiet and
       keeps counting, so the buffer would otherwise print as if it were
       a whole document. Ask it. */
    int rc = 0;
    if (axl_json_writer_error(&d->jw)) {
        axl_printerr("lsacpi: JSON output is incomplete\n");
        rc = 1;
    } else {
        axl_printf("%s\n", axl_string_str(d->buf));
    }
    axl_string_free(d->buf);
    return rc;
}

/* ONE row shape, carrying every header field Linux prints at boot, and it
   needs no flag. The fields were split across a default row and a -v row
   until the split stopped paying: the interesting question on real firmware
   ("why 19 SSDTs?") is answered by CREATOR, and the only field that can tell
   two otherwise-identical tables apart is ADDRESS. Both were behind -v.

   75 columns of data under a 76-column header, so it fits an 80-column UEFI
   console. The address is 12 hex digits rather than dmesg's 16 -- the four
   leading zeros are what made the difference between fitting and wrapping,
   and %012lx still widens on its own if a table ever sits above 2^48.

   -v now means one thing: also dump the raw bytes. */
static void
print_header_line(void)
{
    axl_printf("SIG  ADDRESS        LENGTH REV OEM ID OEM TBL  OEM REV  "
               "CREATOR CRTR REV CHK\n");
}

static void
print_row_text(const AxlAcpiHeader *h)
{
    char sig[5], oem[7], oemtab[9], creator[5];
    acpi_str(sig, sizeof(sig), h->signature, 4);

    /* The header pointer IS the physical address: UEFI hands tables over
       identity-mapped, which is the address Linux prints for the same table. */
    if (is_facs(h)) {
        /* Dashes, not zeroes: FACS has no revision, no OEM strings, no
           creator and no whole-table checksum, and a zero would read as a
           value. Its ADDRESS is real -- that is the pointer we already hold,
           not a header field it lacks. */
        axl_printf("%-4s 0x%012lx %06x %3s %-6s %-8s %-8s %-7s %-8s %s\n",
                   sig, (unsigned long)(uintptr_t)h, table_length(h),
                   "-", "-", "-", "-", "-", "-", "-");
        return;
    }
    acpi_str(oem, sizeof(oem), h->oem_id, 6);
    acpi_str(oemtab, sizeof(oemtab), h->oem_table_id, 8);
    creator_text(creator, sizeof(creator), h);

    axl_printf("%-4s 0x%012lx %06x %3u %-6s %-8s %08x %-7s %08x %s\n",
               sig, (unsigned long)(uintptr_t)h, h->length, h->revision,
               oem, oemtab, h->oem_revision, creator, h->creator_revision,
               axl_acpi_checksum_ok(h) ? "OK" : "BAD");
}

/* Emitted through AxlJsonWriter rather than printf: every string here
   is firmware-controlled, and an OEM ID carrying a quote, a backslash
   or a control byte would otherwise produce unparseable output.

   The KEYS only, without the enclosing object: the inventory row and the
   single-table view then emit the same header schema, and the latter can
   add its decode beside these rather than nesting a second copy. */
static void
emit_header_fields(AxlJsonWriter *jw, const AxlAcpiHeader *h)
{
    char sig[5], oem[7], oemtab[9], creator[5];
    acpi_str(sig, sizeof(sig), h->signature, 4);

    axl_json_key(jw, "signature");
    axl_json_str(jw, sig);
    axl_json_key(jw, "length");
    axl_json_uint(jw, table_length(h));

    if (is_facs(h)) {
        axl_json_key(jw, "revision");    axl_json_null(jw);
        axl_json_key(jw, "oem_id");      axl_json_null(jw);
        axl_json_key(jw, "oem_table_id"); axl_json_null(jw);
        axl_json_key(jw, "oem_revision");     axl_json_null(jw);
        axl_json_key(jw, "creator_id");       axl_json_null(jw);
        axl_json_key(jw, "creator_revision"); axl_json_null(jw);
        axl_json_key(jw, "address");
        axl_json_uint(jw, (uint64_t)(uintptr_t)h);
        axl_json_key(jw, "checksum");    axl_json_null(jw);
        return;
    }

    acpi_str(oem, sizeof(oem), h->oem_id, 6);
    acpi_str(oemtab, sizeof(oemtab), h->oem_table_id, 8);
    axl_json_key(jw, "revision");     axl_json_uint(jw, h->revision);
    axl_json_key(jw, "oem_id");       axl_json_str(jw, oem);
    axl_json_key(jw, "oem_table_id"); axl_json_str(jw, oemtab);
    /* Always emitted, verbose or not: JSON has no column budget, and a
       consumer diffing two machines wants every header field. */
    acpi_str(creator, sizeof(creator), h->creator_id, 4);
    axl_json_key(jw, "oem_revision");     axl_json_uint(jw, h->oem_revision);
    axl_json_key(jw, "creator_id");
    if (creator[0] == '\0') {
        axl_json_null(jw);
    } else {
        axl_json_str(jw, creator);
    }
    axl_json_key(jw, "creator_revision"); axl_json_uint(jw, h->creator_revision);
    axl_json_key(jw, "address");          axl_json_uint(jw, (uint64_t)(uintptr_t)h);
    axl_json_key(jw, "checksum");
    axl_json_str(jw, axl_acpi_checksum_ok(h) ? "OK" : "BAD");
}

static void
print_row_json(AxlJsonWriter *jw, const AxlAcpiHeader *h)
{
    axl_json_obj_begin(jw);
    emit_header_fields(jw, h);
    axl_json_obj_end(jw);
}

static int
render_inventory(bool json)
{
    AxlAcpiHeader *h = NULL;
    size_t         n = 0;

    JsonDoc d;

    if (json) {
        if (!json_doc_begin(&d)) {
            return 1;
        }
        axl_json_key(&d.jw, "tables");
        axl_json_arr_begin(&d.jw);
    } else {
        print_header_line();
    }

    while ((h = axl_acpi_next(h)) != NULL) {
        if (json) {
            print_row_json(&d.jw, h);
        } else {
            print_row_text(h);
        }
        n++;
    }

    if (json) {
        axl_json_arr_end(&d.jw);
        axl_json_key(&d.jw, "count");
        axl_json_uint(&d.jw, n);
        return json_doc_end(&d);
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

/* The table's own bytes, lowercase hex. JSON has no byte type, and a
   consumer that asked for one table by name and got no typed decode
   wants the contents -- the same choice the text view makes when it
   falls back to a hexdump instead of a one-line shrug. */
static void
emit_bytes_hex(AxlJsonWriter *jw, const AxlAcpiHeader *h)
{
    static const char digits[] = "0123456789abcdef";
    uint32_t          len = table_length(h);
    char             *hex = axl_malloc((size_t)len * 2 + 1);

    if (hex == NULL) {
        axl_json_null(jw);
        return;
    }
    const uint8_t *b = (const uint8_t *)h;
    for (uint32_t i = 0; i < len; i++) {
        hex[i * 2]     = digits[b[i] >> 4];
        hex[i * 2 + 1] = digits[b[i] & 0x0fu];
    }
    hex[(size_t)len * 2] = '\0';
    axl_json_str(jw, hex);
    axl_free(hex);
}

/* false when the table is present but will not decode -- the same condition
   render_mcfg() exits 1 on. A JSON consumer must not read success out of a
   document the text view calls an error. */
static bool
emit_mcfg_json(AxlJsonWriter *jw)
{
    AxlAcpiMcfg m;

    if (axl_acpi_read_mcfg(&m) != AXL_OK) {
        axl_json_null(jw);
        return false;
    }
    axl_json_obj_begin(jw);
    axl_json_key(jw, "windows");
    axl_json_arr_begin(jw);
    for (size_t i = 0; i < m.count; i++) {
        axl_json_obj_begin(jw);
        axl_json_key(jw, "segment");
        axl_json_uint(jw, m.segments[i].segment);
        axl_json_key(jw, "start_bus");
        axl_json_uint(jw, m.segments[i].start_bus);
        axl_json_key(jw, "end_bus");
        axl_json_uint(jw, m.segments[i].end_bus);
        axl_json_key(jw, "base");
        axl_json_uint(jw, m.segments[i].base_addr);
        axl_json_obj_end(jw);
    }
    axl_json_arr_end(jw);
    axl_json_obj_end(jw);
    return true;
}

/* The same header schema the inventory emits, plus whatever asking for
   ONE table adds: the typed decode where there is one, the raw bytes
   where there is not. */
static bool
emit_table_json(AxlJsonWriter *jw, const AxlAcpiHeader *h)
{
    char sig[5];
    bool ok = true;

    acpi_str(sig, sizeof(sig), h->signature, 4);
    axl_json_obj_begin(jw);
    emit_header_fields(jw, h);
    if (axl_strcmp(sig, "MCFG") == 0) {
        axl_json_key(jw, "mcfg");
        ok = emit_mcfg_json(jw);
    } else {
        axl_json_key(jw, "bytes");
        emit_bytes_hex(jw, h);
    }
    axl_json_obj_end(jw);
    return ok;
}

/* ALWAYS the inventory's envelope -- {"tables":[...],"count":N} -- even for
   one table. A consumer then decodes ONE shape whichever view produced the
   document, and a signature that matches nineteen tables is not a different
   schema from one that matches one. Pass a signature to take every match, or
   a header to take exactly that table. */
static int
render_many_json(const char *sig, const AxlAcpiHeader *only)
{
    JsonDoc d;
    bool    ok = true;
    size_t  n  = 0;

    if (!json_doc_begin(&d)) {
        return 1;
    }
    axl_json_key(&d.jw, "tables");
    axl_json_arr_begin(&d.jw);

    if (only != NULL) {
        ok = emit_table_json(&d.jw, only);
        n = 1;
    } else {
        AxlAcpiHeader *h = NULL;
        while ((h = axl_acpi_find_next(sig, h)) != NULL) {
            if (!emit_table_json(&d.jw, h)) {
                ok = false;
            }
            n++;
        }
    }

    axl_json_arr_end(&d.jw);
    axl_json_key(&d.jw, "count");
    axl_json_uint(&d.jw, n);
    /* The document is still emitted -- a consumer gets "mcfg": null and can
       see WHICH table failed -- but the exit status matches the text view. */
    int rc = json_doc_end(&d);
    return (rc != 0 || !ok) ? 1 : 0;
}

/* The decode for ONE table: its typed reader if it has one, its bytes if
   not. -v means "also dump the bytes", so it adds a hexdump to a table that
   already decoded and changes nothing for one that had no decode to give --
   there the bytes ARE the answer, and always were. */
static int
render_decode(const AxlAcpiHeader *h, const char *sig, bool verbose)
{
    if (axl_strcmp(sig, "MCFG") == 0) {
        int rc = render_mcfg();
        if (verbose) {
            axl_hexdump(sig, h, table_length(h), 0, 0);
        }
        return rc;
    }
    return render_hexdump(h, sig);
}

/* The table at an exact physical address, or NULL. */
static AxlAcpiHeader *
table_at(uint64_t addr)
{
    AxlAcpiHeader *h = NULL;

    while ((h = axl_acpi_next(h)) != NULL) {
        if ((uint64_t)(uintptr_t)h == addr) {
            return h;
        }
    }
    return NULL;
}

/* EVERY table with this signature, not the first.

   A 2-socket Grace server publishes 19 SSDTs, and ten of them fall into
   groups identical on every ACPI header field -- one 4-way tie and three
   2-way ties across length, revision, OEM ID, OEM table ID, OEM revision,
   creator and creator revision. Decoding the first and saying nothing about
   the other eighteen is a silent wrong answer on exactly the machine this
   view exists for, and no header field can name the one you meant. Only the
   address can, which is what --at is for. */
static int
render_one(const char *want, bool verbose, bool json)
{
    char sig[5];
    acpi_str(sig, sizeof(sig), want, 4);
    if (axl_strlen(sig) != 4) {
        axl_printerr("lsacpi: signature must be exactly 4 characters: '%s'\n",
                     want);
        return 1;
    }

    size_t         n = 0;
    AxlAcpiHeader *h = NULL;
    while ((h = axl_acpi_find_next(sig, h)) != NULL) {
        n++;
    }
    if (n == 0) {
        axl_printerr("lsacpi: no table with signature '%s'\n", sig);
        return 1;
    }

    if (json) {
        return render_many_json(sig, NULL);
    }

    int rc = 0;
    print_header_line();
    h = NULL;
    while ((h = axl_acpi_find_next(sig, h)) != NULL) {
        print_row_text(h);
        if (render_decode(h, sig, verbose) != 0) {
            rc = 1;
        }
        axl_printf("\n");
    }
    /* Said even for one match: "1 table(s)" is how a reader learns the view
       counts at all, and it is the line that would have been missing on the
       machine with nineteen. */
    axl_printf("%zu table(s) with signature %s\n", n, sig);
    return rc;
}

/* One table, named by the only key that is guaranteed unique. */
static int
render_at(const char *spec, bool verbose, bool json)
{
    uint64_t addr = 0;

    if (axl_str_to_u64(spec, 0, &addr, NULL) != AXL_OK) {
        axl_printerr("lsacpi: --at wants an address, not '%s'\n", spec);
        return 1;
    }

    AxlAcpiHeader *h = table_at(addr);
    if (h == NULL) {
        /* Named back in the spelling the user typed AND in canonical hex,
           because "--at 0x1fb79000 found nothing" is a different debugging
           session from "--at 33099776 found nothing". */
        axl_printerr("lsacpi: no ACPI table at %s (0x%012lx)\n",
                     spec, (unsigned long)addr);
        return 1;
    }

    char sig[5];
    acpi_str(sig, sizeof(sig), h->signature, 4);

    if (json) {
        return render_many_json(NULL, h);
    }
    print_header_line();
    print_row_text(h);
    return render_decode(h, sig, verbose);
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

static const char *
aml_kind_name(AxlAmlValueKind k)
{
    switch (k) {
    case AXL_AML_VALUE_STATIC:      return "static";
    case AXL_AML_VALUE_METHOD:      return "method";
    case AXL_AML_VALUE_NON_INTEGER: return "non-integer";
    case AXL_AML_VALUE_ABSENT:
    default:                        return "absent";
    }
}

/* Every value carries its KIND, not just a number. "declared as a Method
   and therefore unreadable without executing AML" is a different fact
   from "not declared at all", and a bare null would conflate the two --
   which is the distinction axl-acpi.h tells callers to branch on. */
static void
emit_aml_value(AxlJsonWriter *jw, const char *key, const AxlAmlValue *v)
{
    axl_json_key(jw, key);
    axl_json_obj_begin(jw);
    axl_json_key(jw, "kind");
    axl_json_str(jw, aml_kind_name(v->kind));
    axl_json_key(jw, "value");
    if (v->kind == AXL_AML_VALUE_STATIC) {
        axl_json_uint(jw, v->value);
    } else {
        axl_json_null(jw);
    }
    axl_json_obj_end(jw);
}

static void
emit_device_json(AxlJsonWriter *jw, const AxlAmlNode *n)
{
    /* _PLD is a Buffer, so the walker reports presence only. It is given
       the same {kind,value} shape as the integers rather than a bare
       bool, so a consumer decodes one row type, not two. */
    const AxlAmlValue pld = { .kind = n->pld_kind, .value = 0 };

    axl_json_obj_begin(jw);
    axl_json_key(jw, "path");
    axl_json_str(jw, n->path);
    axl_json_key(jw, "conditional");
    axl_json_bool(jw, n->conditional);
    axl_json_key(jw, "path_truncated");
    axl_json_bool(jw, n->path_truncated);
    emit_aml_value(jw, "_ADR", &n->adr);
    emit_aml_value(jw, "_SUN", &n->sun);
    emit_aml_value(jw, "_UID", &n->uid);
    emit_aml_value(jw, "_SEG", &n->seg);
    emit_aml_value(jw, "_BBN", &n->bbn);
    emit_aml_value(jw, "_PLD", &pld);
    axl_json_obj_end(jw);
}

static size_t
walk_one_table(AxlAcpiHeader *t, bool verbose, bool *any_incomplete,
               AxlJsonWriter *jw)
{
    AxlAmlWalk walk;
    AxlAmlNode n;
    size_t     count = 0;

    if (axl_aml_walk_begin(&walk, t) != AXL_OK) {
        return 0;
    }
    while (axl_aml_walk_next(&walk, &n)) {
        count++;
        if (jw != NULL) {
            /* No column budget in JSON, so the -v filter below does not
               apply: a machine consumer wants every device, and the
               summary count already counts every device either way. */
            emit_device_json(jw, &n);
            continue;
        }
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
render_namespace(bool verbose, bool json)
{
    AxlAcpiHeader *t;
    size_t         devices = 0, tables = 0;
    bool           incomplete = false;
    JsonDoc        d;
    AxlJsonWriter *jw = NULL;

    if (json) {
        if (!json_doc_begin(&d)) {
            return 1;
        }
        jw = &d.jw;
        axl_json_key(jw, "devices");
        axl_json_arr_begin(jw);
    } else {
        axl_printf("%-32s %-10s %-10s %-10s %-10s %-10s %s\n",
                   "PATH", "_ADR", "_SUN", "_UID", "_SEG", "_BBN", "_PLD");
    }

    t = axl_acpi_find("DSDT");
    if (t != NULL) {
        devices += walk_one_table(t, verbose, &incomplete, jw);
        tables++;
    }
    t = NULL;
    while ((t = axl_acpi_find_next("SSDT", t)) != NULL) {
        devices += walk_one_table(t, verbose, &incomplete, jw);
        tables++;
    }

    if (tables == 0) {
        axl_printerr("lsacpi: no DSDT or SSDT to walk\n");
    }

    if (json) {
        axl_json_arr_end(jw);
        axl_json_key(jw, "count");
        axl_json_uint(jw, devices);
        axl_json_key(jw, "tables");
        axl_json_uint(jw, tables);
        /* Reported, not implied by a short array: a truncated walk still
           yields devices, and a consumer must be able to tell that set
           apart from a complete one. */
        axl_json_key(jw, "incomplete");
        axl_json_bool(jw, incomplete);
        int rc = json_doc_end(&d);
        return (rc != 0 || tables == 0) ? 1 : 0;
    }

    if (tables == 0) {
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

/* WHICH disagreements a row has, in ONE place. The text renderer counts
   them for its summary line and the JSON renderer lists them per row; if
   each decided independently, the count and the list would eventually
   disagree about disagreements.

   "slot in silicon, absent from SMBIOS" is deliberately NOT here. It is
   a note, not a disagreement, which is why it has never been counted --
   slot_silicon_only() reports it separately. */
typedef enum {
    SLOT_DIS_ADDR_NO_DEVICE,
    SLOT_DIS_SLOTNUM_MISMATCH,
    SLOT_DIS_IN_USE_VS_EMPTY
} SlotDisagreement;

#define SLOT_DIS_MAX 3

static size_t
slot_disagreements(const SlotRow *r, SlotDisagreement *out)
{
    size_t n = 0;

    if (r->have_t9 && r->t9_published_addr && !r->dev_responds) {
        out[n++] = SLOT_DIS_ADDR_NO_DEVICE;
    }
    if (r->have_t9 && r->have_caps && r->have_t9_slot_id
        && r->caps.physical_slot_number != r->t9_slot_id) {
        out[n++] = SLOT_DIS_SLOTNUM_MISMATCH;
    }
    if (r->have_t9 && r->have_caps
        && r->t9_usage == T9_USAGE_IN_USE && !r->caps.presence_detect) {
        out[n++] = SLOT_DIS_IN_USE_VS_EMPTY;
    }
    return n;
}

static const char *
slot_disagreement_code(SlotDisagreement d)
{
    switch (d) {
    case SLOT_DIS_ADDR_NO_DEVICE:   return "smbios_address_has_no_device";
    case SLOT_DIS_SLOTNUM_MISMATCH: return "slot_number_mismatch";
    case SLOT_DIS_IN_USE_VS_EMPTY:  return "in_use_but_no_presence_detect";
    default:                        return "unknown";
    }
}

static bool
slot_silicon_only(const SlotRow *r)
{
    return r->have_caps && !r->have_t9;
}

static const char *
slot_designation(const SlotRow *r)
{
    return r->t9_designation  ? r->t9_designation
         : r->t41_designation ? r->t41_designation
                              : NULL;
}

static void
emit_slot_json(AxlJsonWriter *jw, const SlotRow *r,
               const SlotDisagreement *dis, size_t nd)
{
    axl_json_obj_begin(jw);

    axl_json_key(jw, "address");
    if (r->have_addr) {
        char addr[AXL_PCI_ADDR_STR_MAX];
        axl_pci_addr_format(r->addr, addr, sizeof(addr));
        axl_json_str(jw, addr);
    } else {
        axl_json_null(jw);
    }

    axl_json_key(jw, "slot_number");
    if (r->have_slotnum) {
        axl_json_uint(jw, r->slotnum);
    } else {
        axl_json_null(jw);
    }

    /* Presence Detect is the hardware's own live answer. null means no
       silicon source for this row -- which is NOT the same as "empty",
       and the text view spells that difference '?' vs 'no'. */
    axl_json_key(jw, "present");
    if (r->have_caps) {
        axl_json_bool(jw, r->caps.presence_detect);
    } else {
        axl_json_null(jw);
    }

    axl_json_key(jw, "join");
    if (r->join == JOIN_ADDR) {
        axl_json_str(jw, "addr");
    } else if (r->join == JOIN_SLOTNUM) {
        axl_json_str(jw, "slot");
    } else {
        axl_json_null(jw);
    }

    axl_json_key(jw, "designation");
    const char *desig = slot_designation(r);
    if (desig != NULL) {
        axl_json_str(jw, desig);
    } else {
        axl_json_null(jw);
    }

    axl_json_key(jw, "silicon_only");
    axl_json_bool(jw, slot_silicon_only(r));

    /* Codes, not prose: the disagreements are the reason to ask for this
       view, so a script should not have to grep bracketed English. */
    axl_json_key(jw, "disagreements");
    axl_json_arr_begin(jw);
    for (size_t i = 0; i < nd; i++) {
        axl_json_str(jw, slot_disagreement_code(dis[i]));
    }
    axl_json_arr_end(jw);

    axl_json_obj_end(jw);
}

static int
render_slots(bool verbose, bool json)
{
    g_nrows = 0;
    collect_slot_caps();
    collect_type9();
    collect_type41();
    collect_namespace();

    JsonDoc d;

    if (json) {
        if (!json_doc_begin(&d)) {
            return 1;
        }
        axl_json_key(&d.jw, "slots");
        axl_json_arr_begin(&d.jw);
    } else {
        if (g_nrows == 0) {
            axl_printf("no slots described by any source\n");
            return 0;
        }
        axl_printf("%-16s %-6s %-6s %-5s %-24s %s\n",
                   "ADDRESS", "SLOT#", "PRESENT", "JOIN", "DESIGNATION",
                   "NOTES");
    }

    size_t disagreements = 0;
    for (size_t i = 0; i < g_nrows; i++) {
        SlotRow          *r = &g_rows[i];
        SlotDisagreement  dis[SLOT_DIS_MAX];
        size_t            nd = slot_disagreements(r, dis);
        char              addr[AXL_PCI_ADDR_STR_MAX];

        disagreements += nd;

        if (json) {
            emit_slot_json(&d.jw, r, dis, nd);
            continue;
        }

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

        const char *desig = slot_designation(r);

        axl_printf("%-16s %-6s %-7s %-5s %-24s", addr, slot, present, join,
                   desig != NULL ? desig : "-");

        /* --- the disagreements --------------------------------------
           Reported, never adjudicated: which source is right is a
           per-platform question the operator answers. */
        for (size_t j = 0; j < nd; j++) {
            switch (dis[j]) {
            case SLOT_DIS_ADDR_NO_DEVICE:
                axl_printf(" [SMBIOS names an address with no device]");
                break;
            case SLOT_DIS_SLOTNUM_MISMATCH:
                axl_printf(" [slot# SMBIOS %u vs silicon %u]",
                           r->t9_slot_id, r->caps.physical_slot_number);
                break;
            case SLOT_DIS_IN_USE_VS_EMPTY:
                axl_printf(" [SMBIOS says In Use, Presence Detect says empty]");
                break;
            default:
                break;
            }
        }
        if (slot_silicon_only(r) && verbose) {
            axl_printf(" [slot in silicon, absent from SMBIOS]");
        }
        axl_printf("\n");
    }

    if (json) {
        axl_json_arr_end(&d.jw);
        axl_json_key(&d.jw, "count");
        axl_json_uint(&d.jw, g_nrows);
        axl_json_key(&d.jw, "disagreements");
        axl_json_uint(&d.jw, disagreements);
        return json_doc_end(&d);
    }

    axl_printf("\n%zu row(s), %zu disagreement(s)\n", g_nrows, disagreements);
    return 0;
}

// ---------------------------------------------------------------------------

static int
run_lsacpi(AxlArgs *a)
{
    const char *sig     = axl_args_get_string(a, "signature");
    const char *at      = axl_args_get_string(a, "at");
    bool        verbose = axl_args_get_bool(a, "verbose");
    bool        json    = axl_args_get_bool(a, "json");
    bool        ns      = axl_args_get_bool(a, "namespace");

    if (axl_args_get_bool(a, "slots")) {
        return render_slots(verbose, json);
    }
    if (ns) {
        return render_namespace(verbose, json);
    }
    if (at != NULL) {
        return render_at(at, verbose, json);
    }
    if (sig != NULL) {
        return render_one(sig, verbose, json);
    }
    /* -v has exactly one meaning now -- "also dump the raw bytes" -- and the
       inventory has no bytes to dump. Refused rather than quietly ignored:
       a flag that vanishes is the defect this tool spent a day removing. */
    if (verbose) {
        axl_printerr("lsacpi: -v applies to a single table; give a signature "
                     "or --at\n");
        return 1;
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
