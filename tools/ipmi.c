/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file ipmi.c
    Stripped-down AXL port of the `ipmitool` CLI, built on AxlIpmi.

    Build with axl-cc:
      axl-cc ipmi.c -o ipmi.efi

    Subcommands:
      ipmi info                               BMC device ID + detected transport
      ipmi chassis status                     Power state + misc chassis state
      ipmi chassis power on|off|cycle|reset|diag|soft
      ipmi sel list                           System Event Log dump
      ipmi sdr list                           SDR repository entries
      ipmi sensor                             All sensors with raw readings
      ipmi fru list                           FRU inventory (raw bytes)
      ipmi raw <netfn> <cmd> [<hex-bytes>...] Raw IPMI command passthrough

    This tool deliberately covers only the most-used paths; for the
    full ipmitool feature set, see the separate uefi-ipmitool project.
**/

#include <axl.h>
#include <axl/axl-ipmi.h>

static const AxlArgDesc flags[] = {
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose output" },
    { .name = "transport",                    .type = AXL_ARG_STRING,
      .help = "Force transport: kcs|ssif|edkii|dell (default: SMBIOS Type 38)" },
    {0}
};

/* Variadic positional collector — every IPMI verb takes its own
   sub-verb chain (e.g. "mc reset cold", "chassis power on") that
   the verb handler parses internally. */
static const AxlArgDesc verb_args[] = {
    { .name = "args", .type = AXL_ARG_MULTI,
      .help = "Verb-specific arguments (see verb help)" },
    {0}
};

