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

    HF2.1–HF2.3 (this version) captures:
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
      pci.json            every responding PCI function (VID/DID, class,
                          subsystem, BARs, header type) — manifest only
      usb.json + usb/     EFI_USB_IO interface manifest (topology depth,
                          VID/PID, class triplet, strings) + per-device
                          raw config-descriptor blobs — manifest only
      video.json + edid/  GOP mode list, current geometry, framebuffer,
                          pixel format + per-display raw EDID — manifest
                          only
      net.json            per-NIC MAC + link state via SNP (deduped by
                          permanent MAC) — manifest only
      nvme/<n>.json       per-controller Identify Controller + per-
                          namespace Identify via NVMe pass-thru — manifest
                          only

    Write targets (HF2.4): the destination argument is either a local
    directory (the default — also reachable over a virtiofs `--mount`) or
    an http(s):// URL. With a URL, nothing touches the filesystem — every
    artifact is appended to an in-memory ustar tarball (via AxlTar),
    gzipped (AxlCompress), and POSTed (`application/gzip`, a standard
    `.tar.gz`) to the collector at the end, so a disk-less / net-only
    machine can capture straight over the network.
**/

#include <axl.h>
#include <axl/axl-acpi.h>
#include <axl/axl-fs.h>
#include <axl/axl-gfx.h>
#include <axl/axl-path.h>
#include <axl/axl-pci.h>
#include <axl/axl-runtime.h>
#include <axl/axl-smbios.h>
#include <axl/axl-spd.h>
#include <axl/axl-usb.h>
#include <axl/axl-tar.h>
#include <axl/axl-compress.h>
#include <axl/axl-stream.h>
#include <axl/axl-http-client.h>
#include <uefi/axl-uefi.h>

// ---------------------------------------------------------------------------
// Fixture write sink — forward declarations (definitions below the SMBIOS
// dump). Every dump_* routes its writes through fixture_write.
// ---------------------------------------------------------------------------

static bool
dest_is_url(
    const char *dest
);

static int
fixture_write(
    const char *dest,
    const char *relpath,
    const void *data,
    size_t      len
);

static int
fixture_finish(
    const char *dest
);

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
                     "Configuration Table - skipping smbios.bin\n");
        return 0; /* not fatal; some bare aa64 firmware ships none */
    }
    size_t table_size = (size_t)(table_end - table_start);

    int rc = fixture_write(fixture_dir, "smbios.bin", table_start, table_size);
    if (rc == 0) {
        axl_printf("  smbios.bin       %zu bytes (raw SMBIOS structures)\n",
                   table_size);
    }
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

/** Render a JSON string value: a quoted, escaped literal when @p s is
    non-NULL, or the bare token `null` when @p s is NULL. Lets a manifest
    distinguish "the device has no such string" (JSON null) from an empty
    string. Returns a freshly allocated string the caller frees with
    axl_free, or NULL on allocation failure. */
static char *
json_str_or_null(
    const char *s
    )
{
    if (s == NULL) { return axl_strdup("null"); }
    char *esc = json_escape(s);
    if (esc == NULL) { return NULL; }
    char *out = axl_asprintf("\"%s\"", esc);
    axl_free(esc);
    return out;
}

// ---------------------------------------------------------------------------
// Fixture write sink — a local directory, or (when the destination is an
// http(s):// URL) an in-memory ustar tarball POSTed at the end. The URL
// path lets a disk-less, net-only machine capture straight to an HTTP
// collector with no writable storage. Every dump_* routes its writes
// through fixture_write(dest, relpath, ...) instead of touching the
// filesystem directly.
// ---------------------------------------------------------------------------

static AxlStream    *g_tar_buf = NULL;   // in-memory archive (URL mode only)
static AxlTarWriter *g_tar     = NULL;

static bool
dest_is_url(
    const char *dest
    )
{
    return axl_strncmp(dest, "http://", 7) == 0
        || axl_strncmp(dest, "https://", 8) == 0;
}

/** Emit one captured artifact at @p relpath (e.g. "pci.json",
    "acpi/facp.dat"). In URL mode it is appended to the in-memory tar;
    otherwise it is written as a file under @p dest, creating a single
    level of subdirectory as needed. Returns 0 on success, -1 on failure. */
static int
fixture_write(
    const char *dest,
    const char *relpath,
    const void *data,
    size_t      len
    )
{
    if (g_tar != NULL) {
        return (axl_tar_writer_add(g_tar, relpath, 0644, data, len) == AXL_OK)
               ? 0 : -1;
    }

    /* Local: split a one-level "subdir/file" relpath and create the
       subdir on demand (idempotent — set_contents reports real failures). */
    const char *slash = NULL;
    for (const char *p = relpath; *p != '\0'; p++) {
        if (*p == '/') { slash = p; }
    }

    char *path;
    if (slash != NULL) {
        size_t sublen = (size_t)(slash - relpath);
        char   subdir[64];
        if (sublen >= sizeof subdir) { return -1; }
        axl_memcpy(subdir, relpath, sublen);
        subdir[sublen] = '\0';
        char *dir = axl_path_join(dest, subdir);
        if (dir == NULL) { return -1; }
        axl_dir_mkdir(dir);
        path = axl_path_join(dir, slash + 1);
        axl_free(dir);
    } else {
        path = axl_path_join(dest, relpath);
    }
    if (path == NULL) { return -1; }

    int rc = axl_file_set_contents(path, data, len);
    if (rc != 0) {
        axl_printerr("mkfixture: write %s failed\n", path);
    }
    axl_free(path);
    return rc;
}

/** Finalize a URL-mode capture: close the in-memory tar and POST it to
    the collector. No-op (success) in local mode. */
