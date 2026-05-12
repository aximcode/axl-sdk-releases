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

    HF2.1 + HF2.2 (this version) captures:
      smbios.bin          raw SMBIOS structure region (no entry-point
                          prefix; QEMU `-smbios file=` consumable —
                          NOT directly interchangeable with
                          `dmidecode --from-dump`; see design doc
                          §"SMBIOS format note" for why)
      acpi/<sig>.dat      every ACPI table from RSDT/XSDT, indexed
                          by 4-byte signature
      cpu.json            CPU identity (CPUID on x86, MIDR_EL1 on
                          AArch64): vendor, family/model/stepping,
                          brand string, raw feature words. Future
                          axl-emulate `--cpu-from-fixture` maps this
                          to a QEMU `-cpu MODEL` choice.
      esrt.json           EFI System Resource Table — firmware-update
                          inventory. Per-component FwClass GUID,
                          version, lowest-supported version, last
                          attempt status. Manifest only (replay
                          requires a Phase HF9 QEMU patch).
      manifest.json       basic platform metadata (vendor, product,
                          BIOS revision, capture date)

    HF2.3+ adds: pci.json, usb.json + usb/, video.json + edid/,
    net.json, nvme/, plus alternative write targets (HTTP POST).
**/

#include <axl.h>
#include <axl/axl-acpi.h>
#include <axl/axl-fs.h>
#include <axl/axl-path.h>
#include <axl/axl-runtime.h>
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
// Shared helpers
// ---------------------------------------------------------------------------

/** Escape a string for inclusion as a JSON string literal. Returns
    a freshly allocated string the caller frees with axl_free.
    Handles the ECMA-404 minimum: '"', '\\', and ASCII control chars
    (0x00–0x1F) get \\uXXXX or short escapes. UTF-8 multi-byte
    sequences pass through unchanged (valid JSON).

    SMBIOS DMI strings, CPUID brand strings, and EFI variable
    contents are usually 7-bit ASCII without metacharacters, but
    the spec does not forbid them — a vendor with a backslash or
    embedded NUL would produce invalid JSON without this escape. */
static char *
json_escape(
    const char *s
    )
{
    if (s == NULL) { return axl_strdup(""); }

    /* First pass: compute the worst-case escaped size. Each input
       byte expands to at most 6 chars (\uXXXX). */
    size_t in_len = axl_strlen(s);
    size_t out_cap = in_len * 6 + 1;
    char  *out = axl_malloc(out_cap);
    if (out == NULL) { return NULL; }

    size_t j = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  out[j++] = '\\'; out[j++] = '"';  break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\b': out[j++] = '\\'; out[j++] = 'b';  break;
            case '\f': out[j++] = '\\'; out[j++] = 'f';  break;
            case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
            case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
            case '\t': out[j++] = '\\'; out[j++] = 't';  break;
            default:
                if (c < 0x20) {
                    /* ECMA-404 mandates \uXXXX for all unprintables. */
                    j += (size_t)axl_snprintf(out + j, out_cap - j,
                                              "\\u%04x", (unsigned)c);
                } else {
                    /* Pass through — UTF-8 multi-byte sequences
                       (continuation bytes 0x80–0xBF + leads 0xC0+)
                       are valid in JSON strings. */
                    out[j++] = (char)c;
                }
                break;
        }
    }
    out[j] = '\0';
    return out;
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
// CPU dump — cpu.json
// ---------------------------------------------------------------------------

#if defined(__x86_64__)

static void
cpuid(
    uint32_t  leaf,
    uint32_t  subleaf,
    uint32_t *eax,
    uint32_t *ebx,
    uint32_t *ecx,
    uint32_t *edx
    )
{
    uint32_t a = 0, b = 0, c = 0, d = 0;
    __asm__ volatile(
        "cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(leaf), "c"(subleaf)
    );
    if (eax) { *eax = a; }
    if (ebx) { *ebx = b; }
    if (ecx) { *ecx = c; }
    if (edx) { *edx = d; }
}