static const char *
transport_name(AxlIpmiTransport t)
{
    switch (t) {
    case AXL_IPMI_TRANSPORT_KCS:   return "KCS";
    case AXL_IPMI_TRANSPORT_SSIF:  return "SSIF";
    case AXL_IPMI_TRANSPORT_EDKII: return "EDKII IPMI_PROTOCOL";
    case AXL_IPMI_TRANSPORT_DELL:  return "Dell EFI_IPMI_TRANSPORT";
    default:                       return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Subcommand: info
// ---------------------------------------------------------------------------

static int
cmd_info(AxlIpmiSession *ipmi)
{
    axl_printf("Transport           : %s\n",
               transport_name(axl_ipmi_session_transport(ipmi)));

    AxlIpmiDeviceId d;
    if (axl_ipmi_get_device_id(ipmi, &d) != 0) {
        uint8_t cc = axl_ipmi_session_last_cc(ipmi);
        axl_printf("Get Device ID failed: CC=0x%02x (%s)\n",
                   (unsigned)cc, axl_ipmi_completion_code_string(cc));
        return 1;
    }
    axl_printf("Device ID           : 0x%02x\n", (unsigned)d.device_id);
    axl_printf("Device Revision     : 0x%02x\n", (unsigned)d.device_revision);
    axl_printf("Firmware Revision   : %u.%02x\n",
               (unsigned)(d.firmware_major & 0x7F),
               (unsigned)d.firmware_minor);
    axl_printf("IPMI Version        : %u.%u\n",
               (unsigned)(d.ipmi_version & 0x0F),
               (unsigned)((d.ipmi_version >> 4) & 0x0F));
    axl_printf("Manufacturer ID     : 0x%06x\n", (unsigned)d.manufacturer_id);
    axl_printf("Product ID          : 0x%04x\n", (unsigned)d.product_id);
    axl_printf("Aux Firmware Rev    : 0x%08x\n", (unsigned)d.aux_firmware_rev);
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand: chassis
// ---------------------------------------------------------------------------

static int
cmd_chassis_status(AxlIpmiSession *ipmi)
{
    AxlIpmiChassisStatus st;
    if (axl_ipmi_get_chassis_status(ipmi, &st) != 0) {
        axl_printf("Get Chassis Status failed\n");
        return 1;
    }
    axl_printf("Power State         : %s\n",
               (st.current_power_state & 0x01) ? "on" : "off");
    axl_printf("Current Power State : 0x%02x\n", (unsigned)st.current_power_state);
    axl_printf("Last Power Event    : 0x%02x\n", (unsigned)st.last_power_event);
    axl_printf("Misc Chassis State  : 0x%02x\n", (unsigned)st.misc_state);
    axl_printf("Front Panel Caps    : 0x%02x\n", (unsigned)st.front_panel_caps);
    return 0;
}

static int
cmd_chassis_power(AxlIpmiSession *ipmi, const char *action)
{
    AxlIpmiChassisAction a;
    if (axl_strcmp(action, "off") == 0)          a = AXL_IPMI_CHASSIS_POWER_DOWN;
    else if (axl_strcmp(action, "on") == 0)      a = AXL_IPMI_CHASSIS_POWER_UP;
    else if (axl_strcmp(action, "cycle") == 0)   a = AXL_IPMI_CHASSIS_POWER_CYCLE;
    else if (axl_strcmp(action, "reset") == 0)   a = AXL_IPMI_CHASSIS_HARD_RESET;
    else if (axl_strcmp(action, "diag") == 0)    a = AXL_IPMI_CHASSIS_PULSE_DIAG;
    else if (axl_strcmp(action, "soft") == 0)    a = AXL_IPMI_CHASSIS_SOFT_SHUTDOWN;
    else {
        axl_printf("unknown power action: %s\n", action);
        axl_printf("valid: on, off, cycle, reset, diag, soft\n");
        return 1;
    }
    if (axl_ipmi_chassis_control(ipmi, a) != 0) {
        axl_printf("Chassis Control failed\n");
        return 1;
    }
    axl_printf("Chassis power %s: issued\n", action);
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand: mc reset cold|warm
// ---------------------------------------------------------------------------

static int
cmd_mc_reset(AxlIpmiSession *ipmi, const char *mode)
{
    if (axl_strcmp(mode, "cold") == 0) {
        if (axl_ipmi_bmc_cold_reset(ipmi) != AXL_OK) {
            axl_printf("BMC Cold Reset failed\n");
            return 1;
        }
        axl_printf("BMC Cold Reset issued. BMC typically unresponsive for 20-30s.\n");
        return 0;
    }
    if (axl_strcmp(mode, "warm") == 0) {
        if (axl_ipmi_bmc_warm_reset(ipmi) != AXL_OK) {
            axl_printf("BMC Warm Reset failed (not all BMCs implement this).\n");
            return 1;
        }
        axl_printf("BMC Warm Reset issued.\n");
        return 0;
    }
    axl_printf("unknown mc reset mode: %s (want cold or warm)\n", mode);
    return 1;
}

// ---------------------------------------------------------------------------
// Subcommand: probe
// ---------------------------------------------------------------------------

static void
probe_print_proto(const char *label, bool present)
{
    axl_printf("  %-32s %s\n", label, present ? "present" : "-");
}

static int
cmd_probe(void)
{
    AxlIpmiProbe p;
    axl_ipmi_probe(&p);

    axl_printf("IPMI transport protocols:\n");
    probe_print_proto("EDKII IPMI_PROTOCOL",          p.edkii_ipmi_protocol);
    probe_print_proto("Dell EFI_IPMI_TRANSPORT",      p.dell_ipmi_transport);
    probe_print_proto("AMI EFI_IPMI_TRANSPORT (DXE)", p.ami_dxe_ipmi_transport);
    probe_print_proto("AMI EFI_IPMI_TRANSPORT (SMM)", p.ami_smm_ipmi_transport);
    probe_print_proto("Intel SM IPMI Transport",      p.intel_sm_ipmi_transport);
    probe_print_proto("Microsoft Project Mu IPMI",    p.mu_ipmi_transport2);

    axl_printf("\nSupporting infrastructure:\n");
    probe_print_proto("EFI_SMBUS_HC_PROTOCOL",        p.smbus_hc_protocol);
    if (p.smbus_hc_handle_count > 0) {
        axl_printf("    SMBus HC handle count        %zu\n",
                   p.smbus_hc_handle_count);
    }
    probe_print_proto("EFI_I2C_MASTER_PROTOCOL",      p.i2c_master_protocol);
    if (p.i2c_master_protocol) {
        axl_printf("    I2C Master handle count      %zu\n",
                   p.i2c_master_handle_count);
    }

    axl_printf("\nSMBIOS Type 38 (IPMI Device Information):\n");
    if (!p.smbios_type38_present) {
        axl_printf("  not present\n");
    } else {
        const char *iface;
        switch (p.smbios_interface_type) {
        case 1:  iface = "KCS";     break;
        case 2:  iface = "SMIC";    break;
        case 3:  iface = "BT";      break;
        case 4:  iface = "SSIF";    break;
        default: iface = "Unknown"; break;
        }
        axl_printf("  Interface type               %s (%u)\n",
                   iface, (unsigned)p.smbios_interface_type);
        axl_printf("  Base address (raw)           0x%016llx\n",
                   (unsigned long long)p.smbios_base_address);
        if (p.smbios_interface_type == 1) {
            //
            // KCS: bit 0 selects address space, bits 15:1 are the
            // data port. Command port is data + 1 on standard
            // platforms.
            //
            unsigned long long io = (unsigned long long)(p.smbios_base_address & ~1ULL);
            axl_printf("    KCS data port              0x%04llx\n", io);
            axl_printf("    KCS cmd port               0x%04llx\n", io + 1);
        } else if (p.smbios_interface_type == 4) {
            axl_printf("    I2C slave (wire address)   0x%02x\n",
                       (unsigned)p.smbios_i2c_slave);
            axl_printf("    I2C slave (7-bit addr)     0x%02x\n",
                       (unsigned)(p.smbios_i2c_slave >> 1));
        }
    }

    //
    // End-to-end check: see if auto-detect finds something that
    // actually responds to Get Device ID. This is the "does it work?"
    // answer, separate from "what does the firmware expose?"
    //
    axl_printf("\nEnd-to-end check:\n");
    AXL_AUTOPTR(AxlIpmiSession) ipmi = axl_ipmi_session_new();
    if (ipmi == NULL) {
        axl_printf("  No IPMI transport selected by auto-detect.\n");
        return 0;
    }
    axl_printf("  Selected transport: %s\n",
               transport_name(axl_ipmi_session_transport(ipmi)));

    AxlIpmiDeviceId d;
    if (axl_ipmi_get_device_id(ipmi, &d) == 0) {
        axl_printf("  Get Device ID:      OK (Device=0x%02x FW=%u.%02x Mfr=0x%06x)\n",
                   (unsigned)d.device_id,
                   (unsigned)(d.firmware_major & 0x7F),
                   (unsigned)d.firmware_minor,
                   (unsigned)d.manufacturer_id);
    } else {
        axl_printf("  Get Device ID:      FAILED — transport selected but BMC not responding\n");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand: sel list
// ---------------------------------------------------------------------------

static int
cmd_sel_list(AxlIpmiSession *ipmi)
{
    AxlIpmiSelInfo info;
    if (axl_ipmi_sel_info(ipmi, &info) != 0) {
        axl_printf("Get SEL Info failed\n");
        return 1;
    }
    axl_printf("SEL version         : 0x%02x\n", (unsigned)info.version);
    axl_printf("Entries             : %u\n", (unsigned)info.entries);
    axl_printf("Free space (bytes)  : %u\n", (unsigned)info.free_space_bytes);
    axl_printf("\n");

    if (info.entries == 0) {
        axl_printf("SEL is empty.\n");
        return 0;
    }

    uint16_t id = 0x0000;
    for (unsigned i = 0; i < info.entries && id != 0xFFFF; i++) {
        AxlIpmiSelEntry e;
        if (axl_ipmi_sel_get_entry(ipmi, id, &e) != 0) {
            axl_printf("Get SEL Entry %u failed\n", (unsigned)id);
            return 1;
        }
        axl_printf("%04x | type=%02x | ",
                   (unsigned)e.record_id, (unsigned)e.record[2]);
        for (size_t j = 0; j < sizeof(e.record); j++) {
            axl_printf("%02x ", (unsigned)e.record[j]);
        }
        axl_printf("\n");
        id = e.next_record_id;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand: sdr list
// ---------------------------------------------------------------------------

static int
cmd_sdr_list(AxlIpmiSession *ipmi)
{
    AxlIpmiSdrInfo info;
    if (axl_ipmi_sdr_info(ipmi, &info) != 0) {
        axl_printf("Get SDR Info failed\n");
        return 1;
    }
    axl_printf("SDR version         : 0x%02x\n", (unsigned)info.version);
    axl_printf("Records             : %u\n", (unsigned)info.record_count);
    axl_printf("Free space (bytes)  : %u\n", (unsigned)info.free_space_bytes);
    axl_printf("\n");

    uint16_t id = 0x0000;
    uint16_t next = 0;
    for (unsigned i = 0; i < info.record_count && id != 0xFFFF; i++) {
        uint8_t  rec[64];
        size_t   len = sizeof(rec);
        if (axl_ipmi_sdr_get(ipmi, id, &next, rec, &len) != AXL_OK) {
            axl_printf("Get SDR %u failed\n", (unsigned)id);
            return 1;
        }
        //
        // SDR record header (first 5 bytes): RecordID LE, SDR version, RecordType, length
        // Full/Compact sensor records carry a sensor number at offset 7.
        //
        uint16_t rec_id = (len >= 2) ? (uint16_t)(rec[0] | (rec[1] << 8)) : 0;
        uint8_t  type   = (len >= 4) ? rec[3] : 0;
        axl_printf("%04x | type=%02x | ", (unsigned)rec_id, (unsigned)type);
        for (size_t j = 0; j < len && j < 16; j++) {
            axl_printf("%02x ", (unsigned)rec[j]);
        }
        if (len > 16) {
            axl_printf("... (%u bytes)", (unsigned)len);
        }
        axl_printf("\n");
        id = next;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand: sensor (reads all sensors listed in SDR)
// ---------------------------------------------------------------------------

static int
cmd_sensor(AxlIpmiSession *ipmi)
{
    AxlIpmiSdrInfo info;
    if (axl_ipmi_sdr_info(ipmi, &info) != 0) {
        axl_printf("Get SDR Info failed\n");
        return 1;
    }

    uint16_t id = 0x0000;
    uint16_t next = 0;
    for (unsigned i = 0; i < info.record_count && id != 0xFFFF; i++) {
        uint8_t  rec[64];
        size_t   len = sizeof(rec);
        if (axl_ipmi_sdr_get(ipmi, id, &next, rec, &len) != AXL_OK) {
            return 1;
        }
        //
        // Filter to Full Sensor (0x01) and Compact Sensor (0x02)
        // records — the only types that carry sensor numbers. Sensor
        // type byte is at offset 12, entity ID at offset 8 (IPMI v2.0
        // Table 43-1).
        //
        if (len < 13) {
            id = next;
            continue;
        }
        uint8_t type        = rec[3];
        uint8_t sensor      = rec[7];
        uint8_t entity      = rec[8];
        uint8_t sensor_kind = rec[12];
        if (type != 0x01 && type != 0x02) {
            id = next;
            continue;
        }

        AxlIpmiSensorReading r;
        if (axl_ipmi_get_sensor_reading(ipmi, sensor, &r) == 0) {
            axl_printf("sensor %02x | %-20s | %-28s | reading=0x%02x\n",
                       (unsigned)sensor,
                       axl_ipmi_sensor_type_string(sensor_kind),
                       axl_ipmi_entity_id_string(entity),
                       (unsigned)r.reading);
        } else {
            axl_printf("sensor %02x | %-20s | (read failed)\n",
                       (unsigned)sensor,
                       axl_ipmi_sensor_type_string(sensor_kind));
        }
        id = next;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand: fru list (fru 0 only; most BMCs expose one main FRU)
// ---------------------------------------------------------------------------

static int
cmd_fru_list(AxlIpmiSession *ipmi)
{
    AxlIpmiFruInfo info;
    if (axl_ipmi_fru_info(ipmi, 0, &info) != 0) {
        axl_printf("Get FRU Info failed\n");
        return 1;
    }
    axl_printf("FRU 0 size          : %u bytes (access=%s)\n",
               (unsigned)info.size_bytes,
               info.word_access ? "word" : "byte");

    //
    // Dump raw bytes in 16-byte chunks (the SSIF sweet spot). Typed
    // FRU decoding (Common Header → Board Info → Product Info) can
    // come in a follow-up; raw bytes are already useful for debug.
    //
    uint16_t remain = info.size_bytes;
    uint16_t offset = 0;
    while (remain > 0) {
        uint8_t  buf[16];
        size_t   want = (remain < sizeof(buf)) ? remain : sizeof(buf);
        if (axl_ipmi_fru_read(ipmi, 0, offset, buf, &want) != AXL_OK) {
            break;
        }
        axl_printf("%04x:", (unsigned)offset);
        for (size_t j = 0; j < want; j++) {
            axl_printf(" %02x", (unsigned)buf[j]);
        }
        axl_printf("\n");
        if (want == 0) {
            break;
        }
        offset = (uint16_t)(offset + want);
        remain = (uint16_t)(remain - want);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand: raw <netfn> <cmd> [<hex-bytes>...]
// ---------------------------------------------------------------------------

static int
parse_hex_byte(const char *s, uint8_t *out)
{
    const char *p = s;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    /* Accept any number of hex digits as long as the value fits in a
       byte and the input is fully consumed (no trailing junk). */
    uint64_t v = 0;
    int      n = axl_hex_parse_u64(p, 16, &v);
    if (n < 0 || p[n] != '\0' || v > 0xFF) {
        return -1;
    }
    *out = (uint8_t)v;
    return 0;
}

static int
cmd_raw(AxlIpmiSession *ipmi, int count, const char *const *args)
{
    if (count < 2) {
        axl_printf("usage: ipmi raw <netfn> <cmd> [<data>...]\n");
        return 1;
    }
    uint8_t  netfn, cmd;
    if (parse_hex_byte(args[0], &netfn) != 0 ||
        parse_hex_byte(args[1], &cmd)   != 0)
    {
        axl_printf("invalid netfn or cmd (hex bytes expected)\n");
        return 1;
    }

    uint8_t  req[64];
    size_t   req_len = 0;
    for (int i = 2; i < count && req_len < sizeof(req); i++) {
        if (parse_hex_byte(args[i], &req[req_len]) != 0) {
            axl_printf("invalid data byte: %s\n", args[i]);
            return 1;
        }
        req_len++;
    }

    uint8_t  resp[256];
    size_t   resp_len = sizeof(resp);
    if (axl_ipmi_raw(ipmi, netfn, cmd,
                     req_len ? req : NULL, req_len,
                     resp, &resp_len) != AXL_OK)
    {
        axl_printf("ipmi raw: transport error\n");
        return 1;
    }
    axl_printf("CC=%02x (%s)", (unsigned)resp[0],
               axl_ipmi_completion_code_string(resp[0]));
    for (size_t i = 1; i < resp_len; i++) {
        axl_printf(" %02x", (unsigned)resp[i]);
    }
    axl_printf("\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

/* Open a session lazily — most verbs need it, `probe` doesn't.
 * The --transport flag at the root level is consumed here so every
 * verb honors it without each handler having to thread it through. */
static AxlIpmiTransport g_transport_hint = AXL_IPMI_TRANSPORT_UNKNOWN;

static AxlIpmiSession *
get_session(void)
{
    AxlIpmiSession *s =
        axl_ipmi_session_new_with_transport(g_transport_hint);
    if (s == NULL) {
        axl_printf("No IPMI transport available on this system.\n");
        axl_printf("Run 'ipmi probe' for a diagnostic snapshot.\n");
    }
    return s;
}

static AxlIpmiTransport
parse_transport_flag(const char *s)
{
    if (s == NULL || *s == '\0')         return AXL_IPMI_TRANSPORT_UNKNOWN;
    if (axl_strcmp(s, "kcs")   == 0)     return AXL_IPMI_TRANSPORT_KCS;
    if (axl_strcmp(s, "ssif")  == 0)     return AXL_IPMI_TRANSPORT_SSIF;
    if (axl_strcmp(s, "edkii") == 0)     return AXL_IPMI_TRANSPORT_EDKII;
    if (axl_strcmp(s, "dell")  == 0)     return AXL_IPMI_TRANSPORT_DELL;
    axl_printf("ipmi: unknown --transport value '%s' "
               "(expected kcs|ssif|edkii|dell)\n", s);
    return AXL_IPMI_TRANSPORT_UNKNOWN;
}

static void
ipmi_pre_run(AxlArgs *a)
{
    g_transport_hint = parse_transport_flag(
        axl_args_get_string(a, "transport"));
}

static int run_probe(AxlArgs *a)   { (void)a; return cmd_probe(); }

static int
run_info(AxlArgs *a)
{
    (void)a;
    AXL_AUTOPTR(AxlIpmiSession) s = get_session();
    return s ? cmd_info(s) : 1;
}

static int
run_mc(AxlArgs *a)
{
    int n = axl_args_get_pos_count(a);
    if (n < 2 || axl_strcmp(axl_args_get_pos(a, 0), "reset") != 0) {
        axl_printf("usage: ipmi mc reset cold|warm\n");
        return 1;
    }
    AXL_AUTOPTR(AxlIpmiSession) s = get_session();
    return s ? cmd_mc_reset(s, axl_args_get_pos(a, 1)) : 1;
}

static int
run_chassis(AxlArgs *a)
{
    int n = axl_args_get_pos_count(a);
    if (n < 1) {
        axl_printf("usage: ipmi chassis status|power <action>\n");
        return 1;
    }
    AXL_AUTOPTR(AxlIpmiSession) s = get_session();
    if (s == NULL) { return 1; }
    const char *verb = axl_args_get_pos(a, 0);
    if (axl_strcmp(verb, "status") == 0) {
        return cmd_chassis_status(s);
    }
    if (axl_strcmp(verb, "power") == 0) {
        if (n < 2) {
            axl_printf("usage: ipmi chassis power on|off|cycle|reset|diag|soft\n");
            return 1;
        }
        return cmd_chassis_power(s, axl_args_get_pos(a, 1));
    }
    axl_printf("unknown chassis verb: %s\n", verb);
    return 1;
}

static int run_sel(AxlArgs *a)    { (void)a; AXL_AUTOPTR(AxlIpmiSession) s = get_session(); return s ? cmd_sel_list(s)  : 1; }
static int run_sdr(AxlArgs *a)    { (void)a; AXL_AUTOPTR(AxlIpmiSession) s = get_session(); return s ? cmd_sdr_list(s)  : 1; }
static int run_sensor(AxlArgs *a) { (void)a; AXL_AUTOPTR(AxlIpmiSession) s = get_session(); return s ? cmd_sensor(s)    : 1; }
static int run_fru(AxlArgs *a)    { (void)a; AXL_AUTOPTR(AxlIpmiSession) s = get_session(); return s ? cmd_fru_list(s)  : 1; }

static int
run_raw(AxlArgs *a)
{
    AXL_AUTOPTR(AxlIpmiSession) s = get_session();
    if (s == NULL) { return 1; }
    int n = axl_args_get_pos_count(a);
    const char *raw_args[16];
    if (n > (int)(sizeof(raw_args) / sizeof(raw_args[0]))) {
        n = (int)(sizeof(raw_args) / sizeof(raw_args[0]));
    }
    for (int i = 0; i < n; i++) {
        raw_args[i] = axl_args_get_pos(a, i);
    }
    return cmd_raw(s, n, raw_args);
}

static const AxlArgsNode verbs[] = {
    { .name = "probe",   .handler = run_probe,
      .help = "Diagnose available IPMI transports without opening a session" },
    { .name = "info",    .handler = run_info,
      .help = "Print the BMC's device info (firmware rev, GUID, ...)" },
    { .name = "mc",      .handler = run_mc,      .positionals = verb_args,
      .help = "Management controller commands (mc reset cold|warm)" },
    { .name = "chassis", .handler = run_chassis, .positionals = verb_args,
      .help = "Chassis status / power control (status; power on|off|cycle|...)" },
    { .name = "sel",     .handler = run_sel,     .positionals = verb_args,
      .help = "System Event Log (sel list)" },
    { .name = "sdr",     .handler = run_sdr,     .positionals = verb_args,
      .help = "Sensor Data Repository (sdr list)" },
    { .name = "sensor",  .handler = run_sensor,
      .help = "All sensors with current readings" },
    { .name = "fru",     .handler = run_fru,     .positionals = verb_args,
      .help = "FRU inventory (fru list)" },
    { .name = "raw",     .handler = run_raw,     .positionals = verb_args,
      .help = "Raw netfn/cmd/data passthrough" },
    {0}
};

int
main(int argc, char **argv)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "ipmi",
        .help         = "IPMI client — SMBIOS Type 38 auto-detect (override: --transport)",
        .flags        = flags,
        .verbs        = verbs,
        .pre_run      = ipmi_pre_run,
    });
}