static int
fixture_finish(
    const char *dest
    )
{
    if (g_tar == NULL) { return 0; }
    if (axl_tar_writer_finish(g_tar) != AXL_OK) {
        axl_printerr("mkfixture: building fixture tarball failed\n");
        return -1;
    }

    size_t      n     = 0;
    const void *bytes = axl_bufdata(g_tar_buf, &n);
    if (bytes == NULL || n == 0) {
        axl_printerr("mkfixture: empty fixture tarball\n");
        return -1;
    }

    /* gzip the tarball before POST (smaller upload; the body is a
       standard .tar.gz the collector can gunzip). */
    void  *gz     = NULL;
    size_t gz_len = 0;
    if (axl_compress(AXL_COMPRESS_GZIP, bytes, n,
                     AXL_COMPRESS_LEVEL_DEFAULT, &gz, &gz_len) != AXL_OK) {
        axl_printerr("mkfixture: gzip of fixture tarball failed\n");
        return -1;
    }

    /* Bring up networking (driver load + link + DHCP) before POSTing,
       same as fetch.efi — the HTTP client needs a configured IP. */
    if (axl_net_init(AXL_NET_NIC_AUTO, 10) != AXL_OK) {
        axl_printerr("mkfixture: networking did not come up (link/DHCP) - "
                     "cannot POST fixture\n");
        axl_free(gz);
        return -1;
    }

    /* Enable TLS so an https dest works directly or via an http->https
       redirect (the client's TLS path is otherwise strippable — see
       axl-http-client-tls.h). Bail only when the dest is already https. */
    if (axl_tls_init() != AXL_OK && axl_strncmp(dest, "https://", 8) == 0) {
        axl_printerr("mkfixture: https dest needs an AXL_TLS=1 build\n");
        axl_free(gz);
        return -1;
    }

    AxlHttpClient *c = axl_http_client_new();
    if (c == NULL) {
        axl_printerr("mkfixture: out of memory creating HTTP client\n");
        axl_free(gz);
        return -1;
    }
    AxlHttpClientResponse *resp = NULL;
    int rc = -1;
    if (axl_http_post(c, dest, gz, gz_len, "application/gzip", &resp) == AXL_OK
        && resp != NULL
        && resp->status_code >= 200 && resp->status_code < 300) {
        axl_printf("mkfixture: POSTed %zu-byte gzipped fixture tarball "
                   "(%zu raw) to %s (HTTP %zu)\n",
                   gz_len, n, dest, resp->status_code);
        rc = 0;
    } else {
        axl_printerr("mkfixture: POST to %s failed (HTTP %zu)\n", dest,
                     (resp != NULL) ? resp->status_code : 0);
    }
    axl_http_client_response_free(resp);
    axl_http_client_free(c);
    axl_free(gz);
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
        char *relpath = axl_asprintf("acpi/%s", filename);
        if (relpath == NULL) {
            axl_printerr("mkfixture: out of memory composing ACPI path\n");
            axl_free(filename);
            continue;
        }
        if (fixture_write(fixture_dir, relpath, t, t->length) == 0) {
            table_n++;
        }
        axl_free(relpath);
        axl_free(filename);
    }

    axl_printf("  acpi/            %d tables\n", table_n);
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
    uint32_t eax = 0;
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

    int rc = fixture_write(fixture_dir, "cpu.json", json, axl_strlen(json));
    if (rc == 0) {
        axl_printf("  cpu.json         %s family %u model %u stepping %u\n",
                   vendor, family, model, stepping);
    }
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

    int rc = fixture_write(fixture_dir, "cpu.json", json, axl_strlen(json));
    if (rc == 0) {
        axl_printf("  cpu.json         aarch64 implementer 0x%02x part 0x%03x\n",
                   imp, part_num);
    }
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
        axl_printf("  esrt.json        (skipped - no ESRT in config table)\n");
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

    int rc = fixture_write(fixture_dir, "esrt.json", json, axl_strlen(json));
    if (rc == 0) {
        axl_printf("  esrt.json        %u firmware-updatable component%s\n",
                   (unsigned)esrt->FwResourceCount,
                   esrt->FwResourceCount == 1 ? "" : "s");
    }
    axl_free(json);
    return rc;
}

// ---------------------------------------------------------------------------
// PCI dump — pci.json (manifest only; not replayed — see design doc
// §"Intractable": vendor silicon can't be faked, so the capture records
// config-space identity for inspection, not replay)
// ---------------------------------------------------------------------------

/** Build the inner `bars` array body (without the surrounding `[]`) for
    one function. Type 0 endpoints expose six BARs at config offsets
    0x10..0x24; Type 1 PCI-PCI bridges expose two at 0x10/0x14; other
    header types carry none. Raw 32-bit register values are recorded
    verbatim — decoding memory-vs-I/O space, 64-bit BAR pairing, and
    size probing are replay concerns we don't have (manifest only).
    Returns a freshly allocated string (possibly empty) the caller frees
    with axl_free, or NULL on allocation failure. */
static char *
pci_bars_json(
    AxlPciAddr        addr,
    AxlPciHeaderType  htype
    )
{
    unsigned nbars = 0;
    if (htype == AXL_PCI_HEADER_TYPE_NORMAL) {
        nbars = 6;
    } else if (htype == AXL_PCI_HEADER_TYPE_BRIDGE) {
        nbars = 2;
    }

    char *acc = axl_strdup("");
    if (acc == NULL) { return NULL; }
    for (unsigned i = 0; i < nbars; i++) {
        uint32_t bar = 0;
        axl_pci_read_config_32(addr, (uint16_t)(0x10 + i * 4), &bar);
        char *next = axl_asprintf("%s%s\"0x%08x\"",
                                  acc, (i == 0) ? "" : ", ", (unsigned)bar);
        axl_free(acc);
        if (next == NULL) { return NULL; }
        acc = next;
    }
    return acc;
}

