/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file memspd.c
    memspd — read JEDEC SPD content from DDR4/DDR5 DIMMs over the
    platform SMBus.

    Build with axl-cc:
      axl-cc memspd.c -o memspd.efi

    Usage:
      memspd [--jedec-file PATH] list                List populated DIMM slots
      memspd [--jedec-file PATH] show   <slot-hex>   Decoded fields
      memspd [--jedec-file PATH] decode <slot-hex>   Raw hex dump + decoded fields

    Vendor-name lookup is data-driven via @ref axl_path_companion at
    startup: try `jedec.json5` next to the .efi binary first, then the
    current working directory. `--jedec-file` overrides both. When no
    sidecar loads, the tool prints raw 16-bit JEP-106 codes.
**/

#include <axl.h>
#include <axl/axl-spd.h>

// ---------------------------------------------------------------------------
// JEDEC vendor table — code -> name, backed by AxlHashTable
// ---------------------------------------------------------------------------

static AxlHashTable *g_jedec      = NULL;
static char          g_jedec_path[256];

static bool
try_load_jedec(
    const char  *path
    )
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    AxlJsonReader r       = { 0 };
    void         *raw     = NULL;
    if (!axl_json_load_file_flags(path, AXL_JSON_PARSER_JSON5,
                                  &r, &raw, NULL)) {
        return false;
    }

    AxlHashTable *table = axl_hash_table_new_full(
        axl_direct_hash, axl_direct_equal, NULL, axl_free_impl);
    if (table == NULL) {
        axl_json_free(&r);
        axl_free(raw);
        return false;
    }

    AxlJsonArrayIter it;
    if (axl_json_array_begin(&r, "vendors", &it)) {
        AxlJsonReader entry;
        while (axl_json_array_next(&it, &entry)) {
            uint64_t code64 = 0;
            char     name[64] = "";

            if (!axl_json_get_uint(&entry, "code", &code64)
                || code64 == 0 || code64 > 0xFFFF
                || !axl_json_get_string(&entry, "name", name, sizeof(name)))
            {
                continue;
            }
            uint16_t code = (uint16_t)code64;
            char *name_owned = axl_strdup(name);
            if (name_owned == NULL) {
                continue;
            }
            if (axl_hash_table_insert(table,
                                      (void *)(uintptr_t)code,
                                      name_owned) < 0) {
                axl_free(name_owned);
            }
        }
    }

    axl_json_free(&r);
    axl_free(raw);

    g_jedec = table;
    axl_strlcpy(g_jedec_path, path, sizeof(g_jedec_path));
    return true;
}

static void
load_jedec_table_from_args(
    AxlArgs  *a
    )
{
    if (g_jedec != NULL) {
        return;
    }
    const char *override_path = axl_args_get_string(a, "jedec-file");
    if (override_path != NULL && try_load_jedec(override_path)) {
        return;
    }
    char *companion = axl_path_companion(axl_app_argv0(), "jedec.json5");
    if (companion != NULL) {
        bool loaded = try_load_jedec(companion);
        axl_free(companion);
        if (loaded) {
            return;
        }
    }
    (void)try_load_jedec("jedec.json5");
}

static const char *
jedec_lookup(
    uint16_t  code
    )
{
    if (code == 0 || g_jedec == NULL) {
        return NULL;
    }
    return (const char *)axl_hash_table_lookup(g_jedec, (void *)(uintptr_t)code);
}

// ---------------------------------------------------------------------------
// Pretty printers
// ---------------------------------------------------------------------------

static const char *
ddr_label(
    uint8_t  gen
    )
{
    switch (gen) {
        case 4:  return "DDR4";
        case 5:  return "DDR5";
        default: return "Unknown";
    }
}

static void
print_mfg(
    const char  *label,
    uint16_t     code
    )
{
    if (code == 0) {
        axl_printf("  %-22s (unset)\n", label);
        return;
    }
    const char *name = jedec_lookup(code);
    if (name != NULL) {
        axl_printf("  %-22s %s (0x%04X)\n", label, name, code);
    } else {
        axl_printf("  %-22s 0x%04X (no JEDEC table entry)\n", label, code);
    }
}

static void
print_info(
    uint8_t            addr,
    const AxlSpdInfo  *info
    )
{
    axl_printf("Slot 0x%02X (%s)\n", addr, ddr_label(info->ddr_generation));
    if (info->ddr_generation == 0) {
        axl_printf("  (slot responded but contains no recognised SPD payload)\n");
        return;
    }
    if (info->capacity_bytes == 0) {
        axl_printf("  %-22s (not decodable)\n", "Capacity:");
    } else {
        char cap[40];
        axl_format_bytes(info->capacity_bytes, cap, sizeof(cap));
        axl_printf("  %-22s %s\n", "Capacity:", cap);
    }
    if (info->speed_mts != 0) {
        axl_printf("  %-22s %u MT/s\n", "Speed:", info->speed_mts);
    }
    axl_printf("  %-22s %s\n", "ECC:", info->has_ecc ? "yes" : "no");
    axl_printf("  %-22s %s\n", "Registered:", info->registered ? "yes" : "no");
    print_mfg("Module manufacturer:", info->mfg_code_module);
    print_mfg("DRAM manufacturer:",   info->mfg_code_dram);
    if (info->part_number[0] != '\0') {
        axl_printf("  %-22s %s\n", "Part number:", info->part_number);
    }
    if (info->serial != 0) {
        axl_printf("  %-22s 0x%08X\n", "Serial:", info->serial);
    }
    if (info->mfg_year != 0) {
        axl_printf("  %-22s %u/W%02u (location 0x%02X)\n", "Manufactured:",
                   info->mfg_year, info->mfg_week, info->mfg_location);
    }
}