static int
dump_cpu(
    const char *fixture_dir
    )
{
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    uint32_t max_leaf = 0, max_ext_leaf = 0;
    char     vendor[13];
    char     brand[49];
    uint32_t leaf1_ebx = 0, leaf1_ecx = 0, leaf1_edx = 0;
    uint32_t leaf7_ebx = 0, leaf7_ecx = 0;

    /* Leaf 0: vendor string + max basic leaf. The vendor bytes come
       back EBX/EDX/ECX in that order — see Intel SDM Vol 2A §3.2. */
    cpuid(0, 0, &max_leaf,
          (uint32_t *)&vendor[0],
          (uint32_t *)&vendor[8],
          (uint32_t *)&vendor[4]);
    vendor[12] = '\0';

    /* Leaf 1: family/model/stepping + feature words (EDX/ECX). */
    cpuid(1, 0, &eax, &leaf1_ebx, &leaf1_ecx, &leaf1_edx);
    uint32_t family   = (eax >> 8) & 0xF;
    uint32_t model    = (eax >> 4) & 0xF;
    uint32_t stepping = eax & 0xF;
    if (family == 0x6 || family == 0xF) {
        model += ((eax >> 16) & 0xF) << 4;
    }
    if (family == 0xF) {
        family += (eax >> 20) & 0xFF;
    }

    /* Extended leaves: brand string (3 leaves, 16 bytes each = 48 +
       NUL). */
    cpuid(0x80000000, 0, &max_ext_leaf, NULL, NULL, NULL);
    brand[0] = '\0';
    if (max_ext_leaf >= 0x80000004) {
        cpuid(0x80000002, 0,
              (uint32_t *)&brand[0],  (uint32_t *)&brand[4],
              (uint32_t *)&brand[8],  (uint32_t *)&brand[12]);
        cpuid(0x80000003, 0,
              (uint32_t *)&brand[16], (uint32_t *)&brand[20],
              (uint32_t *)&brand[24], (uint32_t *)&brand[28]);
        cpuid(0x80000004, 0,
              (uint32_t *)&brand[32], (uint32_t *)&brand[36],
              (uint32_t *)&brand[40], (uint32_t *)&brand[44]);
        brand[48] = '\0';
    }
    /* Brand string typically has leading spaces; trim for readability. */
    const char *brand_trimmed = brand;
    while (*brand_trimmed == ' ') {
        brand_trimmed++;
    }

    /* Leaf 7 sub-leaf 0: extended feature flags (AVX2, AVX-512, etc.). */
    if (max_leaf >= 7) {
        cpuid(7, 0, NULL, &leaf7_ebx, &leaf7_ecx, NULL);
    }

    /* Vendor (CPUID leaf 0) is always 12-byte ASCII so doesn't
       need escaping in practice, but use the helper for consistency.
       Brand string is more variable — vendors do put colons, hyphens,
       parentheses; in principle quotes/backslashes could appear too. */
    char *vendor_esc = json_escape(vendor);
    char *brand_esc  = json_escape(brand_trimmed);
    if (vendor_esc == NULL || brand_esc == NULL) {
        axl_free(vendor_esc); axl_free(brand_esc);
        axl_printerr("mkfixture: out of memory escaping cpu.json strings\n");
        return -1;
    }
    char *json = axl_asprintf(
        "{\n"
        "  \"arch\": \"x86_64\",\n"
        "  \"vendor\": \"%s\",\n"
        "  \"family\": %u,\n"
        "  \"model\": %u,\n"
        "  \"stepping\": %u,\n"
        "  \"brand\": \"%s\",\n"
        "  \"max_basic_leaf\": \"0x%08x\",\n"
        "  \"max_ext_leaf\": \"0x%08x\",\n"
        "  \"features\": {\n"
        "    \"leaf_1_ebx\": \"0x%08x\",\n"
        "    \"leaf_1_ecx\": \"0x%08x\",\n"
        "    \"leaf_1_edx\": \"0x%08x\",\n"
        "    \"leaf_7_ebx\": \"0x%08x\",\n"
        "    \"leaf_7_ecx\": \"0x%08x\"\n"
        "  }\n"
        "}\n",
        vendor_esc, family, model, stepping, brand_esc,
        max_leaf, max_ext_leaf,
        leaf1_ebx, leaf1_ecx, leaf1_edx,
        leaf7_ebx, leaf7_ecx);
    axl_free(vendor_esc);
    axl_free(brand_esc);
    if (json == NULL) {
        axl_printerr("mkfixture: out of memory composing cpu.json\n");
        return -1;
    }

    char *path = axl_path_join(fixture_dir, "cpu.json");
    if (path == NULL) {
        axl_printerr("mkfixture: out of memory composing cpu.json path\n");
        axl_free(json);
        return -1;
    }
    int rc = axl_file_set_contents(path, json, axl_strlen(json));
    if (rc != 0) {
        axl_printerr("mkfixture: write %s failed\n", path);
    } else {
        axl_printf("  cpu.json         %s family %u model %u stepping %u\n",
                   vendor, family, model, stepping);
    }
    axl_free(path);
    axl_free(json);
    return rc;
}

#elif defined(__aarch64__)

