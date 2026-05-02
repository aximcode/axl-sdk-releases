/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file dmidecode.c
    dmidecode — decode SMBIOS / DMI tables in the style of the Linux
    `dmidecode(8)` utility.

    Build with axl-cc:
      axl-cc dmidecode.c -o dmidecode.efi

    Usage:
      dmidecode.efi                  Dump every record (one section per entry)
      dmidecode.efi -t <type>        Filter by SMBIOS type (0, 1, 17, 42, ...)
      dmidecode.efi -s <keyword>     Print a single string value:
                                       bios-vendor, bios-version, bios-release-date
                                       system-manufacturer, system-product-name,
                                       system-version, system-serial-number,
                                       system-uuid, system-sku-number, system-family
                                       baseboard-manufacturer, baseboard-product-name,
                                       baseboard-version, baseboard-serial-number,
                                       baseboard-asset-tag
                                       chassis-manufacturer, chassis-version,
                                       chassis-serial-number, chassis-asset-tag
      dmidecode.efi -q               Quiet (fewer decorations)
      dmidecode.efi -V               Print SMBIOS version + exit
**/

#include <axl.h>
#include <axl/axl-smbios.h>

static const AxlArgDesc flags[] = {
    { .name = "type",    .short_name = 't', .type = AXL_ARG_STRING,
      .help = "Filter output to records of this SMBIOS type" },
    { .name = "string",  .short_name = 's', .type = AXL_ARG_STRING,
      .help = "Print a single named value (see --help for keywords)" },
    { .name = "quiet",   .short_name = 'q', .type = AXL_ARG_BOOL,
      .help = "Suppress section headers and blank lines" },
    { .name = "version", .short_name = 'V', .type = AXL_ARG_BOOL,
      .help = "Print SMBIOS specification version and exit" },
    {0}
};

// ---------------------------------------------------------------------------
// Pretty-print helpers
// ---------------------------------------------------------------------------

static const char *
type_name(uint8_t t)
{
    switch (t) {
        case 0:   return "BIOS Information";
        case 1:   return "System Information";
        case 2:   return "Base Board Information";
        case 3:   return "Chassis Information";
        case 4:   return "Processor Information";
        case 7:   return "Cache Information";
        case 8:   return "Port Connector Information";
        case 9:   return "System Slot";
        case 11:  return "OEM Strings";
        case 13:  return "BIOS Language Information";
        case 16:  return "Physical Memory Array";
        case 17:  return "Memory Device";
        case 19:  return "Memory Array Mapped Address";
        case 32:  return "System Boot Information";
        case 38:  return "IPMI Device Information";
        case 42:  return "Management Controller Host Interface";
        case 127: return "End Of Table";
        default:  return "Unknown";
    }
}

static void
print_field(const char *name, const char *value)
{
    axl_printf("\t%s: %s\n", name, (value != NULL && value[0] != '\0') ? value : "Not Specified");
}

static void
print_uint(const char *name, uint32_t value, const char *unit)
{
    axl_printf("\t%s: %u %s\n", name, value, unit != NULL ? unit : "");
}

static const char *
chassis_type_name(uint8_t t)
{
    switch (t) {
        case 0x01: return "Other";
        case 0x02: return "Unknown";
        case 0x03: return "Desktop";
        case 0x04: return "Low Profile Desktop";
        case 0x05: return "Pizza Box";
        case 0x06: return "Mini Tower";
        case 0x07: return "Tower";
        case 0x08: return "Portable";
        case 0x09: return "Laptop";
        case 0x0A: return "Notebook";
        case 0x0B: return "Hand Held";
        case 0x0C: return "Docking Station";
        case 0x0D: return "All in One";
        case 0x0E: return "Sub Notebook";
        case 0x0F: return "Space-saving";
        case 0x10: return "Lunch Box";
        case 0x11: return "Main Server Chassis";
        case 0x12: return "Expansion Chassis";
        case 0x17: return "Rack Mount Chassis";
        case 0x1D: return "Blade";
        default:   return "Other";
    }
}