// ---------------------------------------------------------------------------
// Verbs
// ---------------------------------------------------------------------------

static int
do_list(
    AxlArgs  *a
    )
{
    (void)a;
    int      found = 0;
    uint8_t *slot  = NULL;
    while ((slot = axl_spd_next(slot)) != NULL) {
        AxlSpdInfo info;
        if (axl_spd_read(*slot, &info) != 0) {
            axl_printf("Slot 0x%02X: read failed\n", *slot);
            continue;
        }
        if (info.ddr_generation == 0) {
            axl_printf("Slot 0x%02X  unknown SPD (key byte 0x00)\n", *slot);
        } else {
            const char *vendor = jedec_lookup(info.mfg_code_module);
            const char *part   = info.part_number[0] ? info.part_number : "(no part #)";
            axl_printf("Slot 0x%02X  %-4s  %-12s  %s\n",
                       *slot,
                       ddr_label(info.ddr_generation),
                       vendor != NULL ? vendor : "vendor=?",
                       part);
        }
        found++;
    }
    if (found == 0) {
        axl_printf("No populated DIMM slots detected on the SMBus.\n");
        return 1;
    }
    return 0;
}

static int
do_show(
    AxlArgs  *a
    )
{
    uint8_t addr = (uint8_t)axl_args_get_uint(a, "slot");
    AxlSpdInfo info;
    if (axl_spd_read(addr, &info) != 0) {
        axl_printf("Slot 0x%02X: SPD read failed (slot empty or I/O error)\n", addr);
        return 2;
    }
    print_info(addr, &info);
    if (g_jedec != NULL) {
        axl_printf("\n(JEDEC table: %zu entries from %s)\n",
                   axl_hash_table_size(g_jedec), g_jedec_path);
    } else {
        axl_printf("\n(JEDEC table not loaded — pass --jedec-file to resolve vendor codes)\n");
    }
    return 0;
}

static int
do_decode(
    AxlArgs  *a
    )
{
    uint8_t addr = (uint8_t)axl_args_get_uint(a, "slot");
    uint8_t raw[AXL_SPD_RAW_MAX];
    size_t  raw_len = 0;
    if (axl_spd_dump_raw(addr, raw, sizeof(raw), &raw_len) != 0 || raw_len == 0) {
        axl_printf("Slot 0x%02X: raw read failed\n", addr);
        return 2;
    }
    axl_printf("Slot 0x%02X — %zu bytes captured\n\n", addr, raw_len);
    axl_hexdump(NULL, raw, raw_len, 16, 1);
    axl_printf("\n");

    AxlSpdInfo info;
    if (axl_spd_decode(raw, raw_len, &info) == 0) {
        print_info(addr, &info);
    } else {
        axl_printf("(decode failed)\n");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// AxlArgs declaration
// ---------------------------------------------------------------------------

static const AxlArgDesc kSlotArg[] = {
    { .name = "slot", .type = AXL_ARG_U8, .base = 0,
      .min = AXL_SPD_ADDR_FIRST, .max = AXL_SPD_ADDR_LAST,
      .required = true,
      .help = "SMBus slot address (hex)" },
    {0}
};

static const AxlVerb kVerbs[] = {
    { .name = "list",   .handler = do_list,
      .help = "One-line summary per populated slot" },
    { .name = "show",   .handler = do_show,
      .positionals = kSlotArg,
      .help = "Decoded SPD fields for one slot" },
    { .name = "decode", .handler = do_decode,
      .positionals = kSlotArg,
      .help = "Raw hex dump + decoded fields" },
    {0}
};

static const AxlArgDesc kGlobalFlags[] = {
    { .name = "jedec-file", .short_name = 'j', .type = AXL_ARG_STRING,
      .help = "Path to JEDEC vendor JSON sidecar" },
    {0}
};

int
main(
    int    argc,
    char **argv
    )
{
    axl_diag_startup(argc, argv);

    int rc = axl_args_run(argc, argv, &(AxlArgsApp){
        .name         = "memspd",
        .help         = "Read JEDEC SPD content from DDR4/DDR5 DIMMs",
        .global_flags = kGlobalFlags,
        .verbs        = kVerbs,
        .pre_run      = load_jedec_table_from_args,
    });

    if (g_jedec != NULL) {
        axl_hash_table_free(g_jedec);
        g_jedec = NULL;
    }
    return rc;
}