static int
dump_cpu(
    const char *fixture_dir
    )
{
    uint64_t midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));

    uint8_t  imp        = (uint8_t)((midr >> 24) & 0xFF);
    uint8_t  variant    = (uint8_t)((midr >> 20) & 0x0F);
    uint8_t  arch_field = (uint8_t)((midr >> 16) & 0x0F);
    uint16_t part_num   = (uint16_t)((midr >> 4) & 0xFFF);
    uint8_t  revision   = (uint8_t)(midr & 0x0F);

    char *json = axl_asprintf(
        "{\n"
        "  \"arch\": \"aarch64\",\n"
        "  \"midr_el1\": \"0x%016llx\",\n"
        "  \"implementer\": \"0x%02x\",\n"
        "  \"variant\": %u,\n"
        "  \"architecture\": %u,\n"
        "  \"part_num\": \"0x%03x\",\n"
        "  \"revision\": %u\n"
        "}\n",
        (unsigned long long)midr, imp,
        (unsigned)variant, (unsigned)arch_field,
        (unsigned)part_num, (unsigned)revision);
    if (json == NULL) {
        axl_printerr("mkfixture: out of memory composing cpu.json\n");
        return -1;
    }

    char *path = axl_path_join(fixture_dir, "cpu.json");
    if (path == NULL) {
        axl_printerr("mkfixture: out of memory composing cpu.json path\n");
        axl_free(json);
        return -1;
    }
    int rc = axl_file_set_contents(path, json, axl_strlen(json));
    if (rc != 0) {
        axl_printerr("mkfixture: write %s failed\n", path);
    } else {
        axl_printf("  cpu.json         aarch64 implementer 0x%02x part 0x%03x\n",
                   imp, part_num);
    }
    axl_free(path);
    axl_free(json);
    return rc;
}

#else

static int
dump_cpu(
    const char *fixture_dir
    )
{
    (void)fixture_dir;
    axl_printerr("mkfixture: cpu.json: unsupported architecture (skipping)\n");
    return 0;
}

#endif

// ---------------------------------------------------------------------------
// ESRT dump — esrt.json (firmware-update inventory)
// ---------------------------------------------------------------------------

/** Format an EFI_GUID as the canonical 8-4-4-4-12 string. Caller
    frees with axl_free. */