static int
dump_pci(
    const char *fixture_dir
    )
{
    char  *body  = axl_strdup("");
    if (body == NULL) { return -1; }
    size_t count = 0;
    bool   oom   = false;

    AxlPciAddr *p = NULL;
    while ((p = axl_pci_next(p)) != NULL) {
        if (count >= 4096) {
            axl_printerr("mkfixture: PCI enumeration truncated at 4096 "
                         "functions\n");
            break;
        }

        uint16_t vid = 0, did = 0;
        uint32_t class_code = 0;
        AxlPciHeaderType htype = AXL_PCI_HEADER_TYPE_NORMAL;
        bool             multi = false;
        /* Skip any function whose config space can't be read in full — a
           fixture entry with defaulted identity/class would be misleading. */
        if (axl_pci_get_vid_did(*p, &vid, &did) != AXL_OK
            || axl_pci_get_class_code(*p, &class_code) != AXL_OK
            || axl_pci_get_header_type(*p, &htype, &multi) != AXL_OK) {
            continue;
        }

        char addr_str[AXL_PCI_ADDR_STR_MAX];
        if (axl_pci_addr_format(*p, addr_str, sizeof addr_str) < 0) {
            addr_str[0] = '\0';
        }

        char class_name[AXL_PCI_CLASS_NAME_MAX];
        if (axl_pci_class_string(class_code, class_name,
                                 sizeof class_name) <= 0) {
            class_name[0] = '\0';
        }

        /* Subsystem ID lives at 0x2C/0x2E only on Type 0 functions;
           axl_pci_get_subsystem bakes the header-type check in. */
        uint16_t svid = 0, sdid = 0;
        bool has_subsys = (axl_pci_get_subsystem(*p, &svid, &sdid) == AXL_OK);

        char *class_esc   = json_escape(class_name);
        char *bars        = pci_bars_json(*p, htype);
        char *subsys_frag = has_subsys
            ? axl_asprintf(
                  "      \"subsystem_vendor_id\": \"0x%04x\",\n"
                  "      \"subsystem_id\": \"0x%04x\",\n",
                  (unsigned)svid, (unsigned)sdid)
            : axl_strdup("");
        if (class_esc == NULL || bars == NULL || subsys_frag == NULL) {
            axl_free(class_esc); axl_free(bars); axl_free(subsys_frag);
            oom = true;
            break;
        }

        char *entry = axl_asprintf(
            "%s%s    {\n"
            "      \"address\": \"%s\",\n"
            "      \"vendor_id\": \"0x%04x\",\n"
            "      \"device_id\": \"0x%04x\",\n"
            "      \"class_code\": \"0x%06x\",\n"
            "      \"class_name\": \"%s\",\n"
            "      \"header_type\": %u,\n"
            "      \"multi_function\": %s,\n"
            "%s"
            "      \"bars\": [%s]\n"
            "    }",
            body,
            (count == 0) ? "" : ",\n",
            addr_str,
            (unsigned)vid, (unsigned)did,
            (unsigned)class_code,
            class_esc,
            (unsigned)htype,
            multi ? "true" : "false",
            subsys_frag,
            bars);
        axl_free(class_esc);
        axl_free(bars);
        axl_free(subsys_frag);
        axl_free(body);
        if (entry == NULL) { oom = true; body = NULL; break; }
        body = entry;
        count++;
    }

    if (oom) {
        axl_free(body);
        axl_printerr("mkfixture: out of memory composing pci.json\n");
        return -1;
    }

    char *json = axl_asprintf(
        "{\n"
        "  \"count\": %zu,\n"
        "  \"devices\": [\n"
        "%s\n"
        "  ]\n"
        "}\n",
        count, body);
    axl_free(body);
    if (json == NULL) {
        axl_printerr("mkfixture: out of memory composing pci.json\n");
        return -1;
    }

    int rc = fixture_write(fixture_dir, "pci.json", json, axl_strlen(json));
    if (rc == 0) {
        axl_printf("  pci.json         %zu PCI function%s\n",
                   count, count == 1 ? "" : "s");
    }
    axl_free(json);
    return rc;
}

// ---------------------------------------------------------------------------
// USB dump — usb.json + usb/<bus>-<addr>.bin (manifest + raw config
// descriptor blobs; manifest only — see design doc §"Intractable":
// non-class-compliant USB can't be faithfully replayed)
// ---------------------------------------------------------------------------

/** Fetch the full configuration descriptor (9-byte header + interface /
    endpoint descriptors) for the device behind interface @p addr and
    write it to usb/<bus>-<devaddr>.bin. Two control transfers: read the
    9-byte header to learn wTotalLength, then read the whole block — the
    same GET_DESCRIPTOR(CONFIGURATION) dance UEFI's own USB bus driver
    performs at enumeration. Returns the fixture-relative path
    ("usb/<bus>-<addr>.bin") on success (caller frees), or NULL if the
    control transfer failed (real-HW devices that NAK enumeration-time
    re-reads) or on allocation failure. */
static char *
capture_usb_config_descriptor(
    AxlUsbAddr  addr,
    const char *dest,
    uint8_t     bus,
    uint8_t     devaddr
    )
{
    /* GET_DESCRIPTOR(CONFIGURATION, index 0): bmRequestType 0x80
       (IN | standard | device), bRequest 0x06 (GET_DESCRIPTOR),
       wValue (descriptor type 0x02 << 8 | index 0), wIndex 0. */
    enum { GET_DESCRIPTOR = 0x06, DESC_CONFIGURATION = 0x02 };
    uint16_t wvalue = (uint16_t)(DESC_CONFIGURATION << 8);

    uint8_t hdr[9] = {0};
    if (axl_usb_control_transfer(addr, 0x80, GET_DESCRIPTOR, wvalue, 0,
                                 AXL_USB_DATA_IN, 1000, hdr, sizeof hdr)
        != AXL_OK) {
        return NULL;
    }
    /* wTotalLength at offset 2 (little-endian). Clamp to [9, 4096]. */
    uint16_t total = (uint16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));
    if (total < sizeof hdr) { total = (uint16_t)sizeof hdr; }
    if (total > 4096) { total = 4096; }

    uint8_t *buf = axl_malloc(total);
    if (buf == NULL) { return NULL; }
    if (axl_usb_control_transfer(addr, 0x80, GET_DESCRIPTOR, wvalue, 0,
                                 AXL_USB_DATA_IN, 1000, buf, total)
        != AXL_OK) {
        axl_free(buf);
        return NULL;
    }

    char *rel = axl_asprintf("usb/%u-%u.bin", (unsigned)bus, (unsigned)devaddr);
    if (rel == NULL) { axl_free(buf); return NULL; }
    int rc = fixture_write(dest, rel, buf, total);
    axl_free(buf);
    if (rc != 0) { axl_free(rel); return NULL; }
    return rel;
}

