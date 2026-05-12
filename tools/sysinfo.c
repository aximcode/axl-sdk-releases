/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file SysInfo.c
    System inventory tool (UEFI lshw/dmidecode equivalent).

    Build with axl-cc:
      axl-cc SysInfo.c -o SysInfo.efi

    Usage:
      SysInfo.efi [-v] [-h] [cpu|mem|fw|smbios|arch]
**/

#include <axl.h>
#include <axl/axl-smbios.h>

static bool verbose = false;

static const AxlArgDesc flags[] = {
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose output" },
    {0}
};

static const AxlArgDesc section[] = {
    { .name = "section", .type = AXL_ARG_STRING,
      .help = "Optional: cpu | mem | fw | smbios | arch (default: all)" },
    {0}
};

// ===========================================================================
// CPU Info
// ===========================================================================

#ifdef __x86_64__

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

static void
show_cpu_info(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint32_t max_leaf, max_ext_leaf;
    char vendor[13];
    char brand[49];
    uint32_t family, model, stepping;

    axl_printf("=== CPU ===\n");

    /* Leaf 0: vendor string and max leaf */
    cpuid(0, 0, &max_leaf,
          (uint32_t *)&vendor[0],
          (uint32_t *)&vendor[8],
          (uint32_t *)&vendor[4]);
    vendor[12] = '\0';

    /* Leaf 1: family/model/stepping and features */
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);

    family   = (eax >> 8) & 0xF;
    model    = (eax >> 4) & 0xF;
    stepping = eax & 0xF;

    if (family == 0x6 || family == 0xF) {
        model += ((eax >> 16) & 0xF) << 4;
    }
    if (family == 0xF) {
        family += (eax >> 20) & 0xFF;
    }

    /* Extended leaves: brand string */
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

        /* Skip leading spaces */
        char *b = brand;
        while (*b == ' ') {
            b++;
        }
        axl_printf("  CPU:        %s\n", b);
    } else {
        axl_printf("  CPU:        %s Family %u Model %u\n",
                   vendor, family, model);
    }

    axl_printf("  Vendor:     %s\n", vendor);
    axl_printf("  Family:     %u  Model: %u  Stepping: %u\n",
               family, model, stepping);

    uint32_t logical_count = (ebx >> 16) & 0xFF;
    if (logical_count > 0) {
        axl_printf("  Logical:    %u (max per package)\n", logical_count);
    }

    if (verbose) {
        axl_printf("  Features:   ");
        /* EDX features */
        if (edx & (1U << 23)) { axl_printf("MMX "); }
        if (edx & (1U << 25)) { axl_printf("SSE "); }
        if (edx & (1U << 26)) { axl_printf("SSE2 "); }
        if (edx & (1U << 28)) { axl_printf("HTT "); }
        /* ECX features */
        if (ecx & (1U << 0))  { axl_printf("SSE3 "); }
        if (ecx & (1U << 9))  { axl_printf("SSSE3 "); }
        if (ecx & (1U << 19)) { axl_printf("SSE4.1 "); }
        if (ecx & (1U << 20)) { axl_printf("SSE4.2 "); }
        if (ecx & (1U << 25)) { axl_printf("AES-NI "); }
        if (ecx & (1U << 28)) { axl_printf("AVX "); }
        axl_printf("\n");

        /* Extended features (leaf 7) */
        if (max_leaf >= 7) {
            uint32_t ebx7, ecx7;
            cpuid(7, 0, NULL, &ebx7, &ecx7, NULL);
            axl_printf("  Extended:   ");
            if (ebx7 & (1U << 5))  { axl_printf("AVX2 "); }
            if (ebx7 & (1U << 16)) { axl_printf("AVX512F "); }
            if (ebx7 & (1U << 29)) { axl_printf("SHA "); }
            if (ecx7 & (1U << 1))  { axl_printf("AVX512-VBMI "); }
            axl_printf("\n");
        }
    }

    axl_printf("\n");
}