static const char *
ipmi_interface_name(uint8_t t)
{
    switch (t) {
        case AXL_SMBIOS_IPMI_UNKNOWN: return "Unknown";
        case AXL_SMBIOS_IPMI_KCS:     return "KCS (Keyboard Controller Style)";
        case AXL_SMBIOS_IPMI_SMIC:    return "SMIC (Server Management Interface Chip)";
        case AXL_SMBIOS_IPMI_BT:      return "BT (Block Transfer)";
        case AXL_SMBIOS_IPMI_SSIF:    return "SSIF (SMBus System Interface)";
        default:                      return "Reserved";
    }
}

static const char *
hif_type_name(uint8_t t)
{
    switch (t) {
        case AXL_SMBIOS_HIF_KCS:        return "KCS";
        case AXL_SMBIOS_HIF_UART_8250:  return "UART 8250";
        case AXL_SMBIOS_HIF_UART_16450: return "UART 16450";
        case AXL_SMBIOS_HIF_UART_16550: return "UART 16550";
        case AXL_SMBIOS_HIF_UART_16650: return "UART 16650";
        case AXL_SMBIOS_HIF_UART_16750: return "UART 16750";
        case AXL_SMBIOS_HIF_UART_16850: return "UART 16850";
        case AXL_SMBIOS_HIF_NETWORK:    return "Network Host Interface";
        case AXL_SMBIOS_HIF_OEM:        return "OEM";
        default:                        return "Unknown";
    }
}

static const char *
hip_type_name(uint8_t p)
{
    switch (p) {
        case AXL_SMBIOS_HIP_IPMI:            return "IPMI";
        case AXL_SMBIOS_HIP_MCTP:            return "MCTP";
        case AXL_SMBIOS_HIP_REDFISH_OVER_IP: return "Redfish over IP";
        case AXL_SMBIOS_HIP_OEM:             return "OEM";
        default:                             return "Unknown";
    }
}

static void
print_uuid(const char *name, const uint8_t u[16], bool has)
{
    if (!has) {
        axl_printf("\t%s: Not Present\n", name);
        return;
    }
    char buf[37];
    axl_smbios_format_uuid(u, buf);
    axl_printf("\t%s: %s\n", name, buf);
}

// ---------------------------------------------------------------------------
// Per-type decoders
// ---------------------------------------------------------------------------

static void
decode_bios(AxlSmbiosHeader *hdr)
{
    AxlSmbiosBiosInfo b;
    if (axl_smbios_read_bios_info(&b) != 0) { return; }
    (void)hdr;
    print_field("Vendor",       b.vendor);
    print_field("Version",      b.version);
    print_field("Release Date", b.release_date);
    if (b.major_release != 0xFF) {
        axl_printf("\tBIOS Revision: %u.%u\n", b.major_release, b.minor_release);
    }
}

static void
decode_system(AxlSmbiosHeader *hdr)
{
    AxlSmbiosSystemInfo s;
    if (axl_smbios_read_system_info(&s) != 0) { return; }
    (void)hdr;
    print_field("Manufacturer",   s.manufacturer);
    print_field("Product Name",   s.product_name);
    print_field("Version",        s.version);
    print_field("Serial Number",  s.serial_number);
    print_uuid ("UUID",           s.uuid, s.has_uuid);
    print_field("SKU Number",     s.sku);
    print_field("Family",         s.family);
}

static void
decode_baseboard(AxlSmbiosHeader *hdr)
{
    AxlSmbiosBaseboardInfo b;
    if (axl_smbios_read_baseboard(&b) != 0) { return; }
    (void)hdr;
    print_field("Manufacturer",  b.manufacturer);
    print_field("Product Name",  b.product_name);
    print_field("Version",       b.version);
    print_field("Serial Number", b.serial_number);
    print_field("Asset Tag",     b.asset_tag);
}

static void
decode_chassis(AxlSmbiosHeader *hdr)
{
    AxlSmbiosChassisInfo c;
    if (axl_smbios_read_chassis(&c) != 0) { return; }
    (void)hdr;
    print_field("Manufacturer",  c.manufacturer);
    axl_printf("\tType: %s\n", chassis_type_name(c.type));
    print_field("Version",       c.version);
    print_field("Serial Number", c.serial_number);
    print_field("Asset Tag",     c.asset_tag);
}