/** State threaded through the EFI_USB_IO tree walk. The callback runs
    once per interface; descriptor blobs are written once per physical
    device (deduplicated by (bus, addr)). */
typedef struct {
    char       *body;        ///< accumulating interfaces[] body
    size_t      count;       ///< interfaces emitted so far
    bool        oom;         ///< allocation failure → stop the walk
    const char *dest;        ///< fixture destination (dir or URL)
    struct { uint8_t bus, addr; } seen[256];  ///< devices already blobbed
    int         seen_n;
} UsbWalkCtx;

static int
usb_walk_cb(
    AxlUsbAddr  addr,
    unsigned    depth,
    void       *vctx
    )
{
    UsbWalkCtx *c = (UsbWalkCtx *)vctx;

    uint16_t vid = 0, pid = 0;
    axl_usb_get_vid_pid(addr, &vid, &pid);
    uint8_t cls = 0, sub = 0, prot = 0;
    axl_usb_get_class(addr, &cls, &sub, &prot);

    char cname[AXL_USB_CLASS_NAME_MAX];
    if (axl_usb_class_string(cls, sub, prot, cname, sizeof cname) <= 0) {
        cname[0] = '\0';
    }

    char mbuf[AXL_USB_STRING_MAX], pbuf[AXL_USB_STRING_MAX],
         sbuf[AXL_USB_STRING_MAX];
    bool has_m = (axl_usb_get_manufacturer(addr, mbuf, sizeof mbuf) > 0);
    bool has_p = (axl_usb_get_product(addr, pbuf, sizeof pbuf) > 0);
    bool has_s = (axl_usb_get_serial(addr, sbuf, sizeof sbuf) > 0);

    /* Descriptor blob: once per physical device, not per interface. */
    bool device_seen = false;
    for (int i = 0; i < c->seen_n; i++) {
        if (c->seen[i].bus == addr.bus && c->seen[i].addr == addr.addr) {
            device_seen = true;
            break;
        }
    }
    char *blob_rel = NULL;
    if (!device_seen
        && c->seen_n < (int)(sizeof c->seen / sizeof c->seen[0])) {
        /* Record before capturing so the dedup holds; once the tracker
           is full we stop capturing entirely rather than re-dumping a
           device's descriptor on each of its interfaces (mirrors
           dump_acpi's seen[] overflow posture). 256 distinct USB
           devices in one fixture is implausible. */
        c->seen[c->seen_n].bus  = addr.bus;
        c->seen[c->seen_n].addr = addr.addr;
        c->seen_n++;
        blob_rel = capture_usb_config_descriptor(addr, c->dest,
                                                 addr.bus, addr.addr);
    }

    char *cname_esc = json_escape(cname);
    char *m_json    = json_str_or_null(has_m ? mbuf : NULL);
    char *p_json    = json_str_or_null(has_p ? pbuf : NULL);
    char *s_json    = json_str_or_null(has_s ? sbuf : NULL);
    char *blob_json = json_str_or_null(blob_rel);
    axl_free(blob_rel);
    if (cname_esc == NULL || m_json == NULL || p_json == NULL
        || s_json == NULL || blob_json == NULL) {
        axl_free(cname_esc); axl_free(m_json); axl_free(p_json);
        axl_free(s_json); axl_free(blob_json);
        c->oom = true;
        return 1;
    }

    char *entry = axl_asprintf(
        "%s%s    {\n"
        "      \"bus\": %u,\n"
        "      \"address\": %u,\n"
        "      \"interface\": %u,\n"
        "      \"depth\": %u,\n"
        "      \"vendor_id\": \"0x%04x\",\n"
        "      \"product_id\": \"0x%04x\",\n"
        "      \"class\": \"0x%02x\",\n"
        "      \"subclass\": \"0x%02x\",\n"
        "      \"protocol\": \"0x%02x\",\n"
        "      \"class_name\": \"%s\",\n"
        "      \"manufacturer\": %s,\n"
        "      \"product\": %s,\n"
        "      \"serial\": %s,\n"
        "      \"descriptor_blob\": %s\n"
        "    }",
        c->body,
        (c->count == 0) ? "" : ",\n",
        (unsigned)addr.bus, (unsigned)addr.addr, (unsigned)addr.intf, depth,
        (unsigned)vid, (unsigned)pid,
        (unsigned)cls, (unsigned)sub, (unsigned)prot,
        cname_esc, m_json, p_json, s_json, blob_json);
    axl_free(cname_esc); axl_free(m_json); axl_free(p_json);
    axl_free(s_json); axl_free(blob_json);
    axl_free(c->body);
    if (entry == NULL) {
        c->body = NULL;
        c->oom = true;
        return 1;
    }
    c->body = entry;
    c->count++;
    return 0;
}

static int
dump_usb(
    const char *fixture_dir
    )
{
    UsbWalkCtx ctx = {
        .body = axl_strdup(""),
        .dest = fixture_dir,
    };
    if (ctx.body == NULL) { return -1; }

    /* axl_usb_tree_for_each returns -1 when no USB stack is present —
       not an error for a manifest, just an empty interface list. The
       callback only returns non-zero (1) on OOM, which sets ctx.oom.
       Per-device descriptor blobs go to usb/<bus>-<addr>.bin via
       fixture_write (which creates the usb/ subdir on demand). */
    axl_usb_tree_for_each(usb_walk_cb, &ctx);

    if (ctx.oom) {
        axl_free(ctx.body);
        axl_printerr("mkfixture: out of memory composing usb.json\n");
        return -1;
    }

    char *json = axl_asprintf(
        "{\n"
        "  \"count\": %zu,\n"
        "  \"interfaces\": [\n"
        "%s\n"
        "  ]\n"
        "}\n",
        ctx.count, ctx.body);
    axl_free(ctx.body);
    if (json == NULL) {
        axl_printerr("mkfixture: out of memory composing usb.json\n");
        return -1;
    }

    int rc = fixture_write(fixture_dir, "usb.json", json, axl_strlen(json));
    if (rc == 0) {
        axl_printf("  usb.json         %zu USB interface%s\n",
                   ctx.count, ctx.count == 1 ? "" : "s");
    }
    axl_free(json);
    return rc;
}

