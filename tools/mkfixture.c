/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file mkfixture.c
    Hardware-fixture capture tool (HF2 of the Hardware Fixture
    Capture & Replay design — see docs/AXL-Hardware-Fixture-Design.md).

    Walks the running platform's UEFI Configuration Table, ACPI
    table set, and other firmware-published structures, writing a
    fixture directory whose layout matches what `axl-emulate` (HF3)
    knows how to replay. Run from a UEFI shell on real hardware
    (boot a uefi-devkit USB) or in QEMU for plumbing validation:

      mkfixture fs0:\fixtures\my-laptop
      mkfixture hostfs:\my-laptop                  # via run-qemu.sh --mount

    HF2.1 (this version) captures:
      smbios.bin          raw SMBIOS structure region (no entry-point
                          prefix; QEMU `-smbios file=` consumable —
                          NOT directly interchangeable with
                          `dmidecode --from-dump`; see design doc
                          §"SMBIOS format note" for why)
      acpi/<sig>.dat      every ACPI table from RSDT/XSDT, indexed
                          by 4-byte signature
      manifest.json       basic platform metadata (vendor, product,
                          BIOS revision, capture date)

    HF2.2+ adds: pci.json, usb.json + usb/, video.json + edid/,
    cpu.json, net.json, esrt.json, nvme/, plus alternative write
    targets (HTTP POST).
**/

#include <axl.h>
#include <axl/axl-acpi.h>
#include <axl/axl-fs.h>
#include <axl/axl-path.h>
#include <axl/axl-smbios.h>

// ---------------------------------------------------------------------------
// SMBIOS dump
// ---------------------------------------------------------------------------

static int
dump_smbios(
    const char *fixture_dir
    )
{
    /* QEMU's `-smbios file=PATH` reads PATH as raw SMBIOS structure
       data — type/length/handle records terminated by Type 127 — and
       builds its own entry-point structure around them (see
       hw/smbios/smbios.c:smbios_entry_point_setup in QEMU 10.x).
       The dmidecode --dump-bin format prepends a 31-byte SMBIOS 2.x
       entry-point to the table data, which works with QEMU only by
       alignment luck: QEMU walks the EP bytes as a bogus type-95
       record (length 83, the 'S' byte of "_SM_"), skips 83 bytes,
       and lands somewhere in the table data. For some captures that
       lands on a valid record boundary; for others it doesn't and
       the boot hangs.

       So: mkfixture writes just the raw structure region, no EP
       prefix. axl-emulate's --smbios-file path passes that through
       to QEMU as-is. The fixture is NOT directly interchangeable
       with `dmidecode --from-dump`; document this in the design
       doc when HF2 ships. */
    uint8_t *table_start = NULL;
    uint8_t *table_end = NULL;
    if (axl_smbios_table_range(&table_start, &table_end) != 0) {
        axl_printerr("mkfixture: SMBIOS table not found in EFI "
                     "Configuration Table — skipping smbios.bin\n");
        return 0; /* not fatal; some bare aa64 firmware ships none */
    }
    size_t table_size = (size_t)(table_end - table_start);

    char *path = axl_path_join(fixture_dir, "smbios.bin");
    if (path == NULL) {
        axl_printerr("mkfixture: out of memory composing smbios.bin path\n");
        return -1;
    }
    int rc = axl_file_set_contents(path, table_start, table_size);
    if (rc != 0) {
        axl_printerr("mkfixture: write %s failed\n", path);
    } else {
        axl_printf("  smbios.bin       %zu bytes (raw SMBIOS structures)\n",
                   table_size);
    }
    axl_free(path);
    return rc;
}

// ---------------------------------------------------------------------------
// ACPI dump
// ---------------------------------------------------------------------------

/** Build a filename for an ACPI table. Lowercase signature with
    `.dat` suffix, mirroring what `acpidump -b` writes on Linux so
    captured fixtures are interchangeable. Multiple tables sharing
    a signature (typically SSDTs) get a numeric suffix on duplicates:
    `ssdt.dat`, `ssdt1.dat`, `ssdt2.dat`, ... */
static char *
acpi_filename(
    const char    sig[4],
    unsigned int  duplicate_index
    )
{
    char lower[5] = {0};
    for (int i = 0; i < 4; i++) {
        char c = sig[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + ('a' - 'A'));
        }
        lower[i] = c;
    }
    if (duplicate_index == 0) {
        return axl_asprintf("%s.dat", lower);
    }
    return axl_asprintf("%s%u.dat", lower, duplicate_index);
}

