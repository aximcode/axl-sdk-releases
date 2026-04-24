/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ipmi-format.c
    Stringification helpers for IPMI enums.

    Small subset of uefi-ipmitool's IpmiFormat.c (lookup tables
    only). Sensor reading linearization, FRU string decoding, and
    timestamp formatting can follow if a consumer needs them; for
    now, raw bytes + these enum strings cover what `tools/ipmi`
    actually prints.
**/

#include <axl/axl-ipmi.h>

// ---------------------------------------------------------------------------
// Completion codes (IPMI v2.0 Table 5-2)
// ---------------------------------------------------------------------------

const char *
axl_ipmi_completion_code_string(uint8_t cc)
{
    switch (cc) {
    case 0x00: return "OK";
    case 0xC0: return "Node busy";
    case 0xC1: return "Invalid command";
    case 0xC2: return "Invalid command for LUN";
    case 0xC3: return "Timeout while processing command";
    case 0xC4: return "Out of space";
    case 0xC5: return "Reservation cancelled or invalid";
    case 0xC6: return "Request data truncated";
    case 0xC7: return "Request data length invalid";
    case 0xC8: return "Request data field length limit exceeded";
    case 0xC9: return "Parameter out of range";
    case 0xCA: return "Cannot return number of requested data bytes";
    case 0xCB: return "Requested sensor, data, or record not present";
    case 0xCC: return "Invalid data field in request";
    case 0xCD: return "Command illegal for specified sensor or record type";
    case 0xCE: return "Command response could not be provided";
    case 0xCF: return "Cannot execute duplicated request";
    case 0xD0: return "SDR repository in update mode";
    case 0xD1: return "Device in firmware update mode";
    case 0xD2: return "BMC initialization in progress";
    case 0xD3: return "Destination unavailable";
    case 0xD4: return "Insufficient privilege level";
    case 0xD5: return "Command not supported in present state";
    case 0xD6: return "Cannot execute; sub-function disabled";
    case 0xFF: return "Unspecified error";
    default:
        if (cc >= 0x01 && cc <= 0x7E) {
            return "Device-specific (OEM) completion code";
        }
        if (cc >= 0x80 && cc <= 0xBE) {
            return "Command-specific completion code";
        }
        return "Unknown completion code";
    }
}

// ---------------------------------------------------------------------------
// Sensor types (IPMI v2.0 Table 42-3)
// ---------------------------------------------------------------------------

static const char *const sensor_type_names[] = {
    [0x01] = "Temperature",
    [0x02] = "Voltage",
    [0x03] = "Current",
    [0x04] = "Fan",
    [0x05] = "Physical Security",
    [0x06] = "Platform Security Violation Attempt",
    [0x07] = "Processor",
    [0x08] = "Power Supply",
    [0x09] = "Power Unit",
    [0x0A] = "Cooling Device",
    [0x0B] = "Other Units-based",
    [0x0C] = "Memory",
    [0x0D] = "Drive Slot",
    [0x0E] = "POST Memory Resize",
    [0x0F] = "System Firmware Progress",
    [0x10] = "Event Logging Disabled",
    [0x11] = "Watchdog 1",
    [0x12] = "System Event",
    [0x13] = "Critical Interrupt",
    [0x14] = "Button/Switch",
    [0x15] = "Module/Board",
    [0x16] = "Microcontroller/Coprocessor",
    [0x17] = "Add-in Card",
    [0x18] = "Chassis",
    [0x19] = "Chip Set",
    [0x1A] = "Other FRU",
    [0x1B] = "Cable/Interconnect",
    [0x1C] = "Terminator",
    [0x1D] = "System Boot/Restart Initiated",
    [0x1E] = "Boot Error",
    [0x1F] = "OS Boot",
    [0x20] = "OS Critical Stop",
    [0x21] = "Slot/Connector",
    [0x22] = "System ACPI Power State",
    [0x23] = "Watchdog 2",
    [0x24] = "Platform Alert",
    [0x25] = "Entity Presence",
    [0x26] = "Monitor ASIC/IC",
    [0x27] = "LAN",
    [0x28] = "Management Subsystem Health",
    [0x29] = "Battery",
    [0x2A] = "Session Audit",
    [0x2B] = "Version Change",
    [0x2C] = "FRU State",
};