static void
decode_processor(AxlSmbiosHeader *hdr)
{
    AxlSmbiosProcessorInfo p;
    if (axl_smbios_read_processor(hdr, &p) != 0) { return; }
    print_field("Socket Designation", p.socket_designation);
    print_field("Manufacturer",       p.manufacturer);
    print_field("Version",            p.version);
    axl_printf("\tFamily: 0x%02X\n", p.family);
    print_uint("Max Speed",     p.max_speed_mhz,     "MHz");
    print_uint("Current Speed", p.current_speed_mhz, "MHz");
    if (p.core_count > 0) {
        axl_printf("\tCore Count: %u\n",   p.core_count);
        axl_printf("\tThread Count: %u\n", p.thread_count);
    }
    axl_printf("\tStatus: 0x%02X\n", p.status);
    print_field("Serial Number", p.serial_number);
    print_field("Asset Tag",     p.asset_tag);
    print_field("Part Number",   p.part_number);
}

static void
decode_memory_device(AxlSmbiosHeader *hdr)
{
    AxlSmbiosMemoryDevice m;
    if (axl_smbios_read_memory_device(hdr, &m) != 0) { return; }
    print_field("Locator",      m.device_locator);
    print_field("Bank Locator", m.bank_locator);
    if (m.size_mb == 0) {
        axl_printf("\tSize: No Module Installed\n");
    } else {
        print_uint("Size", m.size_mb, "MB");
    }
    if (m.speed_mhz != 0) {
        print_uint("Speed", m.speed_mhz, "MT/s");
    }
    axl_printf("\tType: 0x%02X\n", m.memory_type);
    print_field("Manufacturer",  m.manufacturer);
    print_field("Part Number",   m.part_number);
    print_field("Serial Number", m.serial_number);
    print_field("Asset Tag",     m.asset_tag);
}

// ---------------------------------------------------------------------------
// Fallback decoders — strings + hex dump (used when we don't have a
// specialized decoder for this record's type, and opt-in via --raw for
// types we do decode).
// ---------------------------------------------------------------------------

static void
print_record_strings(AxlSmbiosHeader *hdr)
{
    /* Strings live at hdr + hdr->Length (1-based index). Double-NUL ends
       the list. Returning NUL with index > 0 in the first byte means no
       strings. Walk until the outer NUL. */
    const char *s = axl_smbios_get_string_utf8(hdr, 1);
    if (s[0] == '\0') {
        return;
    }
    axl_printf("\tStrings:\n");
    for (uint8_t idx = 1; idx < 0xFF; idx++) {
        const char *str = axl_smbios_get_string_utf8(hdr, idx);
        if (str[0] == '\0') { break; }
        axl_printf("\t\t%u: %s\n", idx, str);
    }
}

static void
print_record_hex(AxlSmbiosHeader *hdr)
{
    /* Dump the formatted area: offset 0x00 (Type) through hdr->Length-1.
       Matches `dmidecode -u` Header-and-Data style. */
    const uint8_t *b = (const uint8_t *)hdr;
    axl_printf("\tHeader and Data:\n");
    for (size_t i = 0; i < hdr->Length; i += 16) {
        axl_printf("\t\t");
        for (size_t j = 0; j < 16 && (i + j) < hdr->Length; j++) {
            axl_printf("%02X ", b[i + j]);
        }
        axl_printf("\n");
    }
}

static void
decode_ipmi_device_info(AxlSmbiosHeader *hdr)
{
    (void)hdr;
    AxlSmbiosIpmiDeviceInfo ip;
    if (axl_smbios_read_ipmi_device_info(&ip) != 0) { return; }
    axl_printf("\tInterface Type: %s (0x%02X)\n",
               ipmi_interface_name(ip.interface_type), ip.interface_type);
    axl_printf("\tSpecification Version: %u.%u\n", ip.spec_major, ip.spec_minor);
    axl_printf("\tI2C Target Address: 0x%02X\n", ip.i2c_target_address);
    if (ip.nv_storage_address != 0xFF) {
        axl_printf("\tNV Storage Device Address: 0x%02X\n", ip.nv_storage_address);
    } else {
        axl_printf("\tNV Storage Device: Not Present\n");
    }
    axl_printf("\tBase Address: 0x%016lX (%s)\n",
               (unsigned long)ip.base_address,
               ip.is_memory_mapped ? "Memory-mapped" : "I/O");
    if (ip.interrupt_number != 0) {
        axl_printf("\tInterrupt Number: %u\n", ip.interrupt_number);
    }
}