static int
dump_acpi(
    const char *fixture_dir
    )
{
    char *acpi_dir = axl_path_join(fixture_dir, "acpi");
    if (acpi_dir == NULL) {
        return -1;
    }
    if (axl_dir_mkdir(acpi_dir) != AXL_OK && !axl_file_is_dir(acpi_dir)) {
        axl_printerr("mkfixture: mkdir %s failed\n", acpi_dir);
        axl_free(acpi_dir);
        return -1;
    }

    /* Track signatures we've already seen so duplicates (typically
       SSDTs) get numeric suffixes instead of clobbering each other.
       Real-world ACPI table sets stay well under 64 unique signatures
       (server boards typically publish 15–25); the overflow path
       below logs and skips rather than silently dropping a name into
       seen[] where future duplicate-detection would miss it. */
    typedef struct { char sig[4]; unsigned count; } SigCount;
    SigCount seen[64];
    int      seen_n = 0;
    int      table_n = 0;

    AxlAcpiHeader *t = NULL;
    while ((t = axl_acpi_next(t)) != NULL) {
        unsigned int dup_idx = 0;
        bool seen_match = false;
        for (int i = 0; i < seen_n; i++) {
            if (axl_memcmp(seen[i].sig, t->signature, 4) == 0) {
                seen[i].count++;
                dup_idx = seen[i].count;
                seen_match = true;
                break;
            }
        }
        if (!seen_match) {
            if (seen_n < (int)(sizeof seen / sizeof seen[0])) {
                axl_memcpy(seen[seen_n].sig, t->signature, 4);
                seen[seen_n].count = 0;
                seen_n++;
            } else {
                /* > 64 unique signatures — skip rather than risk
                   silently clobbering a write whose duplicate-tracker
                   slot doesn't exist. Bump the seen[] size when this
                   warning ever actually fires in practice. */
                axl_printerr(
                    "mkfixture: ACPI signature tracker full (>%d unique "
                    "signatures); skipping %.4s table\n",
                    (int)(sizeof seen / sizeof seen[0]), t->signature);
                continue;
            }
        }

        char *filename = acpi_filename(t->signature, dup_idx);
        if (filename == NULL) {
            axl_printerr("mkfixture: out of memory composing ACPI filename\n");
            continue;
        }
        char *path = axl_path_join(acpi_dir, filename);
        if (path == NULL) {
            axl_printerr("mkfixture: out of memory composing ACPI path\n");
            axl_free(filename);
            continue;
        }
        int rc = axl_file_set_contents(path, t, t->length);
        if (rc != 0) {
            axl_printerr("mkfixture: write %s failed\n", path);
        } else {
            table_n++;
        }
        axl_free(path);
        axl_free(filename);
    }

    axl_printf("  acpi/            %d tables\n", table_n);
    axl_free(acpi_dir);
    return 0;
}

// ---------------------------------------------------------------------------
// manifest.json
// ---------------------------------------------------------------------------

/** Write a minimal platform-identity manifest.json. JSON is
    machine-generated and machine-consumed; we use plain JSON
    (not JSON5) so host-side tools without JSON5 support (jq,
    DMTF Redfish-Mockup-Server, axl-emulate's stdlib `json`) can
    consume it directly. */
static int
write_manifest(
    const char *fixture_dir
    )
{
    AxlSmbiosBiosInfo bios = {0};
    AxlSmbiosSystemInfo sys = {0};
    int has_bios = (axl_smbios_read_bios_info(&bios) == AXL_OK);
    int has_sys  = (axl_smbios_read_system_info(&sys) == AXL_OK);

    /* JSON-escape would be needed for SMBIOS strings that contain
       quotes or backslashes; in practice DMI strings are 7-bit ASCII
       and rarely contain anything but vendor names + version strings.
       For HF2.1 we accept the limitation and document it. */
    const char *vendor = (has_sys && sys.manufacturer) ? sys.manufacturer : "unknown";
    const char *product = (has_sys && sys.product_name) ? sys.product_name : "unknown";
    const char *bios_ver = (has_bios && bios.version) ? bios.version : "unknown";
    const char *bios_date = (has_bios && bios.release_date) ? bios.release_date : "unknown";

    char *json = axl_asprintf(
        "{\n"
        "  \"vendor\": \"%s\",\n"
        "  \"model\": \"%s\",\n"
        "  \"bios_rev\": \"%s\",\n"
        "  \"bios_date\": \"%s\",\n"
        "  \"capture_tool\": \"mkfixture\",\n"
        "  \"capture_tool_version\": \"%s\",\n"
        "  \"fixture_format\": \"HF2.1\"\n"
        "}\n",
        vendor, product, bios_ver, bios_date, AXL_VERSION_STRING);
    if (json == NULL) {
        axl_printerr("mkfixture: out of memory composing manifest.json\n");
        return -1;
    }

    char *path = axl_path_join(fixture_dir, "manifest.json");
    if (path == NULL) {
        axl_printerr("mkfixture: out of memory composing manifest.json path\n");
        axl_free(json);
        return -1;
    }
    int rc = axl_file_set_contents(path, json, axl_strlen(json));
    if (rc != 0) {
        axl_printerr("mkfixture: write %s failed\n", path);
    } else {
        axl_printf("  manifest.json    %s / %s\n", vendor, product);
    }
    axl_free(path);
    axl_free(json);
    return rc;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static const AxlArgDesc fixture_dir_arg[] = {
    { .name = "fixture-dir", .type = AXL_ARG_STRING, .required = true,
      .help = "Output directory for the captured fixture" },
    {0}
};

static int
run_capture(
    AxlArgs *a
    )
{
    const char *fixture_dir = axl_args_get_string(a, "fixture-dir");
    if (fixture_dir == NULL || fixture_dir[0] == '\0') {
        axl_printerr("mkfixture: fixture-dir is required\n");
        return 1;
    }

    if (axl_dir_mkdir(fixture_dir) != AXL_OK && !axl_file_is_dir(fixture_dir)) {
        axl_printerr("mkfixture: cannot create or access %s\n", fixture_dir);
        return 1;
    }

    axl_printf("mkfixture: writing fixture to %s\n", fixture_dir);

    int rc = 0;
    if (dump_smbios(fixture_dir) != 0) { rc = 1; }
    if (dump_acpi(fixture_dir) != 0)   { rc = 1; }
    if (write_manifest(fixture_dir) != 0) { rc = 1; }

    if (rc == 0) {
        axl_printf("mkfixture: capture complete\n");
    } else {
        axl_printerr("mkfixture: capture completed with errors\n");
    }
    return rc;
}

int
main(
    int    argc,
    char **argv
    )
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "mkfixture",
        .help        = "Capture a UEFI hardware fixture (HF2 — see "
                       "docs/AXL-Hardware-Fixture-Design.md)",
        .positionals = fixture_dir_arg,
        .handler     = run_capture,
    });
}