#elif defined(__aarch64__)

typedef struct {
    uint8_t     code;
    const char *name;
} implementer_name;

static const implementer_name implementers[] = {
    { 0x41, "ARM" },
    { 0x42, "Broadcom" },
    { 0x43, "Cavium" },
    { 0x46, "Fujitsu" },
    { 0x48, "HiSilicon" },
    { 0x4E, "NVIDIA" },
    { 0x50, "APM" },
    { 0x51, "Qualcomm" },
    { 0x56, "Marvell" },
    { 0x61, "Apple" },
    { 0xC0, "Ampere" },
    { 0,    NULL }
};

static const char *
lookup_implementer(
    uint8_t code
    )
{
    for (size_t i = 0; implementers[i].name != NULL; i++) {
        if (implementers[i].code == code) {
            return implementers[i].name;
        }
    }
    return "Unknown";
}

static void
show_cpu_info(void)
{
    uint64_t midr;
    uint8_t  imp, variant, arch, revision;
    uint16_t part_num;

    axl_printf("=== CPU ===\n");

    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));

    imp      = (uint8_t)((midr >> 24) & 0xFF);
    variant  = (uint8_t)((midr >> 20) & 0xF);
    arch     = (uint8_t)((midr >> 16) & 0xF);
    part_num = (uint16_t)((midr >> 4) & 0xFFF);
    revision = (uint8_t)(midr & 0xF);

    axl_printf("  Implementer: %s (0x%02x)\n", lookup_implementer(imp), imp);
    axl_printf("  Part:        0x%03x  Variant: r%up%u\n",
               part_num, variant, revision);
    axl_printf("  Architecture: %u\n", arch);

    if (verbose) {
        axl_printf("  MIDR_EL1:    0x%016llx\n", (unsigned long long)midr);

        uint64_t isar0;
        __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
        axl_printf("  Features:    ");
        if (((isar0 >> 4) & 0xF) >= 1)  { axl_printf("AES "); }
        if (((isar0 >> 4) & 0xF) >= 2)  { axl_printf("PMULL "); }
        if (((isar0 >> 8) & 0xF) >= 1)  { axl_printf("SHA1 "); }
        if (((isar0 >> 12) & 0xF) >= 1) { axl_printf("SHA256 "); }
        if (((isar0 >> 16) & 0xF) >= 1) { axl_printf("CRC32 "); }
        if (((isar0 >> 20) & 0xF) >= 1) { axl_printf("Atomics "); }
        axl_printf("\n");
    }

    axl_printf("\n");
}

#else
#error "Unsupported architecture"
#endif

// ===========================================================================
// Memory Info
// ===========================================================================