const char *
axl_ipmi_sensor_type_string(uint8_t sensor_type)
{
    if (sensor_type < sizeof(sensor_type_names) / sizeof(sensor_type_names[0])) {
        const char *s = sensor_type_names[sensor_type];
        if (s != NULL) {
            return s;
        }
    }
    if (sensor_type >= 0xC0) {
        return "OEM sensor";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Entity IDs (IPMI v2.0 Table 43-13 — common entries only)
// ---------------------------------------------------------------------------

static const char *const entity_id_names[] = {
    [0x00] = "Unspecified",
    [0x01] = "Other",
    [0x02] = "Unknown",
    [0x03] = "Processor",
    [0x04] = "Disk / Disk Bay",
    [0x05] = "Peripheral Bay",
    [0x06] = "System Management Module",
    [0x07] = "System Board",
    [0x08] = "Memory Module",
    [0x09] = "Processor Module",
    [0x0A] = "Power Supply",
    [0x0B] = "Add-in Card",
    [0x0C] = "Front Panel Board",
    [0x0D] = "Back Panel Board",
    [0x0E] = "Power System Board",
    [0x0F] = "Drive Backplane",
    [0x10] = "System Internal Expansion Board",
    [0x11] = "Other System Board",
    [0x12] = "Processor Board",
    [0x13] = "Power Unit / Power Domain",
    [0x14] = "Power Module / DC-DC Converter",
    [0x15] = "Power Management / Distribution Board",
    [0x16] = "Chassis Back Panel Board",
    [0x17] = "System Chassis",
    [0x18] = "Sub-Chassis",
    [0x19] = "Other Chassis Board",
    [0x1A] = "Disk Drive Bay",
    [0x1B] = "Peripheral Bay (alt)",
    [0x1C] = "Device Bay",
    [0x1D] = "Fan / Cooling Device",
    [0x1E] = "Cooling Unit",
    [0x1F] = "Cable / Interconnect",
    [0x20] = "Memory Device",
    [0x21] = "System Management Software",
    [0x22] = "System Firmware",
    [0x23] = "Operating System",
    [0x24] = "System Bus",
    [0x25] = "Group",
    [0x26] = "Remote Management Communication Device",
    [0x27] = "External Environment",
    [0x28] = "Battery",
    [0x29] = "Processing Blade",
    [0x2A] = "Connectivity Switch",
    [0x2B] = "Processor/Memory Module",
    [0x2C] = "I/O Module",
    [0x2D] = "Processor/IO Module",
    [0x2E] = "Management Controller Firmware",
    [0x2F] = "IPMI Channel",
    [0x30] = "PCI Bus",
    [0x31] = "PCI Express Bus",
    [0x32] = "SCSI Bus (parallel)",
    [0x33] = "SATA / SAS Bus",
    [0x34] = "Processor / Front-Side Bus",
    [0x35] = "Real Time Clock",
    [0x37] = "Air Inlet",
    [0x41] = "Air Inlet (alt)",
    [0x42] = "Processor (alt)",
    [0x43] = "Baseboard / Main System Board",
};

const char *
axl_ipmi_entity_id_string(uint8_t entity_id)
{
    if (entity_id < sizeof(entity_id_names) / sizeof(entity_id_names[0])) {
        const char *s = entity_id_names[entity_id];
        if (s != NULL) {
            return s;
        }
    }
    if (entity_id >= 0x90 && entity_id <= 0xAF) {
        return "Chassis-specific";
    }
    if (entity_id >= 0xB0 && entity_id <= 0xCF) {
        return "Board-set-specific";
    }
    if (entity_id >= 0xD0) {
        return "OEM system integrator";
    }
    return "Unknown";
}