// ---------------------------------------------------------------------------
// Network dump — net.json (per-NIC MAC + link state; manifest only —
// replay injects MACs via run-qemu.sh --mac, which is HF4+ scope)
// ---------------------------------------------------------------------------

/** Bind firmware-provided device drivers so protocols that only appear
    after a controller is connected (SNP over a NIC, NVMe pass-thru over
    a controller) are published before we enumerate. UEFI does NOT run
    the `connect -r` equivalent automatically for a shell app — without
    this the NIC / NVMe walks silently see zero devices (same reason
    netinfo forces it). Idempotent and run at most once per capture. */
static void
ensure_controllers_connected(
    void
    )
{
    static bool done = false;
    if (done) { return; }
    done = true;
    axl_driver_connect(NULL);
}

static const char *
snp_state_name(
    uint32_t state
    )
{
    switch (state) {
        case 0:  return "stopped";
        case 1:  return "started";
        case 2:  return "initialized";
        default: return "unknown";
    }
}

/** Format up to @p len bytes of a hardware address as colon-separated
    lower-hex into @p buf. @p len is clamped to the 32-byte EFI_MAC_ADDRESS
    capacity; a zero length yields an empty string. */
static void
format_mac(
    const uint8_t *addr,
    uint32_t       len,
    char          *buf,
    size_t         buflen
    )
{
    if (len > sizeof(((EFI_MAC_ADDRESS *)0)->Addr)) {
        len = sizeof(((EFI_MAC_ADDRESS *)0)->Addr);
    }
    size_t j = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < len && j + 4 < buflen; i++) {
        j += (size_t)axl_snprintf(buf + j, buflen - j, "%s%02x",
                                  (i == 0) ? "" : ":", (unsigned)addr[i]);
    }
}

static int
dump_net(
    const char *fixture_dir
    )
{
    ensure_controllers_connected();

    void  **handles = NULL;
    size_t  count   = 0;
    if (axl_protocol_enumerate("simple-network", &handles, &count) != AXL_OK) {
        count = 0;
    }

    char  *body    = axl_strdup("");
    if (body == NULL) { axl_free(handles); return -1; }
    size_t emitted = 0;
    bool   oom     = false;

    /* One physical NIC surfaces as several EFI_SIMPLE_NETWORK handles —
       the raw SnpDxe instance plus the SNP that the MNP / network stack
       republishes on its child handles — all sharing one burned-in MAC.
       A "per-NIC" manifest (whose replay injects one --mac per address)
       wants one entry per hardware address, so dedup on the permanent
       MAC. Past 64 distinct NICs we stop deduping rather than overflow
       (implausible on any real fixture target). */
    char seen_macs[64][100];
    int  seen_n = 0;

    for (size_t i = 0; i < count; i++) {
        void *iface = NULL;
        if (axl_handle_get_protocol(handles[i], "simple-network", &iface)
                != AXL_OK
            || iface == NULL) {
            continue;
        }
        EFI_SIMPLE_NETWORK_PROTOCOL *snp = (EFI_SIMPLE_NETWORK_PROTOCOL *)iface;
        EFI_SIMPLE_NETWORK_MODE     *m   = snp->Mode;
        if (m == NULL) { continue; }

        char mac[100], pmac[100];
        format_mac(m->CurrentAddress.Addr,   m->HwAddressSize, mac,  sizeof mac);
        format_mac(m->PermanentAddress.Addr, m->HwAddressSize, pmac, sizeof pmac);

        /* Dedup on a non-empty permanent MAC. An empty one (HwAddressSize
           0) can't identify a NIC, so such entries are always kept. */
        if (pmac[0] != '\0') {
            bool dup = false;
            for (int s = 0; s < seen_n; s++) {
                if (axl_strcmp(seen_macs[s], pmac) == 0) { dup = true; break; }
            }
            if (dup) { continue; }
            if (seen_n < (int)(sizeof seen_macs / sizeof seen_macs[0])) {
                axl_strlcpy(seen_macs[seen_n], pmac, sizeof seen_macs[0]);
                seen_n++;
            }
        }

        char *entry = axl_asprintf(
            "%s%s    {\n"
            "      \"index\": %zu,\n"
            "      \"state\": %u,\n"
            "      \"state_name\": \"%s\",\n"
            "      \"if_type\": %u,\n"
            "      \"hw_address_size\": %u,\n"
            "      \"mac\": \"%s\",\n"
            "      \"permanent_mac\": \"%s\",\n"
            "      \"media_present\": %s\n"
            "    }",
            body,
            (emitted == 0) ? "" : ",\n",
            emitted,
            (unsigned)m->State, snp_state_name(m->State),
            (unsigned)m->IfType,
            (unsigned)m->HwAddressSize,
            mac, pmac,
            m->MediaPresent ? "true" : "false");
        axl_free(body);
        if (entry == NULL) { body = NULL; oom = true; break; }
        body = entry;
        emitted++;
    }
    axl_free(handles);

    if (oom) {
        axl_free(body);
        axl_printerr("mkfixture: out of memory composing net.json\n");
        return -1;
    }

    char *json = axl_asprintf(
        "{\n"
        "  \"count\": %zu,\n"
        "  \"nics\": [\n"
        "%s\n"
        "  ]\n"
        "}\n",
        emitted, body);
    axl_free(body);
    if (json == NULL) {
        axl_printerr("mkfixture: out of memory composing net.json\n");
        return -1;
    }

    int rc = fixture_write(fixture_dir, "net.json", json, axl_strlen(json));
    if (rc == 0) {
        axl_printf("  net.json         %zu network interface%s\n",
                   emitted, emitted == 1 ? "" : "s");
    }
    axl_free(json);
    return rc;
}

