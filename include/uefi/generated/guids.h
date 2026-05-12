/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file generated/guids.h
    Auto-generated from UEFI Specification 2.11.
    All UEFI protocol GUIDs.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_GUIDS_H
#define AXL_UEFI_GEN_GUIDS_H

#include "types.h"

static __attribute__((unused)) EFI_GUID EFI_BOOT_MANAGER_POLICY_PROTOCOL_GUID =
    { 0xFEDF8E0C, 0xE147, 0x11E3, { 0x99, 0x03, 0xB8, 0xE8, 0x56, 0x2C, 0xBA, 0xFA } };

static __attribute__((unused)) EFI_GUID EFI_BOOT_MANAGER_POLICY_CONSOLE_GUID =
    { 0xCAB0E94C, 0xE15F, 0x11E3, { 0x91, 0x8D, 0xB8, 0xE8, 0x56, 0x2C, 0xBA, 0xFA } };

static __attribute__((unused)) EFI_GUID EFI_BOOT_MANAGER_POLICY_NETWORK_GUID =
    { 0xD04159DC, 0xE15F, 0x11E3, { 0xB2, 0x61, 0xB8, 0xE8, 0x56, 0x2C, 0xBA, 0xFA } };

static __attribute__((unused)) EFI_GUID EFI_BOOT_MANAGER_POLICY_STORAGE_GUID =
    { 0xCD68FE79, 0xD3CB, 0x436E, { 0xA8, 0x50, 0xF4, 0x43, 0xC8, 0x8C, 0xFB, 0x49 } };

static __attribute__((unused)) EFI_GUID EFI_BOOT_MANAGER_POLICY_CONNECT_ALL_GUID =
    { 0x113B2126, 0xFC8A, 0x11E3, { 0xBD, 0x6C, 0xB8, 0xE8, 0x56, 0x2C, 0xBA, 0xFA } };