static void
show_mem_info(void)
{
    axl_printf("=== Memory ===\n");

    /* Total usable memory */
    uint64_t total_bytes = 0;
    if (axl_sys_get_memory_size(&total_bytes) == AXL_OK && total_bytes > 0) {
        uint64_t total_mb = total_bytes / (1024 * 1024);
        if (total_mb >= 1024) {
            axl_printf("  Total:      %llu MB (%llu GB)\n",
                       (unsigned long long)total_mb,
                       (unsigned long long)(total_mb / 1024));
        } else {
            axl_printf("  Total:      %llu MB\n",
                       (unsigned long long)total_mb);
        }
    }

    /* SMBIOS Type 17 — DIMM info */
    size_t dimm_count = 0;
    AxlSmbiosHeader *hdr = axl_smbios_find(17);

    while (hdr != NULL) {
        if (hdr->Length >= 0x15) {
            uint8_t *raw = (uint8_t *)hdr;
            uint16_t size_raw = *(uint16_t *)(raw + 0x0C);

            if (size_raw != 0 && size_raw != 0xFFFF) {
                uint32_t size_mb;
                if (size_raw == 0x7FFF) {
                    if (hdr->Length >= 0x20) {
                        size_mb = *(uint32_t *)(raw + 0x1C) & 0x7FFFFFFF;
                    } else {
                        goto next_dimm;
                    }
                } else {
                    size_mb = size_raw;
                    if (size_raw & 0x8000) {
                        size_mb = (size_raw & 0x7FFF) / 1024;
                    }
                }

                dimm_count++;

                if (verbose) {
                    uint8_t mem_type = raw[0x12];
                    const char *type_str = "Unknown";
                    switch (mem_type) {
                        case 24: type_str = "DDR3"; break;
                        case 26: type_str = "DDR4"; break;
                        case 27: type_str = "LPDDR4"; break;
                        case 34: type_str = "DDR5"; break;
                        case 35: type_str = "LPDDR5"; break;
                    }

                    uint16_t speed = *(uint16_t *)(raw + 0x15);

                    axl_printf("  DIMM %zu:     %u MB %s",
                               dimm_count, size_mb, type_str);
                    if (speed > 0) {
                        axl_printf("-%u", speed);
                    }
                    axl_printf(" [%s]\n", axl_smbios_get_string_utf8(hdr, raw[0x10]));
                    axl_printf("              Mfr: %s",
                               axl_smbios_get_string_utf8(hdr, raw[0x11]));
                    if (hdr->Length > 0x18) {
                        axl_printf("  P/N: %s",
                                   axl_smbios_get_string_utf8(hdr, raw[0x18]));
                    }
                    axl_printf("\n");
                }
            }
        }
next_dimm:
        hdr = axl_smbios_find_next(17, hdr);
    }

    if (dimm_count > 0 && !verbose) {
        axl_printf("  DIMMs:      %zu populated\n", dimm_count);
    }

    axl_printf("\n");
}

// ===========================================================================
// Firmware Info
// ===========================================================================

static void
show_fw_info(void)
{
    axl_printf("=== Firmware ===\n");

    /* Firmware vendor and revision */
    AxlFirmwareInfo fw;
    if (axl_sys_get_firmware_info(&fw) == AXL_OK) {
        axl_printf("  Vendor:     %s\n", fw.vendor);
        axl_printf("  Revision:   0x%08x\n", fw.firmware_revision);
        axl_printf("  UEFI:       %u.%u\n", fw.spec_major, fw.spec_minor);
    }

    /* Secure Boot state */
    uint8_t sb_val = 0;
    size_t sb_size = sizeof(sb_val);
    if (axl_nvstore_get("global", "SecureBoot", &sb_val, &sb_size) == AXL_OK) {
        axl_printf("  Secure Boot: %s", sb_val ? "Enabled" : "Disabled");

        uint8_t setup_mode = 0;
        size_t sm_size = sizeof(setup_mode);
        if (axl_nvstore_get("global", "SetupMode",
                            &setup_mode, &sm_size) == AXL_OK) {
            axl_printf(" (%s)",
                       setup_mode ? "Setup Mode" : "User Mode");
        }
        axl_printf("\n");
    } else {
        axl_printf("  Secure Boot: not available\n");
    }

    if (verbose) {
        /* Check PK enrollment — pass NULL buf to query size */
        size_t pk_size = 0;
        axl_nvstore_get("global", "PK", NULL, &pk_size);
        axl_printf("  PK:         %s\n",
                   (pk_size > 0) ? "enrolled" : "not enrolled");
    }

    /* TPM — skip for now until axl_protocol_find supports "tcg2" */
    void *tcg2 = NULL;
    if (axl_protocol_find("tcg2", &tcg2) == AXL_OK && tcg2 != NULL) {
        axl_printf("  TPM:        present\n");
    } else {
        axl_printf("  TPM:        not detected\n");
    }

    axl_printf("\n");
}

// ===========================================================================
// SMBIOS Info
// ===========================================================================