// ---------------------------------------------------------------------------
// Video dump — video.json + edid/<n>.bin (GOP mode list + EDID; manifest
// only — replay uses QEMU's stock -vga for GOP discovery, and --edid for
// EDID injection, both HF4+ scope)
// ---------------------------------------------------------------------------

/** The current GOP's pixel format as a fixture string, via the public
    AxlGfx accessor (which AxlGfx normalizes away when drawing).
    Returns "unknown" when no GOP is available. */
static const char *
gop_current_pixel_format(
    void
    )
{
    AxlGfxPixelFormat fmt;
    if (axl_gfx_get_pixel_format(&fmt) != AXL_OK) {
        return "unknown";
    }
    switch (fmt) {
        case AXL_GFX_PIXEL_FORMAT_RGBX8:    return "rgbx8";
        case AXL_GFX_PIXEL_FORMAT_BGRX8:    return "bgrx8";
        case AXL_GFX_PIXEL_FORMAT_BITMASK:  return "bitmask";
        case AXL_GFX_PIXEL_FORMAT_BLT_ONLY: return "blt_only";
        default:                            return "unknown";
    }
}

/** Capture every EFI_EDID_DISCOVERED display's raw EDID bytes to
    edid/<n>.bin. EDID is best-effort: many GPUs / QEMU's std VGA never
    publish the protocol, in which case nothing is written. Returns the
    number of blobs written, or -1 if composing the edid/ path runs out
    of memory (a failed mkdir is non-fatal — returns 0). */
static int
capture_edid(
    const char *fixture_dir
    )
{
    axl_protocol_register_name(
        "edid-discovered",
        (const AxlGuid *)&EFI_EDID_DISCOVERED_PROTOCOL_GUID);
    void  **handles = NULL;
    size_t  count   = 0;
    if (axl_protocol_enumerate("edid-discovered", &handles, &count) != AXL_OK
        || count == 0) {
        axl_free(handles);
        return 0;
    }

    int written = 0;
    for (size_t i = 0; i < count; i++) {
        void *iface = NULL;
        if (axl_handle_get_protocol(handles[i], "edid-discovered", &iface)
                != AXL_OK
            || iface == NULL) {
            continue;
        }
        EFI_EDID_DISCOVERED_PROTOCOL *edid =
            (EFI_EDID_DISCOVERED_PROTOCOL *)iface;
        /* Standard EDID is 128 B; with extension blocks the spec tops out
           around 32 KiB. Clamp the firmware-supplied length to 64 KiB so
           a garbage SizeOfEdid on buggy real hardware can't drive a
           multi-GiB out-of-bounds read off a short Edid buffer. */
        if (edid->SizeOfEdid == 0 || edid->Edid == NULL
            || edid->SizeOfEdid > 64 * 1024) {
            continue;
        }

        char *relpath = axl_asprintf("edid/%zu.bin", i);
        if (relpath != NULL
            && fixture_write(fixture_dir, relpath, edid->Edid,
                             edid->SizeOfEdid) == 0) {
            written++;
        }
        axl_free(relpath);
    }
    axl_free(handles);
    return written;
}

static int
dump_video(
    const char *fixture_dir
    )
{
    bool     avail      = axl_gfx_available();
    uint32_t mode_count = avail ? axl_gfx_mode_count() : 0;

    char  *modes_body = axl_strdup("");
    if (modes_body == NULL) { return -1; }
    size_t emitted = 0;
    bool   oom     = false;

    for (uint32_t i = 0; i < mode_count; i++) {
        AxlGfxMode mode = {0};
        if (axl_gfx_query_mode(i, &mode) != AXL_OK) { continue; }
        char *entry = axl_asprintf(
            "%s%s    {\"index\": %u, \"width\": %u, \"height\": %u, "
            "\"stride\": %u}",
            modes_body,
            (emitted == 0) ? "" : ",\n",
            (unsigned)mode.index, (unsigned)mode.width,
            (unsigned)mode.height, (unsigned)mode.stride);
        axl_free(modes_body);
        if (entry == NULL) { modes_body = NULL; oom = true; break; }
        modes_body = entry;
        emitted++;
    }
    if (oom) {
        axl_free(modes_body);
        axl_printerr("mkfixture: out of memory composing video.json\n");
        return -1;
    }

    int      current = -1;
    uint32_t ci      = 0;
    if (avail && axl_gfx_current_mode(&ci) == AXL_OK) { current = (int)ci; }

    AxlGfxInfo info = {0};
    if (avail) { axl_gfx_get_info(&info); }

    /* EDID first so video.json can report the blob count. */
    int edid_written = capture_edid(fixture_dir);
    if (edid_written < 0) { edid_written = 0; }

    char *json = axl_asprintf(
        "{\n"
        "  \"available\": %s,\n"
        "  \"current_mode\": %d,\n"
        "  \"width\": %u,\n"
        "  \"height\": %u,\n"
        "  \"stride\": %u,\n"
        "  \"framebuffer\": \"0x%016llx\",\n"
        "  \"pixel_format\": \"%s\",\n"
        "  \"edid_count\": %d,\n"
        "  \"mode_count\": %zu,\n"
        "  \"modes\": [\n"
        "%s\n"
        "  ]\n"
        "}\n",
        avail ? "true" : "false",
        current,
        (unsigned)info.width, (unsigned)info.height, (unsigned)info.stride,
        (unsigned long long)info.framebuffer,
        gop_current_pixel_format(),
        edid_written,
        emitted,
        modes_body);
    axl_free(modes_body);
    if (json == NULL) {
        axl_printerr("mkfixture: out of memory composing video.json\n");
        return -1;
    }

    int rc = fixture_write(fixture_dir, "video.json", json, axl_strlen(json));
    if (rc == 0) {
        axl_printf("  video.json       %s, %zu mode%s, %d EDID blob%s\n",
                   avail ? "GOP available" : "no GOP",
                   emitted, emitted == 1 ? "" : "s",
                   edid_written, edid_written == 1 ? "" : "s");
    }
    axl_free(json);
    return rc;
}