static char *
guid_format(
    const EFI_GUID *g
    )
{
    return axl_asprintf(
        "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        (unsigned)g->Data1, (unsigned)g->Data2, (unsigned)g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static const char *
esrt_fw_type_name(
    uint32_t fw_type
    )
{
    switch (fw_type) {
        case 0:  return "unknown";
        case 1:  return "system_firmware";
        case 2:  return "device_firmware";
        case 3:  return "uefi_driver";
        default: return "reserved";
    }
}

static int
dump_esrt(
    const char *fixture_dir
    )
{
    EFI_SYSTEM_RESOURCE_TABLE *esrt =
        axl_efi_find_config_table((const AxlGuid *)&EFI_SYSTEM_RESOURCE_TABLE_GUID);

    if (esrt == NULL) {
        /* Not an error — many firmwares (especially OVMF in QEMU) don't
           publish ESRT. Skip silently with a one-line note. */
        axl_printf("  esrt.json        (skipped — no ESRT in config table)\n");
        return 0;
    }

    /* The generated EFI_SYSTEM_RESOURCE_TABLE struct has only the
       3-field header (FwResourceCount, FwResourceCountMax,
       FwResourceVersion); the trailing `Entries[]` flex array is
       elided by the spec-HTML parser. The entries follow the header
       contiguously per UEFI 2.x §23.4 — compute the pointer manually. */
    EFI_SYSTEM_RESOURCE_ENTRY *entries =
        (EFI_SYSTEM_RESOURCE_ENTRY *)((uint8_t *)esrt + sizeof(*esrt));

    /* Build the JSON in pieces. We use a string-builder approach via
       repeated axl_asprintf + concat to avoid bounding the entry count
       at the format string. */
    char *body = axl_strdup("");
    if (body == NULL) { return -1; }
    for (uint32_t i = 0; i < esrt->FwResourceCount; i++) {
        EFI_SYSTEM_RESOURCE_ENTRY *e = &entries[i];
        char *guid_str = guid_format(&e->FwClass);
        if (guid_str == NULL) { axl_free(body); return -1; }
        char *entry = axl_asprintf(
            "%s    {\n"
            "      \"fw_class\": \"%s\",\n"
            "      \"fw_type\": %u,\n"
            "      \"fw_type_name\": \"%s\",\n"
            "      \"fw_version\": \"0x%08x\",\n"
            "      \"lowest_supported_version\": \"0x%08x\",\n"
            "      \"capsule_flags\": \"0x%08x\",\n"
            "      \"last_attempt_version\": \"0x%08x\",\n"
            "      \"last_attempt_status\": \"0x%08x\"\n"
            "    }%s\n",
            body,
            guid_str,
            (unsigned)e->FwType,
            esrt_fw_type_name(e->FwType),
            (unsigned)e->FwVersion,
            (unsigned)e->LowestSupportedFwVersion,
            (unsigned)e->CapsuleFlags,
            (unsigned)e->LastAttemptVersion,
            (unsigned)e->LastAttemptStatus,
            (i + 1 == esrt->FwResourceCount) ? "" : ",");
        axl_free(guid_str);
        axl_free(body);
        if (entry == NULL) { return -1; }
        body = entry;
    }

    char *json = axl_asprintf(
        "{\n"
        "  \"fw_resource_count\": %u,\n"
        "  \"fw_resource_count_max\": %u,\n"
        "  \"fw_resource_version\": %llu,\n"
        "  \"entries\": [\n"
        "%s"
        "  ]\n"
        "}\n",
        (unsigned)esrt->FwResourceCount,
        (unsigned)esrt->FwResourceCountMax,
        (unsigned long long)esrt->FwResourceVersion,
        body);
    axl_free(body);
    if (json == NULL) {
        axl_printerr("mkfixture: out of memory composing esrt.json\n");
        return -1;
    }

    char *path = axl_path_join(fixture_dir, "esrt.json");
    if (path == NULL) {
        axl_printerr("mkfixture: out of memory composing esrt.json path\n");
        axl_free(json);
        return -1;
    }
    int rc = axl_file_set_contents(path, json, axl_strlen(json));
    if (rc != 0) {
        axl_printerr("mkfixture: write %s failed\n", path);
    } else {
        axl_printf("  esrt.json        %u firmware-updatable component%s\n",
                   (unsigned)esrt->FwResourceCount,
                   esrt->FwResourceCount == 1 ? "" : "s");
    }
    axl_free(path);
    axl_free(json);
    return rc;
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

    /* DMI strings are typically 7-bit ASCII without metacharacters,
       but the spec doesn't forbid quotes/backslashes — escape via
       the shared json_escape helper for safety. */
    const char *vendor    = (has_sys  && sys.manufacturer)  ? sys.manufacturer  : "unknown";
    const char *product   = (has_sys  && sys.product_name)  ? sys.product_name  : "unknown";
    const char *bios_ver  = (has_bios && bios.version)      ? bios.version      : "unknown";
    const char *bios_date = (has_bios && bios.release_date) ? bios.release_date : "unknown";

    char *vendor_esc    = json_escape(vendor);
    char *product_esc   = json_escape(product);
    char *bios_ver_esc  = json_escape(bios_ver);
    char *bios_date_esc = json_escape(bios_date);
    if (!vendor_esc || !product_esc || !bios_ver_esc || !bios_date_esc) {
        axl_free(vendor_esc); axl_free(product_esc);
        axl_free(bios_ver_esc); axl_free(bios_date_esc);
        axl_printerr("mkfixture: out of memory escaping manifest.json strings\n");
        return -1;
    }

    char *json = axl_asprintf(
        "{\n"
        "  \"vendor\": \"%s\",\n"
        "  \"model\": \"%s\",\n"
        "  \"bios_rev\": \"%s\",\n"
        "  \"bios_date\": \"%s\",\n"
        "  \"capture_tool\": \"mkfixture\",\n"
        "  \"capture_tool_version\": \"%s\",\n"
        "  \"fixture_format\": \"HF2.2\"\n"
        "}\n",
        vendor_esc, product_esc, bios_ver_esc, bios_date_esc, AXL_VERSION_STRING);
    axl_free(vendor_esc);
    axl_free(product_esc);
    axl_free(bios_ver_esc);
    axl_free(bios_date_esc);
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
    if (dump_smbios(fixture_dir) != 0)    { rc = 1; }
    if (dump_acpi(fixture_dir) != 0)      { rc = 1; }
    if (dump_cpu(fixture_dir) != 0)       { rc = 1; }
    if (dump_esrt(fixture_dir) != 0)      { rc = 1; }
    if (write_manifest(fixture_dir) != 0) { rc = 1; }

    if (rc == 0) {
        axl_printf("mkfixture: capture complete\n");
    } else {
        axl_printerr("mkfixture: capture completed with errors\n");
    }
    return rc;
}

AXL_TOOL_MAIN(mkfixture)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "mkfixture",
        .help        = "Capture a UEFI hardware fixture (HF2 — see "
                       "docs/AXL-Hardware-Fixture-Design.md)",
        .positionals = fixture_dir_arg,
        .handler     = run_capture,
    });
}