static void
decode_host_interface(AxlSmbiosHeader *hdr)
{
    AxlSmbiosHostInterface h;
    if (axl_smbios_read_host_interface(hdr, &h) != 0) {
        axl_printf("\t(pre-3.0 host interface layout — not decoded)\n");
        return;
    }
    axl_printf("\tInterface Type: %s (0x%02X)\n",
               hif_type_name(h.interface_type), h.interface_type);
    axl_printf("\tInterface Data Length: %u\n", h.interface_data_len);
    axl_printf("\tProtocol Count: %u\n", h.protocol_count);
    for (uint8_t i = 0; i < h.protocol_count; i++) {
        axl_printf("\t  Protocol %u: %s (0x%02X)  data=%u bytes\n",
                   i,
                   hip_type_name(h.protocols[i].protocol_type),
                   h.protocols[i].protocol_type,
                   h.protocols[i].data_len);
    }
}

// ---------------------------------------------------------------------------
// -s single-value mode
// ---------------------------------------------------------------------------

static int
single_string(const char *keyword)
{
    AxlSmbiosBiosInfo bi;
    AxlSmbiosSystemInfo si;
    AxlSmbiosBaseboardInfo ba;
    AxlSmbiosChassisInfo ch;

    if (axl_strcmp(keyword, "bios-vendor") == 0
        && axl_smbios_read_bios_info(&bi) == 0) {
        axl_printf("%s\n", bi.vendor);
        return 0;
    }
    if (axl_strcmp(keyword, "bios-version") == 0
        && axl_smbios_read_bios_info(&bi) == 0) {
        axl_printf("%s\n", bi.version);
        return 0;
    }
    if (axl_strcmp(keyword, "bios-release-date") == 0
        && axl_smbios_read_bios_info(&bi) == 0) {
        axl_printf("%s\n", bi.release_date);
        return 0;
    }
    if (axl_smbios_read_system_info(&si) == 0) {
        if (axl_strcmp(keyword, "system-manufacturer")  == 0) { axl_printf("%s\n", si.manufacturer);  return 0; }
        if (axl_strcmp(keyword, "system-product-name")  == 0) { axl_printf("%s\n", si.product_name);  return 0; }
        if (axl_strcmp(keyword, "system-version")       == 0) { axl_printf("%s\n", si.version);       return 0; }
        if (axl_strcmp(keyword, "system-serial-number") == 0) { axl_printf("%s\n", si.serial_number); return 0; }
        if (axl_strcmp(keyword, "system-sku-number")    == 0) { axl_printf("%s\n", si.sku != NULL ? si.sku : ""); return 0; }
        if (axl_strcmp(keyword, "system-family")        == 0) { axl_printf("%s\n", si.family != NULL ? si.family : ""); return 0; }
        if (axl_strcmp(keyword, "system-uuid") == 0) {
            if (si.has_uuid) {
                char buf[37];
                axl_smbios_format_uuid(si.uuid, buf);
                axl_printf("%s\n", buf);
            } else {
                axl_printf("Not Present\n");
            }
            return 0;
        }
    }
    if (axl_smbios_read_baseboard(&ba) == 0) {
        if (axl_strcmp(keyword, "baseboard-manufacturer")  == 0) { axl_printf("%s\n", ba.manufacturer);  return 0; }
        if (axl_strcmp(keyword, "baseboard-product-name")  == 0) { axl_printf("%s\n", ba.product_name);  return 0; }
        if (axl_strcmp(keyword, "baseboard-version")       == 0) { axl_printf("%s\n", ba.version);       return 0; }
        if (axl_strcmp(keyword, "baseboard-serial-number") == 0) { axl_printf("%s\n", ba.serial_number); return 0; }
        if (axl_strcmp(keyword, "baseboard-asset-tag")     == 0) { axl_printf("%s\n", ba.asset_tag);     return 0; }
    }
    if (axl_smbios_read_chassis(&ch) == 0) {
        if (axl_strcmp(keyword, "chassis-manufacturer")  == 0) { axl_printf("%s\n", ch.manufacturer);  return 0; }
        if (axl_strcmp(keyword, "chassis-version")       == 0) { axl_printf("%s\n", ch.version);       return 0; }
        if (axl_strcmp(keyword, "chassis-serial-number") == 0) { axl_printf("%s\n", ch.serial_number); return 0; }
        if (axl_strcmp(keyword, "chassis-asset-tag")     == 0) { axl_printf("%s\n", ch.asset_tag);     return 0; }
    }

    axl_printf("dmidecode: unknown keyword '%s'\n", keyword);
    return 1;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static int
run_dmidecode(AxlArgs *a)
{
    unsigned char major = 0, minor = 0;
    if (axl_smbios_version(&major, &minor) != 0) {
        axl_printf("dmidecode: no SMBIOS table found\n");
        return 1;
    }

    if (axl_args_get_bool(a, "version")) {
        axl_printf("SMBIOS %u.%u present.\n", major, minor);
        return 0;
    }

    const char *keyword = axl_args_get_string(a, "string");
    if (keyword != NULL) {
        return single_string(keyword);
    }

    const char *type_str = axl_args_get_string(a, "type");
    int filter_type = -1;
    if (type_str != NULL) {
        filter_type = (int)axl_strtou64(type_str);
    }
    bool quiet = axl_args_get_bool(a, "quiet");

    if (!quiet) {
        axl_printf("# dmidecode (axl-sdk)\n");
        axl_printf("SMBIOS %u.%u present.\n\n", major, minor);
    }

    AxlSmbiosHeader *hdr = NULL;
    size_t count = 0;
    while ((hdr = axl_smbios_next(hdr)) != NULL) {
        if (filter_type >= 0 && hdr->Type != (uint8_t)filter_type) {
            continue;
        }
        if (!quiet) {
            axl_printf("Handle 0x%04X, DMI type %u, %u bytes\n",
                       hdr->Handle, hdr->Type, hdr->Length);
            axl_printf("%s\n", type_name(hdr->Type));
        }
        switch (hdr->Type) {
            case AXL_SMBIOS_TYPE_BIOS_INFO:            decode_bios(hdr);           break;
            case AXL_SMBIOS_TYPE_SYSTEM_INFO:          decode_system(hdr);         break;
            case AXL_SMBIOS_TYPE_BASEBOARD:            decode_baseboard(hdr);      break;
            case AXL_SMBIOS_TYPE_CHASSIS:              decode_chassis(hdr);        break;
            case AXL_SMBIOS_TYPE_PROCESSOR:            decode_processor(hdr);      break;
            case AXL_SMBIOS_TYPE_MEMORY_DEVICE:        decode_memory_device(hdr);  break;
            case AXL_SMBIOS_TYPE_IPMI_DEVICE_INFO:     decode_ipmi_device_info(hdr); break;
            case AXL_SMBIOS_TYPE_MGMT_HOST_INTERFACE:  decode_host_interface(hdr); break;
            default:
                /* No specialized decoder — fall back to dmidecode(8) -u style:
                   dump the formatted-area bytes plus the string table so the
                   reader can make sense of OEM-specific or not-yet-decoded
                   records without needing another tool. */
                print_record_hex(hdr);
                print_record_strings(hdr);
                break;
        }
        if (!quiet) { axl_printf("\n"); }
        count++;
    }

    if (!quiet) {
        axl_printf("%lu %s\n",
                   (unsigned long)count,
                   filter_type < 0 ? "structures present" : "structure(s) of the requested type");
    }
    return 0;
}

int
main(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "dmidecode",
        .help         = "Decode SMBIOS / DMI tables (Linux dmidecode-style)",
        .flags        = flags,
        .handler      = run_dmidecode,
    });
}