static __attribute__((unused)) EFI_GUID EFI_ACPI_20_TABLE_GUID =
    {0x8868e871,0xe4f1,0x11d3, {0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};

static __attribute__((unused)) EFI_GUID ACPI_TABLE_GUID =
    {0xeb9d2d30,0x2d88,0x11d3, {0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

static __attribute__((unused)) EFI_GUID SAL_SYSTEM_TABLE_GUID =
    {0xeb9d2d32,0x2d88,0x11d3, {0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

static __attribute__((unused)) EFI_GUID SMBIOS_TABLE_GUID =
    {0xeb9d2d31,0x2d88,0x11d3, {0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

static __attribute__((unused)) EFI_GUID SMBIOS3_TABLE_GUID =
    {0xf2fd1544, 0x9794, 0x4a2c, {0x99,0x2e,0xe5,0xbb,0xcf,0x20,0xe3,0x94}};

static __attribute__((unused)) EFI_GUID MPS_TABLE_GUID =
    {0xeb9d2d2f,0x2d88,0x11d3, {0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

static __attribute__((unused)) EFI_GUID EFI_ACPI_TABLE_GUID =
    {0x8868e871,0xe4f1,0x11d3, {0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};

static __attribute__((unused)) EFI_GUID EFI_JSON_CONFIG_DATA_TABLE_GUID =
    {0x87367f87, 0x1119, 0x41ce, {0xaa, 0xec, 0x8b, 0xe0, 0x11, 0x1f, 0x55, 0x8a }};

static __attribute__((unused)) EFI_GUID EFI_JSON_CAPSULE_DATA_TABLE_GUID =
    {0x35e7a725, 0x8dd2, 0x4cac, { 0x80, 0x11, 0x33, 0xcd, 0xa8, 0x10, 0x90, 0x56 }};

static __attribute__((unused)) EFI_GUID EFI_JSON_CAPSULE_RESULT_TABLE_GUID =
    {0xdbc461c3, 0xb3de, 0x422a, {0xb9, 0xb4, 0x98, 0x86, 0xfd, 0x49, 0xa1, 0xe5 }};

static __attribute__((unused)) EFI_GUID EFI_DTB_TABLE_GUID =
    {0xb1b621d5, 0xf19c, 0x41a5, {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}};

static __attribute__((unused)) EFI_GUID EFI_RT_PROPERTIES_TABLE_GUID =
    { 0xeb66918a, 0x7eef, 0x402a, { 0x84, 0x2e, 0x93, 0x1d, 0x21, 0xc3, 0x8a, 0xe9 }};

static __attribute__((unused)) EFI_GUID EFI_MEMORY_ATTRIBUTES_TABLE_GUID =
    {0xdcfa911d, 0x26eb, 0x469f, {0xa2, 0x20, 0x38, 0xb7, 0xdc, 0x46, 0x12, 0x20}};

static __attribute__((unused)) EFI_GUID EFI_CONFORMANCE_PROFILES_TABLE_GUID =
    { 0x36122546, 0xf7e7, 0x4c8f, { 0xbd, 0x9b, 0xeb, 0x85, 0x25, 0xb5, 0x0c, 0x0b }};

static __attribute__((unused)) EFI_GUID EFI_CONFORMANCE_PROFILES_UEFI_SPEC_GUID =
    { 0x523c91af, 0xa195, 0x4382, { 0x81, 0x8d, 0x29, 0x5f, 0xe4, 0x00, 0x64, 0x65 }};

static __attribute__((unused)) EFI_GUID EFI_HII_PACKAGE_LIST_PROTOCOL_GUID =
    { 0x6a1ee763, 0xd47a, 0x43b4, { 0xaa, 0xbe, 0xef, 0x1d, 0xe2, 0xab, 0x56, 0xfc } };

static __attribute__((unused)) EFI_GUID EFI_MEMORY_RANGE_CAPSULE_GUID =
    { 0xde9f0ec, 0x88b6, 0x428f, { 0x97, 0x7a, 0x25, 0x8f, 0x1d, 0xe, 0x5e, 0x72 } };

static __attribute__((unused)) EFI_GUID EFI_CAPSULE_REPORT_GUID =
    { 0x39b68c46, 0xf7fb, 0x441b, {0xb6, 0xec, 0x16, 0xb0, 0xf6, 0x98, 0x21, 0xf3 }};

static __attribute__((unused)) EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID =
    {0x5B1B31A1,0x9562,0x11d2, {0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};

static __attribute__((unused)) EFI_GUID EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID =
    {0xbc62157e,0x3e33,0x4fec, {0x99,0x20,0x2d,0x3b,0x36,0xd7,0x50,0xdf}};

static __attribute__((unused)) EFI_GUID EFI_DEVICE_PATH_PROTOCOL_GUID =
    {0x09576e91,0x6d3f,0x11d2, {0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};

static __attribute__((unused)) EFI_GUID EFI_PC_ANSI_GUID =
    {0xe0c14753,0xf9be,0x11d2,{0x9a,0x0c,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

static __attribute__((unused)) EFI_GUID EFI_VT_100_GUID =
    {0xdfa66065,0xb419,0x11d3,{0x9a,0x2d,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

static __attribute__((unused)) EFI_GUID EFI_VT_100_PLUS_GUID =
    {0x7baec70b,0x57e0,0x4c76,{0x8e,0x87,0x2f,0x9e,0x28,0x08,0x83,0x43}};

static __attribute__((unused)) EFI_GUID EFI_VT_UTF8_GUID =
    {0xad15a0d6,0x8bec,0x4acf,{0xa0,0x73,0xd0,0x1d,0xe7,0x7e,0x2d,0x88}};

static __attribute__((unused)) EFI_GUID EFI_VIRTUAL_DISK_GUID =
    { 0x77AB535A,0x45FC,0x624B, {0x55,0x60,0xF7,0xB2,0x81,0xD1,0xF9,0x6E }};

static __attribute__((unused)) EFI_GUID EFI_VIRTUAL_CD_GUID =
    { 0x3D5ABD30,0x4175,0x87CE, {0x6D,0x64,0xD2,0xAD,0xE5,0x23,0xC4,0xBB }};

static __attribute__((unused)) EFI_GUID EFI_PERSISTENT_VIRTUAL_DISK_GUID =
    { 0x5CEA02C9,0x4D07,0x69D3, {0x26,0x9F,0x44,0x96,0xFB,0xE0,0x96,0xF9 }};

static __attribute__((unused)) EFI_GUID EFI_PERSISTENT_VIRTUAL_CD_GUID =
    { 0x08018188,0x42CD,0xBB48, {0x10,0x0F,0x53,0x87,0xD5,0x3D,0xED,0x3D }};

static __attribute__((unused)) EFI_GUID EFI_DEVICE_PATH_UTILITIES_PROTOCOL_GUID =
    {0x379be4e,0xd706,0x437d, {0xb0,0x37,0xed,0xb8,0x2f,0xb7,0x72,0xa4 }};

static __attribute__((unused)) EFI_GUID EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID =
    {0x8b843e20,0x8132,0x4852, {0x90,0xcc,0x55,0x1a,0x4e,0x4a,0x7f,0x1c}};

static __attribute__((unused)) EFI_GUID EFI_DEVICE_PATH_FROM_TEXT_PROTOCOL_GUID =
    {0x5c99a21,0xc70f,0x4ad2, {0x8a,0x5f,0x35,0xdf,0x33,0x43,0xf5, 0x1e}};

static __attribute__((unused)) EFI_GUID EFI_DRIVER_BINDING_PROTOCOL_GUID =
    {0x18A031AB,0xB443,0x4D1A, {0xA5,0xC0,0x0C,0x09,0x26,0x1E,0x9F,0x71}};

static __attribute__((unused)) EFI_GUID EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL_GUID =
    {0x6b30c738,0xa391,0x11d4, {0x9a,0x3b,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

static __attribute__((unused)) EFI_GUID EFI_BUS_SPECIFIC_DRIVER_OVERRIDE_PROTOCOL_GUID =
    {0x3bc1b285,0x8a15,0x4a82, {0xaa,0xbf,0x4d,0x7d,0x13,0xfb,0x32,0x65}};

static __attribute__((unused)) EFI_GUID EFI_DRIVER_DIAGNOSTICS_PROTOCOL_GUID =
    {0x4d330321,0x025f,0x4aac, {0x90,0xd8,0x5e,0xd9,0x00,0x17,0x3b,0x63}};

static __attribute__((unused)) EFI_GUID EFI_COMPONENT_NAME2_PROTOCOL_GUID =
    {0x6a7a5cff, 0xe8d9, 0x4f70, {0xba, 0xda, 0x75, 0xab, 0x30,0x25, 0xce, 0x14}};

static __attribute__((unused)) EFI_GUID EFI_PLATFORM_TO_DRIVER_CONFIGURATION_PROTOCOL_GUID =
    { 0x642cd590, 0x8059, 0x4c0a, { 0xa9, 0x58, 0xc5, 0xec, 0x07, 0xd2, 0x3c, 0x4b } };

static __attribute__((unused)) EFI_GUID EFI_PLATFORM_TO_DRIVER_CONFIGURATION_CLP_GUID =
    {0x345ecc0e, 0xcb6, 0x4b75, {0xbb, 0x57, 0x1b, 0x12, 0x9c, 0x47, 0x33,0x3e}};

static __attribute__((unused)) EFI_GUID EFI_DRIVER_SUPPORTED_EFI_VERSION_PROTOCOL_GUID =
    { 0x5c198761, 0x16a8, 0x4e69, { 0x97, 0x2c, 0x89, 0xd6, 0x79, 0x54, 0xf8, 0x1d } };

static __attribute__((unused)) EFI_GUID EFI_DRIVER_FAMILY_OVERRIDE_PROTOCOL_GUID =
    {0xb1ee129e,0xda36,0x4181, {0x91,0xf8,0x04,0xa4,0x92,0x37,0x66,0xa7}};

static __attribute__((unused)) EFI_GUID EFI_DRIVER_HEALTH_PROTOCOL_GUID =
    {0x2a534210,0x9280,0x41d8, {0xae,0x79,0xca,0xda,0x01,0xa2,0xb1,0x27 }};

static __attribute__((unused)) EFI_GUID EFI_ADAPTER_INFORMATION_PROTOCOL_GUID =
    { 0xE5DD1403, 0xD622, 0xC24E, { 0x84, 0x88, 0xC7, 0x1B, 0x17, 0xF5, 0xE8, 0x02 } };

static __attribute__((unused)) EFI_GUID EFI_ADAPTER_INFO_MEDIA_STATE_GUID =
    {0xD7C74207, 0xA831, 0x4A26, {0xB1,0xF5,0xD1,0x93,0x06,0x5C,0xE8,0xB6}};

static __attribute__((unused)) EFI_GUID EFI_ADAPTER_INFO_NETWORK_BOOT_GUID =
    {0x1FBD2960, 0x4130, 0x41E5, {0x94,0xAC,0xD2, 0xCF, 0x03, 0x7F, 0xB3, 0x7C}};

static __attribute__((unused)) EFI_GUID EFI_ADAPTER_INFO_SAN_MAC_ADDRESS_GUID =
    {0x114da5ef, 0x2cf1, 0x4e12, {0x9b, 0xbb, 0xc4, 0x70, 0xb5, 0x52, 0x05, 0xd9}};

static __attribute__((unused)) EFI_GUID EFI_ADAPTER_INFO_UNDI_IPV6_SUPPORT_GUID =
    { 0x4bd56be3, 0x4975, 0x4d8a, {0xa0, 0xad, 0xc4, 0x91, 0x20, 0x4b, 0x5d, 0x4d}};

static __attribute__((unused)) EFI_GUID EFI_ADAPTER_INFO_MEDIA_TYPE_GUID =
    { 0x8484472f, 0x71ec, 0x411a, { 0xb3, 0x9c, 0x62, 0xcd, 0x94, 0xd9, 0x91, 0x6e }};

static __attribute__((unused)) EFI_GUID EFI_ADAPTER_INFO_CDAT_TYPE_GUID =
    {0x77af24d1, 0xb6f0, 0x42b9, {0x83, 0xf5, 0x8f, 0xe6, 0xe8, 0x3e, 0xb6, 0xf0}};

static __attribute__((unused)) EFI_GUID EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID =
    {0xdd9e7534, 0x7762, 0x4698, {0x8c, 0x14, 0xf5, 0x85, 0x17, 0xa6, 0x25, 0xaa}};

static __attribute__((unused)) EFI_GUID EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID =
    {0x387477c1,0x69c7,0x11d2, {0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};

static __attribute__((unused)) EFI_GUID EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID =
    {0x387477c2,0x69c7,0x11d2, {0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};

static __attribute__((unused)) EFI_GUID EFI_SIMPLE_POINTER_PROTOCOL_GUID =
    {0x31878c87,0xb75,0x11d5, {0x9a,0x4f,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

static __attribute__((unused)) EFI_GUID EFI_ABSOLUTE_POINTER_PROTOCOL_GUID =
    {0x8D59D32B, 0xC655, 0x4AE9, {0x9B, 0x15, 0xF2, 0x59, 0x04, 0x99, 0x2A, 0x43}};

static __attribute__((unused)) EFI_GUID EFI_SERIAL_IO_PROTOCOL_GUID =
    {0xBB25CF6F,0xF1D4,0x11D2, {0x9a,0x0c,0x00,0x90,0x27,0x3f,0xc1,0xfd}};

static __attribute__((unused)) EFI_GUID EFI_SERIAL_TERMINAL_DEVICE_TYPE_GUID =
    { 0x6ad9a60f, 0x5815, 0x4c7c, { 0x8a, 0x10, 0x50, 0x53, 0xd2, 0xbf, 0x7a, 0x1b } };

static __attribute__((unused)) EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID =
    {0x9042a9de,0x23dc,0x4a38, {0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};

static __attribute__((unused)) EFI_GUID EFI_EDID_DISCOVERED_PROTOCOL_GUID =
    {0x1c0c34f6,0xd380,0x41fa, {0xa0,0x49,0x8a,0xd0,0x6c,0x1a,0x66,0xaa}};

static __attribute__((unused)) EFI_GUID EFI_EDID_ACTIVE_PROTOCOL_GUID =
    {0xbd8c1056,0x9f36,0x44ec, {0x92,0xa8,0xa6,0x33,0x7f,0x81,0x79,0x86}};

static __attribute__((unused)) EFI_GUID EFI_EDID_OVERRIDE_PROTOCOL_GUID =
    {0x48ecb431,0xfb72,0x45c0, {0xa9,0x22,0xf4,0x58,0xfe,0x04,0x0b,0xd5}};

static __attribute__((unused)) EFI_GUID EFI_LOAD_FILE_PROTOCOL_GUID =
    {0x56EC3091,0x954C,0x11d2, {0x8e,0x3f,0x00,0xa0, 0xc9,0x69,0x72,0x3b}};

static __attribute__((unused)) EFI_GUID EFI_LOAD_FILE2_PROTOCOL_GUID =
    { 0x4006c0c1, 0xfcb3, 0x403e, { 0x99, 0x6d, 0x4a, 0x6c, 0x87, 0x24, 0xe0, 0x6d }};

static __attribute__((unused)) EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID =
    {0x0964e5b22,0x6459,0x11d2, {0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};

static __attribute__((unused)) EFI_GUID EFI_TAPE_IO_PROTOCOL_GUID =
    {0x1e93e633,0xd65a,0x459e, {0xab,0x84,0x93,0xd9,0xec,0x26,0x6d,0x18}};

static __attribute__((unused)) EFI_GUID EFI_DISK_IO_PROTOCOL_GUID =
    {0xCE345171,0xBA0B,0x11d2, {0x8e,0x4F,0x00,0xa0,0xc9,0x69,0x72,0x3b}};

static __attribute__((unused)) EFI_GUID EFI_DISK_IO2_PROTOCOL_GUID =
    { 0x151c8eae, 0x7f2c, 0x472c, {0x9e, 0x54, 0x98, 0x28, 0x19, 0x4f, 0x6a, 0x88 }};

static __attribute__((unused)) EFI_GUID EFI_BLOCK_IO_PROTOCOL_GUID =
    {0x964e5b21,0x6459,0x11d2, {0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};

static __attribute__((unused)) EFI_GUID EFI_BLOCK_IO2_PROTOCOL_GUID =
    {0xa77b2472, 0xe282, 0x4e9f, {0xa2, 0x45, 0xc2, 0xc0, 0xe2, 0x7b, 0xbc, 0xc1}};

static __attribute__((unused)) EFI_GUID EFI_BLOCK_IO_CRYPTO_PROTOCOL_GUID =
    {0xa00490ba,0x3f1a,0x4b4c, {0xab,0x90,0x4f,0xa9,0x97,0x26,0xa1,0xe8}};

static __attribute__((unused)) EFI_GUID EFI_ERASE_BLOCK_PROTOCOL_GUID =
    {0x95A9A93E, 0xA86E, 0x4926, {0xaa, 0xef, 0x99, 0x18, 0xe7, 0x72, 0xd9, 0x87}};

static __attribute__((unused)) EFI_GUID EFI_ATA_PASS_THRU_PROTOCOL_GUID =
    {0x1d3de7f0,0x807,0x424f, {0xaa,0x69,0x11,0xa5,0x4e,0x19,0xa4,0x6f}};

static __attribute__((unused)) EFI_GUID EFI_STORAGE_SECURITY_COMMAND_PROTOCOL_GUID =
    {0xc88b0b6d, 0x0dfc, 0x49a7, {0x9c, 0xb4, 0x49, 0x7, 0x4b, 0x4c, 0x3a, 0x78}};

static __attribute__((unused)) EFI_GUID EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL_GUID =
    { 0x52c78312, 0x8edc, 0x4233, { 0x98, 0xf2, 0x1a, 0x1a, 0xa5, 0xe3, 0x88, 0xa5 } };

static __attribute__((unused)) EFI_GUID EFI_SD_MMC_PASS_THRU_PROTOCOL_GUID =
    { 0x716ef0d9, 0xff83, 0x4f69, { 0x81, 0xe9, 0x51, 0x8b, 0xd3, 0x9a, 0x8e, 0x70 } };

static __attribute__((unused)) EFI_GUID EFI_RAM_DISK_PROTOCOL_GUID =
    { 0xab38a0df, 0x6873, 0x44a9, { 0x87, 0xe6, 0xd4, 0xeb, 0x56, 0x14, 0x84, 0x49 }};

static __attribute__((unused)) EFI_GUID EFI_NVDIMM_LABEL_PROTOCOL_GUID =
    {0xd40b6b80,0x97d5,0x4282, {0xbb,0x1d,0x22,0x3a,0x16,0x91,0x80,0x58}};

static __attribute__((unused)) EFI_GUID EFI_UFS_DEVICE_CONFIG_GUID =
    { 0xb81bfab0, 0xeb3, 0x4cf9, { 0x84, 0x65, 0x7f, 0xa9, 0x86, 0x36, 0x16, 0x64}};

static __attribute__((unused)) EFI_GUID EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID =
    {0x2F707EBB,0x4A1A,0x11d4, {0x9A,0x38,0x00,0x90,0x27,0x3F,0xC1,0x4D}};

static __attribute__((unused)) EFI_GUID EFI_PCI_IO_PROTOCOL_GUID =
    {0x4cf5b200,0x68b8,0x4ca5, {0x9e,0xec,0xb2,0x3e,0x3f,0x50,0x02,0x9a}};

static __attribute__((unused)) EFI_GUID EFI_SCSI_IO_PROTOCOL_GUID =
    {0x932f47e6,0x2362,0x4002, {0x80,0x3e,0x3c,0xd5,0x4b,0x13,0x8f,0x85}};

static __attribute__((unused)) EFI_GUID EFI_EXT_SCSI_PASS_THRU_PROTOCOL_GUID =
    {0x143b7632, 0xb81b, 0x4cb7, {0xab, 0xd3, 0xb6, 0x25, 0xa5, 0xb9, 0xbf, 0xfe}};

static __attribute__((unused)) EFI_GUID EFI_ISCSI_INITIATOR_NAME_PROTOCOL_GUID =
    {0x59324945, 0xec44, 0x4c0d, {0xb1, 0xcd, 0x9d, 0xb1, 0x39, 0xdf, 0x07, 0x0c}};

static __attribute__((unused)) EFI_GUID EFI_USB2_HC_PROTOCOL_GUID =
    {0x3e745226,0x9818,0x45b6, {0xa2,0xac,0xd7,0xcd,0x0e,0x8b,0xa2,0xbc}};

static __attribute__((unused)) EFI_GUID EFI_USB_IO_PROTOCOL_GUID =
    {0x2B2F68D6,0x0CD2,0x44cf, {0x8E,0x8B,0xBB,0xA2,0x0B,0x1B,0x5B,0x75}};

static __attribute__((unused)) EFI_GUID EFI_USBFN_IO_PROTOCOL_GUID =
    {0x32d2963a, 0xfe5d, 0x4f30, {0xb6, 0x33, 0x6e, 0x5d, 0xc5, 0x58, 0x3, 0xcc}};

static __attribute__((unused)) EFI_GUID EFI_DEBUG_SUPPORT_PROTOCOL_GUID =
    {0x2755590C,0x6F3C,0x42FA, {0x9E,0xA4,0xA3,0xBA,0x54,0x3C,0xDA,0x25}};

static __attribute__((unused)) EFI_GUID EFI_DEBUGPORT_PROTOCOL_GUID =
    {0xEBA4E8D2,0x3858,0x41EC, {0xA2,0x81,0x26,0x47,0xBA,0x96,0x60,0xD0}};

static __attribute__((unused)) EFI_GUID EFI_DEBUG_IMAGE_INFO_TABLE_GUID =
    {0x49152E77,0x1ADA,0x4764, {0xB7,0xA2,0x7A,0xFE,0xFE,0xD9,0x5E,0x8B }};

static __attribute__((unused)) EFI_GUID EFI_DECOMPRESS_PROTOCOL_GUID =
    {0xd8117cfe,0x94a6,0x11d4, {0x9a,0x3a,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

static __attribute__((unused)) EFI_GUID EFI_ACPI_TABLE_PROTOCOL_GUID =
    {0xffe06bdd, 0x6107, 0x46a6, {0x7b, 0xb2, 0x5a, 0x9c, 0x7e, 0xc5, 0x27, 0x5c}};

static __attribute__((unused)) EFI_GUID EFI_UNICODE_COLLATION_PROTOCOL2_GUID =
    {0xa4c751fc, 0x23ae, 0x4c3e, {0x92, 0xe9, 0x49, 0x64, 0xcf, 0x63, 0xf3, 0x49}};

static __attribute__((unused)) EFI_GUID EFI_REGULAR_EXPRESSION_PROTOCOL_GUID =
    { 0xB3F79D9A, 0x436C, 0xDC11, { 0xB0, 0x52, 0xCD, 0x85, 0xDF, 0x52, 0x4C, 0xE6 } };

static __attribute__((unused)) EFI_GUID EFI_REGEX_SYNTAX_TYPE_POSIX_EXTENDED_GUID =
    {0x5F05B20F, 0x4A56, 0xC231, { 0xFA, 0x0B, 0xA7, 0xB1, 0xF1, 0x10, 0x04, 0x1D }};

static __attribute__((unused)) EFI_GUID EFI_REGEX_SYNTAX_TYPE_PERL_GUID =
    {0x63E60A51, 0x497D, 0xD427, { 0xC4, 0xA5, 0xB8, 0xAB, 0xDC, 0x3A, 0xAE, 0xB6 }};

static __attribute__((unused)) EFI_GUID EFI_REGEX_SYNTAX_TYPE_ECMA_262_GUID =
    { 0x9A473A4A, 0x4CEB, 0xB95A, {0x41,0x5E, 0x5B, 0xA0, 0xBC, 0x63, 0x9B, 0x2E }};

static __attribute__((unused)) EFI_GUID EFI_REGEX_SYNTAX_TYPE_POSIX_EXTENDED_ASCII_GUID =
    {0x3FD32128, 0x4BB1, 0xF632, { 0xBE, 0x4F, 0xBA, 0xBF, 0x85, 0xC9, 0x36, 0x76 }};

static __attribute__((unused)) EFI_GUID EFI_REGEX_SYNTAX_TYPE_PERL_ASCII_GUID =
    {0x87DFB76D, 0x4B58, 0xEF3A, { 0xF7, 0xC6, 0x16, 0xA4, 0x2A, 0x68, 0x28, 0x10 }};

static __attribute__((unused)) EFI_GUID EFI_REGEX_SYNTAX_TYPE_ECMA_262_ASCII_GUID =
    { 0xB2284A2F, 0x4491, 0x6D9D, { 0xEA, 0xB7, 0x11, 0xB0, 0x67, 0xD4, 0x9B, 0x9A }};

static __attribute__((unused)) EFI_GUID EFI_EBC_PROTOCOL_GUID =
    {0x13ac6dd1,0x73d0,0x11d4, {0xb0,0x6b,0x00,0xaa,0x00,0xbd,0x6d,0xe7}};

static __attribute__((unused)) EFI_GUID EFI_FIRMWARE_MANAGEMENT_PROTOCOL_GUID =
    { 0x86c77a67, 0xb97, 0x4633, {0xa1, 0x87, 0x49, 0x10, 0x4d, 0x06, 0x85, 0xc7 }};

static __attribute__((unused)) EFI_GUID EFI_FIRMWARE_MANAGEMENT_CAPSULE_ID_GUID =
    {0x6dcbd5ed, 0xe82d, 0x4c44, {0xbd, 0xa1, 0x71, 0x94, 0x19, 0x9a, 0xd9, 0x2a }};

static __attribute__((unused)) EFI_GUID EFI_SYSTEM_RESOURCE_TABLE_GUID =
    { 0xb122a263, 0x3661, 0x4f68, { 0x99, 0x29, 0x78, 0xf8, 0xb0, 0xd6, 0x21, 0x80 }};

static __attribute__((unused)) EFI_GUID EFI_JSON_CAPSULE_ID_GUID =
    {0x67d6f4cd, 0xd6b8, 0x4573, {0xbf, 0x4a, 0xde, 0x5e, 0x25, 0x2d, 0x61, 0xae }};

static __attribute__((unused)) EFI_GUID EFI_SIMPLE_NETWORK_PROTOCOL_GUID =
    {0xA19832B9,0xAC25,0x11D3, {0x9A,0x2D,0x00,0x90,0x27,0x3f,0xc1,0x4d}};

static __attribute__((unused)) EFI_GUID EFI_PXE_BASE_CODE_PROTOCOL_GUID =
    {0x03C4E603,0xAC28,0x11d3, {0x9A,0x2D,0x00,0x90,0x27,0x3F,0xC1,0x4D}};

static __attribute__((unused)) EFI_GUID EFI_PXE_BASE_CODE_CALLBACK_PROTOCOL_GUID =
    {0x245DCA21,0xFB7B,0x11d3, {0x8F,0x01,0x00,0xA0, 0xC9,0x69,0x72,0x3B}};

static __attribute__((unused)) EFI_GUID EFI_BIS_PROTOCOL_GUID =
    {0x0b64aab0,0x5429,0x11d4, {0x98,0x16,0x00,0xa0,0xc9,0x1f,0xad,0xcf}};

static __attribute__((unused)) EFI_GUID BOOT_OBJECT_AUTHORIZATION_PARMSET_GUID =
    {0xedd35e31,0x7b9,0x11d2,{0x83,0xa3,0x0,0xa0,0xc9,0x1f,0xad,0xcf}};

static __attribute__((unused)) EFI_GUID EFI_HTTP_BOOT_CALLBACK_PROTOCOL_GUID =
    {0xba23b311, 0x343d, 0x11e6, {0x91, 0x85, 0x58,0x20, 0xb1, 0xd6, 0x52, 0x99}};

static __attribute__((unused)) EFI_GUID EFI_MANAGED_NETWORK_SERVICE_BINDING_PROTOCOL_GUID =
    {0xf36ff770,0xa7e1,0x42cf, {0x9e,0xd2,0x56,0xf0,0xf2,0x71,0xf4,0x4c}};

static __attribute__((unused)) EFI_GUID EFI_MANAGED_NETWORK_PROTOCOL_GUID =
    {0x7ab33a91, 0xace5, 0x4326, {0xb5, 0x72, 0xe7, 0xee, 0x33, 0xd3, 0x9f, 0x16}};

static __attribute__((unused)) EFI_GUID EFI_BLUETOOTH_HC_PROTOCOL_GUID =
    { 0xb3930571, 0xbeba, 0x4fc5, { 0x92, 0x3, 0x94, 0x27, 0x24, 0x2e, 0x6a, 0x43 }};

static __attribute__((unused)) EFI_GUID EFI_BLUETOOTH_IO_PROTOCOL_GUID =
    { 0x467313de, 0x4e30, 0x43f1, { 0x94, 0x3e, 0x32, 0x3f, 0x89, 0x84, 0x5d, 0xb5 }};

static __attribute__((unused)) EFI_GUID EFI_BLUETOOTH_CONFIG_PROTOCOL_GUID =
    { 0x62960cf3, 0x40ff, 0x4263, { 0xa7, 0x7c, 0xdf, 0xde, 0xbd, 0x19, 0x1b, 0x4b }};

static __attribute__((unused)) EFI_GUID EFI_BLUETOOTH_ATTRIBUTE_PROTOCOL_GUID =
    { 0x898890e9, 0x84b2, 0x4f3a, { 0x8c, 0x58, 0xd8, 0x57, 0x78, 0x13, 0xe0, 0xac }};

static __attribute__((unused)) EFI_GUID EFI_BLUETOOTH_LE_CONFIG_PROTOCOL_GUID =
    { 0x8f76da58, 0x1f99, 0x4275, { 0xa4, 0xec, 0x47, 0x56, 0x51, 0x5b, 0x1c, 0xe8 }};

static __attribute__((unused)) EFI_GUID EFI_VLAN_CONFIG_PROTOCOL_GUID =
    {0x9e23d768, 0xd2f3, 0x4366, {0x9f, 0xc3, 0x3a, 0x7a, 0xba, 0x86, 0x43, 0x74}};

static __attribute__((unused)) EFI_GUID EFI_EAP_PROTOCOL_GUID =
    { 0x5d9f96db, 0xe731, 0x4caa, {0xa0, 0x0d, 0x72, 0xe1, 0x87, 0xcd, 0x77, 0x62 }};

static __attribute__((unused)) EFI_GUID EFI_EAP_MANAGEMENT2_PROTOCOL_GUID =
    { 0x5e93c847, 0x456d, 0x40b3, { 0xa6, 0xb4, 0x78, 0xb0, 0xc9, 0xcf, 0x7f, 0x20 }};

static __attribute__((unused)) EFI_GUID EFI_EAP_CONFIGURATION_PROTOCOL_GUID =
    { 0xe5b58dbb, 0x7688, 0x44b4, { 0x97, 0xbf, 0x5f, 0x1d, 0x4b, 0x7c, 0xc8, 0xdb }};

static __attribute__((unused)) EFI_GUID EFI_WIRELESS_MAC_CONNECTION_PROTOCOL_GUID =
    { 0xda55bc9, 0x45f8, 0x4bb4, { 0x87, 0x19, 0x52, 0x24, 0xf1, 0x8a, 0x4d, 0x45 }};

static __attribute__((unused)) EFI_GUID EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL_GUID =
    { 0x1b0fb9bf, 0x699d, 0x4fdd, { 0xa7, 0xc3, 0x25, 0x46, 0x68, 0x1b, 0xf6, 0x3b }};

static __attribute__((unused)) EFI_GUID EFI_SUPPLICANT_SERVICE_BINDING_PROTOCOL_GUID =
    { 0x45bcd98e, 0x59ad, 0x4174, { 0x95, 0x46, 0x34, 0x4a, 0x7, 0x48, 0x58, 0x98 }};

static __attribute__((unused)) EFI_GUID EFI_SUPPLICANT_PROTOCOL_GUID =
    { 0x54fcc43e, 0xaa89, 0x4333, { 0x9a, 0x85, 0xcd, 0xea, 0x24, 0x5, 0x1e, 0x9e }};

static __attribute__((unused)) EFI_GUID EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID =
    {0x00720665,0x67EB,0x4a99, {0xBA,0xF7,0xD3,0xC3,0x3A,0x1C,0x7C,0xC9}};

static __attribute__((unused)) EFI_GUID EFI_TCP4_PROTOCOL_GUID =
    {0x65530BC7,0xA359,0x410f, {0xB0,0x10,0x5A,0xAD,0xC7,0xEC,0x2B,0x62}};

static __attribute__((unused)) EFI_GUID EFI_TCP6_SERVICE_BINDING_PROTOCOL_GUID =
    {0xec20eb79,0x6c1a,0x4664, {0x9a,0x0d,0xd2,0xe4,0xcc,0x16,0xd6, 0x64}};

static __attribute__((unused)) EFI_GUID EFI_TCP6_PROTOCOL_GUID =
    {0x46e44855,0xbd60,0x4ab7, {0xab,0x0d,0xa6,0x79,0xb9,0x44,0x7d,0x77}};

static __attribute__((unused)) EFI_GUID EFI_IP4_SERVICE_BINDING_PROTOCOL_GUID =
    {0xc51711e7,0xb4bf,0x404a, {0xbf,0xb8,0x0a,0x04,0x8e,0xf1,0xff,0xe4}};

static __attribute__((unused)) EFI_GUID EFI_IP4_PROTOCOL_GUID =
    {0x41d94cd2,0x35b6,0x455a, {0x82,0x58,0xd4,0xe5,0x13,0x34,0xaa,0xdd}};

static __attribute__((unused)) EFI_GUID EFI_IP4_CONFIG2_PROTOCOL_GUID =
    { 0x5b446ed1, 0xe30b, 0x4faa, { 0x87, 0x1a, 0x36, 0x54, 0xec, 0xa3, 0x60, 0x80 }};

static __attribute__((unused)) EFI_GUID EFI_IP6_PROTOCOL_GUID =
    {0x2c8759d5,0x5c2d,0x66ef, {0x92,0x5f,0xb6,0x6c,0x10,0x19,0x57,0xe2}};

static __attribute__((unused)) EFI_GUID EFI_IPSEC_CONFIG_PROTOCOL_GUID =
    {0xce5e5929,0xc7a3,0x4602, {0xad,0x9e,0xc9,0xda,0xf9,0x4e,0xbf,0xcf}};

static __attribute__((unused)) EFI_GUID EFI_IPSEC_PROTOCOL_GUID =
    {0xdfb386f7,0xe100,0x43ad, {0x9c,0x9a,0xed,0x90,0xd0,0x8a,0x5e,0x12 }};

static __attribute__((unused)) EFI_GUID EFI_IPSEC2_PROTOCOL_GUID =
    {0xa3979e64, 0xace8, 0x4ddc, {0xbc, 0x07, 0x4d, 0x66, 0xb8, 0xfd, 0x09, 0x77}};

static __attribute__((unused)) EFI_GUID EFI_FTP4_SERVICE_BINDING_PROTOCOL_GUID =
    {0xfaaecb1, 0x226e, 0x4782, {0xaa, 0xce, 0x7d, 0xb9, 0xbc, 0xbf, 0x4d, 0xaf}};

static __attribute__((unused)) EFI_GUID EFI_FTP4_PROTOCOL_GUID =
    {0xeb338826, 0x681b, 0x4295, {0xb3, 0x56, 0x2b, 0x36, 0x4c, 0x75, 0x7b, 0x09}};

static __attribute__((unused)) EFI_GUID EFI_TLS_PROTOCOL_GUID =
    { 0xca959f, 0x6cfa, 0x4db1, {0x95, 0xbc, 0xe4, 0x6c, 0x47, 0x51, 0x43, 0x90 }};

static __attribute__((unused)) EFI_GUID EFI_TLS_CONFIGURATION_PROTOCOL_GUID =
    { 0x1682fe44, 0xbd7a, 0x4407, {0xb7, 0xc7, 0xdc, 0xa3, 0x7c, 0xa3, 0x92, 0x2d }};

static __attribute__((unused)) EFI_GUID EFI_ARP_SERVICE_BINDING_PROTOCOL_GUID =
    {0xf44c00ee,0x1f2c,0x4a00, {0xaa,0x09,0x1c,0x9f,0x3e,0x08,0x00,0xa3}};

static __attribute__((unused)) EFI_GUID EFI_ARP_PROTOCOL_GUID =
    {0xf4b427bb,0xba21,0x4f16, {0xbc,0x4e,0x43,0xe4,0x16,0xab,0x61,0x9c}};

static __attribute__((unused)) EFI_GUID EFI_DHCP4_SERVICE_BINDING_PROTOCOL_GUID =
    {0x9d9a39d8,0xbd42,0x4a73, {0xa4,0xd5,0x8e,0xe9,0x4b,0xe1,0x13,0x80}};

static __attribute__((unused)) EFI_GUID EFI_DHCP4_PROTOCOL_GUID =
    {0x8a219718,0x4ef5,0x4761, {0x91,0xc8,0xc0,0xf0,0x4b,0xda,0x9e,0x56}};

static __attribute__((unused)) EFI_GUID EFI_DHCP6_PROTOCOL_GUID =
    {0x87c8bad7,0x595,0x4053, {0x82,0x97,0xde,0xde,0x39,0x5f,0x5d,0x5b}};

static __attribute__((unused)) EFI_GUID EFI_DNS4_SERVICE_BINDING_PROTOCOL_GUID =
    { 0xb625b186, 0xe063, 0x44f7, { 0x89, 0x5, 0x6a, 0x74, 0xdc, 0x6f, 0x52, 0xb4}};

static __attribute__((unused)) EFI_GUID EFI_DNS4_PROTOCOL_GUID =
    { 0xae3d28cc, 0xe05b, 0x4fa1, {0xa0, 0x11, 0x7e, 0xb5, 0x5a, 0x3f, 0x14, 0x1 }};

static __attribute__((unused)) EFI_GUID EFI_DNS6_SERVICE_BINDING_PROTOCOL_GUID =
    { 0x7f1647c8, 0xb76e, 0x44b2, { 0xa5, 0x65, 0xf7, 0xf, 0xf1, 0x9c, 0xd1, 0x9e}};

static __attribute__((unused)) EFI_GUID EFI_DNS6_PROTOCOL_GUID =
    { 0xca37bc1f, 0xa327, 0x4ae9, { 0x82, 0x8a, 0x8c, 0x40, 0xd8, 0x50, 0x6a, 0x17 }};

static __attribute__((unused)) EFI_GUID EFI_HTTP_SERVICE_BINDING_PROTOCOL_GUID =
    {0xbdc8e6af, 0xd9bc, 0x4379, {0xa7, 0x2a, 0xe0, 0xc4, 0xe7, 0x5d, 0xae, 0x1c}};

static __attribute__((unused)) EFI_GUID EFI_HTTP_PROTOCOL_GUID =
    {0x7A59B29B, 0x910B, 0x4171, {0x82, 0x42, 0xA8, 0x5A, 0x0D, 0xF2, 0x5B, 0x5B}};

static __attribute__((unused)) EFI_GUID EFI_HTTP_UTILITIES_PROTOCOL_GUID =
    { 0x3E35C163, 0x4074, 0x45DD, { 0x43, 0x1E, 0x23, 0x98, 0x9D, 0xD8, 0x6B, 0x32 }};

static __attribute__((unused)) EFI_GUID EFI_REST_PROTOCOL_GUID =
    {0x0DB48A36, 0x4E54, 0xEA9C, { 0x9B, 0x09, 0x1E, 0xA5, 0xBE, 0x3A, 0x66, 0x0B }};

static __attribute__((unused)) EFI_GUID EFI_REST_EX_SERVICE_BINDING_PROTOCOL_GUID =
    {0x456bbe01, 0x99d0, 0x45ea, {0xbb, 0x5f, 0x16, 0xd8, 0x4b, 0xed, 0xc5, 0x59}};

static __attribute__((unused)) EFI_GUID EFI_REST_EX_PROTOCOL_GUID =
    {0x55648b91, 0xe7d, 0x40a3, {0xa9, 0xb3, 0xa8, 0x15, 0xd7, 0xea, 0xdf, 0x97}};

static __attribute__((unused)) EFI_GUID EFI_REST_JSON_STRUCTURE_PROTOCOL_GUID =
    { 0xa9a048f6, 0x48a0, 0x4714, {0xb7, 0xda, 0xa9, 0xad, 0x87, 0xd4, 0xda, 0xc9}};

static __attribute__((unused)) EFI_GUID EFI_UDP4_SERVICE_BINDING_PROTOCOL_GUID =
    {0x83f01464,0x99bd,0x45e5, {0xb3,0x83,0xaf,0x63,0x05,0xd8,0xe9,0xe6}};

static __attribute__((unused)) EFI_GUID EFI_UDP4_PROTOCOL_GUID =
    {0x3ad9df29,0x4501,0x478d, {0xb1,0xf8,0x7f,0x7f,0xe7,0x0e,0x50,0xf3}};

static __attribute__((unused)) EFI_GUID EFI_UDP6_SERVICE_BINDING_PROTOCOL_GUID =
    {0x66ed4721, 0x3c98, 0x4d3e, {0x81, 0xe3, 0xd0, 0x3d, 0xd3, 0x9a, 0x72, 0x54}};

static __attribute__((unused)) EFI_GUID EFI_UDP6_PROTOCOL_GUID =
    {0x4f948815, 0xb4b9, 0x43cb, {0x8a, 0x33, 0x90, 0xe0, 0x60, 0xb3, 0x49, 0x55}};

static __attribute__((unused)) EFI_GUID EFI_MTFTP4_SERVICE_BINDING_PROTOCOL_GUID =
    {0x2e800be,0x8f01,0x4aa6, {0x94,0x6b,0xd7,0x13,0x88,0xe1,0x83,0x3f}};

static __attribute__((unused)) EFI_GUID EFI_MTFTP4_PROTOCOL_GUID =
    {0x78247c57,0x63db,0x4708, {0x99,0xc2,0xa8,0xb4,0xa9,0xa6,0x1f,0x6b}};

static __attribute__((unused)) EFI_GUID EFI_MTFTP6_SERVICE_BINDING_PROTOCOL_GUID =
    {0xd9760ff3,0x3cca,0x4267, {0x80,0xf9,0x75,0x27,0xfa,0xfa,0x42,0x23}};

static __attribute__((unused)) EFI_GUID EFI_REDFISH_DISCOVER_PROTOCOL_GUID =
    {0x5db12509, 0x4550, 0x4347, {0x96, 0xb3, 0x73, 0xc0, 0xff, 0x6e, 0x86, 0x9f}};

static __attribute__((unused)) EFI_GUID EFI_AUTHENTICATION_INFO_PROTOCOL_GUID =
    {0x7671d9d0,0x53db,0x4173, {0xaa,0x69,0x23,0x27,0xf2,0x1f,0x0b,0xc7}};

static __attribute__((unused)) EFI_GUID EFI_AUTHENTICATION_CHAP_RADIUS_GUID =
    {0xd6062b50,0x15ca,0x11da, {0x92,0x19,0x00,0x10,0x83,0xff,0xca,0x4d}};

static __attribute__((unused)) EFI_GUID EFI_AUTHENTICATION_CHAP_LOCAL_GUID =
    {0xc280c73e,0x15ca,0x11da, {0xb0,0xca,0x00,0x10,0x83,0xff,0xca,0x4d}};

static __attribute__((unused)) EFI_GUID EFI_CERT_TYPE_RSA2048_SHA256_GUID =
    {0xa7717414, 0xc616, 0x4977, {0x94, 0x20, 0x84, 0x47, 0x12, 0xa7, 0x35, 0xbf}};

static __attribute__((unused)) EFI_GUID EFI_CERT_TYPE_PKCS7_GUID =
    {0x4aafd29d, 0x68df, 0x49ee, {0x8a, 0xa9, 0x34, 0x7d, 0x37, 0x56, 0x65, 0xa7}};

static __attribute__((unused)) EFI_GUID EFI_CERT_SHA256_GUID =
    { 0xc1c41626, 0x504c, 0x4092, { 0xac, 0xa9, 0x41, 0xf9, 0x36, 0x93, 0x43, 0x28 } };

static __attribute__((unused)) EFI_GUID EFI_CERT_RSA2048_GUID =
    { 0x3c5766e8, 0x269c, 0x4e34, { 0xaa, 0x14, 0xed, 0x77, 0x6e, 0x85, 0xb3, 0xb6 } };

static __attribute__((unused)) EFI_GUID EFI_CERT_RSA2048_SHA256_GUID =
    { 0xe2b36190, 0x879b, 0x4a3d, { 0xad, 0x8d, 0xf2, 0xe7, 0xbb, 0xa3, 0x27, 0x84 } };

static __attribute__((unused)) EFI_GUID EFI_CERT_SHA1_GUID =
    { 0x826ca512, 0xcf10, 0x4ac9, { 0xb1, 0x87, 0xbe, 0x01, 0x49, 0x66, 0x31, 0xbd } };

static __attribute__((unused)) EFI_GUID EFI_CERT_RSA2048_SHA1_GUID =
    { 0x67f8444f, 0x8743, 0x48f1, { 0xa3, 0x28, 0x1e, 0xaa, 0xb8, 0x73, 0x60, 0x80 } };

static __attribute__((unused)) EFI_GUID EFI_CERT_X509_GUID =
    { 0xa5c059a1, 0x94e4, 0x4aa7, { 0x87, 0xb5, 0xab, 0x15, 0x5c, 0x2b, 0xf0, 0x72 } };

static __attribute__((unused)) EFI_GUID EFI_CERT_SHA224_GUID =
    { 0xb6e5233, 0xa65c, 0x44c9, {0x94, 0x07, 0xd9, 0xab, 0x83, 0xbf, 0xc8, 0xbd} };

static __attribute__((unused)) EFI_GUID EFI_CERT_SHA384_GUID =
    { 0xff3e5307, 0x9fd0, 0x48c9, {0x85, 0xf1, 0x8a, 0xd5, 0x6c, 0x70, 0x1e, 0x01}};

static __attribute__((unused)) EFI_GUID EFI_CERT_SHA512_GUID =
    { 0x93e0fae, 0xa6c4, 0x4f50, {0x9f, 0x1b, 0xd4, 0x1e, 0x2b, 0x89, 0xc1, 0x9a}};

static __attribute__((unused)) EFI_GUID EFI_CERT_X509_SHA256_GUID =
    { 0x3bd2a492, 0x96c0, 0x4079, { 0xb4, 0x20, 0xfc, 0xf9, 0x8e, 0xf1, 0x03, 0xed } };

static __attribute__((unused)) EFI_GUID EFI_CERT_X509_SHA384_GUID =
    { 0x7076876e, 0x80c2, 0x4ee6, { 0xaa, 0xd2, 0x28, 0xb3, 0x49, 0xa6, 0x86, 0x5b } };

static __attribute__((unused)) EFI_GUID EFI_CERT_X509_SHA512_GUID =
    { 0x446dbf63, 0x2502, 0x4cda, { 0xbc, 0xfa, 0x24, 0x65, 0xd2, 0xb0, 0xfe, 0x9d } };

static __attribute__((unused)) EFI_GUID EFI_CERT_SM3_GUID =
    { 0x57347f87, 0x7a9b, 0x403a, { 0xb9, 0x3c, 0xdc, 0x4a, 0xfb, 0x7a, 0xe, 0xbc } };

static __attribute__((unused)) EFI_GUID EFI_CERT_X509_SM3_GUID =
    { 0x60d807e5, 0x10b4, 0x49a9, {0x93, 0x31, 0xe4, 0x4, 0x37, 0x88, 0x8d, 0x37 } };

static __attribute__((unused)) EFI_GUID EFI_CERT_EXTERNAL_MANAGEMENT_GUID =
    { 0x452e8ced, 0xdfff, 0x4b8c, { 0xae, 0x01, 0x51, 0x18, 0x86, 0x2e, 0x68, 0x2c } };

static __attribute__((unused)) EFI_GUID EFI_IMAGE_SECURITY_DATABASE_GUID =
    { 0xd719b2cb, 0x3d3a, 0x4596, { 0xa3, 0xbc, 0xda, 0xd0, 0x0e, 0x67, 0x65, 0x6f }};

static __attribute__((unused)) EFI_GUID EFI_HII_STANDARD_FORM_GUID =
    { 0x3bd2f4ec, 0xe524, 0x46e4, { 0xa9, 0xd8, 0x51, 0x01, 0x17, 0x42, 0x55, 0x62 } };

static __attribute__((unused)) EFI_GUID EFI_HII_FONT_PROTOCOL_GUID =
    { 0xe9ca4775, 0x8657, 0x47fc, {0x97, 0xe7, 0x7e, 0xd6, 0x5a, 0x8, 0x43, 0x24 }};

static __attribute__((unused)) EFI_GUID EFI_HII_FONT_EX_PROTOCOL_GUID =
    { 0x849e6875, 0xdb35, 0x4df8, {0xb4,0x1e, 0xc8, 0xf3, 0x37, 0x18, 0x7, 0x3f }};

static __attribute__((unused)) EFI_GUID EFI_HII_STRING_PROTOCOL_GUID =
    { 0xfd96974, 0x23aa, 0x4cdc, { 0xb9, 0xcb, 0x98, 0xd1, 0x77, 0x50, 0x32, 0x2a }};

static __attribute__((unused)) EFI_GUID EFI_HII_IMAGE_PROTOCOL_GUID =
    { 0x31a6406a, 0x6bdf, 0x4e46, { 0xb2, 0xa2, 0xeb, 0xaa, 0x89, 0xc4, 0x9, 0x20 }};

static __attribute__((unused)) EFI_GUID EFI_HII_IMAGE_EX_PROTOCOL_GUID =
    {0x1a1241e6, 0x8f19, 0x41a9, {0xbc,0xe, 0xe8, 0xef,0x39, 0xe0, 0x65, 0x46}};

static __attribute__((unused)) EFI_GUID EFI_HII_IMAGE_DECODER_PROTOCOL_GUID =
    {0x9E66F251, 0x727C, 0x418C, {0xBF, 0xD6, 0xC2, 0xB4, 0x25, 0x28, 0x18, 0xEA}};

static __attribute__((unused)) EFI_GUID EFI_HII_IMAGE_DECODER_NAME_JPEG_GUID =
    {0xefefd093, 0xd9b, 0x46eb, {0xa8,0x56, 0x48, 0x35,0x7, 0x0, 0xc9, 0x8}};

static __attribute__((unused)) EFI_GUID EFI_HII_IMAGE_DECODER_NAME_PNG_GUID =
    {0xaf060190, 0x5e3a, 0x4025, {0xaf,0xbd, 0xe1, 0xf9,0x5, 0xbf, 0xaa, 0x4c}};

static __attribute__((unused)) EFI_GUID EFI_HII_FONT_GLYPH_GENERATOR_PROTOCOL_GUID =
    { 0xf7102853, 0x7787, 0x4dc2, {0xa8,0xa8, 0x21, 0xb5, 0xdd, 0x5, 0xc8, 0x9b }};

static __attribute__((unused)) EFI_GUID EFI_HII_DATABASE_PROTOCOL_GUID =
    { 0xef9fc172, 0xa1b2, 0x4693, { 0xb3, 0x27, 0x6d, 0x32, 0xfc, 0x41, 0x60, 0x42 }};

static __attribute__((unused)) EFI_GUID EFI_HII_SET_KEYBOARD_LAYOUT_EVENT_GUID =
    { 0x14982a4f, 0xb0ed, 0x45b8, { 0xa8, 0x11, 0x5a, 0x7a, 0x9b, 0xc2, 0x32, 0xdf }};

static __attribute__((unused)) EFI_GUID EFI_CONFIG_KEYWORD_HANDLER_PROTOCOL_GUID =
    { 0x0a8badd5, 0x03b8, 0x4d19, {0xb1, 0x28, 0x7b, 0x8f, 0x0e, 0xda, 0xa5, 0x96 }};

static __attribute__((unused)) EFI_GUID EFI_HII_CONFIG_ROUTING_PROTOCOL_GUID =
    { 0x587e72d7, 0xcc50, 0x4f79, { 0x82, 0x09, 0xca, 0x29, 0x1f, 0xc1, 0xa1, 0x0f }};

static __attribute__((unused)) EFI_GUID EFI_HII_CONFIG_ACCESS_PROTOCOL_GUID =
    { 0x330d4706, 0xf2a0, 0x4e4f, {0xa3,0x69, 0xb6, 0x6f,0xa8, 0xd5, 0x43, 0x85}};

static __attribute__((unused)) EFI_GUID EFI_FORM_BROWSER2_PROTOCOL_GUID =
    { 0xb9d4c360, 0xbcfb, 0x4f9b, { 0x92, 0x98, 0x53, 0xc1, 0x36, 0x98, 0x22, 0x58 } };

static __attribute__((unused)) EFI_GUID EFI_HII_PLATFORM_SETUP_FORMSET_GUID =
    { 0x93039971, 0x8545, 0x4b04, { 0xb4, 0x5e, 0x32, 0xeb, 0x83, 0x26, 0x04, 0x0e } };

static __attribute__((unused)) EFI_GUID EFI_HII_DRIVER_HEALTH_FORMSET_GUID =
    { 0xf22fc20c, 0x8cf4, 0x45eb, { 0x8e, 0x06, 0xad, 0x4e, 0x50, 0xb9, 0x5d, 0xd3 } };

static __attribute__((unused)) EFI_GUID EFI_HII_USER_CREDENTIAL_FORMSET_GUID =
    { 0x337f4407, 0x5aee, 0x4b83, { 0xb2, 0xa7, 0x4e, 0xad, 0xca, 0x30, 0x88, 0xcd } };

static __attribute__((unused)) EFI_GUID EFI_HII_REST_STYLE_FORMSET_GUID =
    { 0x790217bd, 0xbecf, 0x485b, { 0x91, 0x70, 0x5f, 0xf7, 0x11, 0x31, 0x8b, 0x27 } };

static __attribute__((unused)) EFI_GUID EFI_HII_POPUP_PROTOCOL_GUID =
    { 0x4311edc0, 0x6054, 0x46d4, { 0x9e, 0x40, 0x89, 0x3e, 0xa9, 0x52, 0xfc, 0xcc } };

static __attribute__((unused)) EFI_GUID EFI_USER_MANAGER_PROTOCOL_GUID =
    { 0x6fd5b00c, 0xd426, 0x4283, { 0x98, 0x87, 0x6c, 0xf5, 0xcf, 0x1c, 0xb1, 0xfe } };

static __attribute__((unused)) EFI_GUID EFI_USER_CREDENTIAL2_PROTOCOL_GUID =
    { 0xe98adb03, 0xb8b9, 0x4af8, { 0xba, 0x20, 0x26, 0xe9, 0x11, 0x4c, 0xbc, 0xe5 } };

static __attribute__((unused)) EFI_GUID EFI_DEFERRED_IMAGE_LOAD_PROTOCOL_GUID =
    { 0x15853d7c, 0x3ddf, 0x43e0, { 0xa1, 0xcb, 0xeb, 0xf8, 0x5b, 0x8f, 0x87, 0x2c } };

static __attribute__((unused)) EFI_GUID EFI_USER_INFO_ACCESS_SETUP_ADMIN_GUID =
    { 0x85b75607, 0xf7ce, 0x471e, { 0xb7, 0xe4, 0x2a, 0xea, 0x5f, 0x72, 0x32, 0xee } };

static __attribute__((unused)) EFI_GUID EFI_USER_INFO_ACCESS_SETUP_NORMAL_GUID =
    { 0x1db29ae0, 0x9dcb, 0x43bc, { 0x8d, 0x87, 0x5d, 0xa1, 0x49, 0x64, 0xdd, 0xe2 } };

static __attribute__((unused)) EFI_GUID EFI_USER_INFO_ACCESS_SETUP_RESTRICTED_GUID =
    { 0xbdb38125, 0x4d63, 0x49f4, { 0x82, 0x12, 0x61, 0xcf, 0x5a, 0x19, 0x0a, 0xf8 } };

static __attribute__((unused)) EFI_GUID EFI_HASH_SERVICE_BINDING_PROTOCOL_GUID =
    {0x42881c98,0xa4f3,0x44b0, {0xa3,0x9d,0xdf,0xa1,0x86,0x67,0xd8,0xcd}};

static __attribute__((unused)) EFI_GUID EFI_HASH_PROTOCOL_GUID =
    {0xc5184932,0xdba5,0x46db, {0xa5,0xba,0xcc,0x0b,0xda,0x9c,0x14,0x35}};

static __attribute__((unused)) EFI_GUID EFI_KMS_PROTOCOL_GUID =
    {0xEC3A978D,0x7C4E, 0x48FA, {0x9A,0xBE,0x6A,0xD9,0x1C,0xC8,0xF8,0x11}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_GENERIC_128_GUID =
    {0xec8a3d69,0x6ddf,0x4108, {0x94,0x76,0x73,0x37,0xfc,0x52,0x21,0x36}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_GENERIC_160_GUID =
    {0xa3b3e6f8,0xefca,0x4bc1, {0x88,0xfb,0xcb,0x87,0x33,0x9b,0x25,0x79}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_GENERIC_256_GUID =
    {0x70f64793,0xc323,0x4261, {0xac,0x2c,0xd8,0x76,0xf2,0x7c,0x53,0x45}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_GENERIC_512_GUID =
    {0x978fe043,0xd7af,0x422e, {0x8a,0x92,0x2b,0x48,0xe4,0x63,0xbd,0xe6}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_GENERIC_1024_GUID =
    {0x43be0b44,0x874b,0x4ead, {0xb0,0x9c,0x24,0x1a,0x4f,0xbd,0x7e,0xb3}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_GENERIC_2048_GUID =
    {0x40093f23,0x630c,0x4626, {0x9c,0x48,0x40,0x37,0x3b,0x19,0xcb,0xbe}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_GENERIC_3072_GUID =
    {0xb9237513,0x6c44,0x4411, {0xa9,0x90,0x21,0xe5,0x56,0xe0,0x5a,0xde}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_GENERIC_DYNAMIC_GUID =
    {0x2156e996, 0x66de, 0x4b27, {0x9c, 0xc9, 0xb0, 0x9f, 0xac, 0x4d, 0x2, 0xbe}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_MD2_128_GUID =
    {0x78be11c4,0xee44,0x4a22, {0x9f,0x05,0x03,0x85,0x2e,0xc5,0xc9,0x78}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_MDC2_128_GUID =
    {0xf7ad60f8,0xefa8,0x44a3, {0x91,0x13,0x23,0x1f,0x39,0x9e,0xb4,0xc7}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_MD4_128_GUID =
    {0xd1c17aa1,0xcac5,0x400f, {0xbe,0x17,0xe2,0xa2,0xae,0x06,0x67,0x7c}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_MDC4_128_GUID =
    {0x3fa4f847,0xd8eb,0x4df4, {0xbd,0x49,0x10,0x3a,0x0a,0x84,0x7b,0xbc}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_MD5_128_GUID =
    {0xdcbc3662,0x9cda,0x4b52, {0xa0,0x4c,0x82,0xeb,0x1d,0x23,0x48,0xc7}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_MD5SHA_128_GUID =
    {0x1c178237,0x6897,0x459e, {0x9d,0x36,0x67,0xce,0x8e,0xf9,0x4f,0x76}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_SHA1_160_GUID =
    {0x453c5e5a,0x482d,0x43f0, {0x87,0xc9,0x59,0x41,0xf3,0xa3,0x8a,0xc2}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_SHA256_256_GUID =
    {0x6bb4f5cd,0x8022,0x448d, {0xbc,0x6d,0x77,0x1b,0xae,0x93,0x5f,0xc6}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_AESXTS_128_GUID =
    {0x4776e33f,0xdb47,0x479a, {0xa2,0x5f,0xa1,0xcd,0x0a,0xfa,0xb3,0x8b}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_AESXTS_256_GUID =
    {0xdc7e8613,0xc4bb,0x4db0, {0x84,0x62,0x13,0x51,0x13,0x57,0xab,0xe2}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_AESCBC_128_GUID =
    {0xa0e8ee6a,0x0e92,0x44d4, {0x86,0x1b,0x0e,0xaa,0x4a,0xca,0x44,0xa2}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_AESCBC_256_GUID =
    {0xd7e69789,0x1f68,0x45e8, {0x96,0xef,0x3b,0x64,0x07,0xa5,0xb2,0xdc}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_RSASHA1_1024_GUID =
    {0x56417bed,0x6bbe,0x4882, {0x86,0xa0,0x3a,0xe8,0xbb,0x17,0xf8,0xf9}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_RSASHA1_2048_GUID =
    {0xf66447d4,0x75a6,0x463e, {0xa8,0x19,0x07,0x7f,0x2d,0xda,0x05,0xe9}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_RSASHA256_2048_GUID =
    {0xa477af13,0x877d,0x4060, {0xba,0xa1,0x25,0xd1,0xbe,0xa0,0x8a,0xd3}};

static __attribute__((unused)) EFI_GUID EFI_KMS_FORMAT_RSASHA256_3072_GUID =
    {0x4e1356c2,0xeed,0x463f, {0x81,0x47,0x99,0x33,0xab,0xdb,0xc7,0xd5}};

static __attribute__((unused)) EFI_GUID EFI_PKCS7_VERIFY_PROTOCOL_GUID =
    { 0x47889fb2, 0xd671, 0x4fab, { 0xa0, 0xca, 0xdf, 0xe, 0x44, 0xdf, 0x70, 0xd6 }};

static __attribute__((unused)) EFI_GUID EFI_RNG_PROTOCOL_GUID =
    { 0x3152bca5, 0xeade, 0x433d, {0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44}};

static __attribute__((unused)) EFI_GUID EFI_RNG_ALGORITHM_SP800_90_HASH_256_GUID =
    {0xa7af67cb, 0x603b, 0x4d42, {0xba, 0x21, 0x70, 0xbf, 0xb6, 0x29, 0x3f, 0x96}};

static __attribute__((unused)) EFI_GUID EFI_RNG_ALGORITHM_SP800_90_HMAC_256_GUID =
    {0xc5149b43, 0xae85, 0x4f53, {0x99, 0x82, 0xb9, 0x43, 0x35, 0xd3, 0xa9, 0xe7}};

static __attribute__((unused)) EFI_GUID EFI_RNG_ALGORITHM_SP800_90_CTR_256_GUID =
    {0x44f0de6e, 0x4d8c, 0x4045, {0xa8, 0xc7, 0x4d, 0xd1, 0x68, 0x85, 0x6b, 0x9e}};

static __attribute__((unused)) EFI_GUID EFI_RNG_ALGORITHM_X9_31_3DES_GUID =
    {0x63c4785a, 0xca34, 0x4012, {0xa3, 0xc8, 0x0b, 0x6a, 0x32, 0x4f, 0x55, 0x46}};

static __attribute__((unused)) EFI_GUID EFI_RNG_ALGORITHM_X9_31_AES_GUID =
    {0xacd03321, 0x777e, 0x4d3d, {0xb1, 0xc8, 0x20, 0xcf, 0xd8, 0x88, 0x20, 0xc9}};

static __attribute__((unused)) EFI_GUID EFI_SMART_CARD_READER_PROTOCOL_GUID =
    {0x2a4d1adf, 0x21dc, 0x4b81, {0xa4, 0x2f, 0x8b, 0x8e, 0xe2, 0x38, 0x00, 0x60}};

static __attribute__((unused)) EFI_GUID EFI_SMART_CARD_EDGE_PROTOCOL_GUID =
    { 0xd317f29b, 0xa325, 0x4712, { 0x9b, 0xf1, 0xc6, 0x19, 0x54, 0xdc, 0x19, 0x8c } };

static __attribute__((unused)) EFI_GUID EFI_PADDING_RSASSA_PKCS1V1P5_GUID =
    {0x9317ec24,0x7cb0,0x4d0e, {0x8b,0x32,0x2e,0xd9,0x20,0x9c,0xd8,0xaf}};

static __attribute__((unused)) EFI_GUID EFI_PADDING_RSASSA_PSS_GUID =
    {0x7b2349e0,0x522d,0x4f8e, {0xb9,0x27,0x69,0xd9,0x7c,0x9e,0x79,0x5f}};

static __attribute__((unused)) EFI_GUID EFI_PADDING_NONE_GUID =
    {0x3629ddb1,0x228c,0x452e, {0xb6,0x16,0x09,0xed,0x31,0x6a,0x97,0x00}};

static __attribute__((unused)) EFI_GUID EFI_PADDING_RSAES_PKCS1V1P5_GUID =
    {0xe1c1d0a9,0x40b1,0x4632, {0xbd,0xcc,0xd9,0xd6,0xe5,0x29,0x56,0x31}};

static __attribute__((unused)) EFI_GUID EFI_PADDING_RSAES_OAEP_GUID =
    {0xc1e63ac4,0xd0cf,0x4ce6, {0x83,0x5b,0xee,0xd0,0xe6,0xa8,0xa4,0x5b}};

static __attribute__((unused)) EFI_GUID EFI_CC_MEASUREMENT_PROTOCOL_GUID =
    {0x96751a3d, 0x72f4, 0x41a6, {0xa7, 0x94, 0xed, 0x5d, 0xe, 0x67, 0xae, 0x6b }};

static __attribute__((unused)) EFI_GUID EFI_CC_FINAL_EVENTS_TABLE_GUID =
    {0xdd4a4648, 0x2de7, 0x4665, {0x96, 0x4d, 0x21, 0xd9, 0xef, 0x5f, 0xb4, 0x46}};

static __attribute__((unused)) EFI_GUID EFI_TIMESTAMP_PROTOCOL_GUID =
    { 0xafbfde41, 0x2e6e, 0x4262, { 0xba, 0x65, 0x62, 0xb9, 0x23, 0x6e, 0x54, 0x95 }};

static __attribute__((unused)) EFI_GUID EFI_RESET_NOTIFICATION_PROTOCOL_GUID =
    { 0x9da34ae0, 0xeaf9, 0x4bbf, { 0x8e, 0xc3, 0xfd, 0x60, 0x22, 0x6c, 0x44, 0xbe } };

static __attribute__((unused)) EFI_GUID EFI_PEI_STALL_PPI_GUID =
    { 0x1f4c6f90, 0xb06b, 0x48d8, {0xa2, 0x01, 0xba, 0xe5, 0xf1, 0xcd, 0x7d, 0x56} };

static __attribute__((unused)) EFI_GUID EFI_SEC_PLATFORM_INFORMATION2_GUID =
    {0x9e9f374b, 0x8f16, 0x4230, { 0x98, 0x24, 0x58, 0x46, 0xee, 0x76, 0x6a, 0x97}};

static __attribute__((unused)) EFI_GUID EFI_SEC_HOB_DATA_PPI_GUID =
    {0x3ebdaf20, 0x6667, 0x40d8, {0xb4, 0xee, 0xf5, 0x99, 0x9a, 0xc1, 0xb7, 0x1f}};

static __attribute__((unused)) EFI_GUID EFI_PEI_RECOVERY_BLOCK_IO2_PPI_GUID =
    { 0x26cc0fad, 0xbeb3, 0x478a, { 0x91, 0xb2, 0xc, 0x18, 0x8f, 0x72, 0x61, 0x98 } };

static __attribute__((unused)) EFI_GUID EFI_PEI_VECTOR_HANDOFF_INFO_PPI_GUID =
    { 0x3cd652b4, 0x6d33, 0x4dce, { 0x89, 0xdb, 0x83, 0xdf, 0x97, 0x66, 0xfc, 0xca } };

static __attribute__((unused)) EFI_GUID EFI_VECTOR_HANDOFF_TABLE_GUID =
    {0x996ec11c, 0x5397, 0x4e73, {0xb5, 0x8f, 0x82, 0x7e, 0x52, 0x90, 0x6d, 0xef}};

static __attribute__((unused)) EFI_GUID EFI_PEI_CAPSULE_PPI_GUID =
    {0x3acf33ee, 0xd892, 0x40f4, {0xa2, 0xfc, 0x38, 0x54, 0xd2, 0xe1, 0x32, 0x3d } };

static __attribute__((unused)) EFI_GUID EFI_PEI_MP_SERVICES_PPI_GUID =
    { 0xee16160a, 0xe8be, 0x47a6, { 0x82, 0xa, 0xc6, 0x90, 0xd, 0xb0, 0x25, 0xa } };

static __attribute__((unused)) EFI_GUID EDKII_PEI_MP_SERVICES2_PPI_GUID =
    { 0x5cb9cb3d, 0x31a4, 0x480c, { 0x94, 0x98, 0x29, 0xd2, 0xb9, 0xba, 0xcf, 0xba } };

static __attribute__((unused)) EFI_GUID EFI_PEI_GRAPHICS_PPI_GUID =
    { 0x6ecd1463, 0x4a4a, 0x461b, {0xaf, 0x5f, 0x5a, 0x33, 0xe3, 0xb2, 0x16, 0x2b }};

static __attribute__((unused)) EFI_GUID EFI_PEI_GRAPHICS_INFO_HOB_GUID =
    { 0x39f62cce, 0x6825, 0x4669, { 0xbb, 0x56, 0x54, 0x1a, 0xba, 0x75, 0x3a, 0x07 }};

static __attribute__((unused)) EFI_GUID EFI_PEI_GRAPHICS_DEVICE_INFO_HOB_GUID =
    { 0xe5cb2ac9, 0xd35d, 0x4430, { 0x93, 0x6e, 0x1d, 0xe3, 0x32, 0x47, 0x8d, 0xe7 }};

static __attribute__((unused)) EFI_GUID EFI_PEI_RESET2_PPI_GUID =
    {0x6cc45765, 0xcce4, 0x42fd, {0xbc, 0x56, 0x1, 0x1a,0xaa, 0xc6, 0xc9, 0xa8}};

static __attribute__((unused)) EFI_GUID EFI_PEI_TEMPORARY_RAM_DONE_PPI_GUID =
    { 0xceab683c, 0xec56, 0x4a2d, { 0xa9, 0x6, 0x40, 0x53, 0xfa, 0x4e, 0x9c, 0x16 } };

static __attribute__((unused)) EFI_GUID EFI_PEI_CORE_FV_LOCATION_GUID =
    {0x52888eae, 0x5b10, 0x47d0, {0xa8, 0x7f, 0xb8, 0x22, 0xab, 0xa0, 0xca, 0xf4}};

static __attribute__((unused)) EFI_GUID EFI_MP_SERVICES_PROTOCOL_GUID =
    {0x3fdda605,0xa76e,0x4f46,{0xad,0x29,0x12,0xf4, 0x53,0x1b,0x3d,0x08}};

static __attribute__((unused)) EFI_GUID EFI_END_OF_DXE_EVENT_GROUP_GUID =
    { 0x2ce967a, 0xdd7e, 0x4ffc, { 0x9e, 0xe7, 0x81, 0xc, 0xf0, 0x47, 0x8, 0x80 } };

static __attribute__((unused)) EFI_GUID EFI_FIRMWARE_FILE_SYSTEM3_GUID =
    { 0x5473c07a, 0x3dcb, 0x4dca, { 0xbd, 0x6f, 0x1e, 0x96, 0x89, 0xe7, 0x34, 0x9a } };

static __attribute__((unused)) EFI_GUID EFI_PEI_DECOMPRESS_PPI_GUID =
    { 0x1a36e4e7, 0xfab6, 0x476a, { 0x8e, 0x75, 0x69, 0x5a, 0x5, 0x76, 0xfd, 0xd7 } };

static __attribute__((unused)) EFI_GUID EFI_PCD_PROTOCOL_GUID =
    { 0x13a3f0f6, 0x264a, 0x3ef0, { 0xf2, 0xe0, 0xde, 0xc5, 0x12, 0x34, 0x2f, 0x34 } };

static __attribute__((unused)) EFI_GUID EFI_GET_PCD_INFO_PROTOCOL_GUID =
    { 0xfd0f4478, 0xefd, 0x461d, { 0xba, 0x2d, 0xe5, 0x8c, 0x45, 0xfd, 0x5f, 0x5e } };

static __attribute__((unused)) EFI_GUID EFI_PEI_PCD_PPI_GUID =
    { 0x1f34d25, 0x4de2, 0x23ad, { 0x3f, 0xf3, 0x36, 0x35, 0x3f, 0xf3, 0x23, 0xf1 } };

static __attribute__((unused)) EFI_GUID EFI_GET_PCD_INFO_PPI_GUID =
    { 0xa60c6b59, 0xe459, 0x425d, { 0x9c, 0x69, 0xb, 0xcc, 0x9c, 0xb2, 0x7d, 0x81 } };

static __attribute__((unused)) EFI_GUID EFI_PEI_MM_CORE_GUID =
    {0x8d1b3618, 0x111b, 0x4cba, {0xb7, 0x9a, 0x55, 0xb3, 0x2f, 0x60, 0xf0, 0x29} };

static __attribute__((unused)) EFI_GUID EFI_MM_READY_TO_LOCK_PROTOCOL_GUID =
    { 0x47b7fa8c, 0xf4bd, 0x4af6, {0x82, 0x0, 0x33, 0x30, 0x86, 0xf0, 0xd2, 0xc8 } };

static __attribute__((unused)) EFI_GUID EFI_MM_MP_PROTOCOL_GUID =
    { 0x5d5450d7, 0x990c, 0x4180, { 0xa8, 0x3, 0x8e, 0x63, 0xf0, 0x60, 0x83, 0x7 } };

static __attribute__((unused)) EFI_GUID EFI_MM_END_OF_DXE_PROTOCOL_GUID =
    { 0x24e70042, 0xd5c5, 0x4260, { 0x8c, 0x39, 0xa, 0xd3, 0xaa, 0x32, 0xe9, 0x3d } };

static __attribute__((unused)) EFI_GUID EFI_MM_HANDLER_STATE_NOTIFICATION_PROTOCOL_GUID =
    {0x30c8340f, 0x4c30, 0x41d9, {0xbf, 0xae, 0x44, 0x4a, 0xcb, 0x2c, 0x1f, 0x76}};

static __attribute__((unused)) EFI_GUID EFI_DELAYED_DISPATCH_PPI_GUID =
    { 0x869c711d, 0x649c, 0x44fe, { 0x8b, 0x9e, 0x2c, 0xbb, 0x29, 0x11, 0xc3, 0xe6} };

static __attribute__((unused)) EFI_GUID EFI_LEGACY_SPI_SMM_CONTROLLER_GUID =
    { 0x62331b78, 0xd8d0, 0x4c8c, { 0x8c, 0xcb, 0xd2, 0x7d, 0xfe, 0x32, 0xdb, 0x9b }};

static __attribute__((unused)) EFI_GUID EXTENDED_SAL_BOOT_SERVICE_PROTOCOL_GUID =
    {0xde0ee9a4,0x3c7a,0x44f2, {0xb7,0x8b,0xe3,0xcc,0xd6,0x9c,0x3a,0xf7}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_BASE_IO_SERVICES_PROTOCOL_GUID =
    {0x5aea42b5,0x31e1,0x4515, {0xbc,0x31,0xb8,0xd5,0x25,0x75,0x65,0xa6}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_STALL_SERVICES_PROTOCOL_GUID =
    {0x53a58d06,0xac27,0x4d8c, {0xb5,0xe9,0xf0,0x8a,0x80,0x65,0x41,0x70}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_RESET_SERVICES_PROTOCOL_GUID =
    {0x7d019990,0x8ce1,0x46f5, {0xa7,0x76,0x3c,0x51,0x98,0x67,0x6a,0xa0}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_PCI_SERVICES_PROTOCOL_GUID =
    {0xa46b1a31,0xad66,0x4905, {0x92,0xf6,0x2b,0x46,0x59,0xdc,0x30,0x63}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_CACHE_SERVICES_PROTOCOL_GUID =
    {0xedc9494,0x2743,0x4ba5, {0x88,0x18,0x0a,0xef,0x52,0x13,0xf1,0x88}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_PAL_SERVICES_PROTOCOL_GUID =
    {0xe1cd9d21,0x0fc2,0x438d, {0x97,0x03,0x04,0xe6,0x6d,0x96,0x1e,0x57}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_STATUS_CODE_SERVICES_PROTOCOL_GUID =
    {0xdbd91d,0x55e9,0x420f, {0x96,0x39,0x5e,0x9f,0x84,0x37,0xb4,0x4f}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_MTC_SERVICES_PROTOCOL_GUID =
    {0x899afd18,0x75e8,0x408b, {0xa4,0x1a,0x6e,0x2e,0x7e,0xcd,0xf4,0x54}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_VARIABLE_SERVICES_PROTOCOL_GUID =
    {0x4ecb6c53,0xc641,0x4370, {0x8c,0xb2,0x3b,0x0e,0x49,0x6e,0x83,0x78}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_FVB_SERVICES_PROTOCOL_GUID =
    {0xa2271df1,0xbcbb,0x4f1d, {0x98,0xa9,0x06,0xbc,0x17,0x2f,0x07,0x1a}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_MCA_LOG_SERVICES_PROTOCOL_GUID =
    {0xcb3fd86e,0x38a3,0x4c03, {0x9a,0x5c,0x90,0xcf,0xa3,0xa2,0xab,0x7a}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_BASE_SERVICES_PROTOCOL_GUID =
    {0xd9e9fa06,0x0fe0,0x41c3, {0x96,0xfb,0x83,0x42,0x5a,0x33,0x94,0xf8}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_MP_SERVICES_PROTOCOL_GUID =
    {0x697d81a2,0xcf18,0x4dc0, {0x9e,0x0d,0x06,0x11,0x3b,0x61,0x8a,0x3f}};

static __attribute__((unused)) EFI_GUID EFI_EXTENDED_SAL_MCA_SERVICES_PROTOCOL_GUID =
    {0x2a591128,0x6cc7,0x42b1, {0x8a,0xf0,0x58,0x93,0x3b,0x68,0x2d,0xbb}};

static __attribute__((unused)) EFI_GUID EFI_ACPI_SDT_PROTOCOL_GUID =
    { 0xeb97088e, 0xcfdf, 0x49c6, { 0xbe, 0x4b, 0xd9, 0x6, 0xa5, 0xb2, 0xe, 0x86 } };

static __attribute__((unused)) EFI_GUID EFI_I2C_MASTER_PROTOCOL_GUID =
    { 0xcd72881f, 0x45b5, 0x4feb, { 0x98, 0xc8, 0x31, 0x3d, 0xa8, 0x11, 0x74, 0x62 }};

static __attribute__((unused)) EFI_GUID EFI_I2C_HOST_PROTOCOL_GUID =
    { 0xa5aab9e3, 0xc727, 0x48cd, { 0x8b, 0xbf, 0x42, 0x72, 0x33, 0x85, 0x49, 0x48 }};

static __attribute__((unused)) EFI_GUID EFI_I2C_IO_PROTOCOL_GUID =
    { 0xb60a3e6b, 0x18c4, 0x46e5, { 0xa2, 0x9a, 0xc9, 0xa1, 0x06, 0x65, 0xa2, 0x8e }};

static __attribute__((unused)) EFI_GUID EFI_I2C_BUS_CONFIGURATION_MANAGEMENT_PROTOCOL_GUID =
    { 0x55b71fb5, 0x17c6, 0x410e, { 0xb5, 0xbd, 0x5f, 0xa2, 0xe3, 0xd4, 0x46, 0x6b }};

static __attribute__((unused)) EFI_GUID EFI_I2C_ENUMERATE_PROTOCOL_GUID =
    { 0xda8cd7c4, 0x1c00, 0x49e2, { 0x80, 0x3e, 0x52, 0x14, 0xe7, 0x01, 0x89, 0x4c }};

static __attribute__((unused)) EFI_GUID EFI_PEI_I2C_MASTER_PPI_GUID =
    { 0xb3bfab9b, 0x9f9c, 0x4e8b, { 0xad, 0x37, 0x7f, 0x8c, 0x51, 0xfc, 0x62, 0x80 }};

static __attribute__((unused)) EFI_GUID EFI_PCI_OVERRIDE_GUID =
    { 0xb5b35764, 0x460c, 0x4a06, { 0x99, 0xfc, 0x77, 0xa1, 0x7c, 0x1b, 0x5c, 0xeb } };

static __attribute__((unused)) EFI_GUID EFI_S3_SAVE_STATE_PROTOCOL_GUID =
    { 0xe857caf6, 0xc046, 0x45dc, { 0xbe, 0x3f, 0xee, 0x7, 0x65, 0xfb, 0xa8, 0x87 } };

static __attribute__((unused)) EFI_GUID EFI_S3_SMM_SAVE_STATE_PROTOCOL_GUID =
    { 0x320afe62, 0xe593, 0x49cb, { 0xa9, 0xf1, 0xd4, 0xc2, 0xf4, 0xaf, 0x1, 0x4c } };

static __attribute__((unused)) EFI_GUID EFI_SMBIOS_PROTOCOL_GUID =
    { 0x3583ff6, 0xcb36, 0x4940, { 0x94, 0x7e, 0xb9, 0xb3, 0x9f, 0x4a, 0xfa, 0xf7 } };

static __attribute__((unused)) EFI_GUID EFI_SPI_CONFIGURATION_GUID =
    { 0x85a6d3e6, 0xb65b, 0x4afc, { 0xb3, 0x8f, 0xc6, 0xd5, 0x4a, 0xf6, 0xdd, 0xc8 }};

static __attribute__((unused)) EFI_GUID EFI_SPI_NOR_FLASH_PROTOCOL_GUID =
    { 0xb57ec3fe, 0xf833, 0x4ba6, { 0x85, 0x78, 0x2a, 0x7d, 0x6a, 0x87, 0x44, 0x4b }};

static __attribute__((unused)) EFI_GUID EFI_LEGACY_SPI_FLASH_PROTOCOL_GUID =
    { 0xf01bed57, 0x04bc, 0x4f3f, { 0x96, 0x60, 0xd6, 0xf2, 0xea, 0x22, 0x82, 0x59 }};

static __attribute__((unused)) EFI_GUID EFI_SPI_HOST_GUID =
    { 0xc74e5db2, 0xfa96, 0x4ae2, { 0xb3, 0x99, 0x15, 0x97, 0x7f, 0xe3, 0x0, 0x2d }};

static __attribute__((unused)) EFI_GUID EFI_SIO_PROTOCOL_GUID =
    { 0x215fdd18, 0xbd50, 0x4feb, { 0x89, 0xb, 0x58, 0xca, 0xb, 0x47, 0x39, 0xe9 } };

static __attribute__((unused)) EFI_GUID EFI_SIO_PPI_GUID =
    {0x23a464ad, 0xcb83, 0x48b8, {0x94, 0xab, 0x1a, 0x6f, 0xef, 0xcf, 0xe5, 0x22}};

static __attribute__((unused)) EFI_GUID EFI_ISA_HC_PPI_GUID =
    {0x8d48bd70, 0xc8a3, 0x4c06, {0x90, 0x1b, 0x74, 0x79, 0x46, 0xaa, 0xc3, 0x58}};

static __attribute__((unused)) EFI_GUID EFI_ISA_HC_PROTOCOL_GUID =
    {0xbcdaf080, 0x1bde, 0x4e22, {0xae, 0x6a, 0x43, 0x54, 0x1e, 0x12, 0x8e, 0xc4}};

static __attribute__((unused)) EFI_GUID EFI_ISA_HC_SERVICE_BINDING_PROTOCOL_GUID =
    {0xfad7933a, 0x6c21, 0x4234, {0xa4, 0x34, 0x0a, 0x8a, 0x0d, 0x2b, 0x07, 0x81}};

static __attribute__((unused)) EFI_GUID EFI_SIO_CONTROL_PROTOCOL_GUID =
    {0xb91978df, 0x9fc1, 0x427d, {0xbb, 0x5, 0x4c, 0x82, 0x84, 0x55, 0xca, 0x27}};


// EDK2-style GUID aliases
#define gEfiBootManagerPolicyProtocolGuid  EFI_BOOT_MANAGER_POLICY_PROTOCOL_GUID
#define gEfiBootManagerPolicyConsoleGuid  EFI_BOOT_MANAGER_POLICY_CONSOLE_GUID
#define gEfiBootManagerPolicyNetworkGuid  EFI_BOOT_MANAGER_POLICY_NETWORK_GUID
#define gEfiBootManagerPolicyStorageGuid  EFI_BOOT_MANAGER_POLICY_STORAGE_GUID
#define gEfiBootManagerPolicyConnectAllGuid  EFI_BOOT_MANAGER_POLICY_CONNECT_ALL_GUID
#define gEfiAcpi20TableGuid  EFI_ACPI_20_TABLE_GUID
#define gEfiAcpiTableGuid  EFI_ACPI_TABLE_GUID
#define gEfiJsonConfigDataTableGuid  EFI_JSON_CONFIG_DATA_TABLE_GUID
#define gEfiJsonCapsuleDataTableGuid  EFI_JSON_CAPSULE_DATA_TABLE_GUID
#define gEfiJsonCapsuleResultTableGuid  EFI_JSON_CAPSULE_RESULT_TABLE_GUID
#define gEfiDtbTableGuid  EFI_DTB_TABLE_GUID
#define gEfiRtPropertiesTableGuid  EFI_RT_PROPERTIES_TABLE_GUID
#define gEfiMemoryAttributesTableGuid  EFI_MEMORY_ATTRIBUTES_TABLE_GUID
#define gEfiConformanceProfilesTableGuid  EFI_CONFORMANCE_PROFILES_TABLE_GUID
#define gEfiConformanceProfilesUefiSpecGuid  EFI_CONFORMANCE_PROFILES_UEFI_SPEC_GUID
#define gEfiHiiPackageListProtocolGuid  EFI_HII_PACKAGE_LIST_PROTOCOL_GUID
#define gEfiMemoryRangeCapsuleGuid  EFI_MEMORY_RANGE_CAPSULE_GUID
#define gEfiCapsuleReportGuid  EFI_CAPSULE_REPORT_GUID
#define gEfiLoadedImageProtocolGuid  EFI_LOADED_IMAGE_PROTOCOL_GUID
#define gEfiLoadedImageDevicePathProtocolGuid  EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID
#define gEfiDevicePathProtocolGuid  EFI_DEVICE_PATH_PROTOCOL_GUID
#define gEfiPcAnsiGuid  EFI_PC_ANSI_GUID
#define gEfiVt100Guid  EFI_VT_100_GUID
#define gEfiVt100PlusGuid  EFI_VT_100_PLUS_GUID
#define gEfiVtUtf8Guid  EFI_VT_UTF8_GUID
#define gEfiVirtualDiskGuid  EFI_VIRTUAL_DISK_GUID
#define gEfiVirtualCdGuid  EFI_VIRTUAL_CD_GUID
#define gEfiPersistentVirtualDiskGuid  EFI_PERSISTENT_VIRTUAL_DISK_GUID
#define gEfiPersistentVirtualCdGuid  EFI_PERSISTENT_VIRTUAL_CD_GUID
#define gEfiDevicePathUtilitiesProtocolGuid  EFI_DEVICE_PATH_UTILITIES_PROTOCOL_GUID
#define gEfiDevicePathToTextProtocolGuid  EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID
#define gEfiDevicePathFromTextProtocolGuid  EFI_DEVICE_PATH_FROM_TEXT_PROTOCOL_GUID
#define gEfiDriverBindingProtocolGuid  EFI_DRIVER_BINDING_PROTOCOL_GUID
#define gEfiPlatformDriverOverrideProtocolGuid  EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL_GUID
#define gEfiBusSpecificDriverOverrideProtocolGuid  EFI_BUS_SPECIFIC_DRIVER_OVERRIDE_PROTOCOL_GUID
#define gEfiDriverDiagnosticsProtocolGuid  EFI_DRIVER_DIAGNOSTICS_PROTOCOL_GUID
#define gEfiComponentName2ProtocolGuid  EFI_COMPONENT_NAME2_PROTOCOL_GUID
#define gEfiPlatformToDriverConfigurationProtocolGuid  EFI_PLATFORM_TO_DRIVER_CONFIGURATION_PROTOCOL_GUID
#define gEfiPlatformToDriverConfigurationClpGuid  EFI_PLATFORM_TO_DRIVER_CONFIGURATION_CLP_GUID
#define gEfiDriverSupportedEfiVersionProtocolGuid  EFI_DRIVER_SUPPORTED_EFI_VERSION_PROTOCOL_GUID
#define gEfiDriverFamilyOverrideProtocolGuid  EFI_DRIVER_FAMILY_OVERRIDE_PROTOCOL_GUID
#define gEfiDriverHealthProtocolGuid  EFI_DRIVER_HEALTH_PROTOCOL_GUID
#define gEfiAdapterInformationProtocolGuid  EFI_ADAPTER_INFORMATION_PROTOCOL_GUID
#define gEfiAdapterInfoMediaStateGuid  EFI_ADAPTER_INFO_MEDIA_STATE_GUID
#define gEfiAdapterInfoNetworkBootGuid  EFI_ADAPTER_INFO_NETWORK_BOOT_GUID
#define gEfiAdapterInfoSanMacAddressGuid  EFI_ADAPTER_INFO_SAN_MAC_ADDRESS_GUID
#define gEfiAdapterInfoUndiIpv6SupportGuid  EFI_ADAPTER_INFO_UNDI_IPV6_SUPPORT_GUID
#define gEfiAdapterInfoMediaTypeGuid  EFI_ADAPTER_INFO_MEDIA_TYPE_GUID
#define gEfiAdapterInfoCdatTypeGuid  EFI_ADAPTER_INFO_CDAT_TYPE_GUID
#define gEfiSimpleTextInputExProtocolGuid  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID
#define gEfiSimpleTextInputProtocolGuid  EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID
#define gEfiSimpleTextOutputProtocolGuid  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID
#define gEfiSimplePointerProtocolGuid  EFI_SIMPLE_POINTER_PROTOCOL_GUID
#define gEfiAbsolutePointerProtocolGuid  EFI_ABSOLUTE_POINTER_PROTOCOL_GUID
#define gEfiSerialIoProtocolGuid  EFI_SERIAL_IO_PROTOCOL_GUID
#define gEfiSerialTerminalDeviceTypeGuid  EFI_SERIAL_TERMINAL_DEVICE_TYPE_GUID
#define gEfiGraphicsOutputProtocolGuid  EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID
#define gEfiEdidDiscoveredProtocolGuid  EFI_EDID_DISCOVERED_PROTOCOL_GUID
#define gEfiEdidActiveProtocolGuid  EFI_EDID_ACTIVE_PROTOCOL_GUID
#define gEfiEdidOverrideProtocolGuid  EFI_EDID_OVERRIDE_PROTOCOL_GUID
#define gEfiLoadFileProtocolGuid  EFI_LOAD_FILE_PROTOCOL_GUID
#define gEfiLoadFile2ProtocolGuid  EFI_LOAD_FILE2_PROTOCOL_GUID
#define gEfiSimpleFileSystemProtocolGuid  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID
#define gEfiTapeIoProtocolGuid  EFI_TAPE_IO_PROTOCOL_GUID
#define gEfiDiskIoProtocolGuid  EFI_DISK_IO_PROTOCOL_GUID
#define gEfiDiskIo2ProtocolGuid  EFI_DISK_IO2_PROTOCOL_GUID
#define gEfiBlockIoProtocolGuid  EFI_BLOCK_IO_PROTOCOL_GUID
#define gEfiBlockIo2ProtocolGuid  EFI_BLOCK_IO2_PROTOCOL_GUID
#define gEfiBlockIoCryptoProtocolGuid  EFI_BLOCK_IO_CRYPTO_PROTOCOL_GUID
#define gEfiEraseBlockProtocolGuid  EFI_ERASE_BLOCK_PROTOCOL_GUID
#define gEfiAtaPassThruProtocolGuid  EFI_ATA_PASS_THRU_PROTOCOL_GUID
#define gEfiStorageSecurityCommandProtocolGuid  EFI_STORAGE_SECURITY_COMMAND_PROTOCOL_GUID
#define gEfiNvmExpressPassThruProtocolGuid  EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL_GUID
#define gEfiSdMmcPassThruProtocolGuid  EFI_SD_MMC_PASS_THRU_PROTOCOL_GUID
#define gEfiRamDiskProtocolGuid  EFI_RAM_DISK_PROTOCOL_GUID
#define gEfiNvdimmLabelProtocolGuid  EFI_NVDIMM_LABEL_PROTOCOL_GUID
#define gEfiUfsDeviceConfigGuid  EFI_UFS_DEVICE_CONFIG_GUID
#define gEfiPciRootBridgeIoProtocolGuid  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID
#define gEfiPciIoProtocolGuid  EFI_PCI_IO_PROTOCOL_GUID
#define gEfiScsiIoProtocolGuid  EFI_SCSI_IO_PROTOCOL_GUID
#define gEfiExtScsiPassThruProtocolGuid  EFI_EXT_SCSI_PASS_THRU_PROTOCOL_GUID
#define gEfiIscsiInitiatorNameProtocolGuid  EFI_ISCSI_INITIATOR_NAME_PROTOCOL_GUID
#define gEfiUsb2HcProtocolGuid  EFI_USB2_HC_PROTOCOL_GUID
#define gEfiUsbIoProtocolGuid  EFI_USB_IO_PROTOCOL_GUID
#define gEfiUsbfnIoProtocolGuid  EFI_USBFN_IO_PROTOCOL_GUID
#define gEfiDebugSupportProtocolGuid  EFI_DEBUG_SUPPORT_PROTOCOL_GUID
#define gEfiDebugportProtocolGuid  EFI_DEBUGPORT_PROTOCOL_GUID
#define gEfiDebugImageInfoTableGuid  EFI_DEBUG_IMAGE_INFO_TABLE_GUID
#define gEfiDecompressProtocolGuid  EFI_DECOMPRESS_PROTOCOL_GUID
#define gEfiAcpiTableProtocolGuid  EFI_ACPI_TABLE_PROTOCOL_GUID
#define gEfiUnicodeCollationProtocol2Guid  EFI_UNICODE_COLLATION_PROTOCOL2_GUID
#define gEfiRegularExpressionProtocolGuid  EFI_REGULAR_EXPRESSION_PROTOCOL_GUID
#define gEfiRegexSyntaxTypePosixExtendedGuid  EFI_REGEX_SYNTAX_TYPE_POSIX_EXTENDED_GUID
#define gEfiRegexSyntaxTypePerlGuid  EFI_REGEX_SYNTAX_TYPE_PERL_GUID
#define gEfiRegexSyntaxTypeEcma262Guid  EFI_REGEX_SYNTAX_TYPE_ECMA_262_GUID
#define gEfiRegexSyntaxTypePosixExtendedAsciiGuid  EFI_REGEX_SYNTAX_TYPE_POSIX_EXTENDED_ASCII_GUID
#define gEfiRegexSyntaxTypePerlAsciiGuid  EFI_REGEX_SYNTAX_TYPE_PERL_ASCII_GUID
#define gEfiRegexSyntaxTypeEcma262AsciiGuid  EFI_REGEX_SYNTAX_TYPE_ECMA_262_ASCII_GUID
#define gEfiEbcProtocolGuid  EFI_EBC_PROTOCOL_GUID
#define gEfiFirmwareManagementProtocolGuid  EFI_FIRMWARE_MANAGEMENT_PROTOCOL_GUID
#define gEfiFirmwareManagementCapsuleIdGuid  EFI_FIRMWARE_MANAGEMENT_CAPSULE_ID_GUID
#define gEfiSystemResourceTableGuid  EFI_SYSTEM_RESOURCE_TABLE_GUID
#define gEfiJsonCapsuleIdGuid  EFI_JSON_CAPSULE_ID_GUID
#define gEfiSimpleNetworkProtocolGuid  EFI_SIMPLE_NETWORK_PROTOCOL_GUID
#define gEfiPxeBaseCodeProtocolGuid  EFI_PXE_BASE_CODE_PROTOCOL_GUID
#define gEfiPxeBaseCodeCallbackProtocolGuid  EFI_PXE_BASE_CODE_CALLBACK_PROTOCOL_GUID
#define gEfiBisProtocolGuid  EFI_BIS_PROTOCOL_GUID
#define gEfiHttpBootCallbackProtocolGuid  EFI_HTTP_BOOT_CALLBACK_PROTOCOL_GUID
#define gEfiManagedNetworkServiceBindingProtocolGuid  EFI_MANAGED_NETWORK_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiManagedNetworkProtocolGuid  EFI_MANAGED_NETWORK_PROTOCOL_GUID
#define gEfiBluetoothHcProtocolGuid  EFI_BLUETOOTH_HC_PROTOCOL_GUID
#define gEfiBluetoothIoProtocolGuid  EFI_BLUETOOTH_IO_PROTOCOL_GUID
#define gEfiBluetoothConfigProtocolGuid  EFI_BLUETOOTH_CONFIG_PROTOCOL_GUID
#define gEfiBluetoothAttributeProtocolGuid  EFI_BLUETOOTH_ATTRIBUTE_PROTOCOL_GUID
#define gEfiBluetoothLeConfigProtocolGuid  EFI_BLUETOOTH_LE_CONFIG_PROTOCOL_GUID
#define gEfiVlanConfigProtocolGuid  EFI_VLAN_CONFIG_PROTOCOL_GUID
#define gEfiEapProtocolGuid  EFI_EAP_PROTOCOL_GUID
#define gEfiEapManagement2ProtocolGuid  EFI_EAP_MANAGEMENT2_PROTOCOL_GUID
#define gEfiEapConfigurationProtocolGuid  EFI_EAP_CONFIGURATION_PROTOCOL_GUID
#define gEfiWirelessMacConnectionProtocolGuid  EFI_WIRELESS_MAC_CONNECTION_PROTOCOL_GUID
#define gEfiWirelessMacConnectionIiProtocolGuid  EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL_GUID
#define gEfiSupplicantServiceBindingProtocolGuid  EFI_SUPPLICANT_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiSupplicantProtocolGuid  EFI_SUPPLICANT_PROTOCOL_GUID
#define gEfiTcp4ServiceBindingProtocolGuid  EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiTcp4ProtocolGuid  EFI_TCP4_PROTOCOL_GUID
#define gEfiTcp6ServiceBindingProtocolGuid  EFI_TCP6_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiTcp6ProtocolGuid  EFI_TCP6_PROTOCOL_GUID
#define gEfiIp4ServiceBindingProtocolGuid  EFI_IP4_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiIp4ProtocolGuid  EFI_IP4_PROTOCOL_GUID
#define gEfiIp4Config2ProtocolGuid  EFI_IP4_CONFIG2_PROTOCOL_GUID
#define gEfiIp6ProtocolGuid  EFI_IP6_PROTOCOL_GUID
#define gEfiIpsecConfigProtocolGuid  EFI_IPSEC_CONFIG_PROTOCOL_GUID
#define gEfiIpsecProtocolGuid  EFI_IPSEC_PROTOCOL_GUID
#define gEfiIpsec2ProtocolGuid  EFI_IPSEC2_PROTOCOL_GUID
#define gEfiFtp4ServiceBindingProtocolGuid  EFI_FTP4_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiFtp4ProtocolGuid  EFI_FTP4_PROTOCOL_GUID
#define gEfiTlsProtocolGuid  EFI_TLS_PROTOCOL_GUID
#define gEfiTlsConfigurationProtocolGuid  EFI_TLS_CONFIGURATION_PROTOCOL_GUID
#define gEfiArpServiceBindingProtocolGuid  EFI_ARP_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiArpProtocolGuid  EFI_ARP_PROTOCOL_GUID
#define gEfiDhcp4ServiceBindingProtocolGuid  EFI_DHCP4_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiDhcp4ProtocolGuid  EFI_DHCP4_PROTOCOL_GUID
#define gEfiDhcp6ProtocolGuid  EFI_DHCP6_PROTOCOL_GUID
#define gEfiDns4ServiceBindingProtocolGuid  EFI_DNS4_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiDns4ProtocolGuid  EFI_DNS4_PROTOCOL_GUID
#define gEfiDns6ServiceBindingProtocolGuid  EFI_DNS6_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiDns6ProtocolGuid  EFI_DNS6_PROTOCOL_GUID
#define gEfiHttpServiceBindingProtocolGuid  EFI_HTTP_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiHttpProtocolGuid  EFI_HTTP_PROTOCOL_GUID
#define gEfiHttpUtilitiesProtocolGuid  EFI_HTTP_UTILITIES_PROTOCOL_GUID
#define gEfiRestProtocolGuid  EFI_REST_PROTOCOL_GUID
#define gEfiRestExServiceBindingProtocolGuid  EFI_REST_EX_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiRestExProtocolGuid  EFI_REST_EX_PROTOCOL_GUID
#define gEfiRestJsonStructureProtocolGuid  EFI_REST_JSON_STRUCTURE_PROTOCOL_GUID
#define gEfiUdp4ServiceBindingProtocolGuid  EFI_UDP4_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiUdp4ProtocolGuid  EFI_UDP4_PROTOCOL_GUID
#define gEfiUdp6ServiceBindingProtocolGuid  EFI_UDP6_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiUdp6ProtocolGuid  EFI_UDP6_PROTOCOL_GUID
#define gEfiMtftp4ServiceBindingProtocolGuid  EFI_MTFTP4_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiMtftp4ProtocolGuid  EFI_MTFTP4_PROTOCOL_GUID
#define gEfiMtftp6ServiceBindingProtocolGuid  EFI_MTFTP6_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiRedfishDiscoverProtocolGuid  EFI_REDFISH_DISCOVER_PROTOCOL_GUID
#define gEfiAuthenticationInfoProtocolGuid  EFI_AUTHENTICATION_INFO_PROTOCOL_GUID
#define gEfiAuthenticationChapRadiusGuid  EFI_AUTHENTICATION_CHAP_RADIUS_GUID
#define gEfiAuthenticationChapLocalGuid  EFI_AUTHENTICATION_CHAP_LOCAL_GUID
#define gEfiCertTypeRsa2048Sha256Guid  EFI_CERT_TYPE_RSA2048_SHA256_GUID
#define gEfiCertTypePkcs7Guid  EFI_CERT_TYPE_PKCS7_GUID
#define gEfiCertSha256Guid  EFI_CERT_SHA256_GUID
#define gEfiCertRsa2048Guid  EFI_CERT_RSA2048_GUID
#define gEfiCertRsa2048Sha256Guid  EFI_CERT_RSA2048_SHA256_GUID
#define gEfiCertSha1Guid  EFI_CERT_SHA1_GUID
#define gEfiCertRsa2048Sha1Guid  EFI_CERT_RSA2048_SHA1_GUID
#define gEfiCertX509Guid  EFI_CERT_X509_GUID
#define gEfiCertSha224Guid  EFI_CERT_SHA224_GUID
#define gEfiCertSha384Guid  EFI_CERT_SHA384_GUID
#define gEfiCertSha512Guid  EFI_CERT_SHA512_GUID
#define gEfiCertX509Sha256Guid  EFI_CERT_X509_SHA256_GUID
#define gEfiCertX509Sha384Guid  EFI_CERT_X509_SHA384_GUID
#define gEfiCertX509Sha512Guid  EFI_CERT_X509_SHA512_GUID
#define gEfiCertSm3Guid  EFI_CERT_SM3_GUID
#define gEfiCertX509Sm3Guid  EFI_CERT_X509_SM3_GUID
#define gEfiCertExternalManagementGuid  EFI_CERT_EXTERNAL_MANAGEMENT_GUID
#define gEfiImageSecurityDatabaseGuid  EFI_IMAGE_SECURITY_DATABASE_GUID
#define gEfiHiiStandardFormGuid  EFI_HII_STANDARD_FORM_GUID
#define gEfiHiiFontProtocolGuid  EFI_HII_FONT_PROTOCOL_GUID
#define gEfiHiiFontExProtocolGuid  EFI_HII_FONT_EX_PROTOCOL_GUID
#define gEfiHiiStringProtocolGuid  EFI_HII_STRING_PROTOCOL_GUID
#define gEfiHiiImageProtocolGuid  EFI_HII_IMAGE_PROTOCOL_GUID
#define gEfiHiiImageExProtocolGuid  EFI_HII_IMAGE_EX_PROTOCOL_GUID
#define gEfiHiiImageDecoderProtocolGuid  EFI_HII_IMAGE_DECODER_PROTOCOL_GUID
#define gEfiHiiImageDecoderNameJpegGuid  EFI_HII_IMAGE_DECODER_NAME_JPEG_GUID
#define gEfiHiiImageDecoderNamePngGuid  EFI_HII_IMAGE_DECODER_NAME_PNG_GUID
#define gEfiHiiFontGlyphGeneratorProtocolGuid  EFI_HII_FONT_GLYPH_GENERATOR_PROTOCOL_GUID
#define gEfiHiiDatabaseProtocolGuid  EFI_HII_DATABASE_PROTOCOL_GUID
#define gEfiHiiSetKeyboardLayoutEventGuid  EFI_HII_SET_KEYBOARD_LAYOUT_EVENT_GUID
#define gEfiConfigKeywordHandlerProtocolGuid  EFI_CONFIG_KEYWORD_HANDLER_PROTOCOL_GUID
#define gEfiHiiConfigRoutingProtocolGuid  EFI_HII_CONFIG_ROUTING_PROTOCOL_GUID
#define gEfiHiiConfigAccessProtocolGuid  EFI_HII_CONFIG_ACCESS_PROTOCOL_GUID
#define gEfiFormBrowser2ProtocolGuid  EFI_FORM_BROWSER2_PROTOCOL_GUID
#define gEfiHiiPlatformSetupFormsetGuid  EFI_HII_PLATFORM_SETUP_FORMSET_GUID
#define gEfiHiiDriverHealthFormsetGuid  EFI_HII_DRIVER_HEALTH_FORMSET_GUID
#define gEfiHiiUserCredentialFormsetGuid  EFI_HII_USER_CREDENTIAL_FORMSET_GUID
#define gEfiHiiRestStyleFormsetGuid  EFI_HII_REST_STYLE_FORMSET_GUID
#define gEfiHiiPopupProtocolGuid  EFI_HII_POPUP_PROTOCOL_GUID
#define gEfiUserManagerProtocolGuid  EFI_USER_MANAGER_PROTOCOL_GUID
#define gEfiUserCredential2ProtocolGuid  EFI_USER_CREDENTIAL2_PROTOCOL_GUID
#define gEfiDeferredImageLoadProtocolGuid  EFI_DEFERRED_IMAGE_LOAD_PROTOCOL_GUID
#define gEfiUserInfoAccessSetupAdminGuid  EFI_USER_INFO_ACCESS_SETUP_ADMIN_GUID
#define gEfiUserInfoAccessSetupNormalGuid  EFI_USER_INFO_ACCESS_SETUP_NORMAL_GUID
#define gEfiUserInfoAccessSetupRestrictedGuid  EFI_USER_INFO_ACCESS_SETUP_RESTRICTED_GUID
#define gEfiHashServiceBindingProtocolGuid  EFI_HASH_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiHashProtocolGuid  EFI_HASH_PROTOCOL_GUID
#define gEfiKmsProtocolGuid  EFI_KMS_PROTOCOL_GUID
#define gEfiKmsFormatGeneric128Guid  EFI_KMS_FORMAT_GENERIC_128_GUID
#define gEfiKmsFormatGeneric160Guid  EFI_KMS_FORMAT_GENERIC_160_GUID
#define gEfiKmsFormatGeneric256Guid  EFI_KMS_FORMAT_GENERIC_256_GUID
#define gEfiKmsFormatGeneric512Guid  EFI_KMS_FORMAT_GENERIC_512_GUID
#define gEfiKmsFormatGeneric1024Guid  EFI_KMS_FORMAT_GENERIC_1024_GUID
#define gEfiKmsFormatGeneric2048Guid  EFI_KMS_FORMAT_GENERIC_2048_GUID
#define gEfiKmsFormatGeneric3072Guid  EFI_KMS_FORMAT_GENERIC_3072_GUID
#define gEfiKmsFormatGenericDynamicGuid  EFI_KMS_FORMAT_GENERIC_DYNAMIC_GUID
#define gEfiKmsFormatMd2128Guid  EFI_KMS_FORMAT_MD2_128_GUID
#define gEfiKmsFormatMdc2128Guid  EFI_KMS_FORMAT_MDC2_128_GUID
#define gEfiKmsFormatMd4128Guid  EFI_KMS_FORMAT_MD4_128_GUID
#define gEfiKmsFormatMdc4128Guid  EFI_KMS_FORMAT_MDC4_128_GUID
#define gEfiKmsFormatMd5128Guid  EFI_KMS_FORMAT_MD5_128_GUID
#define gEfiKmsFormatMd5sha128Guid  EFI_KMS_FORMAT_MD5SHA_128_GUID
#define gEfiKmsFormatSha1160Guid  EFI_KMS_FORMAT_SHA1_160_GUID
#define gEfiKmsFormatSha256256Guid  EFI_KMS_FORMAT_SHA256_256_GUID
#define gEfiKmsFormatAesxts128Guid  EFI_KMS_FORMAT_AESXTS_128_GUID
#define gEfiKmsFormatAesxts256Guid  EFI_KMS_FORMAT_AESXTS_256_GUID
#define gEfiKmsFormatAescbc128Guid  EFI_KMS_FORMAT_AESCBC_128_GUID
#define gEfiKmsFormatAescbc256Guid  EFI_KMS_FORMAT_AESCBC_256_GUID
#define gEfiKmsFormatRsasha11024Guid  EFI_KMS_FORMAT_RSASHA1_1024_GUID
#define gEfiKmsFormatRsasha12048Guid  EFI_KMS_FORMAT_RSASHA1_2048_GUID
#define gEfiKmsFormatRsasha2562048Guid  EFI_KMS_FORMAT_RSASHA256_2048_GUID
#define gEfiKmsFormatRsasha2563072Guid  EFI_KMS_FORMAT_RSASHA256_3072_GUID
#define gEfiPkcs7VerifyProtocolGuid  EFI_PKCS7_VERIFY_PROTOCOL_GUID
#define gEfiRngProtocolGuid  EFI_RNG_PROTOCOL_GUID
#define gEfiRngAlgorithmSp80090Hash256Guid  EFI_RNG_ALGORITHM_SP800_90_HASH_256_GUID
#define gEfiRngAlgorithmSp80090Hmac256Guid  EFI_RNG_ALGORITHM_SP800_90_HMAC_256_GUID
#define gEfiRngAlgorithmSp80090Ctr256Guid  EFI_RNG_ALGORITHM_SP800_90_CTR_256_GUID
#define gEfiRngAlgorithmX9313desGuid  EFI_RNG_ALGORITHM_X9_31_3DES_GUID
#define gEfiRngAlgorithmX931AesGuid  EFI_RNG_ALGORITHM_X9_31_AES_GUID
#define gEfiSmartCardReaderProtocolGuid  EFI_SMART_CARD_READER_PROTOCOL_GUID
#define gEfiSmartCardEdgeProtocolGuid  EFI_SMART_CARD_EDGE_PROTOCOL_GUID
#define gEfiPaddingRsassaPkcs1v1p5Guid  EFI_PADDING_RSASSA_PKCS1V1P5_GUID
#define gEfiPaddingRsassaPssGuid  EFI_PADDING_RSASSA_PSS_GUID
#define gEfiPaddingNoneGuid  EFI_PADDING_NONE_GUID
#define gEfiPaddingRsaesPkcs1v1p5Guid  EFI_PADDING_RSAES_PKCS1V1P5_GUID
#define gEfiPaddingRsaesOaepGuid  EFI_PADDING_RSAES_OAEP_GUID
#define gEfiCcMeasurementProtocolGuid  EFI_CC_MEASUREMENT_PROTOCOL_GUID
#define gEfiCcFinalEventsTableGuid  EFI_CC_FINAL_EVENTS_TABLE_GUID
#define gEfiTimestampProtocolGuid  EFI_TIMESTAMP_PROTOCOL_GUID
#define gEfiResetNotificationProtocolGuid  EFI_RESET_NOTIFICATION_PROTOCOL_GUID
#define gEfiPeiStallPpiGuid  EFI_PEI_STALL_PPI_GUID
#define gEfiSecPlatformInformation2Guid  EFI_SEC_PLATFORM_INFORMATION2_GUID
#define gEfiSecHobDataPpiGuid  EFI_SEC_HOB_DATA_PPI_GUID
#define gEfiPeiRecoveryBlockIo2PpiGuid  EFI_PEI_RECOVERY_BLOCK_IO2_PPI_GUID
#define gEfiPeiVectorHandoffInfoPpiGuid  EFI_PEI_VECTOR_HANDOFF_INFO_PPI_GUID
#define gEfiVectorHandoffTableGuid  EFI_VECTOR_HANDOFF_TABLE_GUID
#define gEfiPeiCapsulePpiGuid  EFI_PEI_CAPSULE_PPI_GUID
#define gEfiPeiMpServicesPpiGuid  EFI_PEI_MP_SERVICES_PPI_GUID
#define gEfiPeiGraphicsPpiGuid  EFI_PEI_GRAPHICS_PPI_GUID
#define gEfiPeiGraphicsInfoHobGuid  EFI_PEI_GRAPHICS_INFO_HOB_GUID
#define gEfiPeiGraphicsDeviceInfoHobGuid  EFI_PEI_GRAPHICS_DEVICE_INFO_HOB_GUID
#define gEfiPeiReset2PpiGuid  EFI_PEI_RESET2_PPI_GUID
#define gEfiPeiTemporaryRamDonePpiGuid  EFI_PEI_TEMPORARY_RAM_DONE_PPI_GUID
#define gEfiPeiCoreFvLocationGuid  EFI_PEI_CORE_FV_LOCATION_GUID
#define gEfiMpServicesProtocolGuid  EFI_MP_SERVICES_PROTOCOL_GUID
#define gEfiEndOfDxeEventGroupGuid  EFI_END_OF_DXE_EVENT_GROUP_GUID
#define gEfiFirmwareFileSystem3Guid  EFI_FIRMWARE_FILE_SYSTEM3_GUID
#define gEfiPeiDecompressPpiGuid  EFI_PEI_DECOMPRESS_PPI_GUID
#define gEfiPcdProtocolGuid  EFI_PCD_PROTOCOL_GUID
#define gEfiGetPcdInfoProtocolGuid  EFI_GET_PCD_INFO_PROTOCOL_GUID
#define gEfiPeiPcdPpiGuid  EFI_PEI_PCD_PPI_GUID
#define gEfiGetPcdInfoPpiGuid  EFI_GET_PCD_INFO_PPI_GUID
#define gEfiPeiMmCoreGuid  EFI_PEI_MM_CORE_GUID
#define gEfiMmReadyToLockProtocolGuid  EFI_MM_READY_TO_LOCK_PROTOCOL_GUID
#define gEfiMmMpProtocolGuid  EFI_MM_MP_PROTOCOL_GUID
#define gEfiMmEndOfDxeProtocolGuid  EFI_MM_END_OF_DXE_PROTOCOL_GUID
#define gEfiMmHandlerStateNotificationProtocolGuid  EFI_MM_HANDLER_STATE_NOTIFICATION_PROTOCOL_GUID
#define gEfiDelayedDispatchPpiGuid  EFI_DELAYED_DISPATCH_PPI_GUID
#define gEfiLegacySpiSmmControllerGuid  EFI_LEGACY_SPI_SMM_CONTROLLER_GUID
#define gEfiExtendedSalBaseIoServicesProtocolGuid  EFI_EXTENDED_SAL_BASE_IO_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalStallServicesProtocolGuid  EFI_EXTENDED_SAL_STALL_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalResetServicesProtocolGuid  EFI_EXTENDED_SAL_RESET_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalPciServicesProtocolGuid  EFI_EXTENDED_SAL_PCI_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalCacheServicesProtocolGuid  EFI_EXTENDED_SAL_CACHE_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalPalServicesProtocolGuid  EFI_EXTENDED_SAL_PAL_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalStatusCodeServicesProtocolGuid  EFI_EXTENDED_SAL_STATUS_CODE_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalMtcServicesProtocolGuid  EFI_EXTENDED_SAL_MTC_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalVariableServicesProtocolGuid  EFI_EXTENDED_SAL_VARIABLE_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalFvbServicesProtocolGuid  EFI_EXTENDED_SAL_FVB_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalMcaLogServicesProtocolGuid  EFI_EXTENDED_SAL_MCA_LOG_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalBaseServicesProtocolGuid  EFI_EXTENDED_SAL_BASE_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalMpServicesProtocolGuid  EFI_EXTENDED_SAL_MP_SERVICES_PROTOCOL_GUID
#define gEfiExtendedSalMcaServicesProtocolGuid  EFI_EXTENDED_SAL_MCA_SERVICES_PROTOCOL_GUID
#define gEfiAcpiSdtProtocolGuid  EFI_ACPI_SDT_PROTOCOL_GUID
#define gEfiI2cMasterProtocolGuid  EFI_I2C_MASTER_PROTOCOL_GUID
#define gEfiI2cHostProtocolGuid  EFI_I2C_HOST_PROTOCOL_GUID
#define gEfiI2cIoProtocolGuid  EFI_I2C_IO_PROTOCOL_GUID
#define gEfiI2cBusConfigurationManagementProtocolGuid  EFI_I2C_BUS_CONFIGURATION_MANAGEMENT_PROTOCOL_GUID
#define gEfiI2cEnumerateProtocolGuid  EFI_I2C_ENUMERATE_PROTOCOL_GUID
#define gEfiPeiI2cMasterPpiGuid  EFI_PEI_I2C_MASTER_PPI_GUID
#define gEfiPciOverrideGuid  EFI_PCI_OVERRIDE_GUID
#define gEfiS3SaveStateProtocolGuid  EFI_S3_SAVE_STATE_PROTOCOL_GUID
#define gEfiS3SmmSaveStateProtocolGuid  EFI_S3_SMM_SAVE_STATE_PROTOCOL_GUID
#define gEfiSmbiosProtocolGuid  EFI_SMBIOS_PROTOCOL_GUID
#define gEfiSpiConfigurationGuid  EFI_SPI_CONFIGURATION_GUID
#define gEfiSpiNorFlashProtocolGuid  EFI_SPI_NOR_FLASH_PROTOCOL_GUID
#define gEfiLegacySpiFlashProtocolGuid  EFI_LEGACY_SPI_FLASH_PROTOCOL_GUID
#define gEfiSpiHostGuid  EFI_SPI_HOST_GUID
#define gEfiSioProtocolGuid  EFI_SIO_PROTOCOL_GUID
#define gEfiSioPpiGuid  EFI_SIO_PPI_GUID
#define gEfiIsaHcPpiGuid  EFI_ISA_HC_PPI_GUID
#define gEfiIsaHcProtocolGuid  EFI_ISA_HC_PROTOCOL_GUID
#define gEfiIsaHcServiceBindingProtocolGuid  EFI_ISA_HC_SERVICE_BINDING_PROTOCOL_GUID
#define gEfiSioControlProtocolGuid  EFI_SIO_CONTROL_PROTOCOL_GUID

#endif /* AXL_UEFI_GEN_GUIDS_H */