// ---------------------------------------------------------------------------
// NVMe dump — nvme/<n>.json (Identify Controller + per-namespace summary
// via AxlNvme over EFI_NVM_EXPRESS_PASS_THRU; manifest only — replay of
// Identify responses is an HF9 QEMU-patch candidate)
// ---------------------------------------------------------------------------

/** Capture one controller's Identify + active-namespace summary into
    nvme/<index>.json via AxlNvme. Returns 0 on a written manifest (or a
    benign skip), -1 on a hard error. */
static int
dump_nvme_controller(
    const char *dest,
    AxlHandle   ctrl,
    size_t      index
    )
{
    AxlNvmeController c;
    if (axl_nvme_identify_controller(ctrl, &c) != AXL_OK) {
        axl_printf("  nvme/%zu.json     (skipped - Identify Controller failed)\n",
                   index);
        return 0;  /* not fatal: a quirky controller, skip it */
    }

    /* Walk active namespaces. The firmware iterator can report every id up
       to the controller's Nn (QEMU reports 256), most unallocated — an
       inactive namespace reports size_blocks 0, so record only the rest. */
    char    *body  = axl_strdup("");
    unsigned count = 0;
    if (body == NULL) { return -1; }
    uint32_t nsid = 0;
    while (count < 256 && (nsid = axl_nvme_namespace_next(ctrl, nsid)) != 0) {
        AxlNvmeNamespace ns;
        if (axl_nvme_identify_namespace(ctrl, nsid, &ns) != AXL_OK
            || ns.size_blocks == 0) {
            continue;
        }
        char *entry = axl_asprintf(
            "%s%s    {\"nsid\": %u, \"size_blocks\": %llu, \"lba_size\": %u}",
            body,
            (count == 0) ? "" : ",\n",
            (unsigned)ns.nsid,
            (unsigned long long)ns.size_blocks,
            ns.block_size);
        axl_free(body);
        if (entry == NULL) { return -1; }
        body = entry;
        count++;
    }

    char *sn = json_escape(c.serial);
    char *mn = json_escape(c.model);
    char *fr = json_escape(c.firmware);
    if (sn == NULL || mn == NULL || fr == NULL) {
        axl_free(sn); axl_free(mn); axl_free(fr); axl_free(body);
        axl_printerr("mkfixture: out of memory composing nvme manifest\n");
        return -1;
    }

    char *json = axl_asprintf(
        "{\n"
        "  \"vendor_id\": \"0x%04x\",\n"
        "  \"subsystem_vendor_id\": \"0x%04x\",\n"
        "  \"serial\": \"%s\",\n"
        "  \"model\": \"%s\",\n"
        "  \"firmware\": \"%s\",\n"
        "  \"namespace_count\": %u,\n"
        "  \"namespaces\": [\n"
        "%s\n"
        "  ]\n"
        "}\n",
        (unsigned)c.pci_vid, (unsigned)c.pci_ssvid, sn, mn, fr, count, body);
    axl_free(sn); axl_free(mn); axl_free(fr); axl_free(body);
    if (json == NULL) {
        axl_printerr("mkfixture: out of memory composing nvme manifest\n");
        return -1;
    }

    char *relpath = axl_asprintf("nvme/%zu.json", index);
    int   rc = -1;
    if (relpath != NULL) {
        rc = fixture_write(dest, relpath, json, axl_strlen(json));
        if (rc == 0) {
            axl_printf("  nvme/%zu.json     %u namespace%s\n",
                       index, count, count == 1 ? "" : "s");
        }
    }
    axl_free(relpath);
    axl_free(json);
    return rc;
}

static int
dump_nvme(
    const char *fixture_dir
    )
{
    ensure_controllers_connected();

    AxlHandle ctrl  = NULL;
    size_t    index = 0;
    int       rc    = 0;
    bool      any   = false;
    while ((ctrl = axl_nvme_next(ctrl)) != NULL) {
        any = true;
        if (dump_nvme_controller(fixture_dir, ctrl, index) != 0) {
            rc = 1;
        }
        index++;
    }
    if (!any) {
        axl_printf("  nvme/            (skipped - no NVMe controllers)\n");
    }
    return rc;
}

// ---------------------------------------------------------------------------
// SPD (HF4) — SMBus DIMM EEPROMs
// ---------------------------------------------------------------------------

/** HF4: capture every populated SMBus SPD EEPROM (0x50..0x57). Each
    raw blob goes to `spd/0xNN.bin` (the exact replay contract
    axl-emulate consumes — `spd/0xNN.bin` → `--spd 0xNN:FILE`), and a
    top-level `spd.json` carries the decoded per-slot summary.

    Gated by @p enabled (the `--spd` flag): a SMBus controller is only
    reachable through EFI_SMBUS_HC_PROTOCOL / EFI_I2C_MASTER_PROTOCOL,
    which many vendor firmwares don't publish, and the walk costs real
    SMBus transactions — so it's opt-in, not part of the always-on
    cheap capture. When enabled but no controller / no DIMMs respond,
    `spd/` is simply empty and spd.json reports count 0 (expected on
    QEMU without the shim, and on firmwares that hide the SMBus — see
    docs/AXL-Hardware-Fixture-Design.md). */