static void
show_smbios_info(void)
{
    axl_printf("=== System (SMBIOS) ===\n");

    /* Type 1 — System Information */
    AxlSmbiosHeader *hdr = axl_smbios_find(1);
    if (hdr != NULL && hdr->Length >= 0x19) {
        uint8_t *raw = (uint8_t *)hdr;
        axl_printf("  Manufacturer: %s\n", axl_smbios_get_string_utf8(hdr, raw[0x04]));
        axl_printf("  Product:      %s\n", axl_smbios_get_string_utf8(hdr, raw[0x05]));
        axl_printf("  Version:      %s\n", axl_smbios_get_string_utf8(hdr, raw[0x06]));
        axl_printf("  Serial:       %s\n", axl_smbios_get_string_utf8(hdr, raw[0x07]));

        if (verbose && hdr->Length >= 0x1B) {
            axl_printf("  SKU:          %s\n",
                       axl_smbios_get_string_utf8(hdr, raw[0x19]));
            axl_printf("  Family:       %s\n",
                       axl_smbios_get_string_utf8(hdr, raw[0x1A]));

            /* UUID at offset 0x08 (16 bytes) — apply SMBIOS §7.2.1
               mixed-endian field-order swap. */
            char uuid_str[37];
            axl_smbios_format_uuid(raw + 0x08, uuid_str);
            axl_printf("  UUID:         %s\n", uuid_str);
        }
    }

    /* Type 0 — BIOS Information */
    hdr = axl_smbios_find(0);
    if (hdr != NULL && hdr->Length >= 0x12) {
        uint8_t *raw = (uint8_t *)hdr;
        axl_printf("  BIOS:         %s", axl_smbios_get_string_utf8(hdr, raw[0x04]));
        axl_printf(" %s", axl_smbios_get_string_utf8(hdr, raw[0x05]));
        axl_printf(" (%s)\n", axl_smbios_get_string_utf8(hdr, raw[0x08]));
    }

    /* Type 2 — Baseboard (verbose only) */
    if (verbose) {
        hdr = axl_smbios_find(2);
        if (hdr != NULL && hdr->Length >= 0x08) {
            uint8_t *raw = (uint8_t *)hdr;
            axl_printf("  Board:        %s",
                       axl_smbios_get_string_utf8(hdr, raw[0x04]));
            axl_printf(" %s", axl_smbios_get_string_utf8(hdr, raw[0x05]));
            axl_printf(" (S/N: %s)\n",
                       axl_smbios_get_string_utf8(hdr, raw[0x07]));
        }
    }

    axl_printf("\n");
}

// ===========================================================================
// Entry point
// ===========================================================================

static int
run_sysinfo(AxlArgs *a)
{
    verbose = axl_args_get_bool(a, "verbose");

    const char *section = axl_args_get_string(a, "section");

    /* "arch" — print architecture tag and exit */
    if (section != NULL && axl_strcmp(section, "arch") == 0) {
#ifdef __x86_64__
        axl_printf("x64\n");
#elif defined(__aarch64__)
        axl_printf("aa64\n");
#endif
        return 0;
    }

    bool show_all = (section == NULL);

    if (show_all || axl_strcmp(section, "cpu") == 0) {
        show_cpu_info();
    }
    if (show_all || axl_strcmp(section, "mem") == 0) {
        show_mem_info();
    }
    if (show_all || axl_strcmp(section, "fw") == 0) {
        show_fw_info();
    }
    if (show_all || axl_strcmp(section, "smbios") == 0) {
        show_smbios_info();
    }

    if (!show_all &&
        axl_strcmp(section, "cpu") != 0 &&
        axl_strcmp(section, "mem") != 0 &&
        axl_strcmp(section, "fw") != 0 &&
        axl_strcmp(section, "smbios") != 0 &&
        axl_strcmp(section, "arch") != 0) {
        axl_printf("SysInfo: unknown section '%s'\n", section);
        return 1;
    }

    return 0;
}

AXL_TOOL_MAIN(sysinfo)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "SysInfo",
        .help         = "Print system information (CPU, memory, firmware, SMBIOS, arch)",
        .flags        = flags,
        .positionals  = section,
        .handler      = run_sysinfo,
    });
}