static int
dump_spd(
    const char *fixture_dir,
    bool        enabled
    )
{
    if (!enabled) {
        return 0;  /* opt-in; default fixtures carry no SPD */
    }

    char  *body    = axl_strdup("");
    if (body == NULL) { return -1; }
    size_t emitted = 0;
    bool   oom     = false;

    for (uint8_t *a = axl_spd_next(NULL); a != NULL; a = axl_spd_next(a)) {
        uint8_t addr = *a;
        uint8_t raw[AXL_SPD_RAW_MAX];
        size_t  len = 0;
        if (axl_spd_dump_raw(addr, raw, sizeof raw, &len) != AXL_OK
            || len == 0) {
            continue;
        }

        char rel[24];
        axl_snprintf(rel, sizeof rel, "spd/0x%02x.bin", (unsigned)addr);
        if (fixture_write(fixture_dir, rel, raw, len) != 0) {
            oom = true;
            break;
        }

        AxlSpdInfo info = {0};
        bool decoded = (axl_spd_decode(raw, len, &info) == AXL_OK);
        char *part_esc = json_escape(decoded ? info.part_number : "");
        if (part_esc == NULL) { oom = true; break; }

        char *entry = axl_asprintf(
            "%s%s    {\n"
            "      \"address\": \"0x%02x\",\n"
            "      \"raw_bytes\": %zu,\n"
            "      \"ddr_generation\": %u,\n"
            "      \"capacity_bytes\": %llu,\n"
            "      \"speed_mts\": %u,\n"
            "      \"ecc\": %s,\n"
            "      \"registered\": %s,\n"
            "      \"part_number\": \"%s\"\n"
            "    }",
            body,
            (emitted == 0) ? "" : ",\n",
            (unsigned)addr,
            len,
            (unsigned)(decoded ? info.ddr_generation : 0),
            (unsigned long long)(decoded ? info.capacity_bytes : 0),
            (unsigned)(decoded ? info.speed_mts : 0),
            (decoded && info.has_ecc)    ? "true" : "false",
            (decoded && info.registered) ? "true" : "false",
            part_esc);
        axl_free(part_esc);
        axl_free(body);
        if (entry == NULL) { body = NULL; oom = true; break; }
        body = entry;
        emitted++;
    }

    if (oom) {
        axl_free(body);
        axl_printerr("mkfixture: out of memory composing spd.json\n");
        return -1;
    }

    char *json = axl_asprintf(
        "{\n"
        "  \"count\": %zu,\n"
        "  \"slots\": [\n"
        "%s\n"
        "  ]\n"
        "}\n",
        emitted, body);
    axl_free(body);
    if (json == NULL) {
        axl_printerr("mkfixture: out of memory composing spd.json\n");
        return -1;
    }

    int rc = fixture_write(fixture_dir, "spd.json", json, axl_strlen(json));
    axl_free(json);
    if (rc == 0) {
        axl_printf("  spd.json         %zu DIMM SPD%s\n",
                   emitted, (emitted == 1) ? "" : "s");
    }
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
        "  \"fixture_format\": \"HF2.3\"\n"
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

    int rc = fixture_write(fixture_dir, "manifest.json", json, axl_strlen(json));
    if (rc == 0) {
        axl_printf("  manifest.json    %s / %s\n", vendor, product);
    }
    axl_free(json);
    return rc;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static const AxlArgDesc capture_flags[] = {
    { .name = "spd", .short_name = 's', .type = AXL_ARG_BOOL,
      .help = "Also capture SMBus DIMM SPD EEPROMs (0x50-0x57) to "
              "spd/0xNN.bin + spd.json. Needs an EFI SMBus/I2C "
              "controller; empty otherwise." },
    {0}
};

static const AxlArgDesc fixture_dir_arg[] = {
    { .name = "fixture-dir", .type = AXL_ARG_STRING, .required = true,
      .help = "Output directory, or an http(s):// URL to POST the fixture "
              "tarball to (for disk-less / net-only capture)" },
    {0}
};

static int
run_capture(
    AxlArgs *a
    )
{
    const char *dest = axl_args_get_string(a, "fixture-dir");
    if (dest == NULL || dest[0] == '\0') {
        axl_printerr("mkfixture: fixture-dir is required\n");
        return 1;
    }

    bool url_mode = dest_is_url(dest);
    if (url_mode) {
        /* No filesystem: every artifact is appended to an in-memory tar
           that fixture_finish POSTs to the collector at the end. */
        g_tar_buf = axl_bufopen();
        g_tar     = (g_tar_buf != NULL) ? axl_tar_writer_new(g_tar_buf) : NULL;
        if (g_tar == NULL) {
            axl_printerr("mkfixture: out of memory preparing fixture tarball\n");
            axl_fclose(g_tar_buf);
            return 1;
        }
        axl_printf("mkfixture: capturing fixture for POST to %s\n", dest);
    } else {
        if (axl_dir_mkdir(dest) != AXL_OK && !axl_file_is_dir(dest)) {
            axl_printerr("mkfixture: cannot create or access %s\n", dest);
            return 1;
        }
        axl_printf("mkfixture: writing fixture to %s\n", dest);
    }

    bool want_spd = axl_args_get_bool(a, "spd");

    int rc = 0;
    if (dump_smbios(dest) != 0)    { rc = 1; }
    if (dump_acpi(dest) != 0)      { rc = 1; }
    if (dump_cpu(dest) != 0)       { rc = 1; }
    if (dump_esrt(dest) != 0)      { rc = 1; }
    if (dump_pci(dest) != 0)       { rc = 1; }
    if (dump_usb(dest) != 0)       { rc = 1; }
    if (dump_net(dest) != 0)       { rc = 1; }
    if (dump_video(dest) != 0)     { rc = 1; }
    if (dump_nvme(dest) != 0)      { rc = 1; }
    if (dump_spd(dest, want_spd) != 0) { rc = 1; }
    if (write_manifest(dest) != 0) { rc = 1; }

    /* URL mode: finalize + POST the tarball; local mode: no-op. */
    if (fixture_finish(dest) != 0) { rc = 1; }
    if (g_tar != NULL)     { axl_tar_writer_free(g_tar); g_tar = NULL; }
    if (g_tar_buf != NULL) { axl_fclose(g_tar_buf); g_tar_buf = NULL; }

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
        .help        = "Capture a UEFI hardware fixture (HF2 - see "
                       "docs/AXL-Hardware-Fixture-Design.md)",
        .flags       = capture_flags,
        .positionals = fixture_dir_arg,
        .handler     = run_capture,
    });
}
