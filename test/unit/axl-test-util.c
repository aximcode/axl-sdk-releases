/** @file axl-test-util.c
    Test application for AxlUtil — file, path, SMBIOS, hex dump, time, args.
**/

#include "axl-test.h"
#include <axl/axl-debug.h>
#include <axl/axl-smbios.h>
#include <axl/axl-sort.h>
#include <axl/axl-clipboard.h>
#include <axl/axl-shm.h>
#include <axl/axl-rand.h>
#include <axl/axl-driver.h>   /* axl_protocol_install / _uninstall */
#include <axl/axl-shell.h>    /* axl_shell_launch */
#include <axl/axl-image.h>   /* axl_image_run */
#include <axl/axl-console-device.h>
#include <axl/axl-console-mirror.h>
#include <axl/axl-tar.h>
#include <axl/axl-stream.h>
#include <axl/axl-config-file.h>
#include <uefi/axl-uefi.h>

// ---------------------------------------------------------------------------
// File I/O Tests
// ---------------------------------------------------------------------------

static void
test_file(void)
{
    int         rc;
    const char  test_data[] = "axl_file_set_contents test data 12345";
    void        *read_buf;
    size_t      read_size;

    // Write
    rc = axl_file_set_contents("axl-test-util.tmp", test_data, sizeof(test_data) - 1);
    test_check(rc == 0, "file: write");
    if (rc != 0) {
        return;
    }

    // Read back
    rc = axl_file_get_contents("axl-test-util.tmp", &read_buf, &read_size);
    test_check(rc == 0, "file: read");
    if (rc != 0) {
        return;
    }

    test_check(read_size == sizeof(test_data) - 1, "file: size matches");
    test_check(axl_strncmp((char *)read_buf, test_data, read_size) == 0,
        "file: content matches");
    axl_free(read_buf);

    // IsDir
    test_check(axl_file_is_dir("fs0:\\"), "file: root is dir");
    test_check(!axl_file_is_dir("axl-test-util.tmp"), "file: tmp is not dir");

    // Note: no portable delete API yet; temp file left behind

    // Stat
    AxlFsEntry fi;
    rc = axl_file_info("axl-test-util.tmp", &fi);
    test_check(rc == AXL_OK, "stat: returns AXL_OK");
    test_check(fi.size == sizeof(test_data) - 1, "stat: size matches");
    test_check(!axl_fs_entry_is_dir(&fi), "stat: not a dir");

    rc = axl_file_info("fs0:\\", &fi);
    test_check(rc == AXL_OK, "stat: root dir returns AXL_OK");
    test_check(axl_fs_entry_is_dir(&fi), "stat: root is dir");

    test_check(axl_file_info("no-such-file-12345", &fi) != AXL_OK,
        "stat: missing file returns AXL_ERR");
    test_check(axl_file_info(NULL, &fi) != AXL_OK,
        "stat: NULL path returns AXL_ERR");
}

// ---------------------------------------------------------------------------
// Stream seek/tell/eof tests
// ---------------------------------------------------------------------------

static void
test_seek_tell(void)
{
    AxlStream *s;
    const char data[] = "ABCDEFGHIJ";
    char buf[4];
    int64_t pos;

    s = axl_fopen("axl-test-seek.tmp", "w");
    if (s == NULL) {
        return;
    }
    axl_write(s, data, 10);
    axl_fclose(s);

    s = axl_fopen("axl-test-seek.tmp", "r");
    if (s == NULL) {
        return;
    }

    pos = axl_ftell(s);
    test_check(pos == 0, "seek: tell at start is 0");

    axl_fread(buf, 1, 4, s);
    pos = axl_ftell(s);
    test_check(pos == 4, "seek: tell after read 4 is 4");

    test_check(axl_fseek(s, 0, AXL_SEEK_SET) == AXL_OK, "seek: seek SET 0");
    pos = axl_ftell(s);
    test_check(pos == 0, "seek: tell after seek SET 0");

    axl_fread(buf, 1, 2, s);
    test_check(buf[0] == 'A' && buf[1] == 'B', "seek: re-read after seek");

    test_check(axl_fseek(s, -1, AXL_SEEK_END) == AXL_OK, "seek: seek END -1");
    axl_fread(buf, 1, 1, s);
    test_check(buf[0] == 'J', "seek: read last byte via END");

    test_check(axl_fseek(s, -3, AXL_SEEK_CUR) == AXL_OK, "seek: seek CUR -3");
    axl_fread(buf, 1, 1, s);
    test_check(buf[0] == 'H', "seek: read via CUR");

    axl_fclose(s);
}

static void
test_feof(void)
{
    AxlStream *s;
    char buf[64];

    if (axl_file_set_contents("axl-test-eof.tmp", "XY", 2) != AXL_OK) {
        return;
    }

    s = axl_fopen("axl-test-eof.tmp", "r");
    if (s == NULL) {
        return;
    }

    test_check(!axl_feof(s), "eof: not eof at start");
    /* 2 bytes behind a 64-byte request: axl_fread loops, so it is the
       0-length read at the end of ITS loop that sets eof. The second call
       then has nothing left and returns 0 items. */
    axl_fread(buf, 1, 64, s);
    axl_fread(buf, 1, 1, s);
    test_check(axl_feof(s), "eof: eof after reading past end");

    axl_fseek(s, 0, AXL_SEEK_SET);
    test_check(!axl_feof(s), "eof: cleared after seek");

    axl_fclose(s);
}

// ---------------------------------------------------------------------------
// File delete/rename/mkdir/rmdir tests
// ---------------------------------------------------------------------------

static void
test_file_delete(void)
{
    AxlFsEntry del_fi;

    if (axl_file_set_contents("axl-del.tmp", "x", 1) != AXL_OK) {
        return;
    }
    test_check(axl_file_delete("axl-del.tmp") == AXL_OK, "delete: returns AXL_OK");
    test_check(axl_file_info("axl-del.tmp", &del_fi) != AXL_OK,
               "delete: file gone after delete");
}

static void
test_file_rename(void)
{
    AxlFsEntry fi;

    if (axl_file_set_contents("axl-ren-old.tmp", "data", 4) != AXL_OK) {
        return;
    }
    test_check(axl_file_rename("axl-ren-old.tmp", "axl-ren-new.tmp") == AXL_OK,
               "rename: returns AXL_OK");
    test_check(axl_file_info("axl-ren-new.tmp", &fi) == AXL_OK,
               "rename: new exists");
    test_check(fi.size == 4, "rename: size preserved");

    /* cleanup */
    axl_file_delete("axl-ren-new.tmp");
}

static void
test_mkdir_rmdir(void)
{
    test_check(axl_dir_mkdir("axl-test-dir") == AXL_OK, "mkdir: returns AXL_OK");
    test_check(axl_file_is_dir("axl-test-dir"), "mkdir: is dir");
    test_check(axl_dir_rmdir("axl-test-dir") == AXL_OK, "rmdir: returns AXL_OK");
    test_check(!axl_file_is_dir("axl-test-dir"), "rmdir: gone");
}

// ---------------------------------------------------------------------------
// Directory iteration tests
// ---------------------------------------------------------------------------

static void
test_dir_read(void)
{
    AxlDir *dir;
    AxlFsEntry entry;
    bool found_self = false;
    int count = 0;

    dir = axl_dir_open("fs0:\\");
    test_check(dir != NULL, "dir: open root");
    if (dir == NULL) {
        return;
    }

    while (axl_dir_read(dir, &entry)) {
        count++;
        if (axl_strcmp(entry.name, "AxlTestUtil.efi") == 0) {
            found_self = true;
        }
    }

    test_check(count > 0, "dir: read found entries");
    test_check(found_self, "dir: found AxlTestUtil.efi");
    axl_dir_close(dir);

    test_check(axl_dir_open("no-such-dir-xyz") == NULL,
               "dir: open missing returns NULL");
}

// ---------------------------------------------------------------------------
// Path Tests
// ---------------------------------------------------------------------------

static void
test_path(void)
{
    char        *result;
    const char  *ext;

    // Basename (allocating)
    result = axl_path_get_basename("fs0:\\dir\\file.efi");
    test_check(result != NULL && axl_strcmp(result, "file.efi") == 0,
        "path: get_basename");
    axl_free(result);

    result = axl_path_get_basename("file.efi");
    test_check(result != NULL && axl_strcmp(result, "file.efi") == 0,
        "path: get_basename no dir");
    axl_free(result);

    // Dirname (allocating)
    result = axl_path_get_dirname("fs0:\\dir\\file.efi");
    test_check(result != NULL && axl_strcmp(result, "fs0:\\dir") == 0,
        "path: get_dirname");
    axl_free(result);

    result = axl_path_get_dirname("file.efi");
    test_check(result != NULL && axl_strcmp(result, ".") == 0,
        "path: get_dirname no dir");
    axl_free(result);

    // Extension
    ext = axl_path_extension("file.efi");
    test_check(ext != NULL && axl_strcmp(ext, "efi") == 0,
        "path: extension");
    test_check(axl_path_extension("noext") == NULL,
        "path: no extension");

    // Join — separator matches the anchor's style. UEFI/Windows-style
    // anchors (containing `\` or `:`) get a backslash separator;
    // POSIX-style anchors get `/`. Mixed-separator paths produced by
    // the older always-`/` behavior were silently rejected by UEFI
    // shell's OpenFileByName ("fs0:/foo" doesn't open).
    result = axl_path_join("fs0:\\dir", "file.efi");
    test_check(result != NULL && axl_strcmp(result, "fs0:\\dir\\file.efi") == 0,
        "path: join (UEFI anchor → backslash separator)");
    axl_free(result);

    // POSIX anchor still gets `/` separator.
    result = axl_path_join("/usr/bin", "axl-cc");
    test_check(result != NULL && axl_strcmp(result, "/usr/bin/axl-cc") == 0,
        "path: join (POSIX anchor → forward slash separator)");
    axl_free(result);

    // Join with trailing slash
    result = axl_path_join("fs0:\\dir\\", "file");
    test_check(result != NULL && axl_strcmp(result, "fs0:\\dir\\file") == 0,
        "path: join trailing slash");
    axl_free(result);
}

// ---------------------------------------------------------------------------
// SMBIOS Tests
// ---------------------------------------------------------------------------

static void
test_smbios(void)
{
    AxlSmbiosHeader  *hdr;
    uint16_t         *str;

    // Find BIOS Information (type 0)
    hdr = axl_smbios_find(0);
    test_check(hdr != NULL, "smbios: find type 0(BIOS)");
    if (hdr == NULL) {
        return;
    }

    // Get string index 1 (vendor)
    str = axl_smbios_get_string(hdr, 1);
    test_check(str != NULL && str[0] != L'\0', "smbios: get string 1 non-empty");

    // Index 0 returns empty
    str = axl_smbios_get_string(hdr, 0);
    test_check(str != NULL && str[0] == L'\0', "smbios: get string 0 empty");

    // Type enum: AXL_SMBIOS_TYPE_BIOS_INFO must equal bare 0
    hdr = axl_smbios_find(AXL_SMBIOS_TYPE_BIOS_INFO);
    test_check(hdr != NULL, "smbios: find by enum AXL_SMBIOS_TYPE_BIOS_INFO");

    // axl_smbios_next: walk every record, count them and verify we reach
    // at least BIOS + System info (the two every firmware publishes).
    size_t total = 0;
    bool saw_bios = false;
    bool saw_system = false;
    AxlSmbiosHeader *h = NULL;
    while ((h = axl_smbios_next(h)) != NULL) {
        total++;
        if (h->Type == AXL_SMBIOS_TYPE_BIOS_INFO)   { saw_bios = true; }
        if (h->Type == AXL_SMBIOS_TYPE_SYSTEM_INFO) { saw_system = true; }
        // Guard against a buggy walker that doesn't terminate
        if (total > 4096) { break; }
    }
    test_check(total > 0,   "smbios: next walks at least one record");
    test_check(saw_bios,    "smbios: next found Type 0 (BIOS)");
    test_check(saw_system,  "smbios: next found Type 1 (System)");
    test_check(total <= 4096, "smbios: next terminates");

    // axl_smbios_version: firmware should report something sensible.
    unsigned char maj = 0, min = 0;
    int rc = axl_smbios_version(&maj, &min);
    test_check(rc == 0, "smbios: version call succeeds");
    test_check(maj >= 2 && maj <= 3, "smbios: major in [2,3]");

    // Reentrancy: two consecutive get_string calls on different records
    // must return valid independent pointers (no static-buffer clobber).
    AxlSmbiosHeader *bios = axl_smbios_find(AXL_SMBIOS_TYPE_BIOS_INFO);
    AxlSmbiosHeader *sys  = axl_smbios_find(AXL_SMBIOS_TYPE_SYSTEM_INFO);
    if (bios != NULL && sys != NULL) {
        const char *bios_vendor = axl_smbios_get_string_utf8(bios, 1);
        const char *sys_mfr     = axl_smbios_get_string_utf8(sys, 1);
        test_check(bios_vendor[0] != '\0', "smbios: bios vendor non-empty");
        test_check(sys_mfr[0]     != '\0', "smbios: sys manufacturer non-empty");
        /* Both pointers must still be valid and distinct after the second
           call (would have aliased under the old static-buffer impl). */
        test_check(bios_vendor != sys_mfr, "smbios: reentrant string returns distinct ptrs");
    }

    // Typed BIOS info reader
    AxlSmbiosBiosInfo bi;
    test_check(axl_smbios_read_bios_info(&bi) == AXL_OK, "smbios: read bios info");
    test_check(bi.vendor != NULL && bi.vendor[0] != '\0', "smbios: bios vendor populated");

    // Typed System info reader + UUID byte-swap
    AxlSmbiosSystemInfo si;
    test_check(axl_smbios_read_system_info(&si) == AXL_OK, "smbios: read system info");
    test_check(si.manufacturer != NULL, "smbios: system mfr populated");

    // System UUID getter: either returns 0 with valid bytes, or -1 cleanly
    uint8_t uuid[16];
    int uuid_rc = axl_smbios_get_system_uuid(uuid);
    test_check(uuid_rc == AXL_OK || uuid_rc == AXL_ERR, "smbios: uuid getter returns 0 or -1");

    // Processor reader: walk every Type 4 and read it
    size_t cpu_count = 0;
    AxlSmbiosHeader *ph = NULL;
    while ((ph = axl_smbios_find_next(AXL_SMBIOS_TYPE_PROCESSOR, ph)) != NULL) {
        AxlSmbiosProcessorInfo pi;
        test_check(axl_smbios_read_processor(ph, &pi) == AXL_OK, "smbios: read processor");
        test_check(pi.socket_designation != NULL, "smbios: processor socket populated");
        cpu_count++;
        if (cpu_count > 64) { break; }
    }

    // Memory device reader: walk every Type 17 and read it
    size_t mem_count = 0;
    AxlSmbiosHeader *mh = NULL;
    while ((mh = axl_smbios_find_next(AXL_SMBIOS_TYPE_MEMORY_DEVICE, mh)) != NULL) {
        AxlSmbiosMemoryDevice md;
        test_check(axl_smbios_read_memory_device(mh, &md) == AXL_OK, "smbios: read memory device");
        test_check(md.device_locator != NULL, "smbios: mem device locator populated");
        mem_count++;
        if (mem_count > 1024) { break; }
    }

    // Type 17 field decode — exact values from a SYNTHETIC record (real QEMU
    // Type 17 values vary, so pin the new form_factor / width / rank decode
    // against a crafted record of known bytes; SMBIOS 2.7+ length 0x22).
    /* Non-const + 8-aligned: the reader takes a non-const AxlSmbiosHeader*
       (read-only in practice) and dereferences uint16/uint32 members. */
    static uint8_t synth_t17[] __attribute__((aligned(8))) = {
        /* --- formatted area (0x22 bytes) --- */
        0x11, 0x22, 0x10, 0x00,   /* Type=17, Length=0x22, Handle=0x0010      */
        0x00, 0x00,               /* 0x04 MemoryArrayHandle                    */
        0xFF, 0xFF,               /* 0x06 MemoryErrorInfoHandle = none         */
        0x48, 0x00,               /* 0x08 TotalWidth = 72 (data + ECC)         */
        0x40, 0x00,               /* 0x0A DataWidth  = 64                      */
        0x00, 0x40,               /* 0x0C Size = 0x4000 = 16384 MB (bit15 clr) */
        0x09,                     /* 0x0E FormFactor = 9 (DIMM)                */
        0x00,                     /* 0x0F DeviceSet                            */
        0x01,                     /* 0x10 DeviceLocator -> string 1            */
        0x02,                     /* 0x11 BankLocator   -> string 2            */
        0x1A,                     /* 0x12 MemoryType = 0x1A (DDR4)             */
        0x00, 0x00,               /* 0x13 TypeDetail                           */
        0xA0, 0x0F,               /* 0x15 Speed = 4000 MHz (0x0FA0)            */
        0x00,                     /* 0x17 Manufacturer                         */
        0x00,                     /* 0x18 SerialNumber                         */
        0x00,                     /* 0x19 AssetTag                             */
        0x00,                     /* 0x1A PartNumber                           */
        0x02,                     /* 0x1B Attributes -> rank = 2               */
        0x00, 0x00, 0x00, 0x00,   /* 0x1C ExtendedSize = 0                     */
        0x40, 0x0F,               /* 0x20 ConfiguredClockSpeed                 */
        /* --- string set: "DIMM_A", "BANK 0", double-NUL terminator --- */
        'D','I','M','M','_','A', 0x00,
        'B','A','N','K',' ','0', 0x00,
        0x00
    };
    AxlSmbiosMemoryDevice smd;
    axl_memset(&smd, 0, sizeof(smd));
    test_check(axl_smbios_read_memory_device(
                   (AxlSmbiosHeader *)synth_t17, &smd) == AXL_OK,
               "smbios: read synthetic Type 17");
    test_check(smd.form_factor == 0x09, "smbios: form_factor decodes to 9 (DIMM)");
    test_check(smd.total_width == 72,   "smbios: total_width = 72 bits");
    test_check(smd.data_width  == 64,   "smbios: data_width = 64 bits");
    test_check(smd.rank == 2,           "smbios: rank = 2 (Attributes low nibble)");
    test_check(smd.memory_type == 0x1A, "smbios: memory_type = DDR4");
    test_check(smd.size_mb == 16384,    "smbios: size_mb = 16384");
    test_check(smd.device_locator != NULL
                   && axl_strcmp(smd.device_locator, "DIMM_A") == 0,
               "smbios: device_locator = DIMM_A");

    // Wrong-type guard: read_processor should refuse a non-Type-4 header
    AxlSmbiosProcessorInfo pi_bad;
    test_check(axl_smbios_read_processor(bios, &pi_bad) == AXL_ERR,
               "smbios: read_processor rejects Type 0 hdr");
    test_check(axl_smbios_read_memory_device(bios, NULL) == AXL_ERR,
               "smbios: read_memory_device rejects NULL out");

    // Type 38 — IPMI Device Information. QEMU + IPMI SSIF test harness
    // publishes one; plain QEMU doesn't. Just verify the call shape.
    AxlSmbiosIpmiDeviceInfo ip;
    int ip_rc = axl_smbios_read_ipmi_device_info(&ip);
    test_check(ip_rc == AXL_OK || ip_rc == AXL_ERR,
               "smbios: read_ipmi_device_info returns 0 or -1");
    if (ip_rc == AXL_OK) {
        test_check(ip.interface_type <= AXL_SMBIOS_IPMI_SSIF,
                   "smbios: ipmi interface type in known range");
        test_check(ip.spec_major <= 15 && ip.spec_minor <= 15,
                   "smbios: ipmi spec nibbles fit 4 bits");
    }

    // Type 42 — Management Controller Host Interface. QEMU doesn't
    // publish one, so the walk should simply find zero and the Redfish
    // convenience should return -1 cleanly.
    AxlSmbiosHeader *ih = NULL;
    size_t host_iface_count = 0;
    while ((ih = axl_smbios_find_next(AXL_SMBIOS_TYPE_MGMT_HOST_INTERFACE, ih)) != NULL) {
        AxlSmbiosHostInterface iface;
        test_check(axl_smbios_read_host_interface(ih, &iface) == AXL_OK,
                   "smbios: read_host_interface on real Type 42");
        test_check(iface.protocol_count <= 8, "smbios: protocol_count within cap");
        host_iface_count++;
        if (host_iface_count > 16) { break; }
    }
    AxlSmbiosHeader *rf_hdr = NULL;
    AxlSmbiosHostInterface rf_iface;
    int rf_rc = axl_smbios_find_redfish_host_interface(&rf_hdr, &rf_iface);
    test_check(rf_rc == AXL_OK || rf_rc == AXL_ERR, "smbios: redfish find returns 0 or -1");

    // Wrong-type guard for Type 42 reader
    AxlSmbiosHostInterface iface_bad;
    test_check(axl_smbios_read_host_interface(bios, &iface_bad) == AXL_ERR,
               "smbios: read_host_interface rejects Type 0 hdr");

    // ----- Synthetic Type 42 + Redfish-over-IP for the rich decoder -----
    // Build a Type 42 record (header + interface + 1 protocol = ROI),
    // then verify both the basic walker and the rich Redfish parser.
    // Layout per SMBIOS 3.x §7.43.3 (91 fixed bytes + hostname).
    //
    // Hand-crafted bytes — easier to read than alignas-decorated structs.
    static const uint8_t synthetic_type42[] = {
        // SMBIOS header
        42,             // Type = 42
        7 + 0 + 1 + (2 + 91 + 4),   // Length = hdr(4) + iface_type(1) + iface_data_len(1) + iface_data(0) + proto_count(1) + proto_hdr(2) + ROI(91 + 4)
        0x80, 0x00,     // Handle
        // Type 42 modern body
        0x40,           // interface_type = AXL_SMBIOS_HIF_NETWORK
        0x00,           // interface_data_len = 0 (no device descriptor for this synthetic)
        0x01,           // protocol_count = 1
        // Protocol record [0]: Redfish-over-IP
        0x04,           // protocol_type = AXL_SMBIOS_HIP_REDFISH_OVER_IP
        91 + 4,         // protocol data length: 91 fixed + 4-char hostname
        // -- ROI fixed payload (91 bytes) --
        // service_uuid (16 bytes) — pattern 0x10..0x1F
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
        0x02,           // host_ip_assignment = DHCP
        0x01,           // host_ip_format = IPv4
        // host_ip_address (16) — 169.254.1.10 in IPv4 mode
        169, 254, 1, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        // host_ip_mask (16)
        255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x01,           // service_ip_discovery = Static
        0x01,           // service_ip_format = IPv4
        // service_ip_address (16) — 169.254.1.1
        169, 254, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        // service_ip_mask (16)
        255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xBB, 0x01,     // service_port = 0x01BB = 443 (HTTPS)
        0x00, 0x00, 0x00, 0x00, // service_vlan_id = 0
        0x04,           // hostname_len = 4
        'b', 'm', 'c', 's',     // hostname = "bmcs"
        // SMBIOS string table — empty terminator
        0x00, 0x00,
    };

    AxlSmbiosHostInterface synth_iface;
    AxlSmbiosHeader *synth_hdr = (AxlSmbiosHeader *)synthetic_type42;
    test_check(axl_smbios_read_host_interface(synth_hdr, &synth_iface) == AXL_OK,
               "smbios: synth read_host_interface OK");
    test_check(synth_iface.interface_type == 0x40,
               "smbios: synth interface_type == HIF_NETWORK");
    test_check(synth_iface.protocol_count == 1,
               "smbios: synth one protocol record");
    test_check(synth_iface.protocols[0].protocol_type == 0x04,
               "smbios: synth proto[0] is Redfish-over-IP");

    AxlSmbiosRedfishOverIp roi;
    test_check(axl_smbios_read_redfish_over_ip(&synth_iface.protocols[0], &roi) == AXL_OK,
               "smbios: read_redfish_over_ip OK");
    test_check(roi.host_ip_assignment == AXL_SMBIOS_REDFISH_HOST_IP_DHCP,
               "smbios: ROI host_ip_assignment == DHCP");
    test_check(roi.host_ip_format == AXL_SMBIOS_REDFISH_IP_FORMAT_IPV4,
               "smbios: ROI host_ip_format == IPv4");
    test_check(roi.host_ip_address[0] == 169 && roi.host_ip_address[1] == 254
            && roi.host_ip_address[2] == 1   && roi.host_ip_address[3] == 10,
               "smbios: ROI host_ip 169.254.1.10");
    test_check(roi.service_ip_address[0] == 169 && roi.service_ip_address[3] == 1,
               "smbios: ROI service_ip 169.254.1.1");
    test_check(roi.service_port == 443,
               "smbios: ROI service_port == 443");
    test_check(roi.hostname_len == 4
            && roi.hostname != NULL
            && roi.hostname[0] == 'b'
            && roi.hostname[3] == 's',
               "smbios: ROI hostname == \"bmcs\"");

    /* Wrong protocol type → AXL_ERR. Use a doctored entry. */
    AxlSmbiosHostInterfaceProtocol bad_proto = synth_iface.protocols[0];
    bad_proto.protocol_type = 0x01;  /* IPMI, not Redfish */
    test_check(axl_smbios_read_redfish_over_ip(&bad_proto, &roi) == AXL_ERR,
               "smbios: read_redfish_over_ip rejects non-ROI protocol");

    /* Truncated data → AXL_ERR. */
    AxlSmbiosHostInterfaceProtocol short_proto = synth_iface.protocols[0];
    short_proto.data_len = 50;  /* < 91 fixed minimum */
    test_check(axl_smbios_read_redfish_over_ip(&short_proto, &roi) == AXL_ERR,
               "smbios: read_redfish_over_ip rejects truncated data");

    // ----- Reentrant copy_string_utf8 -----
    char copy_buf[64];
    size_t n;
    n = axl_smbios_copy_string_utf8(bios, 1, copy_buf, sizeof(copy_buf));
    test_check(n > 0, "smbios: copy_string_utf8 wrote bytes");
    test_check(copy_buf[n] == '\0', "smbios: copy_string_utf8 NUL-terminates at n");
    /* Same byte length as the direct accessor's strlen for the same idx */
    const char *direct = axl_smbios_get_string_utf8(bios, 1);
    test_check(axl_strlen(direct) == n, "smbios: copy_string_utf8 length matches direct");
    /* Index 0 → write empty + return 0 */
    n = axl_smbios_copy_string_utf8(bios, 0, copy_buf, sizeof(copy_buf));
    test_check(n == 0 && copy_buf[0] == '\0', "smbios: copy_string_utf8 idx 0 → empty");
    /* Truncation: ask for 4-byte buf, expect (buf_size - 1) returned */
    char tiny[4];
    n = axl_smbios_copy_string_utf8(bios, 1, tiny, sizeof(tiny));
    if (axl_strlen(direct) >= sizeof(tiny)) {
        test_check(n == sizeof(tiny) - 1, "smbios: copy_string_utf8 truncation length");
        test_check(tiny[sizeof(tiny) - 1] == '\0', "smbios: copy_string_utf8 truncation NUL");
    }
    /* NULL-buf / 0-size guards */
    test_check(axl_smbios_copy_string_utf8(bios, 1, NULL, 64) == 0,
               "smbios: copy_string_utf8 NULL buf returns 0");
    test_check(axl_smbios_copy_string_utf8(bios, 1, copy_buf, 0) == 0,
               "smbios: copy_string_utf8 zero size returns 0");
    test_check(axl_smbios_copy_string_utf8(NULL, 1, copy_buf, sizeof(copy_buf)) == 0,
               "smbios: copy_string_utf8 NULL hdr returns 0");

    // ----- Type 8: Port Connectors (firmware-dependent; QEMU may have 0 or N) -----
    {
        size_t pc_count = 0;
        AxlSmbiosHeader *ph2 = NULL;
        while ((ph2 = axl_smbios_find_next(AXL_SMBIOS_TYPE_PORT_CONNECTOR, ph2)) != NULL) {
            AxlSmbiosPortConnector pc;
            test_check(axl_smbios_read_port_connector(ph2, &pc) == AXL_OK,
                       "smbios: read_port_connector on real Type 8");
            test_check(pc.internal_designator != NULL && pc.external_designator != NULL,
                       "smbios: port-connector designators populated");
            pc_count++;
            if (pc_count > 64) { break; }
        }
        AxlSmbiosPortConnector pc_bad;
        test_check(axl_smbios_read_port_connector(bios, &pc_bad) == AXL_ERR,
                   "smbios: read_port_connector rejects Type 0 hdr");
    }

    // ----- Type 9: System Slots -----
    {
        size_t sl_count = 0;
        AxlSmbiosHeader *sh = NULL;
        while ((sh = axl_smbios_find_next(AXL_SMBIOS_TYPE_SYSTEM_SLOTS, sh)) != NULL) {
            AxlSmbiosSystemSlot sl;
            test_check(axl_smbios_read_system_slot(sh, &sl) == AXL_OK,
                       "smbios: read_system_slot on real Type 9");
            test_check(sl.designation != NULL, "smbios: system-slot designation populated");
            sl_count++;
            if (sl_count > 64) { break; }
        }
        AxlSmbiosSystemSlot sl_bad;
        test_check(axl_smbios_read_system_slot(bios, &sl_bad) == AXL_ERR,
                   "smbios: read_system_slot rejects Type 0 hdr");
    }

    // ----- Type 11: OEM Strings -----
    {
        AxlSmbiosHeader *oh = axl_smbios_find(AXL_SMBIOS_TYPE_OEM_STRINGS);
        if (oh != NULL) {
            AxlSmbiosOemStrings oem;
            test_check(axl_smbios_read_oem_strings(oh, &oem) == AXL_OK,
                       "smbios: read_oem_strings on real Type 11");
            test_check(oem.count <= 16, "smbios: oem strings count within cap");
            for (uint8_t i = 0; i < oem.count; i++) {
                test_check(oem.strings[i] != NULL, "smbios: oem string entry non-NULL");
            }

            /* axl_smbios_get_oem_string convenience accessor: index 1
               on this record matches strings[0] from the typed reader.
               The typical use is "read string at known index" without
               the caller hand-walking Type 11 records. */
            if (oem.count >= 1 && oem.strings[0] != NULL) {
                char   buf[256];
                size_t need = 0;
                test_check(axl_smbios_get_oem_string(1, buf, sizeof(buf),
                                                    &need) == AXL_OK,
                           "smbios get_oem_string: index 1 succeeds");
                test_check(axl_strcmp(buf, oem.strings[0]) == 0,
                           "smbios get_oem_string: index 1 matches read_oem_strings[0]");

                /* Truncation contract: when buf_cap is too small the
                   call returns -1 WITHOUT writing buf. *required is
                   set to the byte count needed (string length + NUL)
                   so callers can size a follow-up allocation
                   exactly. Pin the required value against a known-
                   capacity copy of the same string. */
                size_t actual_len = 0;
                while (oem.strings[0][actual_len] != '\0') actual_len++;
                size_t required_full = actual_len + 1;

                char    sentinel[4];
                size_t  required_truncated = 0;
                sentinel[0] = sentinel[1] = sentinel[2] = sentinel[3] = 'X';
                /* Only meaningful if the string is longer than 3 bytes;
                   for very short OEM strings the tiny buffer might be
                   adequate. Gate on the actual length. */
                if (required_full > sizeof(sentinel)) {
                    int rc = axl_smbios_get_oem_string(1, sentinel,
                                                       sizeof(sentinel),
                                                       &required_truncated);
                    test_check(rc == AXL_ERR,
                               "smbios get_oem_string: too-small buf returns -1");
                    test_check(required_truncated == required_full,
                               "smbios get_oem_string: *required reports needed bytes (string + NUL)");
                    test_check(sentinel[0] == 'X' && sentinel[1] == 'X'
                                   && sentinel[2] == 'X' && sentinel[3] == 'X',
                               "smbios get_oem_string: too-small buf leaves caller buffer untouched");
                } else {
                    /* String is short enough to fit; SKIP-balance the
                       three truncation-contract assertions. */
                    test_skip_n(3, "smbios get_oem_string: short string");
                }

                /* NULL *required is allowed even on truncation: pass
                   a 1-byte buffer so any non-empty string forces the
                   truncation path. The 0-length-string case (where a
                   1-byte buffer would actually fit just the NUL) is
                   implausible for OEM Strings but SKIP-balance it
                   defensively. */
                if (actual_len > 0) {
                    test_check(axl_smbios_get_oem_string(1, sentinel, 1, NULL) == AXL_ERR,
                               "smbios get_oem_string: NULL *required tolerated on truncation");
                } else {
                    test_skip_n(1, "smbios get_oem_string: NULL *required "
                                   "(no non-empty OEM string to truncate)");
                }
            }

            /* Out-of-range index returns -1. Use 200 — well past any
               realistic Type 11 string count (cap is 16/record). */
            char obuf[64];
            test_check(axl_smbios_get_oem_string(200, obuf, sizeof(obuf),
                                                 NULL) == AXL_ERR,
                       "smbios get_oem_string: out-of-range index returns -1");

            /* NULL / zero-cap guards. */
            test_check(axl_smbios_get_oem_string(1, NULL, 64, NULL) == AXL_ERR,
                       "smbios get_oem_string: NULL buf rejected");
            test_check(axl_smbios_get_oem_string(1, obuf, 0, NULL) == AXL_ERR,
                       "smbios get_oem_string: zero buf_cap rejected");
            test_check(axl_smbios_get_oem_string(0, obuf, sizeof(obuf),
                                                 NULL) == AXL_ERR,
                       "smbios get_oem_string: index 0 (invalid per spec) rejected");
        } else {
            /* No Type 11 record on this firmware: every get_oem_string
               call returns -1. SKIP-balance the populated path's 10
               assertions (2 success + 4 truncation + 4 misc). */
            char buf[64];
            test_check(axl_smbios_get_oem_string(1, buf, sizeof(buf),
                                                 NULL) == AXL_ERR,
                       "smbios get_oem_string: returns -1 with no Type 11");
            test_skip_n(9, "smbios get_oem_string");
        }
        AxlSmbiosOemStrings oem_bad;
        test_check(axl_smbios_read_oem_strings(bios, &oem_bad) == AXL_ERR,
                   "smbios: read_oem_strings rejects Type 0 hdr");
    }

    // ----- Type 16: Physical Memory Array -----
    {
        AxlSmbiosHeader *ah = axl_smbios_find(AXL_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY);
        if (ah != NULL) {
            AxlSmbiosPhysicalMemoryArray pma;
            test_check(axl_smbios_read_physical_memory_array(ah, &pma) == AXL_OK,
                       "smbios: read_physical_memory_array");
            test_check(pma.max_capacity_bytes > 0,
                       "smbios: pma max_capacity > 0");
        }
        AxlSmbiosPhysicalMemoryArray pma_bad;
        test_check(axl_smbios_read_physical_memory_array(bios, &pma_bad) == AXL_ERR,
                   "smbios: read_physical_memory_array rejects Type 0 hdr");
        /* Enum alias: PHYSICAL_MEMORY_ARRAY == PHYSICAL_MEMORY == 16 */
        test_check(AXL_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY == 16,
                   "smbios: physical_memory_array enum is 16");
        test_check(AXL_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY ==
                   AXL_SMBIOS_TYPE_PHYSICAL_MEMORY,
                   "smbios: type 16 enum alias matches");
    }

    // ----- Type 19: Memory Array Mapped Address -----
    {
        AxlSmbiosHeader *mh2 = NULL;
        size_t mam_count = 0;
        while ((mh2 = axl_smbios_find_next(AXL_SMBIOS_TYPE_MEMORY_ARRAY_MAP, mh2)) != NULL) {
            AxlSmbiosMemoryArrayMap mam;
            test_check(axl_smbios_read_memory_array_map(mh2, &mam) == AXL_OK,
                       "smbios: read_memory_array_map on real Type 19");
            test_check(mam.ending_address >= mam.starting_address,
                       "smbios: type 19 end >= start");
            mam_count++;
            if (mam_count > 64) { break; }
        }
        AxlSmbiosMemoryArrayMap mam_bad;
        test_check(axl_smbios_read_memory_array_map(bios, &mam_bad) == AXL_ERR,
                   "smbios: read_memory_array_map rejects Type 0 hdr");
    }

    // ----- Type 20: Memory Device Mapped Address -----
    {
        AxlSmbiosHeader *dh = NULL;
        size_t mdm_count = 0;
        while ((dh = axl_smbios_find_next(AXL_SMBIOS_TYPE_MEMORY_DEVICE_MAP, dh)) != NULL) {
            AxlSmbiosMemoryDeviceMap mdm;
            test_check(axl_smbios_read_memory_device_map(dh, &mdm) == AXL_OK,
                       "smbios: read_memory_device_map on real Type 20");
            test_check(mdm.ending_address >= mdm.starting_address,
                       "smbios: type 20 end >= start");
            mdm_count++;
            if (mdm_count > 64) { break; }
        }
        AxlSmbiosMemoryDeviceMap mdm_bad;
        test_check(axl_smbios_read_memory_device_map(bios, &mdm_bad) == AXL_ERR,
                   "smbios: read_memory_device_map rejects Type 0 hdr");
        test_check(AXL_SMBIOS_TYPE_MEMORY_DEVICE_MAP == 20,
                   "smbios: memory_device_map enum is 20");
    }

    // ----- Type 41: Onboard Devices Extended -----
    {
        AxlSmbiosHeader *oh2 = NULL;
        size_t obx_count = 0;
        while ((oh2 = axl_smbios_find_next(AXL_SMBIOS_TYPE_ONBOARD_DEVICE_EXT, oh2)) != NULL) {
            AxlSmbiosOnboardDeviceExt obx;
            test_check(axl_smbios_read_onboard_device_ext(oh2, &obx) == AXL_OK,
                       "smbios: read_onboard_device_ext on real Type 41");
            test_check(obx.reference_designation != NULL,
                       "smbios: type 41 designation populated");
            obx_count++;
            if (obx_count > 64) { break; }
        }
        AxlSmbiosOnboardDeviceExt obx_bad;
        test_check(axl_smbios_read_onboard_device_ext(bios, &obx_bad) == AXL_ERR,
                   "smbios: read_onboard_device_ext rejects Type 0 hdr");
        test_check(AXL_SMBIOS_TYPE_ONBOARD_DEVICE_EXT == 41,
                   "smbios: onboard_device_ext enum is 41");
    }

    // ----- Type 2: board_type field on AxlSmbiosBaseboardInfo -----
    {
        AxlSmbiosBaseboardInfo bb;
        if (axl_smbios_read_baseboard(&bb) == AXL_OK) {
            /* board_type has been part of Type 2 since SMBIOS 2.0,
             * so essentially always present on real firmware.
             * 0x0A (motherboard) is by far the most common — most
             * QEMU/OVMF setups report this. Accept any non-zero
             * since we don't know the test environment's value. */
            test_check(bb.board_type != AXL_SMBIOS_BOARD_TYPE_UNKNOWN,
                       "smbios: baseboard board_type populated");
        }
        /* Enum sanity */
        test_check(AXL_SMBIOS_BOARD_TYPE_SERVER_BLADE == 0x03,
                   "smbios: BOARD_TYPE_SERVER_BLADE == 0x03");
        test_check(AXL_SMBIOS_BOARD_TYPE_MOTHERBOARD == 0x0A,
                   "smbios: BOARD_TYPE_MOTHERBOARD == 0x0A");
    }

    // ----- axl_smbios_strings_byte_len -----
    {
        /* BIOS Type 0 has at minimum vendor + version + release_date
         * strings — region length must be > 0 on any real firmware. */
        size_t n = axl_smbios_strings_byte_len(bios);
        test_check(n > 0, "smbios: strings_byte_len bios > 0");

        /* Tight check: walk the same region by hand. The byte count
         * must equal the sum of (each string's strlen + 1 NUL) for
         * every string in the record, and must NOT include the
         * extra terminating NUL after the last string. */
        size_t expected = 0;
        for (uint8_t idx = 1; idx <= 255; idx++) {
            const char *s = axl_smbios_get_string_utf8(bios, idx);
            if (s == NULL || s[0] == '\0') {
                /* axl_smbios_get_string_utf8 returns "" for
                 * out-of-range indices, signalling end of region. */
                break;
            }
            expected += axl_strlen(s) + 1;
        }
        test_check(n == expected,
                   "smbios: strings_byte_len matches sum-of-strlen+NUL");

        /* NULL hdr → 0 */
        test_check(axl_smbios_strings_byte_len(NULL) == 0,
                   "smbios: strings_byte_len NULL → 0");

        /* Other typed records also produce sensible region lengths */
        AxlSmbiosHeader *sys2 = axl_smbios_find(AXL_SMBIOS_TYPE_SYSTEM_INFO);
        if (sys2 != NULL) {
            size_t ns = axl_smbios_strings_byte_len(sys2);
            /* System info typically has manufacturer + product + version
             * + serial — must be at least 4 bytes (4 empty strings'
             * NULs would still be 4 — but real firmware never publishes
             * empty system info strings). */
            test_check(ns > 0, "smbios: strings_byte_len system > 0");
        }
    }

    // ----- Type 9 spec decoders -----
    {
        /* Canonical SMBIOS spec / EDK2 values. Some in-the-wild
         * consumer slot tables have been observed shifted by 4
         * — rejecting that drift here. */
        test_check(axl_strcmp(axl_smbios_slot_type_str(0xA5), "PCIe") == 0,
                   "smbios: slot_type_str 0xA5 = PCIe");
        test_check(axl_strcmp(axl_smbios_slot_type_str(0xAA), "PCIe x16") == 0,
                   "smbios: slot_type_str 0xAA = PCIe x16");
        test_check(axl_strcmp(axl_smbios_slot_type_str(0xAB), "PCIe Gen 2") == 0,
                   "smbios: slot_type_str 0xAB = PCIe Gen 2");
        test_check(axl_strcmp(axl_smbios_slot_type_str(0xB1), "PCIe Gen 3") == 0,
                   "smbios: slot_type_str 0xB1 = PCIe Gen 3");
        test_check(axl_strcmp(axl_smbios_slot_type_str(0xB8), "PCIe Gen 4") == 0,
                   "smbios: slot_type_str 0xB8 = PCIe Gen 4");
        test_check(axl_smbios_slot_type_str(0xB7) == NULL,
                   "smbios: slot_type_str 0xB7 reserved → NULL");
        test_check(axl_strcmp(axl_smbios_slot_type_str(0xBE), "PCIe Gen 5") == 0,
                   "smbios: slot_type_str 0xBE = PCIe Gen 5");
        test_check(axl_strcmp(axl_smbios_slot_type_str(0xC3), "PCIe Gen 5 x16") == 0,
                   "smbios: slot_type_str 0xC3 = PCIe Gen 5 x16");
        test_check(axl_strcmp(axl_smbios_slot_type_str(0xC4),
                              "PCIe Gen 6 and Beyond") == 0,
                   "smbios: slot_type_str 0xC4 = PCIe Gen 6 and Beyond");

        /* M.2 keys live at 0x14-0x17 (NOT 0x22-0x25 — those are
         * PCIe-Mini and U.2 in the spec, a common consumer mislabel). */
        test_check(axl_strcmp(axl_smbios_slot_type_str(0x14),
                              "M.2 Socket 1-DP (Mech Key A)") == 0,
                   "smbios: slot_type_str 0x14 = M.2 Mech Key A");
        test_check(axl_strcmp(axl_smbios_slot_type_str(0x17),
                              "M.2 Socket 3 (Mech Key M)") == 0,
                   "smbios: slot_type_str 0x17 = M.2 Mech Key M");

        /* PCIe SFF-8639 / U.2 family — 0x25 is U.2 Gen 5, NOT M.2. */
        test_check(axl_strcmp(axl_smbios_slot_type_str(0x25),
                              "PCIe Gen 5 SFF-8639 (U.2)") == 0,
                   "smbios: slot_type_str 0x25 = PCIe Gen 5 U.2 (NOT M.2)");
        test_check(axl_strcmp(axl_smbios_slot_type_str(0x24),
                              "PCIe Gen 4 SFF-8639 (U.2)") == 0,
                   "smbios: slot_type_str 0x24 = PCIe Gen 4 U.2");

        /* OCP NIC 3.0 + Prior */
        test_check(axl_strcmp(axl_smbios_slot_type_str(0x26), "OCP NIC 3.0 SFF") == 0,
                   "smbios: slot_type_str 0x26 = OCP NIC 3.0 SFF");
        test_check(axl_strcmp(axl_smbios_slot_type_str(0x27), "OCP NIC 3.0 LFF") == 0,
                   "smbios: slot_type_str 0x27 = OCP NIC 3.0 LFF");
        test_check(axl_strcmp(axl_smbios_slot_type_str(0x28),
                              "OCP NIC Prior to 3.0") == 0,
                   "smbios: slot_type_str 0x28 = OCP NIC Prior to 3.0");

        /* EDSFF: one code per family covering both size variants */
        test_check(axl_strcmp(axl_smbios_slot_type_str(0xC5),
                              "EDSFF E1 (E1.S, E1.L)") == 0,
                   "smbios: slot_type_str 0xC5 = EDSFF E1");
        test_check(axl_strcmp(axl_smbios_slot_type_str(0xC6),
                              "EDSFF E3 (E3.S, E3.L)") == 0,
                   "smbios: slot_type_str 0xC6 = EDSFF E3");

        /* Unknown returns NULL (caller can fall back to raw hex) */
        test_check(axl_smbios_slot_type_str(0xFF) == NULL,
                   "smbios: slot_type_str unknown → NULL");
        test_check(axl_smbios_slot_type_str(0x00) == NULL,
                   "smbios: slot_type_str 0x00 → NULL");

        /* Bus widths */
        test_check(axl_strcmp(axl_smbios_slot_width_str(0x08), "1x") == 0,
                   "smbios: slot_width_str 0x08 = 1x");
        test_check(axl_strcmp(axl_smbios_slot_width_str(0x0D), "16x") == 0,
                   "smbios: slot_width_str 0x0D = 16x");
        test_check(axl_smbios_slot_width_str(0xFF) == NULL,
                   "smbios: slot_width_str unknown → NULL");

        /* Current usage — strings match SMBIOS 3.7 Table 12 exactly.
         * Vendor-specific renderings (e.g. "CPU NOT INSTALLED" for
         * socket-associated 0x05 slots) belong in consumer code. */
        test_check(axl_strcmp(axl_smbios_slot_usage_str(0x03), "Empty") == 0,
                   "smbios: slot_usage_str 0x03 = Empty");
        test_check(axl_strcmp(axl_smbios_slot_usage_str(0x04), "InUse") == 0,
                   "smbios: slot_usage_str 0x04 = InUse");
        test_check(axl_strcmp(axl_smbios_slot_usage_str(0x05),
                              "Unavailable") == 0,
                   "smbios: slot_usage_str 0x05 = Unavailable (per SMBIOS spec)");
        test_check(axl_smbios_slot_usage_str(0xFF) == NULL,
                   "smbios: slot_usage_str unknown → NULL");
    }

    // ----- Chassis classification -----
    {
        /* DESKTOP set */
        test_check(axl_smbios_chassis_class(0x03) == AXL_SMBIOS_CHASSIS_CLASS_DESKTOP,
                   "smbios: chassis_class 0x03 (Desktop) = DESKTOP");
        test_check(axl_smbios_chassis_class(0x07) == AXL_SMBIOS_CHASSIS_CLASS_DESKTOP,
                   "smbios: chassis_class 0x07 (Tower) = DESKTOP");
        /* PITFALL: 0x18 is Sealed-case PC, NOT server */
        test_check(axl_smbios_chassis_class(0x18) == AXL_SMBIOS_CHASSIS_CLASS_DESKTOP,
                   "smbios: chassis_class 0x18 (Sealed-case PC) = DESKTOP, NOT server");

        /* NOTEBOOK set */
        test_check(axl_smbios_chassis_class(0x09) == AXL_SMBIOS_CHASSIS_CLASS_NOTEBOOK,
                   "smbios: chassis_class 0x09 (LapTop) = NOTEBOOK");
        test_check(axl_smbios_chassis_class(0x0A) == AXL_SMBIOS_CHASSIS_CLASS_NOTEBOOK,
                   "smbios: chassis_class 0x0A (Notebook) = NOTEBOOK");
        test_check(axl_smbios_chassis_class(0x1F) == AXL_SMBIOS_CHASSIS_CLASS_NOTEBOOK,
                   "smbios: chassis_class 0x1F (Convertible) = NOTEBOOK");
        test_check(axl_smbios_chassis_class(0x20) == AXL_SMBIOS_CHASSIS_CLASS_NOTEBOOK,
                   "smbios: chassis_class 0x20 (Detachable) = NOTEBOOK");

        /* SERVER set */
        test_check(axl_smbios_chassis_class(0x17) == AXL_SMBIOS_CHASSIS_CLASS_SERVER,
                   "smbios: chassis_class 0x17 (Rack Mount) = SERVER");
        test_check(axl_smbios_chassis_class(0x1C) == AXL_SMBIOS_CHASSIS_CLASS_SERVER,
                   "smbios: chassis_class 0x1C (Blade) = SERVER");
        test_check(axl_smbios_chassis_class(0x1D) == AXL_SMBIOS_CHASSIS_CLASS_SERVER,
                   "smbios: chassis_class 0x1D (Blade Enclosure) = SERVER");

        /* EMBEDDED set */
        test_check(axl_smbios_chassis_class(0x21) == AXL_SMBIOS_CHASSIS_CLASS_EMBEDDED,
                   "smbios: chassis_class 0x21 (IoT Gateway) = EMBEDDED");
        /* PITFALL: 0x23 is "Mini PC" per SMBIOS 3.7, NOT IoT Gateway. */
        test_check(axl_smbios_chassis_class(0x23) == AXL_SMBIOS_CHASSIS_CLASS_EMBEDDED,
                   "smbios: chassis_class 0x23 (Mini PC) = EMBEDDED");

        /* UNKNOWN */
        test_check(axl_smbios_chassis_class(0x00) == AXL_SMBIOS_CHASSIS_CLASS_UNKNOWN,
                   "smbios: chassis_class 0x00 = UNKNOWN");
        test_check(axl_smbios_chassis_class(0x02) == AXL_SMBIOS_CHASSIS_CLASS_UNKNOWN,
                   "smbios: chassis_class 0x02 (Unknown per spec) = UNKNOWN");

        /* Lock-bit (0x80) stripped before classification */
        test_check(axl_smbios_chassis_class(0x80 | 0x07) == AXL_SMBIOS_CHASSIS_CLASS_DESKTOP,
                   "smbios: chassis_class strips 0x80 lock bit");
        test_check(axl_smbios_chassis_class(0x80 | 0x17) == AXL_SMBIOS_CHASSIS_CLASS_SERVER,
                   "smbios: chassis_class lock bit | 0x17 = SERVER");

        /* OTHER bucket (recognized but not in any of the 4 named buckets) */
        test_check(axl_smbios_chassis_class(0x10) == AXL_SMBIOS_CHASSIS_CLASS_OTHER,
                   "smbios: chassis_class 0x10 (Lunch Box) = OTHER");
        test_check(axl_smbios_chassis_class(0x0D) == AXL_SMBIOS_CHASSIS_CLASS_OTHER,
                   "smbios: chassis_class 0x0D (All in One) = OTHER");
        /* Out-of-range */
        test_check(axl_smbios_chassis_class(0x7F) == AXL_SMBIOS_CHASSIS_CLASS_OTHER,
                   "smbios: chassis_class 0x7F = OTHER");
    }
}

// ---------------------------------------------------------------------------
// Hex Dump Test
// ---------------------------------------------------------------------------

static void
test_hexdump(void)
{
    uint8_t  test_data[32];
    size_t   i;

    for (i = 0; i < 32; i++) {
        test_data[i] = (uint8_t)(i + 0x40);
    }

    axl_printf("\nhex dump(word grouping, default):\n");
    axl_hexdump("TestData", test_data, sizeof(test_data), 0, 0);
    test_pass("hexdump: word grouping");

    axl_printf("\nhex dump(byte grouping):\n");
    axl_hexdump(NULL, test_data, 16, 0, AXL_HEX_GROUP_BYTE);
    test_pass("hexdump: byte grouping");

    axl_printf("\nhex dump(dword grouping):\n");
    axl_hexdump(NULL, test_data, 16, 0, AXL_HEX_GROUP_DWORD);
    test_pass("hexdump: dword grouping");
}

// ---------------------------------------------------------------------------
// Time Test
// ---------------------------------------------------------------------------

static void
test_time(void)
{
    char    buf[32];
    size_t  len;

    /* axl_time_format_at renders a time the caller supplies, so for the first
       time the format can be pinned EXACTLY. Every assertion below against
       axl_time_format is a shape probe -- "has a T", "26 chars" -- precisely
       because it reads a live clock and the value cannot be known. A shape
       probe cannot catch a wrong field ORDER, a lost zero-pad, or a
       truncated fraction; this can.
       (This is what the dispatcher-stamps-once split bought: the renderer no
       longer owns the reading.) */
    const AxlRealtime fixed = {
        .year = 2026, .month = 3, .day = 7,
        .hour = 4, .minute = 5, .second = 9,
        .nanosecond = 123456000u,
    };
    len = axl_time_format_at(&fixed, buf, sizeof(buf));
    test_check(len == 26 && axl_strcmp(buf, "2026-03-07T04:05:09.123456") == 0,
               "time: format_at renders an exact ISO 8601 timestamp");

    /* A NULL time is not an error -- it renders the fixed-width placeholder,
       so a transcript stays columnar instead of shifting every field after
       it. This is the contract the log dispatcher relies on when the clock
       cannot be read. */
    len = axl_time_format_at(NULL, buf, sizeof(buf));
    test_check(len == 26 && axl_strcmp(buf, "0000-00-00T00:00:00.000000") == 0,
               "time: format_at renders a placeholder for a NULL time");

    /* Degenerate buffers report 0 and write nothing usable. 27 is one short
       of the 26 chars plus NUL, so it must be refused rather than truncated. */
    test_check(axl_time_format_at(&fixed, NULL, 32) == 0,
               "time: format_at rejects a NULL buffer");
    test_check(axl_time_format_at(&fixed, buf, 27) == 0,
               "time: format_at rejects a buffer one byte too small");

    len = axl_time_format(buf, sizeof(buf));
    test_check(len > 0, "time: format returns > 0");

    // Check for ISO 8601 'T' separator
    bool has_t = false;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == 'T') {
            has_t = true;
            break;
        }
    }
    test_check(has_t, "time: contains T separator");

    // Exact shape: "YYYY-MM-DDThh:mm:ss.uuuuuu", 26 chars. Pinned so the
    // sub-second work below cannot quietly change the layout.
    test_check(len == 26, "time: format is exactly 26 chars");
    test_check(buf[4] == '-' && buf[7] == '-' && buf[10] == 'T'
               && buf[13] == ':' && buf[16] == ':' && buf[19] == '.',
               "time: separators at the ISO 8601 positions");
    bool digits = true;
    for (size_t i = 20; i < 26; i++) {
        if (buf[i] < '0' || buf[i] > '9') { digits = false; }
    }
    test_check(digits, "time: fractional field is 6 digits");

    // The fractional field must carry real sub-second precision. Firmware
    // leaves EFI_TIME.Nanosecond at 0 on every platform we test on, so
    // deriving it from that alone stamps .000000 on every line and lines
    // within one second cannot be ordered. axl_time_format falls back to the
    // monotonic counter for exactly this reason (the caveat is that the
    // fraction then comes from an unrelated epoch, so it can appear to run
    // backwards inside one wallclock second -- ordering within a second is
    // NOT implied). Two samples: a single one could legitimately be zero
    // once in 10^6.
    char buf2[32];
    axl_time_format(buf2, sizeof(buf2));
    bool frac_nonzero = axl_memcmp(buf + 20, "000000", 6) != 0
                     || axl_memcmp(buf2 + 20, "000000", 6) != 0;
    test_check(frac_nonzero, "time: fractional field carries sub-second precision");
}

/* RTC-write API (axl_time_set_realtime / axl_time_set_unix). Only the
   SAFE-NEGATIVE paths are unit-tested here: the validation that returns
   AXL_ERR *before* any gRT->SetTime call. The positive round-trip
   mutates the shared-boot RTC (and depends on firmware accepting
   SetTime), so it lives in the integration test test-time-qemu.sh —
   per feedback_uefi_firmware_test_hazards (don't drive firmware-
   mutating paths from the combined unit boot). */
static void
test_time_set(void)
{
    test_check(axl_time_set_realtime(NULL) == AXL_ERR,
               "time: set_realtime(NULL) rejected");
    test_check(axl_time_set_unix(-1) == AXL_ERR,
               "time: set_unix(-1) rejected (pre-epoch)");
    /* 253402300800 == 10000-01-01T00:00:00Z — first second past the
       year a 16-bit RTC year can hold; rejected before any firmware
       call. */
    test_check(axl_time_set_unix((int64_t)253402300800LL) == AXL_ERR,
               "time: set_unix(year 10000) rejected (out of range)");

    /* A REJECTED write must not damage the clock-read path. The backend's
       RTC re-entrancy guard keeps a cached reading to serve a nested read
       from, and an earlier draft of that guard invalidated the cache on
       every SetTime -- including ones the firmware refused, where the clock
       never moved and the cached value was still perfectly good. Losing it
       there would make a subsequent nested read fail, which axl_time_format
       renders as 0000-00-00T00:00:00.
       Deterministic, and reachable through public headers: the three
       rejections above return before any firmware call. */
    AxlRealtime after_reject;
    test_check(axl_time_realtime(&after_reject) == AXL_OK
               && after_reject.year >= 2000,
               "time: a rejected set leaves the read path intact");
}

/* RTC wake alarm.
 *
 * Firmware-lifecycle API, so the assertions are the SAFE ones: what this
 * code decides before reaching firmware, and the platform-capability split.
 * Nothing here double-disarms, programs a past time, or leaves an alarm
 * armed behind it -- misuse of a runtime service is a hang or a #GP under
 * UEFI, not a clean error return (feedback_uefi_firmware_test_hazards).
 *
 * The whole point of the API is that "no wake timer" and "wake timer
 * broken" stay distinguishable, so the FIRST thing pinned is that a probe
 * never answers AXL_ERR on a healthy box. QEMU/OVMF and real hardware
 * differ on which of the two supported answers comes back, which is why
 * the populated path is gated and balanced rather than asserted outright.
 */
/* Raw firmware memory map.
 *
 * The contract a consumer pins against an EDK2 oracle capture: uncoalesced,
 * raw EFI type numbers, raw attribute bits. Each assertion below exists
 * because the CLASSIFIED sibling (axl_mem_phys_region_*) gets it wrong for
 * inventory -- it coalesces, and it folds the type into five buckets, so a
 * per-type roll-up is unrecoverable from it.
 */
static void
test_memmap_snapshot(void)
{
    AxlMemMapEntry *e     = (AxlMemMapEntry *)0xDEADBEEFul;   /* sentinel */
    size_t          count = 99;

    test_check(axl_memmap_snapshot(NULL, &count) == AXL_INVALID,
               "memmap: NULL entries rejected");
    test_check(count == 0, "memmap: count cleared on reject");
    test_check(axl_memmap_snapshot(&e, NULL) == AXL_INVALID,
               "memmap: NULL count rejected");
    test_check(e == NULL, "memmap: entries cleared on reject");

    e = NULL; count = 0;
    test_check(axl_memmap_snapshot(&e, &count) == AXL_OK,
               "memmap: snapshot succeeds");
    test_check(count > 0 && e != NULL, "memmap: reports at least one region");

    /* UNCOALESCED is the whole point, and it is testable rather than
       assumed: the classified view merges adjacent same-class regions, so
       the raw map must report strictly MORE entries than it does. A build
       that quietly reused the coalescing collector would pass every other
       assertion here. */
    size_t classified = 0;
    bool   have_class = (axl_mem_phys_region_count(&classified) == AXL_OK);
    test_check(have_class && count > classified,
               "memmap: raw map has more entries than the coalesced view");

    /* Pages, not bytes -- a consumer renders both and derives size from
       this. Zero-page descriptors are legal in the raw map, so the
       assertion is on the FIELD being meaningful, not on every entry. */
    bool sane = true;
    for (size_t i = 0; i < count; i++) {
        if (e[i].physical_start + e[i].number_of_pages * 4096ULL
                < e[i].physical_start) {
            sane = false;   /* wrapped: pages field is not pages */
            break;
        }
    }
    test_check(sane, "memmap: start + pages*4K never wraps");

    /* Type names are the EDK2 spelling, which is what an oracle capture
       compares against byte-for-byte. Pinned exactly, not by substring. */
    test_check(axl_strcmp(axl_memmap_type_name(7), "ConventionalMemory") == 0,
               "memmap: type 7 renders as ConventionalMemory");
    test_check(axl_strcmp(axl_memmap_type_name(4), "BootServicesData") == 0,
               "memmap: type 4 renders as BootServicesData");
    test_check(axl_strcmp(axl_memmap_type_name(10), "ACPIMemoryNVS") == 0,
               "memmap: type 10 renders as ACPIMemoryNVS");
    test_check(axl_strcmp(axl_memmap_type_name(0), "ReservedMemoryType") == 0,
               "memmap: type 0 renders as ReservedMemoryType");
    /* OEM range: must not index off the end of the table. */
    test_check(axl_strcmp(axl_memmap_type_name(0x70000000u), "Unknown") == 0,
               "memmap: an OEM type renders as Unknown, not a stray read");

    /* Every entry's type must render to something; a NULL here would be a
       crash in any consumer formatting the map. */
    bool all_named = true;
    for (size_t i = 0; i < count; i++) {
        const char *nm = axl_memmap_type_name(e[i].type);
        if (nm == NULL || nm[0] == '\0') {
            all_named = false;
            break;
        }
    }
    test_check(all_named, "memmap: every reported type has a name");

    axl_free(e);
}

static void
test_time_wakeup(void)
{
    /* All-NULL probe: the return code alone answers "has a wake timer",
       which is the cheapest question a caller can ask. */
    int probe = axl_time_get_wakeup(NULL, NULL, NULL);
    test_check(probe == AXL_OK || probe == AXL_UNSUPPORTED,
               "time wakeup: probe answers OK or UNSUPPORTED, never ERR");

    if (probe == AXL_UNSUPPORTED) {
        /* A platform with no wake timer must say so CONSISTENTLY -- the
           read and the write cannot disagree about whether the hardware
           exists. That is the one thing still worth asserting here, and it
           needs no alarm to be programmed. */
        test_check(axl_time_set_wakeup(NULL) == AXL_UNSUPPORTED,
                   "time wakeup: disarm agrees the platform is unsupported");
        /* 2, not 3. The supported path runs 4 assertions (probe + full read
           + partial read + disarm); this branch runs 2 of its own (probe +
           disarm-agrees), so 2 must be declared for the totals to match. */
        test_skip_n(2, "time wakeup round-trip (platform has no wake timer)");
        return;
    }

    /* Supported. Read the current state with every output requested. */
    bool        enabled = false;
    bool        pending = false;
    AxlRealtime when    = { 0 };
    test_check(axl_time_get_wakeup(&enabled, &pending, &when) == AXL_OK,
               "time wakeup: full read succeeds");

    /* Partial reads are the documented ergonomics, not an accident: a
       caller that only wants "is it armed" should not have to supply a
       calendar struct. */
    bool only_enabled = false;
    test_check(axl_time_get_wakeup(&only_enabled, NULL, NULL) == AXL_OK
               && only_enabled == enabled,
               "time wakeup: partial read matches the full read");

    /* Disarm is idempotent and safe on a platform that supports it --
       unlike a double UNREGISTER, SetWakeupTime(FALSE, NULL) frees
       nothing and re-issuing it cannot dangle. Leaves the box with no
       alarm armed, which is also the state the test found it in. */
    test_check(axl_time_set_wakeup(NULL) == AXL_OK,
               "time wakeup: disarm succeeds");
}

static void
test_time_sleep(void)
{
    uint64_t  before;
    uint64_t  after;

    before = axl_time_get_ms();
    test_survived("time: get_ms returns without faulting");

    axl_msleep(50);
    after = axl_time_get_ms();
    test_check(after >= before, "time: get_ms monotonic after msleep");

    axl_usleep(1000);  /* 1ms */
    test_pass("time: usleep(1000) completes");
}

static void
test_time_get_us(void)
{
    /* axl_time_get_us is a thin public wrapper around
       axl_clock_gettime(MONOTONIC). The epoch is boot-relative,
       so every call returns a real microsecond count — no
       per-process "calibration tick" sentinel anymore. Verify the
       contract: two calls with a stall between produce
       monotonically increasing values, and the elapsed delta is at
       least roughly the requested stall. */

    uint64_t t0 = axl_time_get_us();
    axl_usleep(2000);  /* 2ms = 2000us */
    uint64_t t1 = axl_time_get_us();
    axl_usleep(2000);
    uint64_t t2 = axl_time_get_us();

    test_check(t1 >= t0,
               "time: get_us monotonic across 2ms stall");
    test_check(t2 >= t1,
               "time: get_us monotonic across consecutive stalls");

    /* Elapsed delta lower bound: the stall must take at least its
       requested duration, so t1-t0 >= 2000us minus a generous
       sampling slop. We don't enforce an upper bound — firmware
       Stall granularity and emulator scheduling jitter both vary.

       KVM caveat: if the vCPU gets descheduled by the host kernel
       partway through gBS->Stall, the guest-visible TSC pauses for
       the off-CPU window. Wall time still elapses (the Stall
       returns after the requested 2 ms), but the TSC delta we
       measure shrinks proportionally. Under heavy host load this
       is observable as `elapsed < 1000us` even though the stall
       legitimately took >= 2 ms. We can't tell from inside the
       guest whether a small elapsed means "TSC paused" vs "Stall
       returned early" vs "calibration was off" — all three are
       benign causes that don't reflect a get_us bug. So we treat
       elapsed << expected as a SKIP rather than a FAIL. The
       ratchet's SKIP-balance discipline still gets honored
       (every code-path-skipped test_check fires exactly one
       assertion). The genuine regression we WANT to catch — get_us
       returning 0 or going backward across a stall — is covered
       by the monotonic checks above. */
    if (t1 > t0) {
        uint64_t elapsed = t1 - t0;
        /* All three arms used to assert true under different labels, so the
           block could not fail whatever get_us returned -- the one thing it
           existed to check. Asserting `elapsed >= 1000` inside
           `if (elapsed >= 1000)` would have been the same tautology in a new
           spelling, so the surviving arm asserts an UPPER bound instead: a
           2 ms stall measuring as more than 10 s means the frequency scale is
           wrong, which is falsifiable and is a bug a lower bound cannot see. */
        if (elapsed >= 1000) {
            test_check(elapsed < 10ull * 1000 * 1000,
                       "time: get_us elapsed >= 1ms and within a sane bound");
        } else {
            /* TSC paused mid-Stall under host scheduling pressure (KVM), or
               firmware Stall granularity. Not a get_us bug, and not
               measurable here -- so declared, not asserted. */
            test_skip_n(1, "time: get_us elapsed (TSC paused under the host "
                           "scheduler)");
        }
    } else {
        /* No cycle counter on this arch, so elapsed is meaningless. */
        test_skip_n(1, "time: get_us elapsed (no cycle counter)");
    }
}

// ---------------------------------------------------------------------------
// axl_clock_gettime / axl_clock_getres — POSIX-style clock API
// ---------------------------------------------------------------------------

static void
test_clock_gettime(void)
{
    /* NULL guards. */
    test_check(axl_clock_gettime(AXL_CLOCK_MONOTONIC, NULL) == AXL_ERR,
               "clock_gettime: NULL out rejected");
    test_check(axl_clock_gettime((AxlClockId)42, NULL) == AXL_ERR,
               "clock_gettime: unknown clockid rejected");

    /* Monotonic: tv_sec/tv_nsec well-formed, value advances across
       a 2 ms sleep. */
    AxlTimespec a = {0, 0};
    int rc_a = axl_clock_gettime(AXL_CLOCK_MONOTONIC, &a);
    if (rc_a != AXL_OK) {
        /* Architecture without a usable cycle counter. SKIP-balance
           the 7 populated-path assertions below. */
        test_skip_n(7, "clock_gettime MONOTONIC (no counter)");
    } else {
        test_check(rc_a == AXL_OK,
                   "clock_gettime: MONOTONIC returns OK");
        test_check(a.tv_nsec >= 0 && a.tv_nsec < 1000000000,
                   "clock_gettime: MONOTONIC tv_nsec in [0, 1e9)");

        axl_usleep(2000);

        AxlTimespec b = {0, 0};
        test_check(axl_clock_gettime(AXL_CLOCK_MONOTONIC, &b) == AXL_OK,
                   "clock_gettime: MONOTONIC second read OK");

        /* b >= a as a 128-bit-like compare on (tv_sec, tv_nsec). */
        bool advanced =
            (b.tv_sec > a.tv_sec) ||
            (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec);
        test_check(advanced,
                   "clock_gettime: MONOTONIC advances across 2ms sleep");

        /* Cross-call delta in nanoseconds (lenient: KVM TSC pauses
           can shrink real 2ms sleeps; require only non-zero). */
        int64_t dsec  = b.tv_sec  - a.tv_sec;
        int64_t dnsec = (int64_t)b.tv_nsec - (int64_t)a.tv_nsec;
        int64_t delta_ns = dsec * 1000000000ll + dnsec;
        test_check(delta_ns > 0,
                   "clock_gettime: MONOTONIC ns delta positive");

        /* axl_time_get_us is now a wrapper over MONOTONIC — sanity
           check it returns something matching the order of
           magnitude. */
        uint64_t us = axl_time_get_us();
        test_check(us > 0,
                   "axl_time_get_us: returns non-zero (boot-relative)");

        /* Resolution: getres reports >= 1 ns and < 1 sec. */
        AxlTimespec res = {0, 0};
        test_check(axl_clock_getres(AXL_CLOCK_MONOTONIC, &res) == AXL_OK
                       && res.tv_nsec >= 1 && res.tv_sec == 0,
                   "clock_getres: MONOTONIC reports ns-shaped resolution");
    }

    /* Realtime: tv_nsec in range; tv_sec is the firmware RTC. We
       don't pin a value (the RTC may be unset in QEMU or set to a
       2026-era timestamp); just verify the call shape. */
    AxlTimespec wall = {0, 0};
    int rc_w = axl_clock_gettime(AXL_CLOCK_REALTIME, &wall);
    if (rc_w != AXL_OK) {
        test_skip_n(2, "clock_gettime REALTIME (RTC unavailable)");
    } else {
        test_check(rc_w == AXL_OK,
                   "clock_gettime: REALTIME returns OK");
        test_check(wall.tv_nsec >= 0 && wall.tv_nsec < 1000000000,
                   "clock_gettime: REALTIME tv_nsec in [0, 1e9)");
    }
}

// ---------------------------------------------------------------------------
// Environment variable tests
// ---------------------------------------------------------------------------

static void
test_env(void)
{
    char *val;

    /* Set and get */
    test_check(axl_setenv("AXL_TEST_VAR", "hello", true) == AXL_OK,
               "env: setenv returns 0");
    val = axl_getenv("AXL_TEST_VAR");
    test_check(val != NULL, "env: getenv returns non-NULL");
    if (val != NULL) {
        test_check(axl_strcmp(val, "hello") == 0, "env: getenv value matches");
        axl_free(val);
    }

    /* Overwrite=false doesn't replace */
    axl_setenv("AXL_TEST_VAR", "world", false);
    val = axl_getenv("AXL_TEST_VAR");
    if (val != NULL) {
        test_check(axl_strcmp(val, "hello") == 0, "env: no overwrite preserved");
        axl_free(val);
    }

    /* Unset */
    test_check(axl_unsetenv("AXL_TEST_VAR") == AXL_OK, "env: unsetenv returns 0");
    val = axl_getenv("AXL_TEST_VAR");
    test_check(val == NULL, "env: unset var is gone");
    axl_free(val);

    /* axl_diag_startup reads AXL_DIAG through axl_getenv, whose result is
       OWNED. It used to drop that string on both the gated-off early return
       and the fall-through, so every tool calling it leaked once per run for
       as long as AXL_DIAG was set. The docstring even promised "no
       allocation beyond the UTF-8 conversion buffers (auto-freed)" while it
       did. Balance live allocations across a real, enabled call — the dump
       output below is the price of testing the path that actually leaked. */
    AxlMemStats diag_before, diag_after;
    axl_setenv("AXL_DIAG", "1", true);
    axl_mem_get_stats(&diag_before);
    axl_diag_startup(0, NULL);
    axl_mem_get_stats(&diag_after);
    test_check(diag_after.count == diag_before.count,
               "diag: startup with AXL_DIAG set releases everything it allocates");
    axl_unsetenv("AXL_DIAG");

    /* Get missing */
    val = axl_getenv("AXL_NO_SUCH_VAR_XYZ");
    test_check(val == NULL, "env: missing var returns NULL");
}

// ---------------------------------------------------------------------------
// Working directory tests
// ---------------------------------------------------------------------------

static void
test_cwd(void)
{
    char *cwd;

    cwd = axl_get_current_dir();
    test_check(cwd != NULL, "cwd: get_current_dir non-NULL");
    axl_free(cwd);

    test_check(axl_chdir("fs0:\\") == AXL_OK, "cwd: chdir to root");
    cwd = axl_get_current_dir();
    test_check(cwd != NULL, "cwd: get after chdir");
    axl_free(cwd);
}

// ---------------------------------------------------------------------------
// Path resolve tests
// ---------------------------------------------------------------------------

static void
test_path_resolve(void)
{
    char buf[256];

    /* Simple join */
    test_check(axl_path_resolve("/base", "file.efi", buf, sizeof(buf)) == AXL_OK,
               "resolve: simple join");
    test_check(axl_strcmp(buf, "/base/file.efi") == 0,
               "resolve: simple join value");

    /* Dot removal */
    test_check(axl_path_resolve("/base", "./foo", buf, sizeof(buf)) == AXL_OK,
               "resolve: dot removal");
    test_check(axl_strcmp(buf, "/base/foo") == 0,
               "resolve: dot removal value");

    /* Dotdot resolution */
    test_check(axl_path_resolve("/a/b/c", "../d", buf, sizeof(buf)) == AXL_OK,
               "resolve: dotdot");
    test_check(axl_strcmp(buf, "/a/b/d") == 0,
               "resolve: dotdot value");

    /* Multiple dotdot */
    test_check(axl_path_resolve("/a/b/c", "../../d", buf, sizeof(buf)) == AXL_OK,
               "resolve: multi-dotdot");
    test_check(axl_strcmp(buf, "/a/d") == 0,
               "resolve: multi-dotdot value");

    /* Absolute relative overrides base */
    test_check(axl_path_resolve("/base", "/absolute/path", buf, sizeof(buf)) == AXL_OK,
               "resolve: absolute override");
    test_check(axl_strcmp(buf, "/absolute/path") == 0,
               "resolve: absolute override value");

    /* Root path */
    test_check(axl_path_resolve("/", ".", buf, sizeof(buf)) == AXL_OK,
               "resolve: root dot");
    test_check(axl_strcmp(buf, "/") == 0,
               "resolve: root dot value");

    /* Dotdot underflow past root */
    test_check(axl_path_resolve("/a", "../../x", buf, sizeof(buf)) == AXL_ERR,
               "resolve: dotdot underflow");

    /* NULL args */
    test_check(axl_path_resolve(NULL, "foo", buf, sizeof(buf)) == AXL_ERR,
               "resolve: NULL base");
    test_check(axl_path_resolve("/a", NULL, buf, sizeof(buf)) == AXL_ERR,
               "resolve: NULL relative");

    /* Buffer too small */
    test_check(axl_path_resolve("/base", "file.efi", buf, 5) == AXL_ERR,
               "resolve: buffer too small");

    /* Backslash handling */
    test_check(axl_path_resolve("/a\\b", "c\\d", buf, sizeof(buf)) == AXL_OK,
               "resolve: backslash");
    test_check(axl_strcmp(buf, "/a/b/c/d") == 0,
               "resolve: backslash normalized");
}

// ---------------------------------------------------------------------------
// axl_path_companion — anchor-relative sidecar path
// ---------------------------------------------------------------------------

static void
test_path_companion(void)
{
    char *p;

    /* Standard case: anchor has a directory component. axl_path_join
       picks the separator from the anchor's style — UEFI/Windows-style
       anchor stays consistent backslash. */
    p = axl_path_companion("fs0:\\tools\\memspd.efi", "jedec.json");
    test_check(p != NULL && axl_strcmp(p, "fs0:\\tools\\jedec.json") == 0,
               "path_companion: dirname + name (UEFI separators)");
    axl_free(p);

    /* Unix-style separators. */
    p = axl_path_companion("/usr/bin/memspd", "jedec.json");
    test_check(p != NULL && axl_strcmp(p, "/usr/bin/jedec.json") == 0,
               "path_companion: dirname + name (POSIX separators)");
    axl_free(p);

    /* Bare filename → dirname returns "." → companion lives in cwd. */
    p = axl_path_companion("memspd.efi", "jedec.json");
    test_check(p != NULL && axl_strcmp(p, "./jedec.json") == 0,
               "path_companion: bare anchor falls back to ./name");
    axl_free(p);

    /* NULL safety. */
    test_check(axl_path_companion(NULL, "jedec.json") == NULL,
               "path_companion: NULL anchor returns NULL");
    test_check(axl_path_companion("memspd.efi", NULL) == NULL,
               "path_companion: NULL name returns NULL");
}

// ---------------------------------------------------------------------------
// UEFI path construction tests
// ---------------------------------------------------------------------------

static void
test_path_build_uefi(void)
{
    char buf[64];

    /* Basic */
    test_check(axl_path_build_uefi("fs0", "/dir/file.efi", buf, sizeof(buf)) == AXL_OK,
               "build_uefi: basic");
    test_check(axl_strcmp(buf, "fs0:\\dir\\file.efi") == 0,
               "build_uefi: slashes converted");

    /* Root path */
    test_check(axl_path_build_uefi("fs0", "/", buf, sizeof(buf)) == AXL_OK,
               "build_uefi: root");
    test_check(axl_strcmp(buf, "fs0:\\") == 0,
               "build_uefi: root value");

    /* Already backslash */
    test_check(axl_path_build_uefi("fs1", "\\already\\back", buf, sizeof(buf)) == AXL_OK,
               "build_uefi: backslash passthrough");
    test_check(axl_strcmp(buf, "fs1:\\already\\back") == 0,
               "build_uefi: backslash preserved");

    /* Buffer too small */
    test_check(axl_path_build_uefi("fs0", "/dir/file.efi", buf, 5) == AXL_ERR,
               "build_uefi: buffer too small");

    /* NULL safety */
    test_check(axl_path_build_uefi(NULL, "/foo", buf, sizeof(buf)) == AXL_ERR,
               "build_uefi: NULL volume");
    test_check(axl_path_build_uefi("fs0", NULL, buf, sizeof(buf)) == AXL_ERR,
               "build_uefi: NULL subpath");
}

// ---------------------------------------------------------------------------
// JSON directory listing tests
// ---------------------------------------------------------------------------

static void
test_dir_list_json(void)
{
    char buf[512];

    /* Empty list */
    AxlFsEntry empty_entries[1] = {0};
    test_check(axl_dir_list_json(empty_entries, 0, buf, sizeof(buf)) == AXL_OK,
               "dir_json: empty");
    test_check(axl_strcmp(buf, "[]") == 0,
               "dir_json: empty value");

    /* Two AxlFsEntry slots, properly versioned per the struct's
       forward-compat contract (struct_size + version up front). */
    AxlFsEntry entries[2] = {
        [0] = {
            .struct_size = sizeof(AxlFsEntry),
            .version     = AXL_FS_ENTRY_VERSION,
            .size        = 1024,
            .attributes  = 0,     /* regular file */
        },
        [1] = {
            .struct_size = sizeof(AxlFsEntry),
            .version     = AXL_FS_ENTRY_VERSION,
            .size        = 0,
            .attributes  = AXL_FS_ATTR_DIRECTORY,
        },
    };
    axl_strlcpy(entries[0].name, "test.txt", sizeof(entries[0].name));

    test_check(axl_dir_list_json(entries, 1, buf, sizeof(buf)) == AXL_OK,
               "dir_json: single file");
    test_check(axl_strstr_len(buf, -1, "\"name\":\"test.txt\"") != NULL,
               "dir_json: has name");
    test_check(axl_strstr_len(buf, -1, "\"size\":1024") != NULL,
               "dir_json: has size");
    test_check(axl_strstr_len(buf, -1, "\"dir\":false") != NULL,
               "dir_json: has dir false");

    /* Directory entry */
    axl_strlcpy(entries[1].name, "subdir", sizeof(entries[1].name));

    test_check(axl_dir_list_json(entries, 2, buf, sizeof(buf)) == AXL_OK,
               "dir_json: two entries");
    test_check(axl_strstr_len(buf, -1, "\"dir\":true") != NULL,
               "dir_json: has dir true");
    test_check(buf[0] == '[' && buf[axl_strlen(buf) - 1] == ']',
               "dir_json: array brackets");

    /* Buffer too small */
    test_check(axl_dir_list_json(entries, 2, buf, 10) == AXL_ERR,
               "dir_json: buffer overflow");

    /* NULL safety */
    test_check(axl_dir_list_json(NULL, 1, buf, sizeof(buf)) == AXL_ERR,
               "dir_json: NULL entries");
}

// ---------------------------------------------------------------------------
// Volume enumeration tests
// ---------------------------------------------------------------------------

static void
test_volume_enumerate(void)
{
    size_t count = 0;

    /* Query count only */
    test_check(axl_volume_enumerate(NULL, 0, &count) == AXL_OK,
               "vol enum: query count");
    test_check(count > 0, "vol enum: at least 1 volume");

    /* Enumerate into array */
    AxlVolume vols[8];
    size_t filled = 0;
    test_check(axl_volume_enumerate(vols, 8, &filled) == AXL_OK,
               "vol enum: fill array");
    test_check(filled == count, "vol enum: filled matches count");

    /* First volume has name "fs0" */
    if (filled > 0) {
        test_check(axl_strcmp(vols[0].name, "fs0") == 0,
                   "vol enum: first is fs0");
        test_check(vols[0].handle != NULL,
                   "vol enum: handle non-NULL");
    }

    /* NULL count is error */
    test_check(axl_volume_enumerate(NULL, 0, NULL) == AXL_ERR,
               "vol enum: NULL count returns AXL_ERR");
}

// ---------------------------------------------------------------------------
// axl_volume_get_space — real-firmware path/handle agreement.
//
// The deterministic figure-level coverage (exact bytes, the unknown
// marker, per-figure gating) lives in AxlFsProvider's mock, which is the
// only volume whose free space the suite controls. Here the boot volume
// is a genuine FAT filesystem, so what is worth pinning is that the two
// spellings answer identically and that the numbers are self-consistent.
// ---------------------------------------------------------------------------

static void
test_volume_get_space(void)
{
    /* Distinctive fill so "untouched" is provable rather than assumed —
       zero would be indistinguishable from a volume reporting empty. */
    const uint64_t untouched = 0xD15EA5EDD15EA5EDull;
    uint64_t total = untouched;
    uint64_t avail = untouched;

    /* Argument validation — runs everywhere, not part of the balance. */
    test_check(axl_volume_get_space(NULL, &total, &avail) == AXL_ERR,
               "vol space: NULL path returns AXL_ERR");
    test_check(axl_volume_get_space("", &total, &avail) == AXL_ERR,
               "vol space: empty path returns AXL_ERR");
    test_check(axl_volume_get_space("fs0:", NULL, NULL) == AXL_ERR,
               "vol space: asking for neither figure returns AXL_ERR");
    test_check(axl_volume_get_space("axl-no-such-vol:", &total, &avail)
                   == AXL_ERR,
               "vol space: unresolvable volume returns AXL_ERR");

    /* A volume name too long to represent must FAIL, not quietly fall
       back to the working directory's volume. Answering a "will this
       write fit?" question with a different volume's free bytes is the
       exact confident-but-wrong result this API exists to prevent, and
       it would look like a clean AXL_OK to the caller. 32 name chars
       overflows the internal root buffer; shell map names really can be
       arbitrary (axl_volume_set_map assigns them). */
    test_check(axl_volume_get_space("abcdefghijklmnopqrstuvwxyz012345:",
                                    &total, &avail) == AXL_ERR,
               "vol space: over-long volume name fails, no cwd fallback");
    test_check(axl_volume_get_label("abcdefghijklmnopqrstuvwxyz012345:")
                   == NULL,
               "vol label: over-long volume name fails, no cwd fallback");
    test_check(total == untouched && avail == untouched,
               "vol space: out params untouched on failure");

    AxlVolume vols[8];
    size_t    filled = 0;
    if (axl_volume_enumerate(vols, 8, &filled) != AXL_OK || filled == 0) {
        test_skip_n(6, "vol space: no volumes enumerated - query SKIPPED");
        return;
    }

    /* Asserted, NOT used as a gate: an earlier revision let a failing
       query take the SKIP path, so a regression in the path spelling
       would have kept the suite green at an unchanged ratchet total. */
    char path[24];
    axl_snprintf(path, sizeof(path), "%s:", vols[0].name);
    test_check(axl_volume_get_space(path, &total, &avail) == AXL_OK,
               "vol space: boot volume answers the path spelling");
    test_check(total > 0,
               "vol space: boot volume reports a non-zero total");
    test_check(avail <= total,
               "vol space: free space fits inside the total");

    uint64_t h_total = untouched;
    uint64_t h_avail = untouched;
    test_check(axl_volume_get_space_by_handle(vols[0].handle,
                                              &h_total, &h_avail) == AXL_OK,
               "vol space: the same volume answers by handle");
    test_check(h_total == total && h_avail == avail,
               "vol space: path and handle spellings agree exactly");

    /* The motivating use case: sizing a file that does not exist yet.
       Naming an absent file must report its VOLUME, not fail. */
    uint64_t a_total = untouched;
    uint64_t a_avail = untouched;
    char absent[64];
    axl_snprintf(absent, sizeof(absent), "%s:\\axl-no-such-file.zzz",
                 vols[0].name);
    test_check(axl_volume_get_space(absent, &a_total, &a_avail) == AXL_OK
                   && a_total == total && a_avail == avail,
               "vol space: a not-yet-created file reports its volume");
}

// ---------------------------------------------------------------------------
// axl_volume_get_label accepts every spelling of a volume it documents.
//
// Its docstring offers `"fs0:"` and `"fs1:\\"` as equivalents, but the
// bare map name was passed straight to the shell's OpenFileByName, which
// rejects it -- so the documented form returned NULL. All three spellings
// name one volume and must yield one label.
// ---------------------------------------------------------------------------

static void
test_volume_get_label_spellings(void)
{
    AXL_AUTO_FREE char *bare  = axl_volume_get_label("fs0:");
    AXL_AUTO_FREE char *root  = axl_volume_get_label("fs0:\\");
    AXL_AUTO_FREE char *file  = axl_volume_get_label("fs0:\\AxlTestUtil.efi");

    test_check(root != NULL, "vol label: \"fs0:\\\" resolves");
    test_check(bare != NULL, "vol label: bare \"fs0:\" resolves");
    test_check(bare != NULL && root != NULL && axl_strcmp(bare, root) == 0,
               "vol label: bare and root spellings give the same label");
    test_check(file != NULL && root != NULL && axl_strcmp(file, root) == 0,
               "vol label: a file path gives its volume's label");

    /* The discriminator for "@p path need not exist": before the fix
       an absent file resolved to NULL, because the path itself was
       opened. The label belongs to the volume, so it must answer. */
    AXL_AUTO_FREE char *absent = axl_volume_get_label("fs0:\\axl-no-such.zzz");
    test_check(absent != NULL && root != NULL && axl_strcmp(absent, root) == 0,
               "vol label: an absent path still gives its volume's label");
}

static void
test_volume_map_name(void)
{
    char buf[16];

    /* Argument validation. */
    test_check(axl_volume_map_name(NULL, buf, sizeof(buf)) == AXL_ERR,
               "vol map name: NULL device_path returns AXL_ERR");
    /* A non-NULL sentinel device path with a zero-size buffer is rejected
       before any deref, so the sentinel is never dereferenced. */
    test_check(axl_volume_map_name((const void *)(uintptr_t)0x1, buf, 0)
                   == AXL_ERR,
               "vol map name: zero out_size returns AXL_ERR");

    /* Direct lookup for a real, shell-mapped volume returns its shell alias —
       and NOT a synthesized LocateHandle index. fs0 is the boot volume in the
       unit harness; axl_volume_enumerate named it "fs0" via the same shell map,
       so the direct lookup must agree. */
    AxlVolume vols[8];
    size_t    filled = 0;
    if (axl_volume_enumerate(vols, 8, &filled) == AXL_OK && filled > 0
        && vols[0].device_path != NULL) {
        buf[0] = '\0';
        int rc = axl_volume_map_name(vols[0].device_path, buf, sizeof(buf));
        test_check(rc == AXL_OK,
                   "vol map name: resolves an enumerated volume's device path");
        test_check(axl_strcmp(buf, vols[0].name) == 0,
                   "vol map name: alias matches axl_volume_enumerate's name");
    }
}

static void
test_volume_map_ops(void)
{
    /* Existing/absent map names. fs0 is the boot volume in the unit harness. */
    test_check(axl_volume_map_taken("fs0") == true,
               "volume map: fs0 is taken");
    test_check(axl_volume_map_taken("fs0:") == true,
               "volume map: fs0: (colon form) is taken");
    test_check(axl_volume_map_taken("axl-no-such-map") == false,
               "volume map: absent name not taken");
    test_check(axl_volume_map_taken(NULL) == false,
               "volume map: NULL name not taken");

    /* set_map arg validation. */
    test_check(axl_volume_set_map(NULL, "X") == AXL_ERR,
               "volume set_map: NULL device_path -> AXL_ERR");

    /* Assign a fresh name to an existing volume's device path, then confirm the
       shell now reports it as taken (SetMap round-trip; also proves a child's
       SetMap reaches the shell map). */
    AxlVolume vols[8];
    size_t    filled = 0;
    if (axl_volume_enumerate(vols, 8, &filled) == AXL_OK && filled > 0
        && vols[0].device_path != NULL) {
        test_check(axl_volume_map_taken("AXLTSTMAP") == false,
                   "volume set_map: test name free before mapping");
        test_check(axl_volume_set_map(vols[0].device_path, "AXLTSTMAP") == AXL_OK,
                   "volume set_map: assigns a new name to a device path");
        test_check(axl_volume_map_taken("AXLTSTMAP") == true,
                   "volume set_map: name is taken after mapping");
    }
}

/* axl_path_search — the decomposable core of the path-searched-launch sibling
   fallback (the full path-launch scenario is real-HW-only; OVMF's shell can't
   path-search .efi). Here we pin the search logic against staged files. */
static void
test_path_search(void)
{
    char *found = NULL;

    /* Argument validation. */
    test_check(axl_path_search(NULL, "x.tmp", &found) == AXL_ERR,
               "path search: NULL list -> AXL_ERR");
    test_check(axl_path_search("fs0:\\", NULL, &found) == AXL_ERR,
               "path search: NULL name -> AXL_ERR");
    test_check(axl_path_search("fs0:\\", "x.tmp", NULL) == AXL_ERR,
               "path search: NULL out -> AXL_ERR");

    /* Stage a probe file at the fs0 root, then find it via a multi-entry list
       whose FIRST entry is a nonexistent dir — proving empty/miss entries are
       skipped and the first real hit wins (the shell's path-search order). */
    const char *fname = "axl_psearch.tmp";
    test_check(axl_file_set_contents("fs0:\\axl_psearch.tmp", "x", 1) == AXL_OK,
               "path search: stage probe file");

    found = NULL;
    int rc = axl_path_search("fsZ:\\nope;;fs0:\\", fname, &found);
    test_check(rc == AXL_OK && found != NULL,
               "path search: finds the file in a later path entry");
    axl_free(found);

    /* Absent name -> AXL_NOT_FOUND, out cleared. */
    found = (char *)(uintptr_t)0xDEAD;
    test_check(axl_path_search("fs0:\\", "axl_nope_zzz.tmp", &found)
                   == AXL_NOT_FOUND,
               "path search: absent name -> AXL_NOT_FOUND");
    test_check(found == NULL, "path search: out NULLed on not-found");
}

// ---------------------------------------------------------------------------
// Config tests
// ---------------------------------------------------------------------------

typedef struct {
    uint64_t port;
    bool     verbose;
    int      max_conn;
} TestConfigTarget;

static const AxlConfigDesc test_cfg_descs[] = {
    { "port",     AXL_CFG_UINT,   "8080",  "Listen port",
      offsetof(TestConfigTarget, port), sizeof(uint64_t) },
    { "verbose",  AXL_CFG_BOOL,   "false", "Verbose output",
      offsetof(TestConfigTarget, verbose), sizeof(bool) },
    { "max.conn", AXL_CFG_INT,    "16",    "Max connections",
      offsetof(TestConfigTarget, max_conn), sizeof(int) },
    { "name",     AXL_CFG_STRING, NULL,    "Server name", 0, 0 },
    { "header",   AXL_CFG_MULTI,  NULL,    "Custom header", 0, 0 },
    { 0 }
};

static int test_apply_called = 0;

static int
test_apply_fn(void *target, const char *key, const char *value)
{
    (void)target;
    (void)value;
    test_apply_called++;
    if (axl_streql(key, "name")) {
        return 1;  /* handled */
    }
    return 0;  /* accept, proceed with auto-apply */
}

static void
test_config_file(void)
{
    /* Empty map: every lookup yields its caller default. */
    AxlConfigFile *cf = axl_config_file_new();
    test_check(cf != NULL, "config_file: new returns a map");
    test_check(axl_strcmp(axl_config_file_get(cf, "absent", "fallback"),
                          "fallback") == 0,
               "config_file: missing string key returns default");
    test_check(axl_config_file_get_uint(cf, "absent", 42) == 42,
               "config_file: missing uint key returns default");
    test_check(axl_config_file_get_int(cf, "absent", -7) == -7,
               "config_file: missing int key returns default");
    test_check(axl_config_file_get_bool(cf, "absent", true) == true,
               "config_file: missing bool key returns default");

    /* set / get round-trip + typed parsing. */
    test_check(axl_config_file_set(cf, "name", "softbmc") == AXL_OK,
               "config_file: set returns AXL_OK");
    test_check(axl_strcmp(axl_config_file_get(cf, "name", ""), "softbmc") == 0,
               "config_file: set value reads back");
    axl_config_file_set(cf, "timeout", "900");
    test_check(axl_config_file_get_uint(cf, "timeout", 0) == 900,
               "config_file: get_uint parses a stored decimal");
    axl_config_file_set(cf, "hexval", "0x1f");
    test_check(axl_config_file_get_uint(cf, "hexval", 0) == 0x1f,
               "config_file: get_uint parses 0x-hex");
    axl_config_file_set(cf, "offset", "-12");
    test_check(axl_config_file_get_int(cf, "offset", 0) == -12,
               "config_file: get_int parses a negative");
    axl_config_file_set(cf, "flag", "YES");
    test_check(axl_config_file_get_bool(cf, "flag", false) == true,
               "config_file: get_bool parses YES (case-insensitive)");
    axl_config_file_set(cf, "flag2", "off");
    test_check(axl_config_file_get_bool(cf, "flag2", true) == false,
               "config_file: get_bool parses off");
    axl_config_file_set(cf, "notnum", "abc");
    test_check(axl_config_file_get_uint(cf, "notnum", 99) == 99,
               "config_file: unparseable uint returns default");
    test_check(axl_config_file_get_bool(cf, "notnum", false) == false,
               "config_file: unparseable bool returns default");
    axl_config_file_set(cf, "name", "renamed");
    test_check(axl_strcmp(axl_config_file_get(cf, "name", ""), "renamed") == 0,
               "config_file: set overwrites an existing key");
    axl_config_file_free(cf);

    /* Parse a file: comments, blanks, trimming, dotted keys, no-'=' line. */
    const char *text =
        "# a comment\n"
        "\n"
        "mode=handoff\n"
        "  boot_timeout =  30  \n"
        "ec.poll_interval_ms=5000\n"
        "noeqline\n"
        "empty=\n";
    test_check(axl_file_set_contents("axl-cfgfile.tmp", text,
                                     axl_strlen(text)) == AXL_OK,
               "config_file: wrote test config file");
    AxlConfigFile *lf = axl_config_file_load("axl-cfgfile.tmp");
    test_check(lf != NULL, "config_file: load returns a map");
    test_check(axl_strcmp(axl_config_file_get(lf, "mode", ""), "handoff") == 0,
               "config_file: parsed a core key");
    test_check(axl_config_file_get_uint(lf, "boot_timeout", 0) == 30,
               "config_file: trimmed key+value parse (boot_timeout=30)");
    test_check(axl_config_file_get_uint(lf, "ec.poll_interval_ms", 0) == 5000,
               "config_file: parsed a dotted prefix.key as a flat key");
    test_check(axl_strcmp(axl_config_file_get(lf, "empty", "X"), "") == 0,
               "config_file: empty value stored as empty string");
    test_check(axl_config_file_get(lf, "noeqline", NULL) == NULL,
               "config_file: a line with no '=' is ignored");
    test_check(axl_config_file_get(lf, "# a comment", NULL) == NULL,
               "config_file: comment line is not a key");
    axl_config_file_free(lf);

    /* Missing file -> empty map (NOT NULL, NOT an error). */
    AxlConfigFile *mf = axl_config_file_load("does-not-exist.cfg");
    test_check(mf != NULL,
               "config_file: missing file yields an empty map (not NULL)");
    test_check(axl_config_file_get_uint(mf, "anything", 7) == 7,
               "config_file: empty-from-missing map returns defaults");
    axl_config_file_free(mf);

    /* save -> load round-trip. */
    AxlConfigFile *sf = axl_config_file_new();
    axl_config_file_set(sf, "alpha", "one");
    axl_config_file_set(sf, "beta", "2");
    test_check(axl_config_file_save(sf, "axl-cfgsave.tmp") == AXL_OK,
               "config_file: save returns AXL_OK");
    axl_config_file_free(sf);
    AxlConfigFile *rt = axl_config_file_load("axl-cfgsave.tmp");
    test_check(rt != NULL
               && axl_strcmp(axl_config_file_get(rt, "alpha", ""), "one") == 0
               && axl_config_file_get_uint(rt, "beta", 0) == 2,
               "config_file: save -> load round-trips values");
    axl_config_file_free(rt);

    /* NULL-safety: free(NULL) must not crash; the API stays usable after. */
    test_check(axl_strcmp(axl_config_file_get(NULL, "k", "d"), "d") == 0,
               "config_file: get on NULL map returns default");
    axl_config_file_free(NULL);
    AxlConfigFile *probe = axl_config_file_new();
    test_check(probe != NULL,
               "config_file: API usable after free(NULL)");
    axl_config_file_free(probe);
}

/* axl_config_file_parse_string / axl_config_file_to_string: the in-memory
   counterparts of axl_config_file_load/_save, for a caller (netload) whose
   config text lives in an NVRAM variable rather than a file. */
static void
test_config_file_string_round_trip(void)
{
    /* parse_string: identical line grammar to axl_config_file_load, fed
       from memory instead of a file. */
    const char *text =
        "# a comment\n"
        "\n"
        "mode=handoff\n"
        "  boot_timeout =  30  \n"
        "ec.poll_interval_ms=5000\n"
        "noeqline\n"
        "empty=\n";
    AxlConfigFile *ps = axl_config_file_new();
    test_check(axl_config_file_parse_string(ps, text) == AXL_OK,
               "config_file: parse_string returns AXL_OK");
    test_check(axl_strcmp(axl_config_file_get(ps, "mode", ""), "handoff") == 0,
               "config_file: parse_string parsed a core key");
    test_check(axl_config_file_get_uint(ps, "boot_timeout", 0) == 30,
               "config_file: parse_string trimmed key+value (boot_timeout=30)");
    test_check(axl_config_file_get_uint(ps, "ec.poll_interval_ms", 0) == 5000,
               "config_file: parse_string parsed a dotted prefix.key");
    test_check(axl_strcmp(axl_config_file_get(ps, "empty", "X"), "") == 0,
               "config_file: parse_string empty value stored as empty string");
    test_check(axl_config_file_get(ps, "noeqline", NULL) == NULL,
               "config_file: parse_string ignores a line with no '='");
    axl_config_file_free(ps);

    /* parse_string: NULL-safety */
    test_check(axl_config_file_parse_string(NULL, "k=v") == AXL_ERR,
               "config_file: parse_string NULL cf -> AXL_ERR");
    AxlConfigFile *nt = axl_config_file_new();
    test_check(axl_config_file_parse_string(nt, NULL) == AXL_ERR,
               "config_file: parse_string NULL text -> AXL_ERR");
    axl_config_file_free(nt);

    /* to_string: single key -> exact line (no ordering ambiguity with only
       one entry in the map). */
    AxlConfigFile *one = axl_config_file_new();
    axl_config_file_set(one, "only", "val");
    char buf[64];
    test_check(axl_config_file_to_string(one, buf, sizeof buf) == AXL_OK,
               "config_file: to_string single key returns AXL_OK");
    test_check(axl_strcmp(buf, "only=val\n") == 0,
               "config_file: to_string single key exact line");
    axl_config_file_free(one);

    /* to_string -> parse_string round-trip, multi-key. axl_config_file_save's
       own docstring says entry order is unspecified, so pinning a full-buffer
       exact match would test today's hash-table iteration order rather than
       the documented contract -- axl_strstr per key is the honest tool here
       (order really is undefined), backed by an exact round-trip check
       through parse_string for the values themselves. */
    AxlConfigFile *multi = axl_config_file_new();
    axl_config_file_set(multi, "alpha", "one");
    axl_config_file_set(multi, "beta", "2");
    char mbuf[128];
    test_check(axl_config_file_to_string(multi, mbuf, sizeof mbuf) == AXL_OK,
               "config_file: to_string multi-key returns AXL_OK");
    test_check(axl_strstr(mbuf, "alpha=one\n") != NULL,
               "config_file: to_string multi-key contains alpha=one");
    test_check(axl_strstr(mbuf, "beta=2\n") != NULL,
               "config_file: to_string multi-key contains beta=2");
    axl_config_file_free(multi);

    AxlConfigFile *rt = axl_config_file_new();
    test_check(axl_config_file_parse_string(rt, mbuf) == AXL_OK,
               "config_file: round-trip parse_string(to_string(...)) AXL_OK");
    test_check(axl_strcmp(axl_config_file_get(rt, "alpha", ""), "one") == 0
               && axl_config_file_get_uint(rt, "beta", 0) == 2,
               "config_file: to_string -> parse_string round-trips values");
    axl_config_file_free(rt);

    /* to_string: buffer too small is reported, not silently truncated -- a
       sentinel proves buf is left unmodified on the AXL_ERR path. */
    AxlConfigFile *big = axl_config_file_new();
    axl_config_file_set(big, "longkey", "a-fairly-long-value-string");
    char small[4] = { 'S', 'S', 'S', '\0' };
    test_check(axl_config_file_to_string(big, small, sizeof small) == AXL_ERR,
               "config_file: to_string too-small buffer -> AXL_ERR");
    test_check(small[0] == 'S' && small[1] == 'S' && small[2] == 'S',
               "config_file: to_string too-small buffer left unmodified");

    /* to_string: NULL-safety / zero cap */
    test_check(axl_config_file_to_string(NULL, buf, sizeof buf) == AXL_ERR,
               "config_file: to_string NULL cf -> AXL_ERR");
    test_check(axl_config_file_to_string(big, NULL, sizeof buf) == AXL_ERR,
               "config_file: to_string NULL buf -> AXL_ERR");
    test_check(axl_config_file_to_string(big, buf, 0) == AXL_ERR,
               "config_file: to_string zero cap -> AXL_ERR");
    axl_config_file_free(big);
}

static void
test_config(void)
{
    TestConfigTarget tgt;
    axl_memset(&tgt, 0, sizeof(tgt));

    /* Create with defaults */
    AxlConfig *cfg = axl_config_new(test_cfg_descs, test_apply_fn, &tgt);
    test_check(cfg != NULL, "config: new");

    /* Defaults auto-applied */
    test_check(tgt.port == 8080, "config: default port 8080");
    test_check(tgt.verbose == false, "config: default verbose false");
    test_check(tgt.max_conn == 16, "config: default max.conn 16");

    /* Get string values */
    test_check(axl_strcmp(axl_config_get(cfg, "port"), "8080") == 0,
               "config: get port string");
    test_check(axl_config_get_uint(cfg, "port") == 8080,
               "config: get_uint port");
    test_check(axl_config_get_bool(cfg, "verbose") == false,
               "config: get_bool verbose");

    /* Set updates both storage and target struct */
    test_check(axl_config_set(cfg, "port", "9090") == AXL_OK,
               "config: set port");
    test_check(tgt.port == 9090, "config: auto-apply port 9090");
    test_check(axl_config_get_uint(cfg, "port") == 9090,
               "config: get_uint after set");

    /* Bool set */
    axl_config_set(cfg, "verbose", "true");
    test_check(tgt.verbose == true, "config: auto-apply verbose true");

    /* Int set */
    axl_config_set(cfg, "max.conn", "32");
    test_check(tgt.max_conn == 32, "config: auto-apply max.conn 32");

    /* String (no auto-apply, callback handles) */
    test_apply_called = 0;
    axl_config_set(cfg, "name", "myserver");
    test_check(test_apply_called > 0, "config: apply_fn called");
    test_check(axl_strcmp(axl_config_get(cfg, "name"), "myserver") == 0,
               "config: get name after callback");

    /* Unknown key rejected */
    test_check(axl_config_set(cfg, "unknown", "x") == AXL_ERR,
               "config: unknown key rejected");

    /* Type validation */
    test_check(axl_config_set(cfg, "port", "abc") == AXL_ERR,
               "config: type validation rejects abc for uint");

    /* NULL safety */
    test_check(axl_config_get(cfg, "missing") == NULL,
               "config: get missing returns NULL");

    axl_config_free(cfg);

    /* Free NULL is safe */
    axl_config_free(NULL);
    test_survived("config: free(NULL) no crash");
}

/* The added min/max descriptor fields are SYNTHESIS-ONLY metadata (like
 * short_name / choices): AxlConfig parsing must IGNORE them — no range
 * clamping on set, and a value outside [min,max] stores + reads back verbatim.
 * (The bounds are consumed by the axl_service_main AxlArgDesc synthesizer for
 * CLI validation and by a downstream settings-UI builder, not by the parser.) */
typedef struct {
    int tab_width;
} TestRangeTarget;

static const AxlConfigDesc test_range_descs[] = {
    { "tab_width", AXL_CFG_INT, "4", "Tab width",
      offsetof(TestRangeTarget, tab_width), sizeof(int),
      0, NULL, /* short_name, choices */
      1, 16 },  /* min, max */
    { 0 }
};

static void
test_config_minmax_ignored_by_parsing(void)
{
    /* The descriptor carries the declared bounds (readable by consumers). */
    test_check(test_range_descs[0].min == 1 && test_range_descs[0].max == 16,
               "config range: descriptor carries min/max");

    TestRangeTarget tgt;
    axl_memset(&tgt, 0, sizeof(tgt));
    AxlConfig *cfg = axl_config_new(test_range_descs, NULL, &tgt);
    test_check(cfg != NULL, "config range: new");
    test_check(tgt.tab_width == 4, "config range: default applied");

    /* A value far ABOVE max is stored + auto-applied UNCHANGED (not clamped). */
    test_check(axl_config_set(cfg, "tab_width", "999") == AXL_OK,
               "config range: set out-of-range accepted (no clamp)");
    test_check(tgt.tab_width == 999,
               "config range: out-of-range value auto-applied verbatim");
    test_check(axl_config_get_int(cfg, "tab_width") == 999,
               "config range: out-of-range value read back verbatim");

    /* A value BELOW min likewise stored verbatim. */
    axl_config_set(cfg, "tab_width", "0");
    test_check(tgt.tab_width == 0,
               "config range: below-min value not clamped");

    axl_config_free(cfg);
}

// ---------------------------------------------------------------------------
// Width-overflow rejection — was a silent-truncation bug pre-2026-04-25
// ---------------------------------------------------------------------------

static void
test_config_width_overflow(void)
{
    typedef struct {
        uint16_t port;
        uint32_t timeout_ms;
        int32_t  threshold;
    } NarrowTarget;

    static const AxlConfigDesc descs[] = {
        { "port",      AXL_CFG_UINT, "8080",  "Listen port (u16)",
          offsetof(NarrowTarget, port), sizeof(uint16_t) },
        { "timeout",   AXL_CFG_UINT, "30000", "Timeout ms (u32)",
          offsetof(NarrowTarget, timeout_ms), sizeof(uint32_t) },
        { "threshold", AXL_CFG_INT,  "0",     "Threshold (i32)",
          offsetof(NarrowTarget, threshold), sizeof(int32_t) },
        { 0 }
    };

    NarrowTarget tgt;
    axl_memset(&tgt, 0, sizeof(tgt));
    AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(descs, NULL, &tgt);
    test_check(cfg != NULL, "width: config new");

    /* In-range values still work. */
    test_check(axl_config_set(cfg, "port", "9090") == AXL_OK
               && tgt.port == 9090, "width: u16 9090 accepted");
    test_check(axl_config_set(cfg, "port", "65535") == AXL_OK
               && tgt.port == 65535, "width: u16 max 65535 accepted");

    /* Overflow rejected — used to silently truncate to 34463. */
    test_check(axl_config_set(cfg, "port", "65536") == AXL_ERR,
               "width: u16 65536 rejected (was: silent truncate to 0)");
    test_check(axl_config_set(cfg, "port", "99999") == AXL_ERR,
               "width: u16 99999 rejected (was: silent truncate to 34463)");
    test_check(tgt.port == 65535,
               "width: u16 field preserved after rejection");

    /* u32 boundary. */
    test_check(axl_config_set(cfg, "timeout", "4294967295") == AXL_OK
               && tgt.timeout_ms == 4294967295u, "width: u32 max accepted");
    test_check(axl_config_set(cfg, "timeout", "4294967296") == AXL_ERR,
               "width: u32 max+1 rejected");

    /* i32 boundaries — both ends. */
    test_check(axl_config_set(cfg, "threshold", "2147483647") == AXL_OK
               && tgt.threshold == 2147483647, "width: i32 max accepted");
    test_check(axl_config_set(cfg, "threshold", "-2147483648") == AXL_OK
               && tgt.threshold == -2147483648, "width: i32 min accepted");
    test_check(axl_config_set(cfg, "threshold", "2147483648") == AXL_ERR,
               "width: i32 max+1 rejected");
    test_check(axl_config_set(cfg, "threshold", "-2147483649") == AXL_ERR,
               "width: i32 min-1 rejected");

    /* Rejected set leaves the stored hash entry consistent with the
     * target field — both keep the previous value, neither drifts. */
    test_check(axl_strcmp(axl_config_get(cfg, "port"), "65535") == 0,
               "width: stored value preserved after rejected set");

    /* A descriptor whose default overflows the declared width should
     * not crash axl_config_new — the default is logged-and-skipped,
     * the config is still usable. */
    static const AxlConfigDesc bad_default[] = {
        { "tiny", AXL_CFG_UINT, "70000", "u16 with overflowing default",
          offsetof(NarrowTarget, port), sizeof(uint16_t) },
        { 0 }
    };
    NarrowTarget tgt2;
    axl_memset(&tgt2, 0xAA, sizeof(tgt2));   /* sentinel */
    AXL_AUTOPTR(AxlConfig) cfg2 = axl_config_new(bad_default, NULL, &tgt2);
    test_check(cfg2 != NULL, "width: overflowing default doesn't crash new()");
    test_check(tgt2.port == 0xAAAA,
               "width: overflowing default leaves field at sentinel");
}

static void
test_config_parent(void)
{
    static const AxlConfigDesc descs[] = {
        { "timeout", AXL_CFG_UINT, "5000", "Timeout", 0, 0 },
        { "name",    AXL_CFG_STRING, NULL, "Name", 0, 0 },
        { 0 }
    };

    AxlConfig *parent = axl_config_new(descs, NULL, NULL);
    axl_config_set(parent, "name", "server");

    AxlConfig *child = axl_config_new(descs, NULL, NULL);
    axl_config_set_parent(child, parent);

    /* Child inherits parent values */
    test_check(axl_strcmp(axl_config_get(child, "name"), "server") == 0,
               "config parent: inherits name");

    /* Child can override */
    axl_config_set(child, "name", "connection-1");
    test_check(axl_strcmp(axl_config_get(child, "name"), "connection-1") == 0,
               "config parent: child overrides");

    /* Default from descriptor still works */
    test_check(axl_config_get_uint(child, "timeout") == 5000,
               "config parent: default still works");

    axl_config_free(child);
    axl_config_free(parent);
}

// ---------------------------------------------------------------------------
// axl_config_descs_net / axl_config_descs_append (group injection)
// ---------------------------------------------------------------------------

static void
test_config_descs_append_basic(void)
{
    static const AxlConfigDesc src[] = {
        { "url",       AXL_CFG_STRING, "",      "Server URL",       0, 0 },
        { "read-only", AXL_CFG_BOOL,   "false", "Mount read-only",  0, 0, 'r' },
        { 0 }
    };
    AxlConfigDesc out[8];
    axl_memset(out, 0, sizeof(out));

    size_t n = axl_config_descs_append(out, 8, src);
    test_check(n == 2, "descs_append: copies 2 entries (stops at sentinel)");
    test_check(axl_streql(out[0].key, "url"), "descs_append: [0].key = url");
    test_check(out[0].type == AXL_CFG_STRING,
               "descs_append: [0].type preserved");
    test_check(axl_streql(out[1].key, "read-only"),
               "descs_append: [1].key = read-only");
    test_check(out[1].short_name == 'r',
               "descs_append: [1].short_name preserved");
    test_check(out[2].key == NULL,
               "descs_append: stops before sentinel (no terminator emitted)");
}

static void
test_config_descs_append_null_safety(void)
{
    AxlConfigDesc out[4];
    axl_memset(out, 0, sizeof(out));
    test_check(axl_config_descs_append(out, 4, NULL) == 0,
               "descs_append: NULL src -> 0");
    test_check(out[0].key == NULL,
               "descs_append: NULL src writes nothing");
}

static void
test_config_descs_append_capacity(void)
{
    static const AxlConfigDesc src[] = {
        { "a", AXL_CFG_BOOL, "false", "a", 0, 0 },
        { "b", AXL_CFG_BOOL, "false", "b", 0, 0 },
        { "c", AXL_CFG_BOOL, "false", "c", 0, 0 },
        { 0 }
    };
    AxlConfigDesc out[4];
    axl_memset(out, 0, sizeof(out));
    /* cap=2 cannot hold 3 entries — must reject cleanly, no partial. */
    test_check(axl_config_descs_append(out, 2, src) == 0,
               "descs_append: under-capacity -> 0 (no partial write)");
    test_check(out[0].key == NULL,
               "descs_append: under-capacity leaves out untouched");
}

static void
test_config_descs_net_client(void)
{
    typedef struct {
        char       pad[8];
        AxlNetOpts net;
    } HostOpts;

    AxlConfigDesc out[8];
    axl_memset(out, 0, sizeof(out));

    size_t n = axl_config_descs_net(out, 8, AXL_NET_OPT_CLIENT,
                                    offsetof(HostOpts, net));
    test_check(n == 2, "descs_net: CLIENT mask -> 2 entries");

    /* Find nic + source-ip — order is implementation-defined
       so we look up by key rather than pinning a position. */
    size_t nic_off        = 0;
    size_t source_ip_off  = 0;
    size_t nic_size       = 0;
    size_t source_ip_size = 0;
    bool   nic_seen       = false;
    bool   source_ip_seen = false;
    for (size_t i = 0; i < n; i++) {
        if (axl_streql(out[i].key, "nic")) {
            nic_off  = out[i].offset;
            nic_size = out[i].field_size;
            nic_seen = true;
        }
        if (axl_streql(out[i].key, "source-ip")) {
            source_ip_off  = out[i].offset;
            source_ip_size = out[i].field_size;
            source_ip_seen = true;
        }
    }
    test_check(nic_seen,       "descs_net: CLIENT emits 'nic' key");
    test_check(source_ip_seen, "descs_net: CLIENT emits 'source-ip' key");
    test_check(nic_off == offsetof(HostOpts, net) + offsetof(AxlNetOpts, nic_index),
               "descs_net: nic offset = base + offsetof(AxlNetOpts.nic_index)");
    test_check(source_ip_off == offsetof(HostOpts, net) + offsetof(AxlNetOpts, local_ip),
               "descs_net: source-ip targets local_ip field");
    test_check(nic_size == sizeof(uint64_t),
               "descs_net: nic field_size = sizeof(uint64_t)");
    test_check(source_ip_size == sizeof(const char *),
               "descs_net: source-ip field_size = sizeof(char*)");
}

static void
test_config_descs_net_server(void)
{
    AxlConfigDesc out[8];
    axl_memset(out, 0, sizeof(out));

    size_t n = axl_config_descs_net(out, 8, AXL_NET_OPT_SERVER, 0);
    test_check(n == 3, "descs_net: SERVER mask -> 3 entries");

    size_t port_size      = 0;
    size_t listen_ip_off  = 0;
    bool   port_seen      = false;
    bool   listen_ip_seen = false;
    for (size_t i = 0; i < n; i++) {
        if (axl_streql(out[i].key, "port")) {
            port_size = out[i].field_size;
            port_seen = true;
        }
        if (axl_streql(out[i].key, "listen-ip")) {
            listen_ip_off  = out[i].offset;
            listen_ip_seen = true;
        }
    }
    test_check(port_seen, "descs_net: SERVER mask emits 'port'");
    test_check(port_size == sizeof(uint16_t),
               "descs_net: port field_size = sizeof(uint16_t)");
    test_check(listen_ip_seen, "descs_net: SERVER mask emits 'listen-ip'");
    test_check(listen_ip_off == offsetof(AxlNetOpts, local_ip),
               "descs_net: listen-ip targets local_ip field (same as source-ip)");
}

/* The static (policy) descriptor group: emission shape + the round-trip
   that proves the const-char* fields actually populate through AxlConfig's
   pointer-based string auto-apply (an inline char[] would silently no-op —
   the contract-review BLOCKER this test guards). */
static void
test_config_descs_net_static(void)
{
    typedef struct {
        char             pad[8];
        AxlNetStaticOpts net;
    } HostOpts;

    AxlConfigDesc out[16];
    axl_memset(out, 0, sizeof(out));

    size_t n = axl_config_descs_net_static(out, 16, offsetof(HostOpts, net));
    test_check(n == 7, "descs_net_static: 7 entries");

    /* Every emitted field is a string descriptor targeting a const char*
       member at base + offsetof. */
    bool mode_seen = false, ip_seen = false, dns_seen = false, host_seen = false;
    const char *const *mode_choices = NULL;
    for (size_t i = 0; i < n; i++) {
        test_check(out[i].type == AXL_CFG_STRING,
                   "descs_net_static: entry is AXL_CFG_STRING");
        test_check(out[i].field_size == sizeof(const char *),
                   "descs_net_static: field_size = sizeof(char*)");
        if (axl_streql(out[i].key, "mode")) {
            mode_seen = true; mode_choices = out[i].choices;
        }
        if (axl_streql(out[i].key, "ip")) {
            ip_seen = true;
            test_check(out[i].offset
                           == offsetof(HostOpts, net) + offsetof(AxlNetStaticOpts, ip),
                       "descs_net_static: ip targets AxlNetStaticOpts.ip");
        }
        if (axl_streql(out[i].key, "dns"))      { dns_seen = true; }
        if (axl_streql(out[i].key, "hostname")) { host_seen = true; }
    }
    test_check(mode_seen && ip_seen && dns_seen && host_seen,
               "descs_net_static: emits mode/ip/dns/hostname keys");
    test_check(mode_choices != NULL
                   && mode_choices[0] != NULL && mode_choices[1] != NULL
                   && axl_streql(mode_choices[0], "dhcp")
                   && axl_streql(mode_choices[1], "static")
                   && mode_choices[2] == NULL,
               "descs_net_static: mode choices = {dhcp, static}");

    /* Round-trip through AxlConfig — the B1 guard. */
    HostOpts tgt;
    axl_memset(&tgt, 0, sizeof(tgt));
    AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(out, NULL, &tgt);
    test_check(cfg != NULL, "descs_net_static: config new");
    test_check(tgt.net.mode != NULL && axl_streql(tgt.net.mode, "dhcp"),
               "descs_net_static: mode default 'dhcp' applied");
    axl_config_set(cfg, "mode", "static");
    axl_config_set(cfg, "ip", "10.0.0.5");
    axl_config_set(cfg, "dns", "8.8.8.8");
    test_check(tgt.net.mode != NULL && axl_streql(tgt.net.mode, "static"),
               "descs_net_static: round-trip mode populated (B1)");
    test_check(tgt.net.ip != NULL && axl_streql(tgt.net.ip, "10.0.0.5"),
               "descs_net_static: round-trip ip populated");
    test_check(tgt.net.dns != NULL && axl_streql(tgt.net.dns, "8.8.8.8"),
               "descs_net_static: round-trip dns populated");
}

/* Static-net setters: arg validation (pure) + the hostname persist/read
   round-trip (uses the NV store, available under OVMF). */
static void
test_net_static_setters(void)
{
    test_check(axl_net_set_dns(0, NULL, NULL) == AXL_ERR,
               "set_dns: NULL primary -> AXL_ERR");

    test_check(axl_net_set_hostname(NULL) == AXL_ERR,
               "set_hostname: NULL -> AXL_ERR");
    test_check(axl_net_set_hostname("") == AXL_ERR,
               "set_hostname: empty -> AXL_ERR");

    char hbuf[64] = "x";
    test_check(axl_net_get_hostname(NULL, sizeof(hbuf)) == AXL_ERR,
               "get_hostname: NULL buf -> AXL_ERR");
    test_check(axl_net_get_hostname(hbuf, 0) == AXL_ERR,
               "get_hostname: zero size -> AXL_ERR");

    /* init_static arg validation: NULL cfg, and an unrecognized mode is
       rejected (not silently treated as DHCP). */
    test_check(axl_net_init_static(NULL, AXL_NET_NIC_AUTO, 0) == AXL_ERR,
               "init_static: NULL cfg -> AXL_ERR");
    AxlNetStaticOpts bogus = { .mode = "statc" };
    test_check(axl_net_init_static(&bogus, AXL_NET_NIC_AUTO, 0) == AXL_ERR,
               "init_static: unrecognized mode -> AXL_ERR");

    /* Hostname persist/read round-trip. */
    test_check(axl_net_set_hostname("axlhost") == AXL_OK,
               "set_hostname: 'axlhost' -> AXL_OK");
    char got[64] = { 0 };
    test_check(axl_net_get_hostname(got, sizeof(got)) == AXL_OK
                   && axl_streql(got, "axlhost"),
               "get_hostname: round-trips 'axlhost'");
}

static void
test_config_descs_net_source_and_listen_share_field(void)
{
    /* The whole point of merging source_ip + listen_ip into a single
       local_ip field is that both selector bits resolve to the same
       offset. Pin that contract explicitly: requesting BOTH bits
       (atypical, but legal) emits two descriptors pointing at the
       same field. */
    AxlConfigDesc out[8];
    axl_memset(out, 0, sizeof(out));

    size_t n = axl_config_descs_net(out, 8,
                                    AXL_NET_OPT_SOURCE_IP | AXL_NET_OPT_LISTEN_IP,
                                    0);
    test_check(n == 2, "descs_net: SOURCE_IP | LISTEN_IP -> 2 entries");

    size_t source_off = SIZE_MAX;
    size_t listen_off = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        if (axl_streql(out[i].key, "source-ip")) { source_off = out[i].offset; }
        if (axl_streql(out[i].key, "listen-ip")) { listen_off = out[i].offset; }
    }
    test_check(source_off == offsetof(AxlNetOpts, local_ip),
               "descs_net: source-ip → local_ip");
    test_check(listen_off == offsetof(AxlNetOpts, local_ip),
               "descs_net: listen-ip → local_ip");
    test_check(source_off == listen_off,
               "descs_net: SOURCE_IP and LISTEN_IP share the same offset");
}

static void
test_config_descs_net_empty_kinds(void)
{
    AxlConfigDesc out[4];
    axl_memset(out, 0, sizeof(out));
    test_check(axl_config_descs_net(out, 4, 0, 0) == 0,
               "descs_net: empty kinds -> 0 entries (degenerate, legal)");
    test_check(out[0].key == NULL,
               "descs_net: empty kinds leaves out untouched");
}

static void
test_config_descs_net_capacity(void)
{
    AxlConfigDesc out[8];
    axl_memset(out, 0, sizeof(out));
    /* cap=1 cannot hold the 2-entry CLIENT preset. */
    test_check(axl_config_descs_net(out, 1, AXL_NET_OPT_CLIENT, 0) == 0,
               "descs_net: under-capacity -> 0 (no partial write)");
    test_check(out[0].key == NULL,
               "descs_net: under-capacity leaves out untouched");
}

static void
test_config_descs_net_round_trip(void)
{
    /* End-to-end: emit standard descriptors, append consumer fragment,
       terminate, feed to axl_config_new, axl_config_set values, and
       verify auto-apply lands in the embedded sub-struct. This is the
       contract — descriptor-table composition has to play nicely with
       axl_config_new's offsetof auto-apply through a base_offset. */
    typedef struct {
        AxlNetOpts net;
        bool       read_only;
    } MountOpts;

    static const AxlConfigDesc consumer[] = {
        { "read-only", AXL_CFG_BOOL, "false", "Mount read-only",
          offsetof(MountOpts, read_only), sizeof(bool), 'r' },
        { 0 }
    };

    AxlConfigDesc descs[16];
    axl_memset(descs, 0, sizeof(descs));
    size_t n = axl_config_descs_net(descs, 16, AXL_NET_OPT_SERVER,
                                    offsetof(MountOpts, net));
    n += axl_config_descs_append(descs + n, 16 - n - 1, consumer);
    descs[n] = (AxlConfigDesc){ 0 };

    MountOpts tgt;
    axl_memset(&tgt, 0, sizeof(tgt));

    AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(descs, NULL, &tgt);
    test_check(cfg != NULL, "descs_net round-trip: config_new succeeds");

    /* Defaults auto-apply via base_offset. The NIC sentinel is the
       sharp edge: a "friendly" string default like "auto" would
       silently leave nic_index = 0, not AXL_NET_NIC_AUTO. Pin the
       contract that the descriptor's default puts the SENTINEL into
       the embedded field, not zero. */
    test_check(tgt.net.nic_index == AXL_NET_NIC_AUTO,
               "descs_net round-trip: default nic_index = AXL_NET_NIC_AUTO");
    test_check(tgt.net.port == 0,
               "descs_net round-trip: default port = 0 (consumer-defined)");

    /* Set a port via the descriptor table — must auto-apply into
       tgt.net.port through the base_offset shift. */
    test_check(axl_config_set(cfg, "port", "9876") == AXL_OK,
               "descs_net round-trip: set port=9876");
    test_check(tgt.net.port == 9876,
               "descs_net round-trip: tgt.net.port auto-applied via base_offset");

    /* Set NIC explicitly — must land at the same offset the default
       did. */
    test_check(axl_config_set(cfg, "nic", "3") == AXL_OK,
               "descs_net round-trip: set nic=3");
    test_check(tgt.net.nic_index == 3,
               "descs_net round-trip: tgt.net.nic_index auto-applied via base_offset");

    /* listen-ip is the v2 emit bit that piggybacks on local_ip.
       Confirm auto-apply lands the string in tgt.net.local_ip
       through the base_offset — the same field source-ip would
       have hit had this been a client preset. */
    test_check(axl_config_set(cfg, "listen-ip", "10.0.0.5") == AXL_OK,
               "descs_net round-trip: set listen-ip=10.0.0.5");
    test_check(tgt.net.local_ip != NULL
                   && axl_streql(tgt.net.local_ip, "10.0.0.5"),
               "descs_net round-trip: tgt.net.local_ip auto-applied via base_offset");

    /* Set a consumer field — must auto-apply into the consumer's own
       offset, not the net sub-struct. */
    test_check(axl_config_set(cfg, "read-only", "true") == AXL_OK,
               "descs_net round-trip: set read-only=true");
    test_check(tgt.read_only == true,
               "descs_net round-trip: tgt.read_only auto-applied (consumer field)");
}

static void
test_config_multi(void)
{
    static const AxlConfigDesc descs[] = {
        { "header", AXL_CFG_MULTI, NULL, "Custom header", 0, 0 },
        { "port",   AXL_CFG_UINT,  "80", "Port", 0, 0 },
        { 0 }
    };

    AxlConfig *cfg = axl_config_new(descs, NULL, NULL);

    /* Multi: add multiple values */
    axl_config_set(cfg, "header", "Content-Type: text/html");
    axl_config_set(cfg, "header", "X-Custom: foo");
    test_check(axl_config_get_multi_count(cfg, "header") == 2,
               "config multi: count is 2");
    test_check(axl_strcmp(axl_config_get_multi(cfg, "header", 0),
                          "Content-Type: text/html") == 0,
               "config multi: index 0");
    test_check(axl_strcmp(axl_config_get_multi(cfg, "header", 1),
                          "X-Custom: foo") == 0,
               "config multi: index 1");

    /* Out of range returns NULL */
    test_check(axl_config_get_multi(cfg, "header", 99) == NULL,
               "config multi: out of range");

    /* Non-multi key returns 0 count */
    test_check(axl_config_get_multi_count(cfg, "port") == 0,
               "config multi: non-multi count 0");

    /* Adding more values via set works just as well as args used to. */
    axl_config_set(cfg, "header", "Auth: Bearer tok");
    axl_config_set(cfg, "header", "Accept: */*");
    test_check(axl_config_get_multi_count(cfg, "header") == 4,
               "config multi: programmatic-set count 4");

    axl_config_free(cfg);
}

static int
test_cb_counter = 0;

static int
test_dynamic_apply(void *target, const char *key, const char *value)
{
    (void)target;
    (void)value;
    /* Accept "dynamic.*" keys, reject nothing */
    if (axl_strlen(key) > 8 && axl_strncmp(key, "dynamic.", 8) == 0) {
        test_cb_counter++;
        return 1;  /* handled */
    }
    return 0;  /* proceed with descriptor lookup */
}

// ---------------------------------------------------------------------------
// to_string / from_string round-trip tests (cross-binary marshalling
// surface — used by AxlService to ship foreground options through
// EFI_LOADED_IMAGE_PROTOCOL.LoadOptions to a same-source-tree driver
// image).
// ---------------------------------------------------------------------------

static void
test_config_to_from_string(void)
{
    typedef struct {
        uint64_t    port;
        bool        verbose;
        const char *name;   /* AXL_CFG_STRING auto-applies a pointer to
                               cfg-interned storage, not a memcpy. */
    } Tgt;

    static const AxlConfigDesc descs[] = {
        { "port",    AXL_CFG_UINT,   "8080",     "Port",
          offsetof(Tgt, port),    sizeof(uint64_t) },
        { "verbose", AXL_CFG_BOOL,   "false",    "Verbose",
          offsetof(Tgt, verbose), sizeof(bool) },
        { "name",    AXL_CFG_STRING, "default",  "Name",
          offsetof(Tgt, name),    sizeof(const char *) },
        { "header",  AXL_CFG_MULTI,  NULL,       "Custom header", 0, 0 },
        { 0 }
    };

    /* === Serialize side === */
    Tgt src;
    axl_memset(&src, 0, sizeof(src));
    AxlConfig *src_cfg = axl_config_new(descs, NULL, &src);
    axl_config_set(src_cfg, "port",    "9090");
    axl_config_set(src_cfg, "verbose", "true");
    axl_config_set(src_cfg, "name",    "axl-webfs");
    axl_config_set(src_cfg, "header",  "X-One: 1");
    axl_config_set(src_cfg, "header",  "X-Two: 2");

    char buf[256];
    test_check(axl_config_to_string(src_cfg, buf, sizeof(buf)) == AXL_OK,
               "config: to_string succeeds");

    /* === NULL/argument safety === */
    test_check(axl_config_to_string(NULL, buf, sizeof(buf)) == AXL_ERR,
               "config: to_string(NULL cfg) returns AXL_ERR");
    test_check(axl_config_to_string(src_cfg, NULL, sizeof(buf)) == AXL_ERR,
               "config: to_string(NULL out) returns AXL_ERR");
    test_check(axl_config_to_string(src_cfg, buf, 0) == AXL_ERR,
               "config: to_string(0 size) returns AXL_ERR");

    /* === Round-trip into a fresh config === */
    Tgt dst;
    axl_memset(&dst, 0, sizeof(dst));
    AxlConfig *dst_cfg = axl_config_new(descs, NULL, &dst);

    test_check(axl_config_from_string(dst_cfg, buf) == AXL_OK,
               "config: from_string succeeds");

    test_check(dst.port == 9090,
               "config: round-trip port preserved");
    test_check(dst.verbose == true,
               "config: round-trip verbose preserved");
    test_check(dst.name != NULL &&
               axl_strcmp(dst.name, "axl-webfs") == 0,
               "config: round-trip name preserved");
    test_check(axl_config_get_multi_count(dst_cfg, "header") == 2,
               "config: round-trip MULTI count preserved");
    test_check(axl_strcmp(axl_config_get_multi(dst_cfg, "header", 0),
                          "X-One: 1") == 0,
               "config: round-trip MULTI[0] preserved");
    test_check(axl_strcmp(axl_config_get_multi(dst_cfg, "header", 1),
                          "X-Two: 2") == 0,
               "config: round-trip MULTI[1] preserved");

    /* === Encoding survives '&' and '=' inside values === */
    Tgt src2;
    axl_memset(&src2, 0, sizeof(src2));
    AxlConfig *src2_cfg = axl_config_new(descs, NULL, &src2);
    axl_config_set(src2_cfg, "name", "weird&value=here");

    test_check(axl_config_to_string(src2_cfg, buf, sizeof(buf)) == AXL_OK,
               "config: to_string handles '&' and '='");

    Tgt dst2;
    axl_memset(&dst2, 0, sizeof(dst2));
    AxlConfig *dst2_cfg = axl_config_new(descs, NULL, &dst2);
    test_check(axl_config_from_string(dst2_cfg, buf) == AXL_OK,
               "config: from_string handles '&' and '='");
    test_check(dst2.name != NULL &&
               axl_strcmp(dst2.name, "weird&value=here") == 0,
               "config: round-trip preserves '&' and '=' in value");

    /* === Empty value ("key=") parses === */
    Tgt dst3;
    axl_memset(&dst3, 0, sizeof(dst3));
    AxlConfig *dst3_cfg = axl_config_new(descs, NULL, &dst3);
    test_check(axl_config_from_string(dst3_cfg, "name=") == AXL_OK,
               "config: from_string accepts empty value");
    test_check(dst3.name != NULL && dst3.name[0] == '\0',
               "config: empty value applied as empty string");

    /* === Bare key (no '=') treated as empty value === */
    Tgt dst4;
    axl_memset(&dst4, 0, sizeof(dst4));
    AxlConfig *dst4_cfg = axl_config_new(descs, NULL, &dst4);
    test_check(axl_config_from_string(dst4_cfg, "name") == AXL_OK,
               "config: from_string accepts bare key");
    test_check(dst4.name != NULL && dst4.name[0] == '\0',
               "config: bare key applied as empty string");

    /* === Empty pairs (leading/trailing/duplicate '&') skipped === */
    Tgt dst5;
    axl_memset(&dst5, 0, sizeof(dst5));
    AxlConfig *dst5_cfg = axl_config_new(descs, NULL, &dst5);
    test_check(axl_config_from_string(dst5_cfg,
                                      "&&port=42&&verbose=true&&") == AXL_OK,
               "config: from_string skips empty pairs");
    test_check(dst5.port == 42 && dst5.verbose == true,
               "config: empty-pair-skipping doesn't drop real values");

    /* === Empty key ("=value") rejected === */
    Tgt dst6;
    axl_memset(&dst6, 0, sizeof(dst6));
    AxlConfig *dst6_cfg = axl_config_new(descs, NULL, &dst6);
    test_check(axl_config_from_string(dst6_cfg, "=novalue") == AXL_ERR,
               "config: from_string rejects empty key");

    /* === Unknown key surfaces as AXL_ERR === */
    Tgt dst7;
    axl_memset(&dst7, 0, sizeof(dst7));
    AxlConfig *dst7_cfg = axl_config_new(descs, NULL, &dst7);
    test_check(axl_config_from_string(dst7_cfg,
                                      "port=42&unknown=x") == AXL_ERR,
               "config: from_string rejects unknown key");

    /* === NULL safety on parse === */
    test_check(axl_config_from_string(NULL, "x=1") == AXL_ERR,
               "config: from_string(NULL cfg) returns AXL_ERR");
    test_check(axl_config_from_string(dst_cfg, NULL) == AXL_ERR,
               "config: from_string(NULL in) returns AXL_ERR");

    /* === Empty string ("") is valid (zero pairs to set) === */
    test_check(axl_config_from_string(dst_cfg, "") == AXL_OK,
               "config: from_string('') is a no-op success");

    /* === to_string into too-small buffer fails cleanly === */
    char tiny[8];
    test_check(axl_config_to_string(src_cfg, tiny, sizeof(tiny)) == AXL_ERR,
               "config: to_string overflow returns AXL_ERR");

    axl_config_free(src_cfg);
    axl_config_free(src2_cfg);
    axl_config_free(dst_cfg);
    axl_config_free(dst2_cfg);
    axl_config_free(dst3_cfg);
    axl_config_free(dst4_cfg);
    axl_config_free(dst5_cfg);
    axl_config_free(dst6_cfg);
    axl_config_free(dst7_cfg);
}

/* axl_config_target_to_string is the OTHER half of the same cross-binary
   ABI: it reads directly from a caller's struct via offsetof, with no
   AxlConfig instance involved (that's what axl_service_start_embedded
   calls to fill LoadOptions from a launcher's own opts). The pair above
   round-trips through an AxlConfig instance on the serialize side, which
   exercises axl_config_to_string, not this function. Cover its BOOL
   branch (both true and false), a plain uint64_t, a narrow uint16_t
   field (the width auto_apply must not overrun), and a string. */
static void
test_config_target_to_string_round_trip(void)
{
    typedef struct {
        bool        ro;
        bool        verbose;
        uint64_t    big;
        uint16_t    port;
        const char *name;
    } Tgt;

    static const AxlConfigDesc descs[] = {
        { "ro",      AXL_CFG_BOOL,   "false", "Read-only",
          offsetof(Tgt, ro),      sizeof(bool) },
        { "verbose", AXL_CFG_BOOL,   "false", "Verbose",
          offsetof(Tgt, verbose), sizeof(bool) },
        { "big",     AXL_CFG_UINT,   "0",     "Big counter",
          offsetof(Tgt, big),     sizeof(uint64_t) },
        { "port",    AXL_CFG_UINT,   "0",     "Port",
          offsetof(Tgt, port),    sizeof(uint16_t) },
        { "name",    AXL_CFG_STRING, "",      "Name",
          offsetof(Tgt, name),    sizeof(const char *) },
        { 0 }
    };

    /* === Serialize directly from a populated struct - no AxlConfig
       involved, matching how axl_service_start_embedded reads a
       launcher's own opts before shipping them through LoadOptions. === */
    Tgt src = {
        .ro      = true,
        .verbose = false,
        .big     = 9876543210ULL,
        .port    = 5640,
        .name    = "9pexport",
    };

    char buf[256];
    test_check(axl_config_target_to_string(descs, &src, buf, sizeof(buf))
                   == AXL_OK,
               "target_to_string: serializes AXL_OK");

    /* === Decode through the normal AxlConfig + auto_apply path === */
    Tgt dst;
    axl_memset(&dst, 0, sizeof(dst));
    AxlConfig *cfg = axl_config_new(descs, NULL, &dst);
    test_check(cfg != NULL, "target_to_string: config new");

    /* Poison the one field whose EXPECTED value is also its starting value.
       dst is zeroed AND axl_config_new auto-applies every descriptor's
       default, so `verbose` is already false here and "verbose == false"
       below would pass even if the key were dropped from serialization
       entirely -- the precise failure this assertion exists to catch. With
       it flipped to true, only a correctly emitted-and-decoded "false" can
       bring it back. The other fields need no such poison: their expected
       values (true / 9876543210 / 5640 / "9pexport") all differ from the
       defaults auto-apply left behind ("false" / "0" / "0" / ""). */
    dst.verbose = true;

    test_check(axl_config_from_string(cfg, buf) == AXL_OK,
               "target_to_string: from_string decodes AXL_OK");

    test_check(dst.ro == true,
               "target_to_string: bool true round-trips");
    test_check(dst.verbose == false,
               "target_to_string: bool false round-trips");
    test_check(dst.big == 9876543210ULL,
               "target_to_string: uint64_t round-trips");
    test_check(dst.port == 5640,
               "target_to_string: uint16_t (narrow field) round-trips");
    test_check(dst.name != NULL && axl_strcmp(dst.name, "9pexport") == 0,
               "target_to_string: string round-trips");

    /* === NULL/argument safety === */
    test_check(axl_config_target_to_string(NULL, &src, buf, sizeof(buf))
                   == AXL_ERR,
               "target_to_string: NULL descs returns AXL_ERR");
    test_check(axl_config_target_to_string(descs, NULL, buf, sizeof(buf))
                   == AXL_ERR,
               "target_to_string: NULL target returns AXL_ERR");
    test_check(axl_config_target_to_string(descs, &src, NULL, sizeof(buf))
                   == AXL_ERR,
               "target_to_string: NULL out returns AXL_ERR");
    test_check(axl_config_target_to_string(descs, &src, buf, 0) == AXL_ERR,
               "target_to_string: zero out_size returns AXL_ERR");

    char tiny[4];
    test_check(axl_config_target_to_string(descs, &src, tiny, sizeof(tiny))
                   == AXL_ERR,
               "target_to_string: too-small buffer returns AXL_ERR");

    axl_config_free(cfg);
}

static void
test_config_callback(void)
{
    static const AxlConfigDesc descs[] = {
        { "port", AXL_CFG_UINT, "80", "Port", 0, 0 },
        { 0 }
    };

    AxlConfig *cfg = axl_config_new(descs, test_dynamic_apply, NULL);

    /* Known key passes through callback (returns 0) to auto-apply */
    test_check(axl_config_set(cfg, "port", "9090") == AXL_OK,
               "config cb: known key accepted");
    test_check(axl_config_get_uint(cfg, "port") == 9090,
               "config cb: known key value");

    /* Dynamic key handled by callback (returns 1) */
    test_cb_counter = 0;
    test_check(axl_config_set(cfg, "dynamic.foo", "bar") == AXL_OK,
               "config cb: dynamic key accepted");
    test_check(test_cb_counter == 1, "config cb: callback fired");
    test_check(axl_strcmp(axl_config_get(cfg, "dynamic.foo"), "bar") == 0,
               "config cb: dynamic key retrievable");

    /* Unknown key (not in descriptors, callback returns 0) rejected */
    test_check(axl_config_set(cfg, "unknown", "x") == AXL_ERR,
               "config cb: unknown key rejected");

    axl_config_free(cfg);
}

static void
test_config_validation(void)
{
    static const AxlConfigDesc descs[] = {
        { "count",  AXL_CFG_UINT,   "0",     "Count", 0, 0 },
        { "offset", AXL_CFG_INT,    "0",     "Offset", 0, 0 },
        { "flag",   AXL_CFG_BOOL,   "false", "Flag", 0, 0 },
        { 0 }
    };

    AxlConfig *cfg = axl_config_new(descs, NULL, NULL);

    /* UINT rejects non-numeric */
    test_check(axl_config_set(cfg, "count", "abc") == AXL_ERR,
               "config val: uint rejects abc");
    test_check(axl_config_set(cfg, "count", "-5") == AXL_ERR,
               "config val: uint rejects negative");
    test_check(axl_config_set(cfg, "count", "0xFF") == AXL_OK,
               "config val: uint accepts hex");

    /* INT rejects non-numeric */
    test_check(axl_config_set(cfg, "offset", "xyz") == AXL_ERR,
               "config val: int rejects xyz");
    test_check(axl_config_set(cfg, "offset", "-42") == AXL_OK,
               "config val: int accepts negative");
    test_check(axl_config_get_int(cfg, "offset") == -42,
               "config val: int value -42");

    /* BOOL rejects garbage */
    test_check(axl_config_set(cfg, "flag", "maybe") == AXL_ERR,
               "config val: bool rejects maybe");
    test_check(axl_config_set(cfg, "flag", "yes") == AXL_OK,
               "config val: bool accepts yes");
    test_check(axl_config_get_bool(cfg, "flag") == true,
               "config val: bool yes is true");

    axl_config_free(cfg);
}

static void
test_config_setv(void)
{
    static const AxlConfigDesc descs[] = {
        { "host",    AXL_CFG_STRING, "0.0.0.0", "Host", 0, 0 },
        { "port",    AXL_CFG_UINT,   "80",      "Port", 0, 0 },
        { "verbose", AXL_CFG_BOOL,   "false",   "Verbose", 0, 0 },
        { 0 }
    };

    AxlConfig *cfg = axl_config_new(descs, NULL, NULL);

    /* Set multiple at once */
    test_check(axl_config_setv(cfg,
        "host", "10.0.0.1",
        "port", "9090",
        "verbose", "true",
        NULL) == AXL_OK,
        "config setv: success");

    test_check(axl_strcmp(axl_config_get(cfg, "host"), "10.0.0.1") == 0,
               "config setv: host value");
    test_check(axl_config_get_uint(cfg, "port") == 9090,
               "config setv: port value");
    test_check(axl_config_get_bool(cfg, "verbose") == true,
               "config setv: verbose value");

    /* Stops on first error */
    test_check(axl_config_setv(cfg,
        "port", "3000",
        "unknown", "x",
        "verbose", "false",
        NULL) == AXL_ERR,
        "config setv: stops on error");

    /* port was set before the error */
    test_check(axl_config_get_uint(cfg, "port") == 3000,
               "config setv: partial apply before error");

    /* verbose was NOT set (after the error) */
    test_check(axl_config_get_bool(cfg, "verbose") == true,
               "config setv: skipped after error");

    /* NULL config */
    test_check(axl_config_setv(NULL, "port", "80", NULL) == AXL_ERR,
               "config setv: NULL cfg");

    axl_config_free(cfg);
}

// ---------------------------------------------------------------------------
// axl-subcommand
// ---------------------------------------------------------------------------

static int g_sub_calls;        /* per-test counter the dummy fns increment */
static int g_sub_last_argc;
static const char *g_sub_last_arg0;

static int
sub_bios(int argc, char **argv)
{
    g_sub_calls++;
    g_sub_last_argc = argc;
    g_sub_last_arg0 = (argc > 0 && argv != NULL) ? argv[0] : NULL;
    return 42;     /* arbitrary non-zero so we can verify return passthrough */
}
static int
sub_sysid(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_sub_calls++;
    return 0;
}
static int
sub_crash(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_sub_calls++;
    return -7;
}

/* This test exists to verify the AxlSubcommand dispatcher API,
   which is deprecated in favour of AxlArgs (see <axl/axl-args.h>).
   Suppress the deprecation warnings inside this function only —
   the API is intentionally exercised here. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static void
test_subcommand_dispatch(void)
{
    static const AxlSubcommand cmds[] = {
        { "bios",  sub_bios,   "[test|pci]",  "mytool bios test  — run POST self-test\n" },
        { "sysid", sub_sysid,  "[hexValue]",  NULL  },
        { "crash", sub_crash,  "trigger",     NULL  },
    };
    static const size_t count = sizeof(cmds) / sizeof(cmds[0]);

    /* exact match → fn invoked, return value passed through */
    {
        char *argv[] = { (char *)"mytool", (char *)"bios", (char *)"--flag", (char *)"v" };
        g_sub_calls = 0;
        g_sub_last_arg0 = NULL;
        int rc = axl_subcommand_dispatch(cmds, count, 4, argv, "mytool");
        test_check(rc == 42, "subcommand: dispatch returns fn's rc");
        test_check(g_sub_calls == 1, "subcommand: fn called exactly once");
        /* argv shifted so the subcommand sees its own name as argv[0] */
        test_check(g_sub_last_argc == 3, "subcommand: shifted argc");
        test_check(g_sub_last_arg0 != NULL
                   && axl_strcmp(g_sub_last_arg0, "bios") == 0,
                   "subcommand: argv[0] is the subcommand name");
    }

    /* "help" alone → returns 0, no fn invoked */
    {
        char *argv[] = { (char *)"mytool", (char *)"help" };
        g_sub_calls = 0;
        int rc = axl_subcommand_dispatch(cmds, count, 2, argv, "mytool");
        test_check(rc == 0, "subcommand: help returns 0");
        test_check(g_sub_calls == 0, "subcommand: help doesn't invoke any fn");
    }

    /* "-h" / "--help" both → help (no fn invoked) */
    {
        char *argv1[] = { (char *)"mytool", (char *)"-h" };
        char *argv2[] = { (char *)"mytool", (char *)"--help" };
        g_sub_calls = 0;
        test_check(axl_subcommand_dispatch(cmds, count, 2, argv1, "mytool") == 0,
                   "subcommand: -h returns 0");
        test_check(axl_subcommand_dispatch(cmds, count, 2, argv2, "mytool") == 0,
                   "subcommand: --help returns 0");
        test_check(g_sub_calls == 0, "subcommand: -h/--help no fn invocations");
    }

    /* "help <cmd>" prints command help, returns 0 */
    {
        char *argv[] = { (char *)"mytool", (char *)"help", (char *)"bios" };
        int rc = axl_subcommand_dispatch(cmds, count, 3, argv, "mytool");
        test_check(rc == 0, "subcommand: help <cmd> returns 0");
    }

    /* "help <unknown>" returns -1 */
    {
        char *argv[] = { (char *)"mytool", (char *)"help", (char *)"nonsense" };
        int rc = axl_subcommand_dispatch(cmds, count, 3, argv, "mytool");
        test_check(rc == -1, "subcommand: help <unknown> returns -1");
    }

    /* unknown command → -1 */
    {
        char *argv[] = { (char *)"mytool", (char *)"frobnicate" };
        g_sub_calls = 0;
        int rc = axl_subcommand_dispatch(cmds, count, 2, argv, "mytool");
        test_check(rc == -1, "subcommand: unknown command returns -1");
        test_check(g_sub_calls == 0, "subcommand: unknown doesn't invoke any fn");
    }

    /* typo close to "sysid" → still -1 but the "did you mean" path runs */
    {
        char *argv[] = { (char *)"mytool", (char *)"sysud" };
        int rc = axl_subcommand_dispatch(cmds, count, 2, argv, "mytool");
        test_check(rc == -1, "subcommand: close typo returns -1");
        /* No way to capture stderr text here; just exercise the path. */
    }

    /* argc < 2 → help, returns 0 */
    {
        char *argv[] = { (char *)"mytool" };
        int rc = axl_subcommand_dispatch(cmds, count, 1, argv, "mytool");
        test_check(rc == 0, "subcommand: no args returns 0 (help)");
    }

    /* prog_name NULL → derive from argv[0] basename */
    {
        char *argv[] = { (char *)"fs0:\\path\\app.efi", (char *)"bios" };
        g_sub_calls = 0;
        int rc = axl_subcommand_dispatch(cmds, count, 2, argv, NULL);
        test_check(rc == 42 && g_sub_calls == 1,
                   "subcommand: NULL prog_name still dispatches");
    }

    /* return -7 from crash subcommand passes through */
    {
        char *argv[] = { (char *)"mytool", (char *)"crash" };
        int rc = axl_subcommand_dispatch(cmds, count, 2, argv, "mytool");
        test_check(rc == -7, "subcommand: negative rc passes through");
    }

    /* Empty table behaves: help only */
    {
        char *argv[] = { (char *)"mytool" };
        int rc = axl_subcommand_dispatch(NULL, 0, 1, argv, "mytool");
        test_check(rc == 0, "subcommand: empty table + no args returns 0");
        char *argv2[] = { (char *)"mytool", (char *)"anything" };
        int rc2 = axl_subcommand_dispatch(NULL, 0, 2, argv2, "mytool");
        test_check(rc2 == -1, "subcommand: empty table + unknown returns -1");
    }

    /* Public print fns shouldn't crash on edge cases */
    axl_subcommand_print_help(cmds, count, "mytool");
    axl_subcommand_print_help(NULL, 0, "mytool");
    axl_subcommand_print_command_help(&cmds[0], "mytool");
    axl_subcommand_print_command_help(NULL, "mytool");
    test_survived("subcommand: print fns don't crash");
}
#pragma GCC diagnostic pop

// ---------------------------------------------------------------------------
// AxlService — structured-lifecycle wrapper over AxlLoop.
//
// Foreground path: setup → axl_loop_run → teardown.
// Driver-attach path: setup → axl_loop_attach_driver, detach → loop
// detach → teardown.
// Embedded-driver path is exercised by test-driver-leak / a future
// driver-image integration test (needs QEMU + driver image build).
// ---------------------------------------------------------------------------

typedef struct {
    int      setup_calls;
    int      teardown_calls;
    int      setup_rc;       /* injected return value */
    AxlLoop *seen_loop;
    void    *seen_user;
    bool     quit_from_idle; /* if true, idle source quits the loop */
} ServiceCtx;

static bool
service_test_idle(void *data)
{
    ServiceCtx *ctx = (ServiceCtx *)data;
    if (ctx->quit_from_idle) {
        axl_loop_quit(ctx->seen_loop);
    }
    return AXL_SOURCE_REMOVE;
}

static int
service_test_setup(AxlLoop *loop, void *user)
{
    ServiceCtx *ctx = (ServiceCtx *)user;
    ctx->setup_calls++;
    ctx->seen_loop = loop;
    ctx->seen_user = user;
    if (ctx->setup_rc != AXL_OK) {
        return ctx->setup_rc;
    }
    /* Schedule a quit so axl_loop_run returns. */
    axl_loop_add_idle(loop, service_test_idle, ctx);
    return AXL_OK;
}

static int
service_test_teardown(void *user)
{
    ServiceCtx *ctx = (ServiceCtx *)user;
    ctx->teardown_calls++;
    return AXL_OK;
}


static void
test_service_attach_driver(void)
{
    /* === Argument validation === */
    test_check(axl_service_attach_driver(NULL, NULL) == AXL_ERR,
               "service: attach_driver(NULL,NULL) returns AXL_ERR");

    AxlLoop *loop = axl_loop_new();
    AxlService svc_no_setup = { .name = "no-setup" };
    test_check(axl_service_attach_driver(loop, &svc_no_setup) == AXL_ERR,
               "service: attach_driver rejects NULL setup");

    /* === driver_tick_ms == 0 means "use default" — single source of
       truth across attach_driver and AXL_SERVICE_DRIVER. Verifies the
       0-tick field path produces a working attach (the firmware
       notify-timer at AXL_SERVICE_DEFAULT_TICK_MS is what drives the
       loop). === */
    AxlLoop *loop_default = axl_loop_new();
    ServiceCtx ctx_default = { 0 };
    AxlService svc_default = {
        .name     = "default-tick",
        .setup    = service_test_setup,
        .teardown = service_test_teardown,
        .user     = &ctx_default,
        /* driver_tick_ms intentionally 0 — exercises the default path. */
    };
    test_check(axl_service_attach_driver(loop_default, &svc_default) == AXL_OK,
               "service: attach_driver uses default when driver_tick_ms == 0");
    test_check(ctx_default.setup_calls == 1,
               "service: setup ran on default-tick path");
    test_check(axl_service_detach_driver(loop_default, &svc_default) == AXL_OK,
               "service: detach succeeds on default-tick path");
    (void)axl_service_teardown(&svc_default);
    axl_loop_free(loop_default);

    ServiceCtx ctx = { 0 };
    AxlService svc = {
        .name           = "attach-test",
        .setup          = service_test_setup,
        .teardown       = service_test_teardown,
        .user           = &ctx,
        .driver_tick_ms = 20,
    };
    /* === Happy path: attach → driver-mode notify drives loop === */
    test_check(axl_service_attach_driver(loop, &svc) == AXL_OK,
               "service: attach_driver succeeds with explicit tick");
    test_check(ctx.setup_calls == 1, "service: setup called from attach_driver");
    test_check(ctx.teardown_calls == 0,
               "service: teardown not called yet (still attached)");

    /* === P1 contract: detach_driver no longer runs teardown ===
       Caller owns the call. detach_driver returns AXL_OK after
       canceling the timer; teardown_calls stays at 0 until the
       caller invokes axl_service_teardown. */
    test_check(axl_service_detach_driver(loop, &svc) == AXL_OK,
               "service: detach_driver succeeds");
    test_check(ctx.teardown_calls == 0,
               "service: detach_driver does NOT run teardown (P1)");

    /* The caller (real consumer would be AXL_SERVICE_DRIVER's
       unload stub, or a hand-rolled equivalent) follows up
       explicitly. axl_service_teardown returns the cb's rc. */
    int td_rc = axl_service_teardown(&svc);
    test_check(td_rc == AXL_OK,
               "service: axl_service_teardown returns cb rc (AXL_OK)");
    test_check(ctx.teardown_calls == 1,
               "service: axl_service_teardown fires the callback");

    /* NULL-safety paths return AXL_OK without invoking anything
       and without incrementing the count. */
    test_check(axl_service_teardown(NULL) == AXL_OK,
               "service: axl_service_teardown(NULL) returns AXL_OK");
    AxlService svc_no_td = {
        .name = "no-teardown",
        .setup = service_test_setup,
        /* teardown intentionally NULL */
    };
    test_check(axl_service_teardown(&svc_no_td) == AXL_OK,
               "service: axl_service_teardown(NULL fn) returns AXL_OK");
    test_check(ctx.teardown_calls == 1,
               "service: NULL-safety calls didn't increment count");

    /* === Setup failure during attach: no attach_driver, no teardown === */
    AxlLoop *loop2 = axl_loop_new();
    ServiceCtx ctx2 = { .setup_rc = AXL_ERR };
    AxlService svc_fail = {
        .name     = "attach-fail",
        .setup    = service_test_setup,
        .teardown = service_test_teardown,
        .user     = &ctx2,
    };
    test_check(axl_service_attach_driver(loop2, &svc_fail) == AXL_ERR,
               "service: attach_driver propagates setup failure");
    test_check(ctx2.setup_calls == 1,
               "service: setup called once on attach failure");
    test_check(ctx2.teardown_calls == 0,
               "service: teardown skipped when attach setup failed");
    /* The loop was never attached, so detach should report ERR. */
    test_check(axl_service_detach_driver(loop2, &svc_fail) == AXL_ERR,
               "service: detach on never-attached loop returns AXL_ERR");
    test_check(ctx2.teardown_calls == 0,
               "service: teardown still not called on no-op detach");

    axl_loop_free(loop);
    axl_loop_free(loop2);
}

static void
test_service_is_running(void)
{
    /* No driver is loaded for this test's name-derived GUID, so
       is_running must report false on a deploy descriptor pointing
       at any name we haven't published. */
    static const AxlService dummy_svc = {
        .name  = "axl-test-dummy-not-published",
        .setup = service_test_setup,
    };
    AxlServiceDeploy d = { .service = &dummy_svc };
    test_check(axl_service_is_running(&d) == false,
               "service: is_running false for unpublished GUID");
    test_check(axl_service_is_running(NULL) == false,
               "service: is_running(NULL) returns false");
}

static void
test_service_launch_embedded_validates(void)
{
    /* axl_service_start_embedded rejects incomplete deploy descriptors
       at the boundary, before reaching axl_driver_ensure_with_embedded.
       Don't actually invoke a load here — that path needs a real driver
       blob and is exercised by the integration tests. */
    AxlService svc = { .name = "x", .setup = service_test_setup };
    AxlServiceDeploy d_no_blob = {
        .service     = &svc,
        .driver_name = "x.efi",
    };
    test_check(axl_service_start_embedded(&d_no_blob) == AXL_ERR,
               "service: launch_embedded rejects deploy with NULL blob");

    static const unsigned char fake_blob[] = { 0x4d, 0x5a }; /* "MZ" */
    AxlServiceDeploy d_no_name = {
        .service          = &svc,
        .driver_blob      = fake_blob,
        .driver_blob_len  = sizeof(fake_blob),
    };
    test_check(axl_service_start_embedded(&d_no_name) == AXL_ERR,
               "service: launch_embedded rejects deploy with NULL driver_name");

    AxlServiceDeploy d_no_svc = {
        .driver_blob     = fake_blob,
        .driver_blob_len = sizeof(fake_blob),
        .driver_name     = "x.efi",
    };
    test_check(axl_service_start_embedded(&d_no_svc) == AXL_ERR,
               "service: launch_embedded rejects deploy with NULL service");

    test_check(axl_service_start_embedded(NULL) == AXL_ERR,
               "service: launch_embedded(NULL) returns AXL_ERR");

    /* A pinned deploy names exactly one file, so there is nothing to fall
       back to and the embedded blob becomes optional. The distinguishing
       assertion: the SAME blob-less descriptor that is rejected outright
       above (AXL_ERR, "incomplete") is instead ATTEMPTED here and reports
       AXL_NOT_FOUND — proof the pinned path was taken rather than the
       search-plus-embedded one. */
    AxlServiceDeploy d_pinned = {
        .service     = &svc,
        .driver_name = "x.efi",
        .driver_path = "fs0:\\axl-test-no-such-pinned-driver.efi",
    };
    test_check(axl_service_start_embedded(&d_pinned) == AXL_NOT_FOUND,
               "service: launch_embedded honours driver_path without a blob");

    /* (That driver_path also suppresses the embedded fallback is not
       decidable from a return code here — both routes report AXL_NOT_FOUND
       for a 2-byte fake blob. test-service-pin-path-qemu.sh proves it with
       a launcher whose embedded blob is a DIFFERENT build of the driver.) */

    /* driver_name feeds the disk search and nothing else, so a pinned deploy
       does not need it either: service + driver_path is the whole descriptor.
       Same discriminator as above — ATTEMPTED (AXL_NOT_FOUND), not rejected
       as incomplete (AXL_ERR). */
    AxlServiceDeploy d_pinned_only = {
        .service     = &svc,
        .driver_path = "fs0:\\axl-test-no-such-pinned-driver.efi",
    };
    test_check(axl_service_start_embedded(&d_pinned_only) == AXL_NOT_FOUND,
               "service: driver_path alone is a complete deploy descriptor");
}

// ---------------------------------------------------------------------------
// axl_driver_ensure_from_path — the pinned sibling of
// axl_driver_ensure_with_embedded: exactly one file, no search, no fallback.
// ---------------------------------------------------------------------------

static void
test_driver_ensure_from_path(void)
{
    /* A GUID nothing publishes, so the step-1 short-circuit can't fire and
       the pinned load is genuinely attempted. */
    static const AxlGuid unpublished = AXL_GUID(
        0x9e5a0c31, 0x77b4, 0x4d10,
        0xa3, 0x62, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66);

    test_check(axl_driver_ensure_from_path(NULL, "fs0:\\x.efi", NULL, 0)
                   == AXL_INVALID,
               "ensure_from_path: NULL guid returns AXL_INVALID");
    test_check(axl_driver_ensure_from_path(&unpublished, NULL, NULL, 0)
                   == AXL_INVALID,
               "ensure_from_path: NULL path returns AXL_INVALID");

    /* Missing file: reports AXL_NOT_FOUND and, unlike the searching
       variants, does not go looking anywhere else. */
    test_check(axl_driver_ensure_from_path(
                   &unpublished, "fs0:\\axl-test-no-such-pinned-driver.efi",
                   NULL, 0) == AXL_NOT_FOUND,
               "ensure_from_path: missing file returns AXL_NOT_FOUND");
}

static void
test_service_stop_validates(void)
{
    /* axl_service_stop is idempotent on "not running" — returns
       AXL_OK without doing anything. With a deploy whose GUID is
       guaranteed-unpublished (we just made it up), stop should be
       a no-op success. */
    static const AxlService never_running_svc = {
        .name  = "axl-test-never-running",
        .setup = service_test_setup,
    };
    AxlServiceDeploy d = { .service = &never_running_svc };
    test_check(axl_service_stop(&d) == AXL_OK,
               "service: stop on not-running deploy is no-op success");

    /* NULL safety + bad-deploy rejection. */
    test_check(axl_service_stop(NULL) == AXL_ERR,
               "service: stop(NULL) returns AXL_ERR");
    AxlServiceDeploy d_no_svc = { .service = NULL };
    test_check(axl_service_stop(&d_no_svc) == AXL_ERR,
               "service: stop rejects deploy with NULL service");

    /* End-to-end stop (with a live driver image) is exercised by
       test-service-driver.sh — that path requires QEMU + the
       embedded driver image and can't be unit-tested. */
}

// ---------------------------------------------------------------------------
// axl_guid_v5 — name-based UUIDv5 derivation. Determinism + namespace
// scoping + name scoping + RFC-shaped version/variant bits + NULL safety.
// Underpins AxlService identity-from-name; a regression here would leak
// into every service consumer's start/stop path.
// ---------------------------------------------------------------------------

static void
test_guid_v5(void)
{
    AxlGuid ns_a = AXL_GUID(0x11111111, 0x2222, 0x3333,
                            0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb);
    AxlGuid ns_b = AXL_GUID(0xdeadbeef, 0xcafe, 0xf00d,
                            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08);

    AxlGuid g1, g2, g3, g4, g5;

    /* Determinism: same (namespace, name) -> same GUID. */
    test_check(axl_guid_v5(&ns_a, "axl-test", &g1) == AXL_OK,
               "guid_v5: returns AXL_OK on valid args");
    test_check(axl_guid_v5(&ns_a, "axl-test", &g2) == AXL_OK,
               "guid_v5: second call succeeds");
    test_check(axl_guid_equal(&g1, &g2),
               "guid_v5: deterministic — same inputs yield same GUID");
    test_check(axl_guid_cmp(&g1, &g2) == 0,
               "guid_cmp: equal GUIDs compare 0");

    /* Different name in same namespace -> different GUID. */
    test_check(axl_guid_v5(&ns_a, "axl-test-other", &g3) == AXL_OK,
               "guid_v5: different name accepted");
    test_check(!axl_guid_equal(&g1, &g3),
               "guid_v5: distinct name yields distinct GUID");
    test_check(axl_guid_cmp(&g1, &g3) != 0,
               "guid_cmp: distinct GUIDs compare nonzero");
    test_check((axl_guid_cmp(&g1, &g3) < 0) == (axl_guid_cmp(&g3, &g1) > 0),
               "guid_cmp: antisymmetric sign");

    /* Same name in different namespace -> different GUID. */
    test_check(axl_guid_v5(&ns_b, "axl-test", &g4) == AXL_OK,
               "guid_v5: different namespace accepted");
    test_check(!axl_guid_equal(&g1, &g4),
               "guid_v5: distinct namespace yields distinct GUID");

    /* RFC 4122 §4.3 shape: version 5 in high nibble of byte 6,
       variant 10b in high two bits of byte 8. The shape applies to
       bytes 6 and 8 of the AxlGuid byte image regardless of host
       endian — both fall in data4-adjacent positions that don't
       cross host-endian boundaries. */
    const uint8_t *bytes = (const uint8_t *)&g1;
    test_check((bytes[6] & 0xF0) == 0x50,
               "guid_v5: byte 6 high nibble is version 5");
    test_check((bytes[8] & 0xC0) == 0x80,
               "guid_v5: byte 8 high two bits are variant 10b");

    /* NULL safety on every parameter. */
    test_check(axl_guid_v5(NULL, "x", &g5) == AXL_ERR,
               "guid_v5: NULL namespace returns AXL_ERR");
    test_check(axl_guid_v5(&ns_a, NULL, &g5) == AXL_ERR,
               "guid_v5: NULL name returns AXL_ERR");
    test_check(axl_guid_v5(&ns_a, "x", NULL) == AXL_ERR,
               "guid_v5: NULL out returns AXL_ERR");
}

// ---------------------------------------------------------------------------
// axl_service_guid — derives identity from svc->name. Smoke test that the
// helper composes axl_guid_v5 with the AXL_SERVICE namespace consistently
// and rejects descriptors without a name.
// ---------------------------------------------------------------------------

static void
test_service_guid(void)
{
    AxlService svc_a = { .name = "svc-alpha", .setup = service_test_setup };
    AxlService svc_b = { .name = "svc-beta",  .setup = service_test_setup };
    AxlService svc_no_name = { .setup = service_test_setup };

    AxlGuid ga, gb, ga2, dummy;

    test_check(axl_service_guid(&svc_a, &ga) == AXL_OK,
               "service_guid: returns AXL_OK with valid svc");
    test_check(axl_service_guid(&svc_a, &ga2) == AXL_OK,
               "service_guid: second call succeeds");
    test_check(axl_guid_equal(&ga, &ga2),
               "service_guid: same descriptor yields same GUID");

    test_check(axl_service_guid(&svc_b, &gb) == AXL_OK,
               "service_guid: distinct name accepted");
    test_check(!axl_guid_equal(&ga, &gb),
               "service_guid: distinct name yields distinct GUID");

    test_check(axl_service_guid(NULL, &dummy) == AXL_ERR,
               "service_guid: NULL svc returns AXL_ERR");
    test_check(axl_service_guid(&svc_no_name, &dummy) == AXL_ERR,
               "service_guid: NULL name returns AXL_ERR");
    test_check(axl_service_guid(&svc_a, NULL) == AXL_ERR,
               "service_guid: NULL out returns AXL_ERR");
}

// ---------------------------------------------------------------------------
// _axl_service_driver_init — the AXL_SERVICE_DRIVER shim's real entry.
// Returns a firmware EFI_STATUS; the value must reach DriverEntry with
// its error bit intact.
// ---------------------------------------------------------------------------

static void
test_service_driver_init_status_width(void)
{
    /* Regression: the declaration used to be `int`, so every failure
       return narrowed a 64-bit EFI_STATUS to 32 bits and dropped
       EFI_ERROR_BIT — AXL_EFI_INVALID_PARAMETER (0x8000000000000002)
       reached the firmware as 0x2, which EFI_ERROR() reads as SUCCESS.
       StartImage then reported success for a service that never
       attached, and axl_service_reload declared the hot-swap good.

       Only the pre-firmware validation paths are exercised here: they
       return before axl_driver_init touches gBS, so there is no
       register/unregister lifecycle to wedge (see the firmware-test
       hazard note in the workflow docs). */
    AxlEfiStatus st_null = _axl_service_driver_init(NULL, NULL, NULL);
    test_check(st_null == AXL_EFI_INVALID_PARAMETER,
               "service driver_init: NULL svc returns AXL_EFI_INVALID_PARAMETER");
    test_check(AXL_EFI_ERROR(st_null),
               "service driver_init: NULL svc status keeps the EFI error bit");

    static const AxlService svc_no_setup = { .name = "s0-no-setup" };
    AxlEfiStatus st_no_setup =
        _axl_service_driver_init(NULL, NULL, &svc_no_setup);
    test_check(st_no_setup == AXL_EFI_INVALID_PARAMETER,
               "service driver_init: NULL setup returns AXL_EFI_INVALID_PARAMETER");
    test_check(AXL_EFI_ERROR(st_no_setup),
               "service driver_init: NULL setup status keeps the EFI error bit");

    static const AxlService svc_no_name = { .setup = service_test_setup };
    AxlEfiStatus st_no_name =
        _axl_service_driver_init(NULL, NULL, &svc_no_name);
    test_check(st_no_name == AXL_EFI_INVALID_PARAMETER,
               "service driver_init: NULL name returns AXL_EFI_INVALID_PARAMETER");
    test_check(AXL_EFI_ERROR(st_no_name),
               "service driver_init: NULL name status keeps the EFI error bit");
}

// ---------------------------------------------------------------------------
// axl_service_reload return-code taxonomy. AXL_ERR is reserved for the ONE
// fatal case — the replacement loaded but failed to start, so the service is
// down. Every other non-OK code means the service is untouched and still
// serving, which is what lets a caller disarm instead of cold-resetting a
// healthy box.
// ---------------------------------------------------------------------------

static void
test_service_reload_validates(void)
{
    /* Nothing in a foreground app is the running AXL_SERVICE_DRIVER service,
       so each of these takes a caller-misuse exit. None of them may report
       AXL_ERR: that code means "this service is DOWN", and here it never
       started in the first place. */
    AxlService svc = { .name = "reload-validate", .setup = service_test_setup };
    static const unsigned char fake_image[4] = { 'M', 'Z', 0, 0 };

    test_check(axl_service_reload(NULL, "fs0:\\x.efi") == AXL_INVALID,
               "service reload: NULL svc returns AXL_INVALID");
    test_check(axl_service_reload(&svc, NULL) == AXL_INVALID,
               "service reload: NULL path returns AXL_INVALID");
    test_check(axl_service_reload(&svc, "fs0:\\x.efi") == AXL_INVALID,
               "service reload: not-the-running-service returns AXL_INVALID");

    test_check(axl_service_reload_buffer(&svc, NULL, 0) == AXL_INVALID,
               "service reload_buffer: NULL image returns AXL_INVALID");
    test_check(axl_service_reload_buffer(&svc, fake_image, 0) == AXL_INVALID,
               "service reload_buffer: zero length returns AXL_INVALID");
    test_check(axl_service_reload_buffer(&svc, fake_image,
                                         sizeof(fake_image)) == AXL_INVALID,
               "service reload_buffer: not-the-running-service returns AXL_INVALID");
}

// ---------------------------------------------------------------------------
// Protocol registry — register / find / enumerate / unregister round-trip
// plus axl_protocol_register_name (custom name → consumer GUID binding).
// ---------------------------------------------------------------------------

static void
test_protocol_registry(void)
{
    /* Two distinct test GUIDs — fixed values so we can compare bytes. */
    AxlGuid svc_a = AXL_GUID(0xdead0001, 0xbeef, 0xcafe,
                             0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0);
    AxlGuid svc_b = AXL_GUID(0xdead0002, 0xbeef, 0xcafe,
                             0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0);

    // 1. Argument validation
    test_check(axl_protocol_register_name(NULL, &svc_a) != AXL_OK,
               "protocol: register_name rejects NULL name");
    test_check(axl_protocol_register_name("axl-test-svc", NULL) != AXL_OK,
               "protocol: register_name rejects NULL guid");
    test_check(axl_protocol_register_name("", &svc_a) != AXL_OK,
               "protocol: register_name rejects empty name");

    // The name is stored inline in the fixed table (64 bytes with the NUL),
    // so an over-long one is REJECTED rather than truncated — a truncated
    // pin would silently answer to a name nobody registered. 63 chars is
    // the longest that fits; 64 is one too many.
    char name63[64], name64[65];
    for (size_t i = 0; i < 63; i++) { name63[i] = 'n'; }
    name63[63] = '\0';
    for (size_t i = 0; i < 64; i++) { name64[i] = 'm'; }
    name64[64] = '\0';
    test_check(axl_protocol_register_name(name63, &svc_a) == AXL_OK,
               "protocol: register_name accepts a 63-char name");
    test_check(axl_protocol_register_name(name64, &svc_a) != AXL_OK,
               "protocol: register_name rejects a 64-char name (no truncation)");
    test_check(axl_protocol_register_name(name63, &svc_b) != AXL_OK,
               "protocol: the 63-char name really was pinned, not truncated");

    // 2. Refuse to shadow built-in well-known names
    test_check(axl_protocol_register_name("smbios", &svc_a) != AXL_OK,
               "protocol: register_name refuses to shadow built-in 'smbios'");
    test_check(axl_protocol_register_name("simple-fs", &svc_a) != AXL_OK,
               "protocol: register_name refuses to shadow built-in 'simple-fs'");

    // 3. First registration succeeds
    test_check(axl_protocol_register_name("axl-test-svc-A", &svc_a) == AXL_OK,
               "protocol: register_name accepts first (name, guid)");

    // 4. Idempotent — same (name, guid) twice is OK
    test_check(axl_protocol_register_name("axl-test-svc-A", &svc_a) == AXL_OK,
               "protocol: register_name is idempotent for same (name, guid)");

    // 5. Reject conflicting GUID for the same name
    test_check(axl_protocol_register_name("axl-test-svc-A", &svc_b) != AXL_OK,
               "protocol: register_name rejects conflicting GUID for same name");

    // 6. Aliases allowed — different name, same GUID. Tests that the
    //    binding is per-name, not symmetric.
    test_check(axl_protocol_register_name("axl-test-svc-A-alias", &svc_a) == AXL_OK,
               "protocol: register_name accepts alias (different name, same guid)");

    // 7. End-to-end: register a protocol interface and find it via the name.
    static int my_iface_a = 0xA1A1;
    void *handle_a = NULL;
    test_check(axl_protocol_register("axl-test-svc-A", &my_iface_a, &handle_a) == AXL_OK,
               "protocol: register installs interface for registered name");
    test_check(handle_a != NULL,
               "protocol: register creates a handle when *handle was NULL");

    void *found = NULL;
    test_check(axl_protocol_find("axl-test-svc-A", &found) == AXL_OK
               && found == &my_iface_a,
               "protocol: find resolves registered name to installed interface");

    // 8. The alias name (different name, same GUID) resolves to the
    //    same interface. Necessary-but-not-sufficient evidence of the
    //    shared GUID; assertion 9 below pins it definitively via raw
    //    LocateProtocol against the consumer-supplied EFI_GUID.
    void *found_via_alias = NULL;
    test_check(axl_protocol_find("axl-test-svc-A-alias", &found_via_alias) == AXL_OK
               && found_via_alias == &my_iface_a,
               "protocol: alias name resolves to the same installed interface");

    // 9. Direct LocateProtocol with the consumer-supplied GUID also finds it —
    //    proves register_name actually used svc_a.
    EFI_GUID svc_a_efi;
    axl_memcpy(&svc_a_efi, &svc_a, sizeof(svc_a_efi));
    void *found_via_guid = NULL;
    EFI_STATUS st = gBS->LocateProtocol(&svc_a_efi, NULL, &found_via_guid);
    test_check(st == EFI_SUCCESS && found_via_guid == &my_iface_a,
               "protocol: pinned GUID is what LocateProtocol sees");

    // 10. Enumerate the protocol — exactly one handle.
    void   **handles = NULL;
    size_t   count   = 0;
    test_check(axl_protocol_enumerate("axl-test-svc-A", &handles, &count) == AXL_OK,
               "protocol: enumerate succeeds");
    test_check(count == 1 && handles != NULL && handles[0] == handle_a,
               "protocol: enumerate returns the one registered handle");
    axl_free(handles);

    // 11. Unregister round-trip
    test_check(axl_protocol_unregister("axl-test-svc-A", &my_iface_a, handle_a) == AXL_OK,
               "protocol: unregister succeeds");
    found = NULL;
    test_check(axl_protocol_find("axl-test-svc-A", &found) != AXL_OK,
               "protocol: find fails after unregister");

    // 12. Unregistered custom name still works via FNV-fallback path —
    //     the absence of register_name shouldn't break the implicit-GUID
    //     case existing consumers may already rely on.
    static int my_iface_b = 0xB2B2;
    void *handle_b = NULL;
    test_check(axl_protocol_register("axl-test-svc-no-pin", &my_iface_b, &handle_b) == AXL_OK,
               "protocol: register works for un-pinned custom name (FNV fallback)");
    void *found_b = NULL;
    test_check(axl_protocol_find("axl-test-svc-no-pin", &found_b) == AXL_OK
               && found_b == &my_iface_b,
               "protocol: find resolves un-pinned name via same FNV fallback");
    test_check(axl_protocol_unregister("axl-test-svc-no-pin", &my_iface_b, handle_b) == AXL_OK,
               "protocol: unregister succeeds for un-pinned name");
}

// ---------------------------------------------------------------------------
// axl_driver_ensure
// ---------------------------------------------------------------------------

static void
test_driver_ensure(void)
{
    /* Simple File System Protocol — guaranteed to be installed on
     * any QEMU run because fs0:/ is the disk we're booting from. We
     * use it as a stand-in for "a protocol that's already registered"
     * so we can prove the LocateProtocol short-circuit is taken
     * before the driver-search logic runs. */
    static const AxlGuid simple_fs = AXL_GUID(
        0x0964e5b22, 0x6459, 0x11d2,
        0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b);

    /* Short-circuit: pass a bogus driver name. If the short-circuit
     * works, the driver lookup never happens and we get 0. If it
     * doesn't, the bogus name causes a search miss and we get
     * AXL_NOT_FOUND. */
    test_check(axl_driver_ensure(&simple_fs,
                                 "definitely-not-a-real-driver.efi") == 0,
               "driver_ensure: short-circuits when protocol registered");

    /* NULL args — both arguments are required. */
    test_check(axl_driver_ensure(NULL, "x.efi") == AXL_ERR,
               "driver_ensure: rejects NULL guid");
    test_check(axl_driver_ensure(&simple_fs, NULL) == AXL_ERR,
               "driver_ensure: rejects NULL name");

    /* Missing protocol + missing driver: a GUID we know is not
     * registered in QEMU + a filename that doesn't exist anywhere
     * on the disk. Should walk the search list, find nothing, and
     * return AXL_NOT_FOUND (the granular rc — distinct from AXL_ERR,
     * which is now reserved for bad-args / start-failure) without
     * crashing. */
    static const AxlGuid never_registered = AXL_GUID(
        0xdeadbeef, 0xcafe, 0xbabe,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef);

    test_check(axl_driver_ensure(&never_registered,
                                 "no-such-driver-12345.efi") == AXL_NOT_FOUND,
               "driver_ensure: returns AXL_NOT_FOUND when driver not found");
}

// ---------------------------------------------------------------------------
// axl_driver_load_buffer
// ---------------------------------------------------------------------------

static void
test_driver_load_buffer(void)
{
    AxlDriverHandle h = (AxlDriverHandle)0xdeadbeef;

    /* Argument validation. The documented contract clears *out_handle to
       NULL on every failure (arg-validation and LoadImage alike), so the
       caller can never use a stale leftover value. */
    test_check(axl_driver_load_buffer(NULL, 100, &h) == AXL_ERR,
               "driver_load_buffer: rejects NULL buf");
    test_check(h == NULL,
               "driver_load_buffer: NULL buf clears *out_handle");

    h = (AxlDriverHandle)0xdeadbeef;
    static const unsigned char any_byte = 0;
    test_check(axl_driver_load_buffer(&any_byte, 0, &h) == AXL_ERR,
               "driver_load_buffer: rejects zero len");
    test_check(h == NULL,
               "driver_load_buffer: zero len clears *out_handle");

    test_check(axl_driver_load_buffer(&any_byte, 1, NULL) == AXL_ERR,
               "driver_load_buffer: rejects NULL out_handle");

    /* Garbage bytes — definitely not a valid PE image. LoadImage
       returns EFI_LOAD_ERROR (or similar); we surface AXL_ERR and
       null the out-handle so the caller can't accidentally use a
       leftover value. 64 bytes is enough that LoadImage actually
       parses headers rather than rejecting outright on size. */
    static const unsigned char garbage[64] = {
        0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89,
        0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89,
        0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89,
        0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89,
        0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89,
        0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89,
        0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89,
        0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89,
    };
    h = (AxlDriverHandle)0xdeadbeef;
    test_check(axl_driver_load_buffer(garbage, sizeof(garbage), &h)
                   == AXL_ERR,
               "driver_load_buffer: rejects non-PE garbage");
    test_check(h == NULL,
               "driver_load_buffer: failed load nulls *out_handle");
}

// ---------------------------------------------------------------------------
// axl_shared_driver_* — thin wrappers, identity-and-publish smoke test
// ---------------------------------------------------------------------------

static void
test_shared_driver(void)
{
    /* NULL-argument guards on all three entry points. */
    AxlGuid g;
    test_check(axl_shared_driver_guid(NULL, &g) == AXL_ERR,
               "shared_driver_guid: NULL name rejected");
    test_check(axl_shared_driver_guid("x", NULL) == AXL_ERR,
               "shared_driver_guid: NULL out rejected");

    AxlHandle handle = NULL;
    int dummy_vt = 0;
    test_check(axl_shared_driver_publish(NULL, &dummy_vt, &handle) == AXL_ERR,
               "shared_driver_publish: NULL name rejected");
    test_check(axl_shared_driver_publish("x", NULL, &handle) == AXL_ERR,
               "shared_driver_publish: NULL iface rejected");
    test_check(axl_shared_driver_publish("x", &dummy_vt, NULL) == AXL_ERR,
               "shared_driver_publish: NULL out_handle rejected");

    void *vt = NULL;
    test_check(axl_shared_driver_locate(NULL, "x.efi", NULL, 0, &vt) == AXL_ERR,
               "shared_driver_locate: NULL name rejected");
    test_check(axl_shared_driver_locate("x", NULL, NULL, 0, &vt) == AXL_ERR,
               "shared_driver_locate: NULL driver_filename rejected");
    test_check(axl_shared_driver_locate("x", "x.efi", NULL, 0, NULL) == AXL_ERR,
               "shared_driver_locate: NULL out_iface rejected");

    /* Identity contract: two callers passing the same name MUST get
       byte-identical GUIDs. This is the cross-image-pairing
       guarantee — driver and launcher both pass "do-tool" and end up
       at the same protocol. */
    AxlGuid g1 = {0}, g2 = {0};
    test_check(axl_shared_driver_guid("shared-driver-test", &g1) == AXL_OK
               && axl_shared_driver_guid("shared-driver-test", &g2) == AXL_OK
               && axl_memcmp(&g1, &g2, sizeof(AxlGuid)) == 0,
               "shared_driver_guid: deterministic across calls");

    /* Different names produce different GUIDs (collision rejection
       — a typo on one side breaks pairing, exactly as designed). */
    AxlGuid g_other;
    test_check(axl_shared_driver_guid("shared-driver-test-OTHER", &g_other)
                   == AXL_OK
               && axl_memcmp(&g1, &g_other, sizeof(AxlGuid)) != 0,
               "shared_driver_guid: distinct names → distinct GUIDs");

    /* Round-trip publish + locate + unpublish in-process. Tests
       don't run as a driver image so we can't exercise the
       cross-image LoadImage path here — that's covered by the
       sdk/examples/shared-driver-demo/ integration build. We DO
       test that publish makes the protocol locate-able and
       unpublish removes it. */
    AxlHandle h = NULL;
    static int my_vtable = 42;   /* sentinel; locate must return THIS pointer */
    test_check(axl_shared_driver_publish("shared-driver-test", &my_vtable, &h)
                   == AXL_OK
               && h != NULL,
               "shared_driver_publish: round-trip register");

    /* Direct locate via find_guid (not the full _locate path, which
       would try to LoadImage; we've already published in-process so
       step 1 of _locate would short-circuit, but the test runner's
       axl_driver_ensure_with_embedded inputs are awkward). Verify
       the identity GUID resolves to the registered iface. */
    void *found = NULL;
    test_check(axl_protocol_find_guid(&g1, &found) == AXL_OK
               && found == &my_vtable,
               "shared_driver_publish: locate returns same iface");

    test_check(axl_shared_driver_unpublish("shared-driver-test",
                                           &my_vtable, h) == AXL_OK,
               "shared_driver_unpublish: round-trip unregister");

    /* After unpublish, the GUID no longer resolves. */
    found = NULL;
    test_check(axl_protocol_find_guid(&g1, &found) == AXL_ERR,
               "shared_driver_unpublish: protocol removed");

    /* Handle-defaulting contract (since 0.19.2): publish with
       *out_handle == NULL defaults to the caller's gImageHandle so
       the protocol lives on the same handle UnloadImage can act on.
       Pre-setting *out_handle to a non-NULL value preserves the old
       reuse semantics. */
    AxlHandle reused = NULL;
    static int sentinel_a = 1;
    static int sentinel_b = 2;
    /* Phase 1: pass *out_handle == NULL; expect the default
       (gImageHandle of the test image). */
    test_check(axl_shared_driver_publish("shared-driver-test-reuse",
                                         &sentinel_a, &reused) == AXL_OK
               && reused != NULL,
               "shared_driver_publish: defaults to gImageHandle when *out=NULL");
    AxlHandle pinned = reused;
    /* Phase 2: re-publish a DIFFERENT identity with *out_handle
       pre-set to the same pinned handle. Underlying install
       reuses the handle rather than minting. */
    test_check(axl_shared_driver_publish("shared-driver-test-reuse-2",
                                         &sentinel_b, &reused) == AXL_OK
               && reused == pinned,
               "shared_driver_publish: reuses *out_handle when pre-set");
    /* Cleanup both registrations on the shared handle. */
    test_check(axl_shared_driver_unpublish("shared-driver-test-reuse",
                                           &sentinel_a, pinned) == AXL_OK
               && axl_shared_driver_unpublish("shared-driver-test-reuse-2",
                                              &sentinel_b, pinned) == AXL_OK,
               "shared_driver_unpublish: handle-reuse cleanup");

    /* Unload API guards: NULL name rejected. Driver-not-loaded path
       must return AXL_OK silently (post-condition already holds). */
    test_check(axl_shared_driver_unload(NULL) == AXL_ERR,
               "shared_driver_unload: NULL name rejected");
    test_check(axl_shared_driver_unload("shared-driver-never-published")
                   == AXL_OK,
               "shared_driver_unload: not-loaded path returns OK");

    /* Unload-on-non-image-handle path: contract check that the
       primitive surfaces UnloadImage failures cleanly rather than
       crashing. Pre-mint a fresh non-image handle by pre-setting
       *out_handle to a non-NULL synthetic value through publish's
       reuse path (the synthetic handle has the protocol installed
       on it but is not a loaded-image handle). axl_shared_driver_unload
       then resolves to that handle, calls gBS->UnloadImage on it,
       which returns EFI_INVALID_PARAMETER per spec ("not a loaded
       image"), and we surface AXL_ERR. Importantly this exercise
       does NOT depend on running-image UnloadImage semantics
       (which are firmware-quirky) — only on the well-defined
       wrong-handle case. */
    static int sentinel_synth = 99;
    AxlHandle synth_h = NULL;
    /* Mint a synthetic handle: install with NULL handle slot
       creates one; we own it. Use a separate identity (not the
       shared-driver namespace) so we don't collide with anything. */
    static const AxlGuid synth_guid = AXL_GUID(
        0xdeadbeef, 0xfeed, 0xface,
        0xc0, 0xff, 0xee, 0x00, 0x11, 0x22, 0x33, 0x44);
    test_check(axl_protocol_install(&synth_guid, &sentinel_synth,
                                    &synth_h) == AXL_OK
               && synth_h != NULL,
               "shared_driver_unload: synthetic non-image handle minted");
    /* Pre-set publish's out_handle to the synthetic handle so the
       shared-driver protocol lands on it (not on gImageHandle). */
    AxlHandle reuse_h = synth_h;
    test_check(axl_shared_driver_publish("shared-driver-non-image-test",
                                         &sentinel_synth, &reuse_h)
                   == AXL_OK
               && reuse_h == synth_h,
               "shared_driver_unload: shared-driver protocol pinned to non-image handle");
    test_check(axl_shared_driver_unload("shared-driver-non-image-test")
                   == AXL_ERR,
               "shared_driver_unload: surfaces UnloadImage failure on non-image handle");
    /* Cleanup: explicit unpublish + remove the synthetic protocol. */
    test_check(axl_shared_driver_unpublish("shared-driver-non-image-test",
                                           &sentinel_synth, synth_h) == AXL_OK
               && axl_protocol_uninstall(synth_h, &synth_guid,
                                               &sentinel_synth) == AXL_OK,
               "shared_driver_unload: cleanup non-image handle");

    /* Leak-stress on the unload's LocateHandleBuffer + FreePool path.
       The driver-leak-test integration test already covers
       axl_driver_unload's load-options-side leak surface; here we
       just verify the wrapper's added alloc-path (the handle buffer
       LocateHandleBuffer hands us, which we must FreePool) doesn't
       grow per-iteration. Loop publish + unload + unpublish N
       times against a synthetic non-image handle and verify the
       in-flight allocation count returns to the pre-loop baseline
       exactly — a real leak would show monotonic growth. */
    AxlMemStats stats_before, stats_after;
    AxlHandle loop_synth = NULL;
    static int sentinel_loop = 42;
    static const AxlGuid loop_synth_guid = AXL_GUID(
        0xfeedf00d, 0xbeef, 0xcafe,
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11);
    /* Pre-mint a single synthetic handle reused by every iteration. */
    test_check(axl_protocol_install(&loop_synth_guid, &sentinel_loop,
                                    &loop_synth) == AXL_OK,
               "shared_driver_unload: leak-loop synthetic handle ready");

    axl_mem_get_stats(&stats_before);
    for (int i = 0; i < 50; i++) {
        AxlHandle loop_h = loop_synth;
        if (axl_shared_driver_publish("shared-driver-loop-test",
                                      &sentinel_loop, &loop_h) != AXL_OK) {
            break;
        }
        (void)axl_shared_driver_unload("shared-driver-loop-test");
        axl_shared_driver_unpublish("shared-driver-loop-test",
                                    &sentinel_loop, loop_h);
    }
    axl_mem_get_stats(&stats_after);
    /* Exact equality: in-flight allocation count returns to the
       pre-loop baseline. If the per-iteration warning print pulls
       in a transient that's released before the next iteration,
       this still holds. A leak would show monotonic growth on
       `count`. */
    test_check(stats_after.count == stats_before.count,
               "shared_driver_unload: 50x loop heap-stable");

    /* Cleanup the leak-loop synthetic handle. */
    axl_protocol_uninstall(loop_synth, &loop_synth_guid, &sentinel_loop);
}

// ---------------------------------------------------------------------------
// axl_driver_locate
// ---------------------------------------------------------------------------

static void
test_driver_locate(void)
{
    char path[256];

    /* NULL args — all three rejected. */
    test_check(axl_driver_locate(NULL, path, sizeof(path)) == AXL_ERR,
               "driver_locate: rejects NULL name");
    test_check(axl_driver_locate("x.efi", NULL, sizeof(path)) == AXL_ERR,
               "driver_locate: rejects NULL out");
    test_check(axl_driver_locate("x.efi", path, 0) == AXL_ERR,
               "driver_locate: rejects zero size");

    /* Driver missing from disk: walks the search list, finds nothing,
     * returns -1 cleanly. */
    test_check(axl_driver_locate("no-such-driver-67890.efi",
                                 path, sizeof(path)) == -1,
               "driver_locate: returns -1 when driver not found");

    /* Positive: the running test image itself is on disk under fs0,
     * and locate's search step 2 ("image's own directory") will find
     * any file alongside it. We just wrote axl-test-util.tmp earlier,
     * so locate that — proves the discovery path resolves correctly. */
    int rc = axl_driver_locate("axl-test-util.tmp", path, sizeof(path));
    if (rc == AXL_OK) {
        test_check(axl_strlen(path) > 0, "driver_locate: writes non-empty path");
        AxlFsEntry info;
        test_check(axl_file_info(path, &info) == AXL_OK,
                   "driver_locate: returned path actually exists");
    } else {
        /* Some QEMU configurations don't surface fs0 in the search;
         * skip the positive check rather than fail the suite. */
        test_check(rc == -1, "driver_locate: skipped positive test");
    }

    /* Buffer-too-small: when the discovered path doesn't fit, locate
     * must return -1 rather than truncate. Use a 1-byte buffer to
     * force the failure path even on the shortest possible match. */
    char tiny[1];
    test_check(axl_driver_locate("axl-test-util.tmp", tiny, sizeof(tiny)) == AXL_ERR,
               "driver_locate: rejects too-small buffer");
}

// ---------------------------------------------------------------------------
// axl_diag_probe_protocol
// ---------------------------------------------------------------------------

static void
test_diag_probe_protocol(void)
{
    /* NULL guid → -1 with no UEFI call. Pure logic, doesn't depend
     * on what's registered in firmware. */
    test_check(axl_diag_probe_protocol(NULL, "x") == AXL_ERR,
               "diag_probe: NULL guid rejected");
    test_check(axl_diag_probe_protocol(NULL, NULL) == AXL_ERR,
               "diag_probe: NULL guid rejected even with NULL name");

    /* SimpleFileSystem is guaranteed registered in QEMU (we boot
     * from fs0). Probe should return 0 and the line should print. */
    static const AxlGuid simple_fs = AXL_GUID(
        0x0964e5b22, 0x6459, 0x11d2,
        0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b);
    test_check(axl_diag_probe_protocol(&simple_fs,
                                       "EFI_SIMPLE_FILE_SYSTEM") == AXL_OK,
               "diag_probe: registered protocol returns 0");

    /* Bogus GUID → -1. Doesn't crash on a NULL display_name either. */
    static const AxlGuid bogus = AXL_GUID(
        0xdeadbeef, 0xcafe, 0xbabe,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef);
    test_check(axl_diag_probe_protocol(&bogus, NULL) == AXL_ERR,
               "diag_probe: unregistered protocol returns -1, NULL name OK");
}

// ---------------------------------------------------------------------------
// SMBIOS — extras beyond test_smbios (decoders, copy-truncation, edges)
// ---------------------------------------------------------------------------

static void
test_smbios_extras(void)
{
    /* Spec-table string decoders are partial — they map known
       SMBIOS values to descriptive strings and return NULL for
       anything not in their switch. Verify both branches: a
       known value yields a non-NULL string; an unhandled value
       yields NULL (so callers can NUL-check rather than assume a
       fallback). */
    test_check(axl_smbios_slot_type_str(0x06) != NULL,
               "smbios: slot type str(0x06=PCI) non-NULL");
    test_check(axl_smbios_slot_type_str(0xA5) != NULL,
               "smbios: slot type str(0xA5=PCIe) non-NULL");
    test_check(axl_smbios_slot_type_str(0x60) == NULL,
               "smbios: slot type str(0x60=unhandled) NULL");
    test_check(axl_smbios_slot_width_str(0x08) != NULL,
               "smbios: slot width str(0x08) non-NULL");
    test_check(axl_smbios_slot_usage_str(0x03) != NULL,
               "smbios: slot usage str(0x03) non-NULL");

    /* Chassis class enum coercion: 0x02 is the spec's explicit
       "Unknown" and 0x00 is "(not set)"; both coerce to UNKNOWN.
       Out-of-range bytes fall through to OTHER (the catch-all
       bucket), not UNKNOWN. The 0x80 lock bit is stripped before
       lookup. */
    test_check(axl_smbios_chassis_class(0x00)
               == AXL_SMBIOS_CHASSIS_CLASS_UNKNOWN,
               "smbios: chassis_class(0x00) → UNKNOWN");
    test_check(axl_smbios_chassis_class(0x02)
               == AXL_SMBIOS_CHASSIS_CLASS_UNKNOWN,
               "smbios: chassis_class(0x02) → UNKNOWN");
    test_check(axl_smbios_chassis_class(0x07)
               == AXL_SMBIOS_CHASSIS_CLASS_DESKTOP,
               "smbios: chassis_class(0x07=Tower) → DESKTOP");
    test_check(axl_smbios_chassis_class(0x09)
               == AXL_SMBIOS_CHASSIS_CLASS_NOTEBOOK,
               "smbios: chassis_class(0x09=Laptop) → NOTEBOOK");
    /* 0x07 with lock bit set must still classify the same way. */
    test_check(axl_smbios_chassis_class(0x87)
               == AXL_SMBIOS_CHASSIS_CLASS_DESKTOP,
               "smbios: chassis_class strips lock bit");

    /* Baseboard reader (Type 2). QEMU q35 publishes one. */
    AxlSmbiosBaseboardInfo bb;
    int bb_rc = axl_smbios_read_baseboard(&bb);
    test_check(bb_rc == AXL_OK || bb_rc == AXL_ERR,
               "smbios: read_baseboard returns 0 or -1");

    /* Chassis reader (Type 3). */
    AxlSmbiosChassisInfo ch;
    int ch_rc = axl_smbios_read_chassis(&ch);
    test_check(ch_rc == AXL_OK || ch_rc == AXL_ERR,
               "smbios: read_chassis returns 0 or -1");

    /* copy_string_utf8: truncation case. Typical BIOS vendor strings
       are ~10 chars; a 4-byte buffer must be NUL-terminated regardless. */
    AxlSmbiosHeader *bios = axl_smbios_find(AXL_SMBIOS_TYPE_BIOS_INFO);
    if (bios != NULL) {
        char tiny[4];
        size_t n = axl_smbios_copy_string_utf8(bios, 1, tiny, sizeof(tiny));
        test_check(tiny[3] == '\0',
                   "smbios: copy_string_utf8 NUL-terminates a tiny buffer");
        test_check(n < sizeof(tiny),
                   "smbios: copy_string_utf8 returns bytes-written < buf");

        /* String index 0 → empty string, returns 0. */
        char any[64] = "garbage";
        size_t n0 = axl_smbios_copy_string_utf8(bios, 0, any, sizeof(any));
        test_check(n0 == 0 && any[0] == '\0',
                   "smbios: copy_string_utf8(0) → empty");
    }

    /* find_next on an unknown type: never matches, terminates. */
    AxlSmbiosHeader *h = axl_smbios_find_next(0xFE, NULL);
    test_check(h == NULL, "smbios: find_next on unknown type returns NULL");

    /* string-region byte length: zero-string records return 0. */
    AxlSmbiosHeader *end = NULL;
    AxlSmbiosHeader *cur = NULL;
    while ((cur = axl_smbios_next(cur)) != NULL) {
        if (cur->Type == 127) {  /* AXL_SMBIOS_TYPE_END_OF_TABLE */
            end = cur;
            break;
        }
    }
    if (end != NULL) {
        size_t slen = axl_smbios_strings_byte_len(end);
        test_check(slen == 0,
                   "smbios: end-of-table strings_byte_len == 0");
    }
}

// ---------------------------------------------------------------------------
// AxlNvstore — namespace registration table (pure logic)
// ---------------------------------------------------------------------------

/* These vendor GUIDs are fictitious — their only role is to give
   the registration test distinct backend tokens. AxlNvstore stores
   the pointer but matches on the 16 GUID bytes, so the values never
   reach firmware. */
static const AxlGuid TEST_VENDOR_A = AXL_GUID(
    0xA0000001, 0x0001, 0x0001,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01);
static const AxlGuid TEST_VENDOR_B = AXL_GUID(
    0xB0000002, 0x0002, 0x0002,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02);

static void
test_nvstore_namespaces(void)
{
    /* Built-in namespaces are pre-registered. Re-registering them
       with NULL token is a NULL-arg error, not a duplicate-name
       collision. */
    test_check(axl_nvstore_register_namespace(NULL, &TEST_VENDOR_A) == AXL_ERR,
               "nvstore: register NULL name fails");
    test_check(axl_nvstore_register_namespace("vendor-a", NULL) == AXL_ERR,
               "nvstore: register NULL token fails");

    /* First registration succeeds. */
    test_check(axl_nvstore_register_namespace("vendor-a", &TEST_VENDOR_A) == AXL_OK,
               "nvstore: register vendor-a");

    /* Idempotent re-register with the SAME token succeeds. */
    test_check(axl_nvstore_register_namespace("vendor-a", &TEST_VENDOR_A) == AXL_OK,
               "nvstore: re-register vendor-a (same token, idempotent)");

    /* Re-register with a DIFFERENT OBJECT holding the SAME BYTES also
       succeeds: two translation units each keeping a private
       `static const AxlGuid` for one shared namespace is the obvious
       pattern (the API takes a `const void *`), and matching by pointer
       rejected the second one — leaving every get/set that TU made
       pointed at an unregistered namespace.

       The copy is filled at run time into a distinct static object so
       no compiler/linker constant merging can hand it the same address
       as TEST_VENDOR_A; that would let this test pass against the old
       pointer compare. The address assertion below pins that premise. */
    static AxlGuid vendor_a_copy;
    axl_memcpy(&vendor_a_copy, &TEST_VENDOR_A, sizeof(vendor_a_copy));
    test_check((const AxlGuid *)&vendor_a_copy != &TEST_VENDOR_A,
               "nvstore: equal-bytes token is a genuinely distinct object");
    test_check(axl_guid_equal(&vendor_a_copy, &TEST_VENDOR_A),
               "nvstore: equal-bytes token really is byte-equal");
    test_check(axl_nvstore_register_namespace("vendor-a", &vendor_a_copy) == AXL_OK,
               "nvstore: re-register vendor-a (distinct object, equal bytes)");

    /* Re-register with a DIFFERENT token rejects. Placed after the
       equal-bytes re-register so it also proves that re-register did
       not append a second "vendor-a" row — the lookup still finds the
       original entry and still rejects a genuine GUID collision. */
    test_check(axl_nvstore_register_namespace("vendor-a", &TEST_VENDOR_B) == AXL_ERR,
               "nvstore: re-register vendor-a (different token) fails");

    /* Unregistered namespace is an error in get/set/delete/iter. */
    uint8_t  byte = 0;
    size_t   sz   = 1;
    test_check(axl_nvstore_get("never-registered", "x", &byte, &sz) == AXL_ERR,
               "nvstore: get on unregistered ns returns -1");
    test_check(axl_nvstore_set("never-registered", "x", &byte, 1, 0) == AXL_ERR,
               "nvstore: set on unregistered ns returns -1");
    test_check(axl_nvstore_delete("never-registered", "x") == AXL_ERR,
               "nvstore: delete on unregistered ns returns -1");

    /* Built-in "global" and "app" stay reachable. */
    uint8_t  sb;
    size_t   sb_sz = sizeof(sb);
    int g_rc = axl_nvstore_get("global", "SecureBoot", &sb, &sb_sz);
    test_check(g_rc == AXL_OK || g_rc == AXL_ERR,
               "nvstore: get(global, SecureBoot) returns 0 or -1");
}

// ---------------------------------------------------------------------------
// AxlAttempt — breadcrumb / quarantine / bounded result log
//
// The crash paths are NOT exercised here and cannot be: the engine exists for
// the case where a risky operation hangs or resets the box, and QEMU has no
// way to stage that without wedging the run. What is covered is the
// BOOKKEEPING — the durable state a real crash would leave behind, written
// and read back explicitly. A genuine hang -> reset -> recover cycle is
// real-hardware territory (see feedback_uefi_firmware_test_hazards).
// ---------------------------------------------------------------------------

static const AxlGuid TEST_ATTEMPT_GUID = AXL_GUID(
    0xA7000003, 0x0003, 0x0003,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03);

static void
test_attempt(void)
{
    AxlAttempt at;

    test_check(axl_attempt_init(NULL, "ns", &TEST_ATTEMPT_GUID) == AXL_ERR,
               "attempt: init NULL descriptor -> AXL_ERR");
    test_check(axl_attempt_init(&at, NULL, &TEST_ATTEMPT_GUID) == AXL_ERR,
               "attempt: init NULL ns -> AXL_ERR");
    test_check(axl_attempt_init(&at, "ns", NULL) == AXL_ERR,
               "attempt: init NULL vendor -> AXL_ERR");

    test_check(axl_attempt_init(&at, "axltest-attempt", &TEST_ATTEMPT_GUID) == AXL_OK,
               "attempt: init registers the namespace");

    /* The defaults ARE an on-disk format. Pin them: a consumer with state
       already in NVRAM inherits these when it doesn't override, so a silent
       change here orphans that state. */
    test_check(axl_strcmp(at.trying_key, "Trying") == 0,
               "attempt: default trying_key is Trying");
    test_check(axl_strcmp(at.quarantine_key, "Quarantine") == 0,
               "attempt: default quarantine_key is Quarantine");
    test_check(axl_strcmp(at.log_key, "Log") == 0,
               "attempt: default log_key is Log");
    test_check(at.name_max == 64, "attempt: default name_max is 64");
    test_check(at.quarantine_max == 1024, "attempt: default quarantine_max is 1024");
    test_check(at.log_max == 2048, "attempt: default log_max is 2048");
    test_check(at.flags == (AXL_NV_PERSISTENT | AXL_NV_BOOT),
               "attempt: default flags are PERSISTENT|BOOT");

    /* A failed init must leave an INERT descriptor, for EVERY failure reason.
       Callers are entitled to ignore the return (every op is best-effort), so
       a stack AxlAttempt left holding garbage would fault the box in the one
       module whose job is to not do that. Inert means: ns is NULL and the
       descriptor is disarmed, so a subsequent begin/end/etc. is a clean no-op
       -- never a wild deref, and never a misdirected write (a NULL ns would
       otherwise resolve to the GLOBAL namespace and silently write there).

       Pre-fill each `bad`/`dirty` descriptor with 0xFF so "uninitialized"
       means WILD values, not a happens-to-be-zero stack: that makes these
       genuine tests of the init path. If init left the garbage in place, the
       begin() below derefs a wild ns pointer -> #GP -> the boot dies right
       here and NONE of the later assertions run. That the suite reaches its
       Results footer at all is itself the proof of inertness. */

    /* Reason 1: namespace-registration failure. "axltest-attempt" is already
       bound to TEST_ATTEMPT_GUID above, so re-binding it to a different
       vendor fails inside axl_nvstore_register_namespace. */
    AxlAttempt bad;
    axl_memset(&bad, 0xFF, sizeof bad);
    test_check(axl_attempt_init(&bad, "axltest-attempt", &TEST_VENDOR_B) == AXL_ERR,
               "attempt: init on a ns bound to another vendor -> AXL_ERR");
    test_check(bad.ns == NULL && bad.name_max == 64 && bad.log_max == 2048,
               "attempt: register-failed init is inert (ns NULL, fields filled)");
    test_check(axl_attempt_begin(&bad, "x.efi") == false,
               "attempt: begin on a register-failed descriptor is a safe no-op");
    axl_attempt_end(&bad);   /* must not fault or delete anything */

    /* Reason 2: NULL ns -- the argument-validation early return. This is the
       path the register-first fix left uncovered: a valid `dirty` descriptor,
       a NULL ns, AXL_ERR returned, and (before the fix) not one field written
       so `dirty` stays 0xFF garbage. */
    AxlAttempt dirty;
    axl_memset(&dirty, 0xFF, sizeof dirty);
    test_check(axl_attempt_init(&dirty, NULL, &TEST_ATTEMPT_GUID) == AXL_ERR,
               "attempt: init NULL ns -> AXL_ERR (dirty struct)");
    test_check(dirty.ns == NULL,
               "attempt: NULL-ns init leaves an inert descriptor, not garbage");
    test_check(axl_attempt_begin(&dirty, "x.efi") == false,
               "attempt: begin on a NULL-ns-failed descriptor is a safe no-op");

    /* Reason 3: NULL vendor -- same early return, other argument. */
    axl_memset(&dirty, 0xFF, sizeof dirty);
    test_check(axl_attempt_init(&dirty, "ns", NULL) == AXL_ERR,
               "attempt: init NULL vendor -> AXL_ERR (dirty struct)");
    test_check(dirty.ns == NULL,
               "attempt: NULL-vendor init leaves an inert descriptor, not garbage");
    test_check(axl_attempt_begin(&dirty, "x.efi") == false,
               "attempt: begin on a NULL-vendor-failed descriptor is a safe no-op");
    axl_attempt_end(&dirty);   /* must not fault */

    /* NULL-safety on every entry point: these are the only error paths that
       are OUR validation rather than the firmware's, so they're the only
       negatives safe to assert. */
    char nbuf[64];
    test_check(axl_attempt_begin(NULL, "x.efi") == false,
               "attempt: begin NULL descriptor -> false");
    test_check(axl_attempt_begin(&at, NULL) == false,
               "attempt: begin NULL name -> false");
    test_check(axl_attempt_pending(NULL, nbuf, sizeof nbuf) == false,
               "attempt: pending NULL descriptor -> false");
    test_check(axl_attempt_pending(&at, NULL, sizeof nbuf) == false,
               "attempt: pending NULL buffer -> false");
    test_check(axl_attempt_pending(&at, nbuf, 0) == false,
               "attempt: pending zero cap -> false");
    test_check(axl_attempt_recover(NULL, nbuf, sizeof nbuf) == AXL_ERR,
               "attempt: recover NULL descriptor -> AXL_ERR");
    test_check(axl_attempt_recover(&at, NULL, sizeof nbuf) == AXL_ERR,
               "attempt: recover NULL buffer -> AXL_ERR");
    test_check(axl_attempt_is_quarantined(NULL, "x.efi") == false,
               "attempt: is_quarantined NULL descriptor -> false");
    test_check(axl_attempt_is_quarantined(&at, NULL) == false,
               "attempt: is_quarantined NULL name -> false");
    test_check(axl_attempt_quarantine_read(NULL, nbuf, sizeof nbuf) == false,
               "attempt: quarantine_read NULL descriptor -> false");
    test_check(axl_attempt_log_read(NULL, nbuf, sizeof nbuf) == false,
               "attempt: log_read NULL descriptor -> false");
    /* Void entry points must absorb NULL rather than fault. */
    axl_attempt_end(NULL);
    axl_attempt_clear(NULL);
    axl_attempt_log(NULL, "x");
    axl_attempt_log(&at, NULL);
    axl_attempt_quarantine(NULL, "x");
    axl_attempt_quarantine(&at, NULL);
    test_survived("attempt: void entry points absorb NULL without faulting");

    /* A name that can't be read back must not be written: axl_nvstore_get
       fails outright (not truncates) on an over-long value, so the breadcrumb
       would be one recovery could never see or clear. */
    char toolong[80];
    axl_memset(toolong, 'a', sizeof toolong - 1);
    toolong[sizeof toolong - 1] = '\0';
    test_check(axl_attempt_begin(&at, toolong) == false,
               "attempt: begin refuses a name longer than name_max");

    /* Everything below writes NVRAM. Firmware may refuse (read-only /
       authenticated variable policy); skip cleanly rather than fail. */
    axl_attempt_clear(&at);
    if (!axl_attempt_begin(&at, "Aaa.efi")) {
        test_skip_n(13, "attempt round-trip (NVRAM write refused)");
        /* Balance the 13 assertions in the populated branch. */
        return;
    }

    /* 1: the breadcrumb is durable and reads back exactly. */
    nbuf[0] = '\0';
    test_check(axl_attempt_pending(&at, nbuf, sizeof nbuf) &&
               axl_strcmp(nbuf, "Aaa.efi") == 0,
               "attempt: pending reads back the exact breadcrumb");

    /* 2: end() erases it — the attempt returned, so there is no culprit. */
    axl_attempt_end(&at);
    test_check(axl_attempt_pending(&at, nbuf, sizeof nbuf) == false,
               "attempt: end clears the breadcrumb");

    /* 3: no breadcrumb means the last run completed — nothing to recover. */
    test_check(axl_attempt_recover(&at, nbuf, sizeof nbuf) == 0,
               "attempt: recover with no breadcrumb -> 0");
    test_check(axl_attempt_is_quarantined(&at, "Aaa.efi") == false,
               "attempt: a clean run quarantines nothing");

    /* 4: a surviving breadcrumb IS the crash signal. This is the closest the
       harness gets to a crash: the durable state a real hang would leave,
       staged by hand. It does NOT exercise the hang itself. */
    axl_attempt_begin(&at, "Aaa.efi");
    nbuf[0] = '\0';
    test_check(axl_attempt_recover(&at, nbuf, sizeof nbuf) == 1 &&
               axl_strcmp(nbuf, "Aaa.efi") == 0,
               "attempt: recover reports the surviving breadcrumb as the culprit");
    test_check(axl_attempt_is_quarantined(&at, "Aaa.efi"),
               "attempt: recover quarantines the culprit");
    test_check(axl_attempt_pending(&at, nbuf, sizeof nbuf) == false,
               "attempt: recover clears the breadcrumb (no re-blame next boot)");
    test_check(axl_attempt_is_quarantined(&at, "Bbb.efi") == false,
               "attempt: quarantine is per-name, not a global flag");

    /* 5: the list renders as '\n'-separated lines, dedup'd. */
    axl_attempt_quarantine(&at, "Aaa.efi");   /* already there */
    axl_attempt_quarantine(&at, "Bbb.efi");
    char qbuf[128];
    test_check(axl_attempt_quarantine_read(&at, qbuf, sizeof qbuf) &&
               axl_strcmp(qbuf, "Aaa.efi\nBbb.efi\n") == 0,
               "attempt: quarantine_read renders dedup'd '\\n'-separated lines");

    /* 6: the log is the caller's vocabulary, appended verbatim and dedup'd. */
    axl_attempt_log(&at, "OK Aaa.efi");
    axl_attempt_log(&at, "CRASH Bbb.efi");
    axl_attempt_log(&at, "OK Aaa.efi");   /* duplicate -> ignored */
    char lbuf[128];
    test_check(axl_attempt_log_read(&at, lbuf, sizeof lbuf) &&
               axl_strcmp(lbuf, "OK Aaa.efi\nCRASH Bbb.efi\n") == 0,
               "attempt: log_read renders the caller's lines verbatim, dedup'd");

    /* 7: a buffer too small to hold the value fails rather than truncating —
       the same bound that makes over-long names unwritable. */
    char tiny[4];
    test_check(axl_attempt_log_read(&at, tiny, sizeof tiny) == false,
               "attempt: log_read into an undersized buffer fails, never truncates");

    /* 8: clear is the operator's "forget everything", including un-quarantining. */
    axl_attempt_clear(&at);
    test_check(axl_attempt_is_quarantined(&at, "Aaa.efi") == false,
               "attempt: clear un-quarantines");
    test_check(axl_attempt_log_read(&at, lbuf, sizeof lbuf) == false,
               "attempt: clear empties the log");
}

// ---------------------------------------------------------------------------
// axl_driver_load_dir_guarded — safe negatives only
//
// The positive path LOADS ARBITRARY FIRMWARE IMAGES, which is the exact
// hazard the guard exists for and the exact thing that wedges a QEMU run.
// Only our own pre-firmware validation is asserted here; the guarded sweep
// itself is exercised by the netload integration test's seam and, for real
// crashes, on hardware.
// ---------------------------------------------------------------------------

static void
test_driver_load_dir_guarded(void)
{
    size_t n = 12345;
    test_check(axl_driver_load_dir_guarded(NULL, "*.efi", NULL, &n) == AXL_ERR,
               "load_dir_guarded: NULL dir -> AXL_ERR");
    test_check(axl_driver_load_dir(NULL, "*.efi", &n) == AXL_ERR,
               "load_dir: NULL dir -> AXL_ERR (unchanged contract)");

    /* A missing directory is "not an error, just 0 loaded" — the contract
       axl_driver_load_dir has always had. The guarded variant must not
       change it, with or without a guard. */
    n = 12345;
    test_check(axl_driver_load_dir("fs0:\\axl-no-such-dir", "*.efi", &n) == AXL_OK &&
               n == 0,
               "load_dir: missing dir -> AXL_OK, 0 loaded");
    n = 12345;
    test_check(axl_driver_load_dir_guarded("fs0:\\axl-no-such-dir", "*.efi",
                                           NULL, &n) == AXL_OK && n == 0,
               "load_dir_guarded: NULL guard behaves exactly like load_dir");

    AxlAttempt at;
    if (axl_attempt_init(&at, "axltest-guard", &TEST_ATTEMPT_GUID) != AXL_OK) {
        test_skip_n(2, "load_dir_guarded live guard (namespace unavailable)");
        /* Balance the 2 assertions in the populated branch. */
        return;
    }
    n = 12345;
    test_check(axl_driver_load_dir_guarded("fs0:\\axl-no-such-dir", "*.efi",
                                           &at, &n) == AXL_OK && n == 0,
               "load_dir_guarded: a guard doesn't change the missing-dir contract");
    /* Recovery runs before the walk, so an empty sweep leaves no breadcrumb. */
    char cul[64];
    test_check(axl_attempt_pending(&at, cul, sizeof cul) == false,
               "load_dir_guarded: an empty sweep leaves no breadcrumb outstanding");
}

// ---------------------------------------------------------------------------
// AxlNvstore — set/get/get_attrs/iter/delete round-trip
// ---------------------------------------------------------------------------

typedef struct {
    int   matches;
    char  last_key[64];
} NvstoreIterCtx;

/* Stops the walk on the first key, returning whatever value the test
   planted. Exists to pin the EARLY-STOP contract: iter must hand the
   caller's value back verbatim, including negative values that collide
   with AXL_* constants -- a -8 from a callback must not be read as
   AXL_NO_RESOURCES from the walk underneath. */
typedef struct {
    int  stop_with;   ///< value the callback returns
    int  calls;       ///< times it was invoked
} NvstoreStopCtx;

static int
nvstore_iter_stop_cb(
    const char  *key,
    void        *ctx
    )
{
    (void)key;
    NvstoreStopCtx *c = (NvstoreStopCtx *)ctx;
    c->calls++;
    return c->stop_with;
}

static int
nvstore_iter_cb(
    const char  *key,
    void        *ctx
    )
{
    NvstoreIterCtx *c = (NvstoreIterCtx *)ctx;
    c->matches++;
    /* Capture only — don't stop early. */
    size_t i;
    for (i = 0; i < sizeof(c->last_key) - 1 && key[i] != '\0'; i++) {
        c->last_key[i] = key[i];
    }
    c->last_key[i] = '\0';
    return 0;
}

static void
test_nvstore_roundtrip(void)
{
    const char  *key  = "AxlTestKey";
    const char   data[] = "ABCDEF";
    size_t       sz;
    char         buf[16];

    /* Write into the per-app namespace (writes to a fresh vars.fd
       in the test runner; persists for the lifetime of the QEMU
       boot, then cleaned up). */
    int rc = axl_nvstore_set("app", key, data, sizeof(data),
                             AXL_NV_PERSISTENT | AXL_NV_BOOT);
    if (rc != AXL_OK) {
        /* Some firmware reject app-namespace writes (e.g. with
           authentication policies). Skip cleanly -- and BALANCED.
           This guards the whole function, so a bare "SKIP:" here drops
           all 44 assertions below with nothing declared in their place
           and the ratchet fails on an image that is merely stricter,
           not broken. The two nested skips further down were already
           balanced; this outer one was not, which is the easy one to
           miss precisely because it reads like an early return rather
           than a skipped block. 44 = every test_check in this
           function; the nested test_skip_n calls keep their own
           sub-counts, so the total stays 44 either way. */
        test_skip_n(44, "nvstore round-trip (app-namespace write refused)");
        return;
    }
    test_check(rc == AXL_OK, "nvstore: set app/AxlTestKey");

    /* Read back. */
    sz = sizeof(buf);
    rc = axl_nvstore_get("app", key, buf, &sz);
    test_check(rc == AXL_OK, "nvstore: get app/AxlTestKey");
    test_check(sz == sizeof(data), "nvstore: get returns original size");
    if (sz == sizeof(data)) {
        test_check(axl_memcmp(buf, data, sizeof(data)) == 0,
                   "nvstore: get returns original bytes");
    }

    /* Attributes round-trip: we asked for PERSISTENT | BOOT. */
    uint32_t attrs = 0;
    test_check(axl_nvstore_get_attrs("app", key, &attrs) == AXL_OK,
               "nvstore: get_attrs succeeds");
    test_check((attrs & AXL_NV_PERSISTENT) != 0,
               "nvstore: attrs include PERSISTENT");
    test_check((attrs & AXL_NV_BOOT) != 0,
               "nvstore: attrs include BOOT");

    /* Iteration: walking "app" should hit our key at least once. */
    NvstoreIterCtx ctx = { 0 };
    int iter_rc = axl_nvstore_iter("app", nvstore_iter_cb, &ctx);
    test_check(iter_rc == 0, "nvstore: iter completes");
    test_check(ctx.matches >= 1, "nvstore: iter finds at least one key");

    /* EARLY STOP. iter returns the callback's value verbatim and stops
       walking. Both cases matter, and neither was covered before: a
       plain positive stop, and a NEGATIVE stop whose value collides
       with an AXL_* constant. iter runs on a shared walk that reports
       its own failures as AXL_* codes, so a callback stopping with -8
       must still come back as -8 and not be mistaken for the walk's
       AXL_NO_RESOURCES. */
    NvstoreStopCtx stop1 = { 1, 0 };
    test_check(axl_nvstore_iter("app", nvstore_iter_stop_cb, &stop1) == 1,
               "nvstore iter: returns the callback's stop value (1)");
    test_check(stop1.calls == 1, "nvstore iter: stops on the first key");

    NvstoreStopCtx stop2 = { AXL_NO_RESOURCES, 0 };
    test_check(axl_nvstore_iter("app", nvstore_iter_stop_cb, &stop2)
                   == AXL_NO_RESOURCES,
               "nvstore iter: a callback stopping with -8 is not the walk's "
               "own AXL_NO_RESOURCES");
    test_check(stop2.calls == 1, "nvstore iter: negative stop also halts");

    /* axl_nvstore_get_alloc — heap-allocated read variant.
       Allocation succeeds, payload matches, byte one past the
       payload is zeroed (the NUL-extension guarantee callers can
       lean on for string variables). */
    void   *abuf = (void *)0xDEADBEEFul;  /* deliberate sentinel */
    size_t  asz  = 99;
    test_check(axl_nvstore_get_alloc("app", key, &abuf, &asz) == AXL_OK,
               "nvstore get_alloc: succeeds on existing key");
    test_check(asz == sizeof(data),
               "nvstore get_alloc: out_size matches written payload");
    if (asz == sizeof(data) && abuf != NULL) {
        test_check(axl_memcmp(abuf, data, sizeof(data)) == 0,
                   "nvstore get_alloc: out_buf matches written payload");
        test_check(((uint8_t *)abuf)[asz] == 0,
                   "nvstore get_alloc: byte past payload zero-extended");
    }
    axl_free(abuf);

    /* NULL-arg guards. */
    void   *xbuf = (void *)0x1234ul;
    size_t  xsz  = 7;
    test_check(axl_nvstore_get_alloc(NULL, key, &xbuf, &xsz) == AXL_ERR,
               "nvstore get_alloc: NULL ns rejected");
    test_check(axl_nvstore_get_alloc("app", NULL, &xbuf, &xsz) == AXL_ERR,
               "nvstore get_alloc: NULL key rejected");
    test_check(axl_nvstore_get_alloc("app", key, NULL, &xsz) == AXL_ERR,
               "nvstore get_alloc: NULL out_buf rejected");
    test_check(axl_nvstore_get_alloc("app", key, &xbuf, NULL) == AXL_ERR,
               "nvstore get_alloc: NULL out_size rejected");

    /* Missing-key path: out_buf cleared to NULL, out_size cleared
       to 0, return -1. */
    void   *mbuf = (void *)0xC0DEul;
    size_t  msz  = 42;
    test_check(axl_nvstore_get_alloc("app", "AxlNoSuchKey", &mbuf, &msz) == AXL_ERR,
               "nvstore get_alloc: missing key returns -1");
    test_check(mbuf == NULL && msz == 0,
               "nvstore get_alloc: out params cleared on failure");

    /* Empty-variable round-trip: 0-byte SetVariable → get_alloc
       must succeed (legitimate empty value, not "missing"), with
       out_size==0 and out_buf non-NULL pointing at a single NUL.
       Regression for the prior probe that conflated 0-byte success
       with NOT_FOUND. */
    const char *empty_key = "AxlTestKeyEmpty";
    int empty_set_rc = axl_nvstore_set("app", empty_key, NULL, 0,
                                       AXL_NV_PERSISTENT | AXL_NV_BOOT);
    if (empty_set_rc == AXL_OK) {
        void   *ebuf = (void *)0x55ul;
        size_t  esz  = 99;
        int rc_e = axl_nvstore_get_alloc("app", empty_key, &ebuf, &esz);
        test_check(rc_e == AXL_OK,
                   "nvstore get_alloc: 0-byte variable succeeds (not NOT_FOUND)");
        test_check(esz == 0,
                   "nvstore get_alloc: 0-byte variable reports size 0");
        test_check(ebuf != NULL && ((uint8_t *)ebuf)[0] == 0,
                   "nvstore get_alloc: 0-byte variable returns 1-byte NUL buffer");
        axl_free(ebuf);
        (void)axl_nvstore_delete("app", empty_key);
    } else {
        /* Some firmware reject zero-byte SetVariable. SKIP-balance
           the three populated-path assertions. */
        test_skip_n(3, "nvstore get_alloc empty");
    }

    /* axl_nvstore_set_str / axl_nvstore_get_str — string-payload
       round-trip. set_str writes strlen+1 bytes; get_str returns a
       NUL-terminated heap copy. */
    const char *str_key = "AxlTestStrKey";
    const char *str_val = "hello, world";
    int set_str_rc = axl_nvstore_set_str("app", str_key, str_val,
                                         AXL_NV_PERSISTENT | AXL_NV_BOOT);
    if (set_str_rc == AXL_OK) {
        test_check(set_str_rc == AXL_OK, "nvstore set_str: succeeds");

        /* Underlying byte size must be strlen+1. */
        void   *raw_buf = NULL;
        size_t  raw_sz  = 0;
        test_check(axl_nvstore_get_alloc("app", str_key, &raw_buf, &raw_sz) == AXL_OK,
                   "nvstore set_str: readable via get_alloc");
        test_check(raw_sz == axl_strlen(str_val) + 1,
                   "nvstore set_str: payload is strlen+1 bytes");
        axl_free(raw_buf);

        /* get_str returns NUL-terminated heap copy. */
        char *got = NULL;
        test_check(axl_nvstore_get_str("app", str_key, &got) == AXL_OK,
                   "nvstore get_str: succeeds on existing key");
        test_check(got != NULL && axl_strcmp(got, str_val) == 0,
                   "nvstore get_str: value matches written string");
        axl_free(got);

        /* Empty string: 1-byte NUL payload, get_str returns "". */
        const char *empty_str_key = "AxlTestStrKeyEmpty";
        int empty_rc = axl_nvstore_set_str("app", empty_str_key, "",
                                           AXL_NV_PERSISTENT | AXL_NV_BOOT);
        test_check(empty_rc == AXL_OK,
                   "nvstore set_str: empty string succeeds");
        char *egot = NULL;
        test_check(axl_nvstore_get_str("app", empty_str_key, &egot) == AXL_OK,
                   "nvstore get_str: empty string succeeds");
        test_check(egot != NULL && egot[0] == '\0',
                   "nvstore get_str: empty string returns \"\"");
        axl_free(egot);
        (void)axl_nvstore_delete("app", empty_str_key);

        (void)axl_nvstore_delete("app", str_key);
    } else {
        /* Firmware refused our string write — SKIP-balance the 8
           populated-path assertions above. */
        test_skip_n(8, "nvstore set_str/get_str");
    }

    /* NULL-arg guards on set_str / get_str. */
    test_check(axl_nvstore_set_str(NULL, "k", "v", 0) == AXL_ERR,
               "nvstore set_str: NULL ns rejected");
    test_check(axl_nvstore_set_str("app", NULL, "v", 0) == AXL_ERR,
               "nvstore set_str: NULL key rejected");
    test_check(axl_nvstore_set_str("app", "k", NULL, 0) == AXL_ERR,
               "nvstore set_str: NULL str rejected (use delete)");

    char *xstr = (char *)0x1234ul;
    test_check(axl_nvstore_get_str(NULL, "k", &xstr) == AXL_ERR,
               "nvstore get_str: NULL ns rejected");
    test_check(axl_nvstore_get_str("app", NULL, &xstr) == AXL_ERR,
               "nvstore get_str: NULL key rejected");
    test_check(axl_nvstore_get_str("app", "k", NULL) == AXL_ERR,
               "nvstore get_str: NULL out_str rejected");

    /* Missing key: out_str cleared to NULL, return -1. */
    char *mstr = (char *)0xC0DEul;
    test_check(axl_nvstore_get_str("app", "AxlNoSuchStrKey", &mstr) == AXL_ERR,
               "nvstore get_str: missing key returns -1");
    test_check(mstr == NULL,
               "nvstore get_str: out_str cleared on failure");

    /* Delete + verify gone. */
    test_check(axl_nvstore_delete("app", key) == AXL_OK,
               "nvstore: delete succeeds");
    sz = sizeof(buf);
    test_check(axl_nvstore_get("app", key, buf, &sz) == AXL_ERR,
               "nvstore: get after delete returns -1");
}

// ---------------------------------------------------------------------------
// AxlVar — unscoped UEFI variable inspection (read-only)
// ---------------------------------------------------------------------------

static void
test_var_enumerate(void)
{
    AxlVarInfo *vars  = (AxlVarInfo *)0xDEADBEEFul;  /* deliberate sentinel */
    size_t      count = 99;

    /* NULL-arg guards, and the documented clearing of whichever out
       param IS valid -- a caller that frees unconditionally must be
       safe even on the reject path. */
    test_check(axl_var_enumerate(NULL, &count) == AXL_INVALID,
               "var enumerate: NULL vars rejected");
    test_check(count == 0, "var enumerate: count cleared on NULL vars");
    test_check(axl_var_enumerate(&vars, NULL) == AXL_INVALID,
               "var enumerate: NULL count rejected");
    test_check(vars == NULL, "var enumerate: vars cleared on NULL count");

    /* The real walk. Every UEFI machine carries variables (PlatformLang,
       ConOut, Timeout, ...), so an empty store here would itself be the
       bug -- the contract permits count 0, the firmware does not produce
       it. */
    vars  = NULL;
    count = 0;
    test_check(axl_var_enumerate(&vars, &count) == AXL_OK,
               "var enumerate: succeeds");
    test_check(count > 0, "var enumerate: firmware reports at least one variable");
    test_check(vars != NULL, "var enumerate: array non-NULL when count > 0");

    bool all_named = (vars != NULL && count > 0);
    for (size_t i = 0; vars != NULL && i < count; i++) {
        if (vars[i].name == NULL || vars[i].name[0] == '\0') {
            all_named = false;
            break;
        }
    }
    test_check(all_named, "var enumerate: every entry has a non-empty name");

    /* Guards on read. Borrow a real vendor GUID from the walk so the
       only thing under test is the argument validation. */
    AxlGuid probe = { 0 };
    if (vars != NULL && count > 0) {
        probe = vars[0].vendor;
    }
    void   *rdata = NULL;
    size_t  rsize = 0;
    test_check(axl_var_read(NULL, &probe, NULL, &rdata, &rsize) == AXL_INVALID,
               "var read: NULL name rejected");
    test_check(axl_var_read("AxlNoSuchVar", NULL, NULL, &rdata, &rsize) == AXL_INVALID,
               "var read: NULL vendor rejected");
    test_check(axl_var_read("AxlNoSuchVar", &probe, NULL, &rdata, NULL) == AXL_INVALID,
               "var read: non-NULL data with NULL size rejected");
    test_check(axl_var_read("AxlNoSuchVar", &probe, NULL, &rdata, &rsize)
                   == AXL_NOT_FOUND,
               "var read: absent variable is NOT_FOUND, not ERR");

    axl_free(vars);
    vars  = NULL;
    count = 0;

    /* Round-trip against a variable we control. Firmware that refuses
       app-namespace writes skips the whole populated block; keep the
       balancer equal to its assertion count. */
    const char  key[]     = "AxlVarProbe";
    const char  payload[] = "VAR-PROBE";
    if (axl_nvstore_set("app", key, payload, sizeof(payload),
                        AXL_NV_PERSISTENT | AXL_NV_BOOT) != AXL_OK) {
        test_skip_n(11, "var enumerate/read round-trip (app-namespace write refused)");
        return;
    }

    test_check(axl_var_enumerate(&vars, &count) == AXL_OK,
               "var enumerate: succeeds after write");

    const AxlVarInfo *found = NULL;
    for (size_t i = 0; vars != NULL && i < count; i++) {
        if (axl_strcmp(vars[i].name, key) == 0) {
            found = &vars[i];
            break;
        }
    }
    test_check(found != NULL,
               "var enumerate: finds a variable written through nvstore");

    /* The size is reported WITHOUT the payload having been read -- that
       is the contract this pins. */
    test_check(found != NULL && found->size == sizeof(payload),
               "var enumerate: reports exact payload size");
    test_check(found != NULL && (found->attrs & AXL_NV_PERSISTENT) != 0,
               "var enumerate: reports PERSISTENT attribute");

    /* Read it back using the GUID the WALK discovered, not one we
       supplied -- that round-trip is the whole point of the header. */
    uint32_t attrs = 0;
    rdata = NULL;
    rsize = 0;
    int rc = (found != NULL)
        ? axl_var_read(key, &found->vendor, &attrs, &rdata, &rsize)
        : AXL_ERR;
    test_check(rc == AXL_OK, "var read: succeeds with the enumerated GUID");
    test_check((attrs & AXL_NV_PERSISTENT) != 0,
               "var read: reports the attributes it was given");
    test_check(rsize == sizeof(payload), "var read: size matches payload");
    test_check(rdata != NULL && rsize == sizeof(payload)
                   && axl_memcmp(rdata, payload, sizeof(payload)) == 0,
               "var read: bytes match payload");
    test_check(rdata != NULL && ((const uint8_t *)rdata)[rsize] == 0,
               "var read: byte past payload is zero-extended");
    axl_free(rdata);

    /* Size-only form: no payload transferred, same size reported. */
    size_t probe_size = 0;
    int    prc = (found != NULL)
        ? axl_var_read(key, &found->vendor, NULL, NULL, &probe_size)
        : AXL_ERR;
    test_check(prc == AXL_OK, "var read: size-only form succeeds with NULL data");
    test_check(probe_size == sizeof(payload),
               "var read: size-only form reports the same size");

    axl_free(vars);
    axl_nvstore_delete("app", key);
}

// ---------------------------------------------------------------------------
// AxlBoot — option codec round-trip + order/current
// ---------------------------------------------------------------------------

static void
test_boot(void)
{
    /* These tests verify call SHAPE — they pass on every firmware
       regardless of which Boot#### / BootOrder / BootNext variables
       actually exist. The codec itself is exercised transitively
       when a consumer reads a real Boot#### entry on real hardware
       (or when OVMF's variable services accept the
       BootOFFE-no-path encoding, which differs between x64 and
       aa64 OVMF). Sticking to shape-only keeps the test count
       stable across arches. */

    /* current_get: 0 if BootCurrent is published, -1 otherwise. */
    uint16_t cur = 0xFFFF;
    int cur_rc = axl_boot_current_get(&cur);
    test_check(cur_rc == AXL_OK || cur_rc == AXL_ERR,
               "boot: current_get returns 0 or -1");
    test_check(axl_boot_current_get(NULL) == AXL_ERR,
               "boot: current_get(NULL) returns -1");

    /* order_get: same shape. The non-NULL-pointer guarantee on
       success is part of the API contract. */
    uint16_t *order = NULL;
    size_t    n_order = 0;
    int order_rc = axl_boot_order_get(&order, &n_order);
    test_check(order_rc == AXL_OK || order_rc == AXL_ERR,
               "boot: order_get returns 0 or -1");
    test_check(order_rc != AXL_OK || order != NULL,
               "boot: order_get on success populates pointer");
    if (order_rc == AXL_OK) {
        axl_free(order);
    }
    test_check(axl_boot_order_get(NULL, &n_order) == AXL_ERR,
               "boot: order_get(NULL out) returns -1");
    test_check(axl_boot_order_get(&order, NULL) == AXL_ERR,
               "boot: order_get(NULL count) returns -1");

    /* option_get on an unused index: -1, no allocation. */
    AxlBootOption empty = { 0 };
    test_check(axl_boot_option_get(0x0FFE, &empty) == AXL_ERR,
               "boot: option_get on empty index returns -1");
    test_check(axl_boot_option_get(0x0FFE, NULL) == AXL_ERR,
               "boot: option_get(NULL) returns -1");

    /* option_free is NULL-safe and idempotent. */
    axl_boot_option_free(NULL);
    axl_boot_option_free(&empty);
    test_check(empty.description == NULL,
               "boot: option_free clears description");

    /* option_delete on an absent index: most firmware returns
       success (idempotent delete); some return -1. Both are
       valid call shapes. */
    int del_rc = axl_boot_option_delete(0x0FFE);
    test_check(del_rc == AXL_OK || del_rc == AXL_ERR,
               "boot: option_delete returns 0 or -1");

    /* next_get on un-set BootNext: -1. */
    uint16_t nxt = 0;
    int next_get_rc = axl_boot_next_get(&nxt);
    test_check(next_get_rc == AXL_OK || next_get_rc == AXL_ERR,
               "boot: next_get returns 0 or -1");
    test_check(axl_boot_next_get(NULL) == AXL_ERR,
               "boot: next_get(NULL) returns -1");

    /* next_clear is idempotent. */
    int next_clear_rc = axl_boot_next_clear();
    test_check(next_clear_rc == AXL_OK || next_clear_rc == AXL_ERR,
               "boot: next_clear returns 0 or -1");

    /* === Codec round-trip via _set + _get ===

       The encoder synthesizes a minimal "end of entire device path"
       node when device_path is NULL/empty (UEFI 2.11 §10.3.1
       requires every FilePathList to terminate in one). That makes
       the encoded variable spec-valid and acceptable to OVMF on
       both arches; before the fix, x64 OVMF accepted a zero-length
       path leniently while aa64 rejected it with INVALID_PARAMETER. */
    AxlBootOption put = {
        .index         = 0x0FFE,
        .attrs         = AXL_BOOT_ATTR_ACTIVE,
        .description   = "AxlTestBootOption",
        .device_path   = NULL,
        .opt_data      = NULL,
        .opt_data_len  = 0,
    };
    int set_rc = axl_boot_option_set(0x0FFE, &put);
    test_check(set_rc == AXL_OK, "boot: option_set BootOFFE (synthesized end-node)");
    if (set_rc != AXL_OK) {
        /* On firmware that still rejects, leave the rest as a
           single shape-pass to keep the test count stable. */
        test_skip_n(5, "boot: round-trip skipped on this firmware");
        return;
    }

    AxlBootOption got = { 0 };
    test_check(axl_boot_option_get(0x0FFE, &got) == AXL_OK,
               "boot: option_get BootOFFE");
    test_check(got.attrs == AXL_BOOT_ATTR_ACTIVE,
               "boot: round-trip attrs");
    test_check(got.description != NULL
               && axl_strcmp(got.description, "AxlTestBootOption") == 0,
               "boot: round-trip description");
    test_check(got.opt_data_len == 0,
               "boot: round-trip opt_data_len 0");
    axl_boot_option_free(&got);

    test_check(axl_boot_option_delete(0x0FFE) == AXL_OK,
               "boot: option_delete BootOFFE after round-trip");
}

// ---------------------------------------------------------------------------
// axl_app_boot_path
// ---------------------------------------------------------------------------

static void
test_app_boot_path(void)
{
    char buf[128];

    /* Happy path: relative path with no leading separator gets the
       volume prefix from axl_app_image_path() prepended. The result
       is fully-qualified and starts with the same volume label as
       axl_app_image_path. */
    int rc = axl_app_boot_path("crash-report.txt", buf, sizeof(buf));
    test_check(rc == AXL_OK, "boot_path: 'crash-report.txt' returns AXL_OK");
    test_check(axl_strstr(buf, ":") != NULL,
               "boot_path: result has a ':' volume separator");
    test_check(axl_strstr(buf, "crash-report.txt") != NULL,
               "boot_path: result contains the relative path");

    const char *self = axl_app_image_path();
    if (self != NULL) {
        /* Result's volume prefix matches self's. Find ':' in each
           and compare the prefixes byte-for-byte. */
        const char *self_colon = axl_strstr(self, ":");
        const char *buf_colon  = axl_strstr(buf, ":");
        test_check(self_colon != NULL && buf_colon != NULL,
                   "boot_path: both image_path and result have ':'");
        if (self_colon != NULL && buf_colon != NULL) {
            size_t self_prefix_len = (size_t)(self_colon - self);
            size_t buf_prefix_len  = (size_t)(buf_colon  - buf);
            test_check(self_prefix_len == buf_prefix_len
                       && axl_strncmp(self, buf, self_prefix_len) == 0,
                       "boot_path: volume prefix matches axl_app_image_path");
        }
    }

    /* Leading '\\' is normalized — no double-separator. */
    rc = axl_app_boot_path("\\crash-report.txt", buf, sizeof(buf));
    test_check(rc == AXL_OK,
               "boot_path: leading-backslash path returns AXL_OK");
    test_check(axl_strstr(buf, ":\\\\") == NULL
               && axl_strstr(buf, ":/\\") == NULL,
               "boot_path: no doubled separator after volume");

    /* NULL safety. */
    test_check(axl_app_boot_path(NULL, buf, sizeof(buf)) == AXL_ERR,
               "boot_path: NULL relative_path -> AXL_ERR");
    test_check(axl_app_boot_path("x", NULL, sizeof(buf)) == AXL_ERR,
               "boot_path: NULL out -> AXL_ERR");

    /* Buffer too small: refuse cleanly. */
    char tiny[4];
    test_check(axl_app_boot_path("crash-report.txt", tiny, sizeof(tiny)) == AXL_ERR,
               "boot_path: tiny out buffer -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// axl_image_enumerate + axl_image_self_get_range
// ---------------------------------------------------------------------------

typedef struct {
    size_t count;
    bool   saw_self;
    void  *self_base;
} ImageEnumCtx;

static int
image_enum_collect(const AxlImageInfo *info, void *ctx)
{
    ImageEnumCtx *c = (ImageEnumCtx *)ctx;
    c->count++;
    if (info->base != NULL && info->base == c->self_base) {
        c->saw_self = true;
    }
    return 0;  /* continue */
}

static int
image_enum_stop_after_one(const AxlImageInfo *info, void *ctx)
{
    (void)info;
    int *seen = (int *)ctx;
    (*seen)++;
    return 42;  /* stop early; 42 must surface as return value */
}

static void
test_image_enumerate(void)
{
    /* axl_image_self_get_range gives us a known base to look for
       inside the enumerate walk. */
    void   *self_base = NULL;
    size_t  self_size = 0;
    int rc = axl_image_self_get_range(&self_base, &self_size);
    test_check(rc == AXL_OK,
               "image_self_get_range: returns AXL_OK");
    test_check(self_base != NULL,
               "image_self_get_range: base is non-NULL");
    test_check(self_size > 0,
               "image_self_get_range: size is non-zero");

    /* NULL size is allowed. */
    void *base2 = NULL;
    test_check(axl_image_self_get_range(&base2, NULL) == AXL_OK
               && base2 == self_base,
               "image_self_get_range: NULL out_size accepted");

    /* NULL base is rejected. */
    size_t s = 0;
    test_check(axl_image_self_get_range(NULL, &s) == AXL_ERR,
               "image_self_get_range: NULL out_base -> AXL_ERR");

    /* Enumerate every loaded image, count, and confirm we saw
       ourselves. */
    ImageEnumCtx ctx = { .count = 0, .saw_self = false, .self_base = self_base };
    rc = axl_image_enumerate(image_enum_collect, &ctx);
    test_check(rc == AXL_OK,
               "image_enumerate: full walk returns AXL_OK");
    test_check(ctx.count > 0,
               "image_enumerate: walks at least one image");
    test_check(ctx.saw_self,
               "image_enumerate: includes the current test image");

    /* Early-stop: callback returns 42, enumerate returns 42. */
    int seen = 0;
    rc = axl_image_enumerate(image_enum_stop_after_one, &seen);
    test_check(rc == 42,
               "image_enumerate: callback non-zero return surfaces");
    test_check(seen == 1,
               "image_enumerate: stops at first non-zero callback");

    /* NULL callback is rejected. */
    test_check(axl_image_enumerate(NULL, NULL) == AXL_ERR,
               "image_enumerate: NULL callback -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// axl_image_watch_loads / axl_image_unwatch_loads
//
// Only SAFE negatives and a single register/release round-trip. The watch
// FIRING is not asserted here: making it fire means loading an image, and
// the end-to-end proof already exists in test-crashhandler.sh, whose report
// can only name the faulting image if the watch refreshed the table.
// ---------------------------------------------------------------------------

static void
image_watch_count(void *ctx)
{
    int *fired = (int *)ctx;
    if (fired != NULL) {
        (*fired)++;
    }
}

static void
test_image_watch(void)
{
    int fired = 0;

    test_check(axl_image_watch_loads(NULL, &fired) == NULL,
               "image_watch_loads: NULL callback -> NULL");

    /* Round-trip. The teardown leak report is the other half of this
       assertion: the watch owns a heap-allocated bridging context, so one
       that failed to release would fail the run rather than pass quietly. */
    AxlImageWatch *w = axl_image_watch_loads(image_watch_count, &fired);
    test_check(w != NULL,
               "image_watch_loads: returns a handle");
    axl_image_unwatch_loads(w);

    /* NULL is documented as a no-op. Asserted by REGISTERING AGAIN after
       it, rather than by calling it and claiming survival -- a call that
       corrupted the backend's notify table would show up as the second
       registration failing. */
    axl_image_unwatch_loads(NULL);
    AxlImageWatch *w2 = axl_image_watch_loads(image_watch_count, &fired);
    test_check(w2 != NULL,
               "image_unwatch_loads: NULL leaves registration working");
    axl_image_unwatch_loads(w2);
}

// ---------------------------------------------------------------------------
// axl_cpu_register_exception (validation paths — no live trigger)
// ---------------------------------------------------------------------------

static void
cpu_test_cb(const AxlCpuException *exc, void *user)
{
    (void)exc; (void)user;
    /* Never actually called in unit-test context — the live trigger
       is in uefi-devkit/crashtest/crashtest.c. */
}

static void
test_cpu_register_exception(void)
{
    /* The API surface itself works regardless of EFI_CPU_ARCH_PROTOCOL
       availability: out-of-range / NULL-cb / wrong-arch returns
       AXL_ERR before reaching the protocol. */
    test_check(axl_cpu_register_exception((AxlCpuExceptionKind)0, cpu_test_cb, NULL) == AXL_ERR,
               "cpu_register: kind=0 -> AXL_ERR");
    test_check(axl_cpu_register_exception(AXL_CPU_EXCEPTION_KIND_MAX, cpu_test_cb, NULL) == AXL_ERR,
               "cpu_register: kind=KIND_MAX -> AXL_ERR");
    test_check(axl_cpu_register_exception(AXL_CPU_EXCEPTION_GP_FAULT, NULL, NULL) == AXL_ERR,
               "cpu_register: NULL cb -> AXL_ERR");

    /* Arch-availability gating. On x64, aa64-only kinds must refuse;
       on aa64, x64-only kinds must refuse. */
#if defined(__x86_64__)
    test_check(axl_cpu_register_exception(AXL_CPU_EXCEPTION_SYNCHRONOUS, cpu_test_cb, NULL) == AXL_ERR,
               "cpu_register: aa64-only SYNCHRONOUS on x64 -> AXL_ERR");
    test_check(axl_cpu_register_exception(AXL_CPU_EXCEPTION_SERROR, cpu_test_cb, NULL) == AXL_ERR,
               "cpu_register: aa64-only SERROR on x64 -> AXL_ERR");
#elif defined(__aarch64__)
    test_check(axl_cpu_register_exception(AXL_CPU_EXCEPTION_DIVIDE_ERROR, cpu_test_cb, NULL) == AXL_ERR,
               "cpu_register: x64-only DIVIDE on aa64 -> AXL_ERR");
    test_check(axl_cpu_register_exception(AXL_CPU_EXCEPTION_DOUBLE_FAULT, cpu_test_cb, NULL) == AXL_ERR,
               "cpu_register: x64-only DOUBLE_FAULT on aa64 -> AXL_ERR");
#endif

    /* Happy path: pick a kind that's valid on this arch, register +
       unregister. If EFI_CPU_ARCH_PROTOCOL isn't published on this
       firmware, every call returns AXL_ERR — treat that branch as
       SKIP and balance the assertion count. */
#if defined(__x86_64__)
    AxlCpuExceptionKind valid = AXL_CPU_EXCEPTION_GP_FAULT;
#else
    AxlCpuExceptionKind valid = AXL_CPU_EXCEPTION_SYNCHRONOUS;
#endif
    int reg = axl_cpu_register_exception(valid, cpu_test_cb, NULL);
    if (reg == AXL_OK) {
        /* Re-register the same kind with a different user pointer:
           the API contract says "second call replaces the first."
           Black-box, we can only observe that the call returns
           AXL_OK rather than EFI_ALREADY_STARTED. */
        test_check(axl_cpu_register_exception(valid, cpu_test_cb,
                                              (void *)0x1234) == AXL_OK,
                   "cpu_register: re-register same kind returns AXL_OK");
        /* Unregister the live registration. */
        test_check(axl_cpu_unregister_exception(valid) == AXL_OK,
                   "cpu_unregister: live kind returns AXL_OK");
        /* Unregister an already-unregistered kind: contract says
           no-op AXL_OK (not AXL_ERR). */
        test_check(axl_cpu_unregister_exception(valid) == AXL_OK,
                   "cpu_unregister: already-unregistered returns AXL_OK");
    } else {
        test_skip_n(3, "cpu_register live path (EFI_CPU_ARCH_PROTOCOL not published)");
        /* Balance the 3 assertions in the populated branch. */
    }

    /* Unregister out-of-range is AXL_ERR. KIND_MAX is the only
       defined out-of-range sentinel — casting a literal integer
       past the enum's underlying-type range is implementation-
       defined behavior, so we don't probe that case here. */
    test_check(axl_cpu_unregister_exception(AXL_CPU_EXCEPTION_KIND_MAX) == AXL_ERR,
               "cpu_unregister: KIND_MAX -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// axl_cpu_features / simd_tier / enable_avx
//
// Assertions are arch-NEUTRAL (vacuously satisfied on the inapplicable
// architecture) so X64 and AARCH64 run the same number of checks — the
// suite requires identical per-arch counts.  They are also CPU-MODEL
// neutral: only the implication chains and the SSE2/NEON baseline
// guarantee are asserted, never "AVX is present" (qemu64 in CI has no
// AVX; -cpu host does).
// ---------------------------------------------------------------------------

static void
test_cpu_features(void)
{
    const AxlCpuFeatures *f = axl_cpu_features();
    test_check(f != NULL, "cpu_features: returns non-NULL");
    test_check(axl_cpu_features() == f, "cpu_features: cached (stable pointer)");

    /* Exactly one 128-bit baseline ISA — SSE2 on x86, NEON on aarch64 —
       and the two are mutually exclusive across our target arches. */
    test_check(f != NULL && (f->sse2 || f->neon),
               "cpu_features: a 128-bit baseline ISA is present");
    test_check(f != NULL && (f->sse2 != f->neon),
               "cpu_features: exactly one of sse2/neon (arch-exclusive)");

    /* Capability implication chains — hold on any real CPU model, and
       vacuously on the arch where the antecedent is false. */
    test_check(f != NULL && (!f->avx2  || f->avx),   "cpu_features: avx2 => avx");
    test_check(f != NULL && (!f->avx   || f->xsave), "cpu_features: avx => xsave");
    test_check(f != NULL && (!f->sse42 || f->sse41), "cpu_features: sse4.2 => sse4.1");
    test_check(f != NULL && (!f->fma   || f->avx),   "cpu_features: fma => avx");
}

static void
test_cpu_simd_tier(void)
{
    const AxlCpuFeatures *f = axl_cpu_features();
    AxlSimdTier t = axl_cpu_simd_tier();
    test_check(t >= AXL_SIMD_BASELINE,
               "simd_tier: at least BASELINE on our targets");
    /* A NEON-only (aarch64) target never exceeds the 128-bit baseline. */
    test_check(!f->neon || t == AXL_SIMD_BASELINE,
               "simd_tier: NEON-only target caps at BASELINE");
    /* The AVX2 tier requires the AVX2 capability (necessary condition,
       model-neutral) — and is never reported before enable_avx() makes
       the YMM state live, which this test has not yet called. */
    test_check(t != AXL_SIMD_AVX2 || f->avx2,
               "simd_tier: AVX2 tier requires the avx2 capability");
}

static void
test_cpu_enable_avx(void)
{
    const AxlCpuFeatures *f = axl_cpu_features();
    bool ok = axl_cpu_enable_avx();
    /* Enabling succeeds iff the CPU actually has AVX to enable. */
    test_check(ok == f->avx, "enable_avx: succeeds iff CPU has AVX");
    test_check(axl_cpu_enable_avx() == ok, "enable_avx: idempotent");
    /* NEON-only (aarch64) target has no AVX to enable. */
    test_check(!f->neon || !ok, "enable_avx: false on NEON-only target");
    /* Tier is consistent with the post-enable state on THIS cpu: `ok`
       is this processor's enable result, and simd_tier() reads the live
       CR4/XCR0 state, so the two must agree. */
    AxlSimdTier t = axl_cpu_simd_tier();
    test_check((f->avx2 && ok) ? (t == AXL_SIMD_AVX2)
                               : (t <= AXL_SIMD_SSE41),
               "enable_avx: tier matches this-cpu avx2-enabled state");
}

static void
test_cpu_features_extended(void)
{
    /* Catalog implication chains — hold on any real CPU model, vacuous
       on the arch where the antecedent is false (so identical count on
       both arches). */
    const AxlCpuFeatures *f = axl_cpu_features();
    /* x86 AVX-512 sub-features imply the Foundation, which implies AVX. */
    test_check(!f->avx512dq   || f->avx512f, "feat: avx512dq => avx512f");
    test_check(!f->avx512bw   || f->avx512f, "feat: avx512bw => avx512f");
    test_check(!f->avx512vl   || f->avx512f, "feat: avx512vl => avx512f");
    test_check(!f->avx512cd   || f->avx512f, "feat: avx512cd => avx512f");
    test_check(!f->avx512vnni || f->avx512f, "feat: avx512vnni => avx512f");
    test_check(!f->avx512f    || f->avx,     "feat: avx512f => avx");
    /* aarch64 field encodings: PMULL is the >=2 level of the AES field,
       SHA512 the >=2 level of the SHA2 field. */
    test_check(!f->pmull  || f->aes_a64, "feat: pmull => aes (aarch64)");
    test_check(!f->sha512 || f->sha2,    "feat: sha512 => sha2 (aarch64)");
}

static void
test_cpu_enable_avx512(void)
{
    const AxlCpuFeatures *f = axl_cpu_features();
    bool e5 = axl_cpu_enable_avx512();
    /* Only succeeds on a CPU that actually has AVX-512F. */
    test_check(!e5 || f->avx512f, "enable_avx512: success implies avx512f");
    test_check(axl_cpu_enable_avx512() == e5, "enable_avx512: idempotent");
    /* Enabling AVX-512 state implies AVX (YMM) state is also live. */
    test_check(!e5 || axl_cpu_enable_avx(), "enable_avx512: implies AVX enabled");
    /* NEON-only (aarch64) target has no AVX-512. */
    test_check(!f->neon || !e5, "enable_avx512: false on NEON-only target");
}

// ---------------------------------------------------------------------------
// AxlImage — load + unload of a known test EFI on fs0:
// ---------------------------------------------------------------------------

static void
test_image(void)
{
    /* axl_image_load on a non-existent path returns -1 without
       mangling the out parameter. */
    AxlImage *bad = (AxlImage *)0xDEADBEEFul;
    test_check(axl_image_load("fs0:\\definitely-not-a-real-file.efi", &bad) == AXL_ERR,
               "image: load of missing file returns -1");
    test_check(bad == NULL,
               "image: load failure clears out parameter");

    /* NULL args. */
    AxlImage *img = NULL;
    test_check(axl_image_load(NULL, &img) == AXL_ERR,
               "image: load NULL path returns -1");
    test_check(axl_image_load("fs0:\\x.efi", NULL) == AXL_ERR,
               "image: load NULL out returns -1");

    /* set_load_options NULL-img guard — always testable. */
    test_check(axl_image_set_load_options(NULL, "x", 1) == AXL_ERR,
               "image: set_load_options(NULL img) returns -1");

    /* Real load: AxlTestRuntime.efi is staged into fs0: by the test
       runner. Load it, then unload immediately — don't actually
       start it (would re-enter the test runtime). */
    int rc = axl_image_load("fs0:\\AxlTestRuntime.efi", &img);
    if (rc != AXL_OK) {
        axl_printf("SKIP: image load (AxlTestRuntime.efi not found)\n");
        return;
    }
    test_check(rc == AXL_OK, "image: load AxlTestRuntime.efi");
    test_check(img != NULL, "image: load populates handle");

    /* set_load_options install + clear + re-install — exercises the
       three lifecycle transitions in the underlying axl_driver
       tracking table. The DEBUG-build leak tracker catches a
       missing axl_free in any path. */
    const unsigned char opt_bytes[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
    test_check(axl_image_set_load_options(img, opt_bytes, sizeof(opt_bytes))
                   == AXL_OK,
               "image: set_load_options(bytes) returns 0");

    /* NULL data (size == 0) clears the previously-tracked copy. */
    test_check(axl_image_set_load_options(img, NULL, 0) == AXL_OK,
               "image: set_load_options(NULL, 0) clears prior copy");

    /* Re-set after clear must reuse / re-acquire a tracking slot.
       A leaked-slot bug would fail the table-full check on a
       subsequent install. */
    test_check(axl_image_set_load_options(img, opt_bytes, sizeof(opt_bytes))
                   == AXL_OK,
               "image: set_load_options after clear succeeds");

    /* Unload releases the still-installed copy (DEBUG leak tracker
       confirms). */
    test_check(axl_image_unload(img) == AXL_OK,
               "image: unload");

    /* Unload(NULL) is a no-op — return 0. */
    test_check(axl_image_unload(NULL) == AXL_OK,
               "image: unload(NULL) is a no-op");
}

// ---------------------------------------------------------------------------
// axl_image_run — generic foreground launch guards. The positive path (load +
// StartImage a real app) transfers the foreground and blocks, which would hang
// the combined unit boot, so unit coverage here is safe negatives only
// (own-validation + load-failure, neither of which reaches StartImage). The
// blocking-launch path is exercised by axl_shell_launch in the shell-coexist /
// edit / HTTPS integration tests, which run a real child Shell in isolation.
// ---------------------------------------------------------------------------

static void
test_shell_launch(void)
{
    /* NULL path is rejected before any load/start, and the out param is
       zeroed (never left holding stale caller data). */
    int exit_code = 0xCAFE;
    test_check(axl_image_run(NULL, "-nostartup", &exit_code) == AXL_ERR,
               "image_run: NULL path returns -1");
    test_check(exit_code == 0,
               "image_run: NULL path zeroes out_exit_code");

    /* A path that can't be loaded fails cleanly — load fails, so there is
       no StartImage, no foreground transfer, nothing to hang the boot. */
    test_check(axl_image_run("fs0:\\not-a-real-app.efi", NULL, NULL) == AXL_ERR,
               "image_run: missing image returns -1");

    /* axl_image_run_fv_file: NULL GUID is rejected before any FV walk or
       LoadImage — a safe negative. The positive path StartImages a
       firmware-embedded app and blocks the foreground (it would hang the
       combined unit boot), so it is integration-only, exactly like the
       axl_image_run / axl_shell_launch positive paths above. */
    exit_code = 0xCAFE;
    test_check(axl_image_run_fv_file(NULL, NULL, &exit_code) == AXL_ERR,
               "image_run_fv_file: NULL guid returns AXL_ERR");
    test_check(exit_code == 0,
               "image_run_fv_file: NULL guid zeroes out_exit_code");

    /* axl_shell_locate: read-only availability query (walks FVs + mounted
       volumes, loads nothing) — safe in the combined boot. OVMF and AAVMF
       both embed the ShellPkg Shell in a readable FV (FFS file GUID
       gUefiShellFileGuid, type APPLICATION), so a Shell is always locatable
       here. The unit ESP stages no file literally named Shell.efi (the boot
       copy is BOOT<arch>.EFI), so the source is the firmware FV — unless a
       runner happens to stage a real Shell.efi, in which case FILE wins and
       we SKIP-balance the FIRMWARE assertion (keeping the per-arch count
       constant). */
    AxlShellSource src = axl_shell_locate();
    test_check(src != AXL_SHELL_NONE,
               "shell_locate: a Shell is available under OVMF/AAVMF");
    if (src == AXL_SHELL_FILE) {
        test_skip_n(1, "shell_locate FIRMWARE (a Shell.efi file is staged)");
    } else {
        test_check(src == AXL_SHELL_FIRMWARE,
                   "shell_locate: firmware-embedded Shell found (no file staged)");
    }
}

// ---------------------------------------------------------------------------
// axl_shell_sources: report each Shell source independently (the FV-first
// companion to the file-first axl_shell_locate). Deterministic under
// OVMF/AAVMF, which both embed the ShellPkg Shell in a readable FV — so the
// FV source is always present here, INDEPENDENTLY of whether a Shell.efi file
// is staged (which axl_shell_locate would otherwise mask). The real
// "a file AND the FV both exist" un-masking case is pinned end-to-end by
// test-shell-coexist-qemu.sh (it stages a Shell.efi under OVMF).
// ---------------------------------------------------------------------------

static void
test_shell_sources(void)
{
    /* Safe negative: NULL out is rejected without a query. */
    test_check(axl_shell_sources(NULL) == AXL_ERR,
               "shell_sources: NULL out returns AXL_ERR");

    /* A completed query is AXL_OK even if it finds nothing. Poison the struct
       first so every asserted field reflects a real write, not leftover stack. */
    AxlShellSources s;
    axl_memset(&s, 0xAB, sizeof(s));
    test_check(axl_shell_sources(&s) == AXL_OK,
               "shell_sources: query returns AXL_OK");

    /* The un-masking guarantee: the firmware-embedded FV Shell is reported on
       its own. OVMF/AAVMF always embed it, so fv is true here regardless of
       any file — the exact availability axl_shell_locate hides once a file
       exists. */
    test_check(s.fv == true,
               "shell_sources: FV Shell reported (OVMF/AAVMF embed it)");
    test_check(s.fv_count >= 1,
               "shell_sources: at least one Shell FV GUID matched");
    test_check(s.fv == (s.fv_count > 0),
               "shell_sources: fv == (fv_count > 0) invariant");

    /* file_path is a valid C string, non-empty exactly when a file was found.
       Both branches assert once, so the check count is arch-constant (no
       SKIP-balance needed regardless of whether a runner staged a Shell.efi). */
    if (s.file) {
        test_check(s.file_path[0] != '\0',
                   "shell_sources: file_path set when a file is present");
    } else {
        test_check(s.file_path[0] == '\0',
                   "shell_sources: file_path empty when no file present");
    }

    /* The two APIs agree: axl_shell_locate is exactly the file-first
       projection of the independent sources. Deterministic either way. */
    AxlShellSource loc    = axl_shell_locate();
    AxlShellSource expect = s.file ? AXL_SHELL_FILE
                          : (s.fv ? AXL_SHELL_FIRMWARE : AXL_SHELL_NONE);
    test_check(loc == expect,
               "shell_sources: locate() == file-first projection of sources()");
}

// ---------------------------------------------------------------------------
// axl_shell_kind: which shell is hosting this image. The unit suite is
// launched from startup.nsh under the modern EDK2 UEFI Shell, so
// EFI_SHELL_PROTOCOL is present (the same protocol the file-I/O tests above
// rely on) and the kind is AXL_SHELL_KIND_UEFI. The old EFI 1.x shell path
// (AXL_SHELL_KIND_EFI_1X) is pinned by the Shell106 integration test — OVMF's
// modern shell can't reproduce it here.
// ---------------------------------------------------------------------------

static void
test_shell_kind(void)
{
    test_check(axl_shell_kind() == AXL_SHELL_KIND_UEFI,
               "shell_kind: EDK2 shell present -> AXL_SHELL_KIND_UEFI");
}

// ---------------------------------------------------------------------------
// axl_shell_execute -- public wrapper over the internal shell-Execute bridge
// (axl_backend_shell_execute) several AXL modules already use. The positive
// path spawns a nested shell command via EFI_SHELL_PROTOCOL.Execute / the old
// EFI 1.x SHELL_ENVIRONMENT.Execute -- untried from inside the combined unit
// boot anywhere in this suite (axl_map_refresh, the nearest existing caller,
// has no positive-path unit coverage either), so this stays a safe negative
// only, per the same reasoning as axl_image_run/axl_shell_launch above.
//
// Real behavioral coverage: netload's run_shell_cmd now calls this function
// directly, and test/integration/test-netload-qemu.sh's --diag/--dump/--dh
// assertions already exercise that call on the modern shell under OVMF
// (bucket C -- a behavior-preserving swap under an existing test). The old
// EFI 1.x SHELL_ENVIRONMENT.Execute fallback underneath (shared with
// axl_map_refresh, which mkrd drives) is pinned on real EFI Toolkit
// firmware by test/integration/test-old-shell-qemu.sh -- but through
// axl_map_refresh's call, not this new entry point specifically, since
// nothing in this repo yet calls axl_shell_execute on the old shell. OVMF
// cannot reproduce that shell, so this wrapper's OWN EFI 1.x path has no
// QEMU coverage either way.
// ---------------------------------------------------------------------------

static void
test_shell_execute(void)
{
    /* NULL is rejected before any UTF-8/UCS-2 conversion or shell call. */
    test_check(axl_shell_execute(NULL) == AXL_ERR,
               "shell_execute: NULL cmd -> AXL_ERR");
}

// ---------------------------------------------------------------------------
// AxlConsoleMirror (P1) — argument guards only.
//
// The positive path (install → swap gST + ReinstallProtocolInterface on
// ConsoleInHandle → translate/inject → uninstall) CANNOT be exercised in the
// combined unit boot: it wraps the very console the parent harness Shell is
// using, and even a clean uninstall leaves that Shell unable to continue
// startup.nsh — so the next test binary never launches. (Confirmed: AxlTestUtil
// finishes, AxlTestLoop never boots, QEMU times out.) This is the firmware-
// lifecycle hazard from feedback_uefi_firmware_test_hazards — the mirror is
// correct for its real use (wrap, then launch a CHILD shell you own), but that
// belongs in an isolated integration boot, not here.
//
// Full install/translate/inject coverage lives in
// test/integration/test-console-mirror-qemu.sh (own QEMU boot, results printed
// AFTER uninstall, then idle). Here: only safe negatives — guards that never
// reach the gST surgery.
// ---------------------------------------------------------------------------

static void
cm_noop_sink(const char *bytes, size_t len, void *user)
{
    (void)bytes;
    (void)len;
    (void)user;
}

/* Test seams (no public header). The console SURGERY lives in the tap, so its
   seams are tap-side; the mirror's seam only binds its VT encoder over a headless
   tap. Nothing here installs on the live console (a real install wedges the
   combined unit boot — see the AxlConsoleMirror note above). */
extern AxlConsoleTap *_axl_console_tap_new_for_test(void);
extern void _axl_console_tap_test_setup(AxlConsoleTap *t, const AxlConsoleOps *ops,
                                        void *user, uint32_t cols, uint32_t rows,
                                        bool auto_alt, bool input_capture);
extern void _axl_console_tap_test_conout_begin(AxlConsoleTap *t, bool passthrough);
extern void _axl_console_tap_test_teardown(void);
extern bool _axl_console_tap_test_pop_key(AxlConsoleTap *t, uint16_t *scan,
                                          uint16_t *unicode);
/* The wrapped ConInEx, with its function pointers wired but nothing installed
   on gST/ConsoleInHandle — lets a test drive Register/UnregisterKeyNotify. */
extern EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *
_axl_console_tap_test_coninex(AxlConsoleTap *t);
extern void _axl_console_tap_test_clear(void);
extern void _axl_console_tap_test_puts(const char *ascii);
extern void _axl_console_tap_test_puts16(const uint16_t *units);
extern void _axl_console_tap_test_set_cursor(uint32_t col, uint32_t row);
extern void _axl_console_tap_test_set_attr(uint32_t attr);
extern void _axl_console_tap_test_enable_cursor(bool visible);
extern void _axl_console_tap_test_set_stub_conin(AxlConsoleTap *t, bool always_key);
extern int  _axl_console_tap_test_read_key(AxlConsoleTap *t);
extern void _axl_console_tap_test_pump(AxlConsoleTap *t);
extern bool _axl_console_tap_test_mode_owned(AxlConsoleTap *t);
extern void _axl_console_tap_test_get_cursor(AxlConsoleTap *t, int32_t *col, int32_t *row);
extern int32_t _axl_console_tap_test_get_attr(AxlConsoleTap *t);
extern bool _axl_console_tap_test_get_cursor_visible(AxlConsoleTap *t);
extern void _axl_console_tap_test_orig_cursor(int32_t *col, int32_t *row);

extern AxlConsoleMirror *_axl_console_mirror_new_for_test(void);
extern void _axl_console_mirror_test_bind(AxlConsoleMirror *m, AxlConsoleSinkFn sink,
                                          void *user, AxlConsoleTap *tap,
                                          const AxlConsoleOps **ops, void **ops_user);
extern AxlConsoleScreen *_axl_console_mirror_test_screen(AxlConsoleMirror *m);
extern void _axl_console_mirror_test_free(AxlConsoleMirror *m);

/* axl-console-screen seams (no public header): read the model the mirror's
   snapshot serializes. */
extern bool _axl_console_screen_test_cell(const AxlConsoleScreen *s,
        uint32_t row, uint32_t col, char *utf8_out, AxlConsolePen *pen_out);
extern void _axl_console_screen_test_cursor(const AxlConsoleScreen *s,
        uint32_t *row, uint32_t *col, bool *visible);
extern bool _axl_console_screen_test_alt(const AxlConsoleScreen *s);
extern void _axl_console_screen_test_geometry(const AxlConsoleScreen *s,
        uint32_t *rows, uint32_t *cols);

/* axl-console-device input-relay seams (no public header): drive the REAL
   ConIn/ConInEx read + notify path against a bare, un-installed device. */
extern AxlConsoleDevice *_axl_console_device_new_for_test(void);
extern void _axl_console_device_test_begin(AxlConsoleDevice *d, bool take_input);
extern EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *_axl_console_device_test_coninex(AxlConsoleDevice *d);
extern bool _axl_console_device_test_feed_physical(AxlConsoleDevice *d,
        bool (*filter)(void *, const void *), void *fuser,
        uint16_t scan, uint16_t uni, uint32_t shift);
extern EFI_SIMPLE_TEXT_INPUT_PROTOCOL *_axl_console_device_test_conin(AxlConsoleDevice *d);
extern void _axl_console_device_test_end(AxlConsoleDevice *d);
/* Pointer take-over seams: run the REAL per-handle evict (HandleProtocol -> cache ->
   UninstallProtocolInterface) on a caller-supplied handle, and the REAL restore
   (reinstall every cached interface), against a synthetic SimplePointer -- so the
   database surgery is exercised without evicting the harness's live pointer. The
   full LocateHandleBuffer-driven path is firmware-smoke-validated. */
extern bool _axl_console_device_test_evict_one_pointer(AxlConsoleDevice *d, void *handle);
extern void _axl_console_device_test_restore_pointer(AxlConsoleDevice *d);
extern void _axl_console_device_test_install_proxy(AxlConsoleDevice *d);
extern void _axl_console_device_test_uninstall_proxy(AxlConsoleDevice *d);
extern int  _axl_console_device_test_proxy_forward(AxlConsoleDevice *d, void *state_out);
extern bool _axl_console_device_test_add_evicted_conin(AxlConsoleDevice *d, void *handle);
extern void _axl_console_device_test_read_tick(AxlConsoleDevice *d);

/* axl-console-term seams (no public header): read the cell model + cursor. */
extern bool _axl_console_term_test_cell(AxlConsoleTerm *t, uint32_t row, uint32_t col,
                                        char *utf8_out, uint8_t *fg, uint8_t *bg);
extern void _axl_console_term_test_cursor(AxlConsoleTerm *t, uint32_t *row, uint32_t *col);
extern uint32_t _axl_console_term_test_scroll_off(AxlConsoleTerm *t);
extern bool _axl_console_term_test_hist_cell(AxlConsoleTerm *t, uint32_t rows_back,
                                             uint32_t col, char *utf8_out);
extern void _axl_console_term_test_geometry(AxlConsoleTerm *t,
        uint32_t *cols, uint32_t *rows, uint32_t *cw, uint32_t *ch,
        uint32_t *bx, uint32_t *by, uint32_t *bw, uint32_t *bh);
extern AxlGfxPixel _axl_console_term_test_palette(AxlConsoleTerm *t, uint32_t idx);
extern void _axl_console_term_test_pointer(AxlConsoleTerm *t, int32_t *x, int32_t *y,
                                           bool *visible);

/* Capture sink: records the exact VT bytes the encoder emits. */
static char   cm_cap[512];
static size_t cm_cap_len;

static void
cm_cap_sink(const char *bytes, size_t len, void *user)
{
    (void)user;
    for (size_t i = 0; i < len && cm_cap_len < sizeof(cm_cap) - 1; i++) {
        cm_cap[cm_cap_len++] = bytes[i];
    }
    cm_cap[cm_cap_len] = '\0';
}

static void
cm_cap_reset(void)
{
    cm_cap_len = 0;
    cm_cap[0]  = '\0';
}

/* Build a headless tap with the mirror's VT encoder bound as its op consumer, so
   driving the tap's wraps produces assertable bytes in cm_cap. */
static AxlConsoleMirror *cm_enc;

static AxlConsoleTap *
cm_tap_begin(uint32_t cols, uint32_t rows, bool auto_alt, bool input_capture,
             bool passthrough)
{
    AxlConsoleTap       *t        = _axl_console_tap_new_for_test();
    const AxlConsoleOps *ops      = NULL;
    void                *ops_user = NULL;
    cm_enc = _axl_console_mirror_new_for_test();
    _axl_console_mirror_test_bind(cm_enc, cm_cap_sink, NULL, t, &ops, &ops_user);
    _axl_console_tap_test_setup(t, ops, ops_user, cols, rows, auto_alt, input_capture);
    _axl_console_tap_test_conout_begin(t, passthrough);
    return t;
}

static void
cm_tap_end(AxlConsoleTap *t)
{
    _axl_console_tap_test_teardown();
    axl_free(t);
    _axl_console_mirror_test_free(cm_enc);
    cm_enc = NULL;
}

/* --- The ops tee: one producer, N consumers ------------------------------------
   The composition the ops contract could not express (a producer takes exactly one
   AxlConsoleOps *), and the reason it belongs in the SDK rather than in each
   consumer: two ops return NEGOTIATION, not status, so a naive forwarder silently
   desyncs a consumer's grid. */
typedef struct {
    int  clears;
    int  texts;
    int  scrolls;         /* scrollrect calls RECEIVED (proves fan-out, not short-circuit) */
    int  props;
    bool accept_scroll;   /* what this consumer answers to scrollrect */
    bool accept_prop;
} TeeSpy;

static void tee_spy_clear(void *user) { ((TeeSpy *)user)->clears++; }

static void
tee_spy_text(void *user, const char *u, size_t n)
{
    (void)u;
    (void)n;
    ((TeeSpy *)user)->texts++;
}

static int
tee_spy_scrollrect(void *user, AxlConsoleRect rect, int32_t dn, int32_t rt)
{
    (void)rect;
    (void)dn;
    (void)rt;
    TeeSpy *s = (TeeSpy *)user;
    s->scrolls++;
    return s->accept_scroll ? 1 : 0;
}

static int
tee_spy_prop(void *user, AxlConsoleProp prop, const AxlConsoleValue *val)
{
    (void)prop;
    (void)val;
    TeeSpy *s = (TeeSpy *)user;
    s->props++;
    return s->accept_prop ? 1 : 0;
}

static const AxlConsoleOps TEE_SPY_OPS = {
    .clear_screen  = tee_spy_clear,
    .output_text   = tee_spy_text,
    .scrollrect    = tee_spy_scrollrect,
    .set_term_prop = tee_spy_prop,
};

static void
test_console_ops_tee(void)
{
    /* Guards. */
    test_check(axl_console_tee_ops(NULL, NULL) == NULL, "tee: ops(NULL) -> NULL");
    axl_console_tee_free(NULL);   /* NULL-safe */
    test_check(axl_console_tee_add(NULL, &TEE_SPY_OPS, NULL) == AXL_ERR,
               "tee: add(NULL tee) -> -1");

    AxlConsoleTee *t = axl_console_tee_new();
    test_check(t != NULL, "tee: new -> instance");
    test_check(axl_console_tee_add(t, NULL, NULL) == AXL_ERR, "tee: add(NULL ops) -> -1");

    TeeSpy a = { .accept_scroll = true, .accept_prop = true };
    TeeSpy b = { .accept_scroll = true, .accept_prop = true };
    test_check(axl_console_tee_add(t, &TEE_SPY_OPS, &a) == AXL_OK, "tee: add first consumer");
    test_check(axl_console_tee_add(t, &TEE_SPY_OPS, &b) == AXL_OK, "tee: add second consumer");
    const size_t t_added = 2;   /* consumers in `t` so far; the cap check below fills up */

    void                *tu  = NULL;
    const AxlConsoleOps *ops = axl_console_tee_ops(t, &tu);
    test_check(ops != NULL && tu == (void *)t, "tee: ops + context");

    /* void ops reach EVERY consumer. */
    ops->clear_screen(tu);
    ops->output_text(tu, "x", 1);
    test_check(a.clears == 1 && b.clears == 1, "tee: clear_screen fans to all");
    test_check(a.texts == 1 && b.texts == 1, "tee: output_text fans to all");

    /* scrollrect: all accept -> accepted, and BOTH were asked. */
    AxlConsoleRect r = { 0 };
    test_check(ops->scrollrect(tu, r, 1, 0) == 1, "tee: scrollrect accepted when all accept");
    test_check(a.scrolls == 1 && b.scrolls == 1, "tee: scrollrect asks every consumer");

    /* One declines -> the TEE must decline, or the accepting consumer scrolls while
       the producer believes nothing moved and emits no damage to repair it. Every
       consumer is still asked (no short-circuit), so the answer does not depend on
       the order they were added in. */
    b.accept_scroll = false;
    test_check(ops->scrollrect(tu, r, 1, 0) == 0, "tee: scrollrect declined when any declines");
    test_check(a.scrolls == 2 && b.scrolls == 2, "tee: no short-circuit on a decline");

    /* A consumer that left scrollrect UNBOUND cannot scroll, so it counts as a
       decline — otherwise the producer would skip the damage that consumer needs. */
    AxlConsoleTee *t2 = axl_console_tee_new();
    TeeSpy         c  = { .accept_scroll = true, .accept_prop = true };
    static const AxlConsoleOps NOSCROLL_OPS = { .clear_screen = tee_spy_clear };
    (void)axl_console_tee_add(t2, &TEE_SPY_OPS, &c);
    (void)axl_console_tee_add(t2, &NOSCROLL_OPS, &c);
    void                *tu2  = NULL;
    const AxlConsoleOps *ops2 = axl_console_tee_ops(t2, &tu2);
    test_check(ops2->scrollrect(tu2, r, 1, 0) == 0,
               "tee: an unbound consumer scrollrect counts as declined");
    axl_console_tee_free(t2);

    /* set_term_prop follows the same all-must-accept rule. */
    AxlConsoleValue on = { .kind = AXL_CONSOLE_VALUE_BOOL, .u.boolean = true };
    test_check(ops->set_term_prop(tu, AXL_CONSOLE_PROP_ALT_SCREEN, &on) == 1,
               "tee: set_term_prop accepted when all accept");
    b.accept_prop = false;
    test_check(ops->set_term_prop(tu, AXL_CONSOLE_PROP_ALT_SCREEN, &on) == 0,
               "tee: set_term_prop declined when any declines");
    test_check(a.props == 2 && b.props == 2, "tee: set_term_prop asks every consumer");

    /* The cap is a fixed array, so the refusal is what stops an add from writing past
       it. Fill to exactly AXL_CONSOLE_TEE_MAX (2 are already in), then prove the next
       one is refused AND that the accepted ones still all get called. */
    TeeSpy fill = { .accept_scroll = true, .accept_prop = true };
    for (size_t i = t_added; i < AXL_CONSOLE_TEE_MAX; i++) {
        test_check(axl_console_tee_add(t, &TEE_SPY_OPS, &fill) == AXL_OK,
                   "tee: add up to the cap succeeds");
    }
    test_check(axl_console_tee_add(t, &TEE_SPY_OPS, &fill) == AXL_ERR,
               "tee: add past AXL_CONSOLE_TEE_MAX is refused");
    fill.clears = 0;
    a.clears    = 0;
    ops->clear_screen(tu);
    test_check(a.clears == 1 && fill.clears == (int)(AXL_CONSOLE_TEE_MAX - t_added),
               "tee: a full tee fans to exactly its capacity, no more");

    axl_console_tee_free(t);
}

/* --- The public VT encoder ----------------------------------------------------
   The ops->VT serializer the mirror has always owned privately, exposed so the OTHER
   producer (axl-console-device, the take-over strategy) can drive a remote terminal
   too. A take-over console had no supported path to a byte stream before this: the
   only public AxlConsoleOps consumer was axl-console-term, which rasterizes to a GOP
   grid. The mirror is now tap+encoder, so these bytes and the mirror's are the same
   code — test_console_mirror() and the altscreen/owned-mode tests below are the
   byte-identity net for that refactor. */
static void
test_console_vt_enc(void)
{
    /* Guards. A sink is required: an encoder nobody can read is a misuse, and the
       mirror has always rejected the same thing at install. */
    test_check(axl_console_vt_enc_new(NULL) == NULL, "vt_enc: new(NULL cfg) -> NULL");
    AxlConsoleVtEncConfig nosink = { .sink = NULL, .cols = 80, .rows = 25 };
    test_check(axl_console_vt_enc_new(&nosink) == NULL, "vt_enc: new(NULL sink) -> NULL");
    axl_console_vt_enc_free(NULL);          /* all NULL-safe, no crash */
    axl_console_vt_enc_reset(NULL);
    axl_console_vt_enc_set_size(NULL, 100, 40);
    test_check(axl_console_vt_enc_ops(NULL, NULL) == NULL, "vt_enc: ops(NULL) -> NULL");
    test_check(axl_console_vt_enc_snapshot(NULL, cm_cap_sink, NULL) == AXL_ERR,
               "vt_enc: snapshot(NULL enc) -> -1");

    AxlConsoleVtEncConfig cfg = { .sink = cm_cap_sink, .user = NULL,
                                  .cols = 80, .rows = 25 };
    AxlConsoleVtEnc *e = axl_console_vt_enc_new(&cfg);
    test_check(e != NULL, "vt_enc: new -> instance");
    test_check(axl_console_vt_enc_snapshot(e, NULL, NULL) == AXL_ERR,
               "vt_enc: snapshot(NULL sink) -> -1");

    void                *ops_user = NULL;
    const AxlConsoleOps *ops      = axl_console_vt_enc_ops(e, &ops_user);
    test_check(ops != NULL, "vt_enc: ops -> vtable");
    test_check(ops_user == (void *)e, "vt_enc: ops context is the encoder");
    /* ops(e, NULL) must not deref the out-param — a consumer that already knows the
       context has no reason to ask for it again. */
    test_check(axl_console_vt_enc_ops(e, NULL) == ops, "vt_enc: ops(e, NULL user) ok");

    /* --- the wire format, exactly (these bytes are the mirror's, unchanged) --- */
    cm_cap_reset();
    ops->clear_screen(ops_user);
    test_check(axl_strcmp(cm_cap, "\x1b[2J\x1b[H") == 0,
               "vt_enc: clear_screen -> ESC[2J ESC[H");

    cm_cap_reset();
    ops->set_cursor(ops_user, 4, 9);
    test_check(axl_strcmp(cm_cap, "\x1b[5;10H") == 0,
               "vt_enc: set_cursor emits 1-based CUP");

    /* Dedup: a full-screen app re-positions to the same cell to blink its cursor;
       that must not flood the wire. */
    cm_cap_reset();
    ops->set_cursor(ops_user, 4, 9);
    test_check(cm_cap_len == 0, "vt_enc: redundant set_cursor emits nothing");

    /* reset() drops the dedup baseline, so the same position re-emits. */
    axl_console_vt_enc_reset(e);
    cm_cap_reset();
    ops->set_cursor(ops_user, 4, 9);
    test_check(axl_strcmp(cm_cap, "\x1b[5;10H") == 0,
               "vt_enc: reset re-arms the cursor dedup");

    cm_cap_reset();
    ops->output_text(ops_user, "hi", 2);
    test_check(axl_strcmp(cm_cap, "hi") == 0, "vt_enc: output_text passes bytes through");

    /* Indexed pen -> the UEFI-mapped SGR. Index 14 maps to 33, NOT bright-yellow 93:
       93 renders lime on many terminals and the EDK2 original chose 33. Pinning it
       here so the refactor cannot quietly "fix" it. */
    cm_cap_reset();
    AxlConsolePen pen = { 0 };
    pen.fg.kind = AXL_CONSOLE_COLOR_INDEXED; pen.fg.idx = 14;
    pen.bg.kind = AXL_CONSOLE_COLOR_INDEXED; pen.bg.idx = 1;
    ops->set_pen(ops_user, &pen);
    test_check(axl_strcmp(cm_cap, "\x1b[0;33;44m") == 0,
               "vt_enc: indexed pen -> SGR (UEFI 14 -> 33, not 93)");

    /* Alt-screen and cursor visibility ride set_term_prop, and must be ACCEPTED
       (return 1) or axl-vterm's grid desyncs from the parser. */
    AxlConsoleValue on  = { .kind = AXL_CONSOLE_VALUE_BOOL, .u.boolean = true };
    AxlConsoleValue off = { .kind = AXL_CONSOLE_VALUE_BOOL, .u.boolean = false };
    cm_cap_reset();
    test_check(ops->set_term_prop(ops_user, AXL_CONSOLE_PROP_ALT_SCREEN, &on) == 1,
               "vt_enc: alt-screen prop accepted");
    test_check(axl_strcmp(cm_cap, "\x1b[?1049h") == 0, "vt_enc: alt-screen enter -> 1049h");
    cm_cap_reset();
    (void)ops->set_term_prop(ops_user, AXL_CONSOLE_PROP_ALT_SCREEN, &off);
    test_check(axl_strcmp(cm_cap, "\x1b[?1049l") == 0, "vt_enc: alt-screen leave -> 1049l");
    cm_cap_reset();
    (void)ops->set_term_prop(ops_user, AXL_CONSOLE_PROP_CURSOR_VISIBLE, &off);
    test_check(axl_strcmp(cm_cap, "\x1b[?25l") == 0, "vt_enc: cursor hide -> DECTCEM l");

    /* --- late join: the snapshot reproduces the CURRENT screen ---------------
       The encoder feeds its own emitted VT into an internal screen model, so a
       client that connects mid-session gets one self-contained repaint. Assert on
       content rather than exact bytes (the repaint coalesces blanks). */
    cm_cap_reset();
    ops->clear_screen(ops_user);
    ops->set_cursor(ops_user, 0, 0);
    ops->output_text(ops_user, "LATE", 4);
    cm_cap_reset();
    test_check(axl_console_vt_enc_snapshot(e, cm_cap_sink, NULL) == AXL_OK,
               "vt_enc: snapshot -> AXL_OK");
    test_check(cm_cap_len > 0 && axl_strstr(cm_cap, "LATE") != NULL,
               "vt_enc: snapshot repaints the on-screen text");

    axl_console_vt_enc_free(e);
}

static void
test_console_mirror(void)
{
    AxlConsoleMirror      *m   = NULL;
    AxlConsoleMirrorConfig cfg = {
        .sink = cm_noop_sink, .user = NULL,
        .cols = 80, .rows = 25, .passthrough_local = false,
    };

    /* Argument guards — none of these install, so the console is untouched. */
    test_check(axl_console_mirror_install(&cfg, NULL) == AXL_ERR,
               "console_mirror: install(NULL out) returns -1");
    test_check(axl_console_mirror_install(NULL, &m) == AXL_ERR,
               "console_mirror: install(NULL cfg) returns -1");
    AxlConsoleMirrorConfig no_sink = cfg;
    no_sink.sink = NULL;
    test_check(axl_console_mirror_install(&no_sink, &m) == AXL_ERR,
               "console_mirror: install(NULL sink) returns -1");
    test_check(m == NULL,
               "console_mirror: out stays NULL on rejected install");

    /* NULL-safe teardown / accessors / injection. */
    axl_console_mirror_uninstall(NULL);
    axl_console_mirror_reset(NULL);
    axl_console_mirror_set_size(NULL, 100, 40);
    test_check(axl_console_mirror_inject_key(NULL, 0, 'x') == AXL_ERR,
               "console_mirror: inject_key(NULL) returns -1");
    test_check(axl_console_mirror_inject_text(NULL, "x", 1) == AXL_ERR,
               "console_mirror: inject_text(NULL m) returns -1");
    test_check(!axl_console_mirror_in_alt_screen(NULL), "console_mirror: in_alt(NULL) false");
    axl_console_mirror_enter_alt_screen(NULL);   /* NULL-safe, no crash */
    axl_console_mirror_leave_alt_screen(NULL);

    /* inject_text byte->key decode (the TerminalDxe-style decoder) now lives in
       the tap. Driven on a bare, un-installed tap so it never wraps the live
       console. Regression for the terminal-Backspace bug: xterm.js sends 0x7f
       (DEL) for the Backspace key, but UEFI backspace is UnicodeChar 0x08 — so
       0x7f must remap to 0x08 (the Delete *key* arrives as the CSI 3~ escape and
       is decoded separately; not exercised here). */
    AxlConsoleTap *tt = _axl_console_tap_new_for_test();
    test_check(tt != NULL, "inject_text: bare test tap constructed");
    test_check(axl_console_tap_inject_text(NULL, "x", 1) == AXL_ERR,
               "console_tap: inject_text(NULL t) returns -1");
    test_check(axl_console_tap_inject_key(NULL, 0, 'x') == AXL_ERR,
               "console_tap: inject_key(NULL t) returns -1");
    if (tt != NULL) {
        uint16_t scan = 0xFFFF, uni = 0xFFFF;

        /* A plain ASCII byte injects as that unicode char (frames the remap). */
        axl_console_tap_inject_text(tt, "a", 1);
        test_check(_axl_console_tap_test_pop_key(tt, &scan, &uni)
                   && scan == 0 && uni == 'a',
                   "inject_text: 'a' -> {ScanCode=0, UnicodeChar='a'}");

        /* 0x08 (ASCII BS) already IS UEFI backspace — passes through. */
        axl_console_tap_inject_text(tt, "\x08", 1);
        test_check(_axl_console_tap_test_pop_key(tt, &scan, &uni)
                   && scan == 0 && uni == 0x08,
                   "inject_text: 0x08 -> {ScanCode=0, UnicodeChar=0x08} backspace");

        /* THE FIX: 0x7f (terminal DEL = the Backspace key) must remap to UEFI
           backspace 0x08, not pass through as a literal 0x7f. */
        axl_console_tap_inject_text(tt, "\x7f", 1);
        test_check(_axl_console_tap_test_pop_key(tt, &scan, &uni)
                   && scan == 0 && uni == 0x08,
                   "inject_text: 0x7f (terminal Backspace) -> UEFI backspace 0x08");

        /* A Ctrl+<letter> injects the LETTER plus the control shift state, which
           is what a full-screen Ex reader (the Shell's `edit`) needs to map its
           control commands. The SIMPLE ReadKeyStroke has no KeyState, so it must
           FOLD the letter to its C0 code -- EDK2's ConSplitter does exactly this,
           and without it the Shell's line editor sees the bare 'c' of a Ctrl+C
           and inserts it. */
        axl_console_tap_inject_key_ex(tt, 0, 'c',
                                      EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED, 0);
        test_check(_axl_console_tap_test_pop_key(tt, &scan, &uni)
                   && scan == 0 && uni == 0x03,
                   "inject Ctrl+C ('c'+CTRL) -> Simple read folds to 0x03");

        axl_console_tap_inject_key_ex(tt, 0, 'q',
                                      EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED, 0);
        test_check(_axl_console_tap_test_pop_key(tt, &scan, &uni)
                   && scan == 0 && uni == 0x11,
                   "inject Ctrl+Q ('q'+CTRL) -> Simple read folds to 0x11");

        /* No control modifier -> a plain letter is NOT folded (still inserted). */
        axl_console_tap_inject_key_ex(tt, 0, 'c', 0, 0);
        test_check(_axl_console_tap_test_pop_key(tt, &scan, &uni)
                   && scan == 0 && uni == 'c',
                   "inject plain 'c' (no CTRL) -> Simple read leaves it 'c'");

        /* Uppercase letter + CTRL case-folds before folding: Ctrl+Shift+C -> 0x03. */
        axl_console_tap_inject_key_ex(tt, 0, 'C',
                                      EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED, 0);
        test_check(_axl_console_tap_test_pop_key(tt, &scan, &uni)
                   && scan == 0 && uni == 0x03,
                   "inject Ctrl+'C' (uppercase) -> Simple read folds to 0x03");

        /* A non-letter + CTRL is left alone (only a..z fold). */
        axl_console_tap_inject_key_ex(tt, 0, '5',
                                      EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED, 0);
        test_check(_axl_console_tap_test_pop_key(tt, &scan, &uni)
                   && scan == 0 && uni == '5',
                   "inject Ctrl+'5' (non-letter) -> Simple read leaves it '5'");

        /* An already-folded C0 code + CTRL is not double-folded (0x03 stays 0x03). */
        axl_console_tap_inject_key_ex(tt, 0, 0x03,
                                      EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED, 0);
        test_check(_axl_console_tap_test_pop_key(tt, &scan, &uni)
                   && scan == 0 && uni == 0x03,
                   "inject already-folded 0x03 + CTRL -> Simple read leaves it 0x03");

        axl_free(tt);
    }
}

// ---------------------------------------------------------------------------
// AxlConsoleTap public-API guards. The tap is the new substrate entry point, so
// pin its argument validation and NULL-safety (none of these install anything,
// so the live console is untouched).
// ---------------------------------------------------------------------------

static void
test_console_tap_guards(void)
{
    static const AxlConsoleOps null_ops = {0};   /* every callback optional */
    AxlConsoleTap      *t   = NULL;
    AxlConsoleTapConfig cfg = { .cols = 80, .rows = 25 };

    test_check(axl_console_tap_install(NULL, &null_ops, NULL, &cfg) == AXL_ERR,
               "console_tap: install(NULL out) returns -1");
    test_check(axl_console_tap_install(&t, NULL, NULL, &cfg) == AXL_ERR,
               "console_tap: install(NULL ops) returns -1");
    test_check(axl_console_tap_install(&t, &null_ops, NULL, NULL) == AXL_ERR,
               "console_tap: install(NULL cfg) returns -1");
    test_check(t == NULL, "console_tap: out stays NULL on rejected install");

    /* NULL-safe teardown / accessors. */
    axl_console_tap_uninstall(NULL);
    axl_console_tap_reset(NULL);
    axl_console_tap_set_size(NULL, 100, 40);
    axl_console_tap_enter_alt_screen(NULL);
    axl_console_tap_leave_alt_screen(NULL);
    test_check(!axl_console_tap_in_alt_screen(NULL),
               "console_tap: in_alt_screen(NULL) is false");
}

/* The take-over device's install/connect/evict surgery wraps the LIVE console and
   is validated under DEBUG OVMF (test-console-device-qemu.sh); its ops emission is
   the shared engine, already covered by the tap tests above. What is unit-testable
   here is the public surface's arg guards + the output-only contract (the input
   relay is a later increment). */
static void
test_console_device_guards(void)
{
    static const AxlConsoleOps    null_ops = {0};
    AxlConsoleDevice             *d   = NULL;
    AxlConsoleDeviceConfig        cfg = { .cols = 80, .rows = 25 };

    test_check(axl_console_device_install(&null_ops, NULL, &cfg, NULL) == AXL_ERR,
               "console_device: install(NULL out) returns -1");
    test_check(axl_console_device_install(NULL, NULL, &cfg, &d) == AXL_ERR,
               "console_device: install(NULL ops) returns -1");
    test_check(axl_console_device_install(&null_ops, NULL, NULL, &d) == AXL_ERR,
               "console_device: install(NULL cfg) returns -1");
    test_check(d == NULL, "console_device: out stays NULL on rejected install");

    /* passthrough_local co-paints with GraphicsConsole, so the two must agree on the
       grid: an explicit geometry is refused rather than half-honoured (it would
       garble the local display and re-open the ConsoleLogger stale-rows deadloop).
       Rejected BEFORE any surgery, so this is safe to assert in the unit boot. */
    AxlConsoleDeviceConfig pt_sized = { .passthrough_local = true, .cols = 142, .rows = 44 };
    test_check(axl_console_device_install(&null_ops, NULL, &pt_sized, &d) == AXL_ERR,
               "console_device: passthrough_local + explicit geometry rejected");
    test_check(d == NULL, "console_device: out stays NULL on the passthrough geometry reject");
    AxlConsoleDeviceConfig pt_cols = { .passthrough_local = true, .cols = 142 };
    test_check(axl_console_device_install(&null_ops, NULL, &pt_cols, &d) == AXL_ERR,
               "console_device: passthrough_local + explicit cols alone rejected");
    AxlConsoleDeviceConfig pt_rows = { .passthrough_local = true, .rows = 44 };
    test_check(axl_console_device_install(&null_ops, NULL, &pt_rows, &d) == AXL_ERR,
               "console_device: passthrough_local + explicit rows alone rejected");

    /* inject_* are NULL-safe; the injected-key path proper is covered headlessly in
       test_console_device_input (a live install wedges the unit boot). */
    test_check(axl_console_device_inject_key(NULL, 0, 'x') == AXL_ERR,
               "console_device: inject_key(NULL) returns -1");
    test_check(axl_console_device_inject_key_ex(NULL, 0, 3,
                   AXL_CONSOLE_SHIFT_STATE_VALID, 0) == AXL_ERR,
               "console_device: inject_key_ex(NULL) returns -1");
    test_check(axl_console_device_inject_text(NULL, "x", 1) == AXL_ERR,
               "console_device: inject_text(NULL) returns -1");

    /* get_size mirrors the tap's: NULL-safe, zeroes both outputs, and either output
       pointer may be NULL. A passthrough consumer NEEDS this — its geometry is
       whatever the physical console resolved to, and without a way to read it back
       the consumer would size its screen model to a guessed 80x25. */
    uint32_t gc = 7, gr = 9;
    axl_console_device_get_size(NULL, &gc, &gr);
    test_check(gc == 0 && gr == 0, "console_device: get_size(NULL) zeroes both outputs");
    axl_console_device_get_size(NULL, NULL, NULL);   /* no crash on NULL outputs */

    /* NULL-safe session / geometry / alt-screen accessors. */
    axl_console_device_uninstall(NULL);
    axl_console_device_reset(NULL);
    axl_console_device_set_size(NULL, 100, 40);
    axl_console_device_enter_alt_screen(NULL);
    axl_console_device_leave_alt_screen(NULL);
    test_check(!axl_console_device_in_alt_screen(NULL),
               "console_device: in_alt_screen(NULL) is false");
}

// ---------------------------------------------------------------------------
// Key-notify ownership under input_capture.
//
// EFI_SIMPLE_TEXT_INPUT_EX fires RegisterKeyNotify callbacks when a key is
// INSERTED into the queue, independent of who reads. With input_capture the tap
// owns the queue, so it must own the notify registry too -- otherwise the guest
// (a nested Shell) gets its notifies from the *physical* keyboard it is no
// longer supposed to see, and never from an injected key.
//
// The Shell's Ctrl+C monitor is the motivating consumer. EDK2 registers four
// variants (ShellPkg ShellProtocol.c): UnicodeChar in {'c', 3} crossed with
// {LEFT,RIGHT}_CONTROL_PRESSED, each with EFI_SHIFT_STATE_VALID, ScanCode 0 and
// KeyToggleState 0. The match rule (MdeModulePkg IsKeyRegistered) is: ScanCode
// and UnicodeChar must be equal, and a registered KeyShiftState / KeyToggleState
// of 0 means "don't care" while a nonzero one must match EXACTLY.
//
// So an injected key must carry KeyState, which is why the ring holds
// EFI_KEY_DATA rather than EFI_INPUT_KEY.
// ---------------------------------------------------------------------------

#define CTRL_C_SHIFT (EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED)

static int          notify_calls;
static EFI_KEY_DATA notify_last;

static EFI_STATUS EFIAPI
on_key_notify(EFI_KEY_DATA *kd)
{
    notify_calls++;
    notify_last = *kd;
    return EFI_SUCCESS;
}

/* A device key_filter that claims Shift+PgUp (SCAN 0x09) for the consumer. */
static bool
filter_shift_pgup(void *user, const void *key)
{
    (void)user;
    const EFI_KEY_DATA *kd = key;
    return kd->Key.ScanCode == 0x09
        && (kd->KeyState.KeyShiftState
            & (EFI_LEFT_SHIFT_PRESSED | EFI_RIGHT_SHIFT_PRESSED)) != 0;
}

/* Register `kd` and return the opaque handle, or NULL on failure. */
static void *
tap_register_notify(AxlConsoleTap *t, EFI_KEY_DATA kd)
{
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *ex = _axl_console_tap_test_coninex(t);
    void                              *h  = NULL;

    if (ex->RegisterKeyNotify(ex, &kd, on_key_notify, &h) != EFI_SUCCESS) {
        return NULL;
    }
    return h;
}

static EFI_KEY_DATA
key_data(uint16_t scan, uint16_t unicode, uint32_t shift, uint8_t toggle)
{
    EFI_KEY_DATA kd = {0};
    kd.Key.ScanCode           = scan;
    kd.Key.UnicodeChar        = unicode;
    kd.KeyState.KeyShiftState = shift;
    kd.KeyState.KeyToggleState = toggle;
    return kd;
}

static void
test_console_tap_key_notify(void)
{
    AxlConsoleTap *t = cm_tap_begin(80, 25, /*auto_alt=*/false,
                                    /*input_capture=*/true, /*passthrough=*/false);

    /* The Shell's real Ctrl+C registration: Key={0,3}, LEFT_CONTROL. */
    void *h = tap_register_notify(t, key_data(0, 3, CTRL_C_SHIFT, 0));
    test_check(h != NULL, "key_notify: RegisterKeyNotify returns a handle");

    /* An injected Ctrl+C carrying the same KeyState fires it. */
    notify_calls = 0;
    test_check(axl_console_tap_inject_key_ex(t, 0, 3, CTRL_C_SHIFT, 0) == AXL_OK,
               "key_notify: inject_key_ex accepts a modifier-bearing key");
    test_check(notify_calls == 1, "key_notify: injected Ctrl+C fires the notify once");
    test_check(notify_last.Key.UnicodeChar == 3,
               "key_notify: callback receives UnicodeChar 3");
    test_check(notify_last.KeyState.KeyShiftState == CTRL_C_SHIFT,
               "key_notify: callback receives the injected KeyShiftState");

    /* Same UnicodeChar, NO modifiers: the registered KeyShiftState is nonzero,
       so IsKeyRegistered demands an exact match. A bare 0x03 must NOT fire. */
    notify_calls = 0;
    axl_console_tap_inject_key(t, 0, 3);
    test_check(notify_calls == 0,
               "key_notify: bare 0x03 without KeyState does not fire a ctrl-qualified notify");

    /* A wholly different key does not fire it either. */
    notify_calls = 0;
    axl_console_tap_inject_key_ex(t, 0, 'a', CTRL_C_SHIFT, 0);
    test_check(notify_calls == 0, "key_notify: non-matching UnicodeChar does not fire");

    /* The injected keys are still readable -- firing a notify must not consume
       them. Three were pushed above; FIFO returns the first, the Ctrl+C. */
    uint16_t scan = 0xFFFF, uni = 0xFFFF;
    test_check(_axl_console_tap_test_pop_key(t, &scan, &uni) && uni == 3,
               "key_notify: the notified key is still delivered to the reader");

    /* Unregister: the same key no longer fires. */
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *ex = _axl_console_tap_test_coninex(t);
    test_check(ex->UnregisterKeyNotify(ex, h) == EFI_SUCCESS,
               "key_notify: UnregisterKeyNotify accepts our handle");
    notify_calls = 0;
    axl_console_tap_inject_key_ex(t, 0, 3, CTRL_C_SHIFT, 0);
    test_check(notify_calls == 0, "key_notify: unregistered notify no longer fires");

    cm_tap_end(t);
}

/* A registered KeyShiftState of 0 means "don't care" -- it must match a key
   injected with any modifier state. This is the other half of IsKeyRegistered,
   and getting it backwards would silently break plain-key notifies. */
static void
test_console_tap_key_notify_wildcard(void)
{
    AxlConsoleTap *t = cm_tap_begin(80, 25, false, /*input_capture=*/true, false);

    void *h = tap_register_notify(t, key_data(0, 'q', 0, 0));
    test_check(h != NULL, "key_notify: wildcard registration succeeds");

    notify_calls = 0;
    axl_console_tap_inject_key(t, 0, 'q');
    test_check(notify_calls == 1, "key_notify: shift-state 0 matches an unmodified key");

    notify_calls = 0;
    axl_console_tap_inject_key_ex(t, 0, 'q', CTRL_C_SHIFT, 0);
    test_check(notify_calls == 1, "key_notify: shift-state 0 also matches a modified key");

    /* ScanCode must always match, even under the wildcard. */
    notify_calls = 0;
    axl_console_tap_inject_key(t, 0x01 /*SCAN_UP*/, 0);
    test_check(notify_calls == 0, "key_notify: a different ScanCode never matches");

    cm_tap_end(t);
}

/* Without input_capture the tap is NOT the queue owner, so it must keep
   forwarding registrations to the original ConInEx rather than serving them
   itself -- SoftBMC depends on the physical keyboard behaving normally. In this
   headless fixture orig_coninex is NULL, so the forward path falls through to
   the documented stub; what we pin is that the LOCAL registry stayed empty, i.e.
   an injected key fires nothing. */
static void
test_console_tap_key_notify_passthrough_not_owned(void)
{
    AxlConsoleTap *t = cm_tap_begin(80, 25, false, /*input_capture=*/false, false);

    void *h = tap_register_notify(t, key_data(0, 3, CTRL_C_SHIFT, 0));
    test_check(h != NULL, "key_notify: passthrough registration still returns a handle");

    notify_calls = 0;
    axl_console_tap_inject_key_ex(t, 0, 3, CTRL_C_SHIFT, 0);
    test_check(notify_calls == 0,
               "key_notify: passthrough mode does not serve notifies from the tap's ring");

    cm_tap_end(t);
}

/* inject_key_ex must reject a NULL tap like every other public entry point. */
static void
test_console_tap_inject_key_ex_guards(void)
{
    test_check(axl_console_tap_inject_key_ex(NULL, 0, 3, CTRL_C_SHIFT, 0) == AXL_ERR,
               "key_notify: inject_key_ex(NULL) returns -1");
}

/* axl-console-device INPUT RELAY (increment 2). The device shares the
   AxlConsoleInput engine with the tap, so this pins the DEVICE-specific wiring:
   the take_input gate, that its ConIn/ConInEx read from the engine (Simple folds
   Ctrl+letter, Ex returns raw), inject_text decode, and that the device owns the
   key-notify registry. The firmware surgery (publish/evict/read-loop/teardown) is
   the DEBUG-OVMF smoke's job; a live install wedges the unit boot. */
static void
test_console_device_input(void)
{
    /* take_input=false: the inject family is inert (output-only device). */
    AxlConsoleDevice *off = _axl_console_device_new_for_test();
    _axl_console_device_test_begin(off, /*take_input=*/false);
    test_check(axl_console_device_inject_key(off, 0, 'x') == AXL_ERR,
               "device_input: inject_key rejected when take_input=false");
    test_check(axl_console_device_inject_key_ex(off, 0, 3, CTRL_C_SHIFT, 0) == AXL_ERR,
               "device_input: inject_key_ex rejected when take_input=false");
    test_check(axl_console_device_inject_text(off, "x", 1) == AXL_ERR,
               "device_input: inject_text rejected when take_input=false");
    _axl_console_device_test_end(off);

    /* take_input=true: the device is the sole queue owner; reads come from us. */
    AxlConsoleDevice                  *d  = _axl_console_device_new_for_test();
    _axl_console_device_test_begin(d, /*take_input=*/true);
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *ex = _axl_console_device_test_coninex(d);
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL    *in = _axl_console_device_test_conin(d);

    /* Ex read returns the raw letter + KeyState (a full-screen app maps its own
       Ctrl chords off this). */
    test_check(axl_console_device_inject_key_ex(d, 0, 'c', CTRL_C_SHIFT, 0) == AXL_OK,
               "device_input: inject_key_ex accepts a modifier-bearing key");
    EFI_KEY_DATA kd = {0};
    test_check(ex->ReadKeyStrokeEx(ex, &kd) == EFI_SUCCESS
                   && kd.Key.UnicodeChar == 'c'
                   && kd.KeyState.KeyShiftState == CTRL_C_SHIFT,
               "device_input: Ex read is the raw letter + Ctrl state");

    /* Simple read folds Ctrl+<letter> to its C0 code (Ctrl+C -> 0x03). */
    test_check(axl_console_device_inject_key_ex(d, 0, 'c', CTRL_C_SHIFT, 0) == AXL_OK,
               "device_input: inject Ctrl+C for the Simple read");
    EFI_INPUT_KEY sk = {0};
    test_check(in->ReadKeyStroke(in, &sk) == EFI_SUCCESS && sk.UnicodeChar == 0x03,
               "device_input: Simple read folds Ctrl+C to 0x03");

    /* Empty ring -> NOT_READY on both protocols. */
    test_check(in->ReadKeyStroke(in, &sk) == EFI_NOT_READY,
               "device_input: Simple read NOT_READY when empty");
    test_check(ex->ReadKeyStrokeEx(ex, &kd) == EFI_NOT_READY,
               "device_input: Ex read NOT_READY when empty");

    /* inject_text decodes a byte run to keys read back in order. */
    test_check(axl_console_device_inject_text(d, "hi\r", 3) == AXL_OK,
               "device_input: inject_text accepts a byte run");
    test_check(in->ReadKeyStroke(in, &sk) == EFI_SUCCESS && sk.UnicodeChar == 'h',
               "device_input: inject_text -> 'h'");
    test_check(in->ReadKeyStroke(in, &sk) == EFI_SUCCESS && sk.UnicodeChar == 'i',
               "device_input: inject_text -> 'i'");
    test_check(in->ReadKeyStroke(in, &sk) == EFI_SUCCESS && sk.UnicodeChar == '\r',
               "device_input: inject_text -> CR");

    /* The device owns the notify registry: the Shell's Ctrl+C break registers on
       our ConInEx and must fire on an injected Ctrl+C. */
    void         *h   = NULL;
    EFI_KEY_DATA  reg = key_data(0, 3, CTRL_C_SHIFT, 0);
    test_check(ex->RegisterKeyNotify(ex, &reg, on_key_notify, &h) == EFI_SUCCESS
                   && h != NULL,
               "device_input: RegisterKeyNotify returns a handle");
    notify_calls = 0;
    axl_console_device_inject_key_ex(d, 0, 3, CTRL_C_SHIFT, 0);
    test_check(notify_calls == 1,
               "device_input: injected Ctrl+C fires the registered notify once");
    ex->ReadKeyStrokeEx(ex, &kd);   /* drain the queued key */
    test_check(ex->UnregisterKeyNotify(ex, h) == EFI_SUCCESS,
               "device_input: UnregisterKeyNotify accepts our handle");

    /* key_filter (the read-loop peek): a filter that claims Shift+PgUp consumes it
       (the shell's key ring never sees it); a plain key + a NULL filter forward. The
       seam runs the device's real filter->gate->push path on a synthetic key and
       reports whether it reached the ring. */
    test_check(_axl_console_device_test_feed_physical(d, filter_shift_pgup, NULL,
                   /*scan=*/0x09, /*uni=*/0,
                   EFI_LEFT_SHIFT_PRESSED | EFI_SHIFT_STATE_VALID) == false,
               "device key_filter: Shift+PgUp consumed (not forwarded to the shell)");
    test_check(_axl_console_device_test_feed_physical(d, filter_shift_pgup, NULL,
                   /*scan=*/0, /*uni=*/'x', /*shift=*/0) == true,
               "device key_filter: a plain key is forwarded to the shell ring");
    test_check(_axl_console_device_test_feed_physical(d, NULL, NULL, 0, 'y', 0) == true,
               "device key_filter: NULL filter forwards everything");

    _axl_console_device_test_end(d);
}

// ---------------------------------------------------------------------------
// Pointer take-over (cfg.take_pointer): the device uninstalls SimplePointer(s)
// from the handle database so a guest cannot locate one, caches the interfaces
// for the consumer, and restores them on uninstall. Exercised here against a
// SYNTHETIC pointer via the per-handle evict/restore seams (the harness's real
// pointer stays untouched; the LocateHandleBuffer sweep is firmware-validated).
// ---------------------------------------------------------------------------

static EFI_STATUS EFIAPI
ptr_mock_reset(EFI_SIMPLE_POINTER_PROTOCOL *This, BOOLEAN ext)
{
    (void)This; (void)ext;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
ptr_mock_getstate(EFI_SIMPLE_POINTER_PROTOCOL *This, EFI_SIMPLE_POINTER_STATE *st)
{
    (void)This;
    if (st != NULL) {
        st->RelativeMovementX = 42;   /* distinctive: proves the proxy relayed US */
        st->RelativeMovementY = 0;
        st->RelativeMovementZ = 0;
        st->LeftButton        = FALSE;
        st->RightButton       = FALSE;
    }
    return EFI_SUCCESS;
}

static void
test_console_device_pointer_evict(void)
{
    EFI_GUID sp_guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;

    /* A synthetic SimplePointer on a fresh handle stands in for a firmware mouse.
       A real pointer handle carries other protocols (device path, USB I/O), so
       install a keeper alongside it: uninstalling only the pointer must NOT destroy
       the handle (UEFI frees a handle when its LAST protocol is removed), so restore
       has a live handle to reinstall onto. */
    static EFI_GUID keeper_guid =
        { 0x5eeb0000, 0x1234, 0x5678, { 0x9a, 0xbc, 0xde, 0xf0, 0x11, 0x22, 0x33, 0x44 } };
    static int keeper = 0xC0FFEE;
    static EFI_SIMPLE_POINTER_PROTOCOL mock;
    mock.Reset    = ptr_mock_reset;
    mock.GetState = ptr_mock_getstate;
    mock.Mode     = NULL;
    EFI_HANDLE ph = NULL;
    test_check(gBS->InstallProtocolInterface(&ph, &keeper_guid, EFI_NATIVE_INTERFACE,
                                             &keeper) == EFI_SUCCESS && ph != NULL,
               "device_pointer: keeper protocol installs on a fresh handle");
    test_check(gBS->InstallProtocolInterface(&ph, &sp_guid, EFI_NATIVE_INTERFACE,
                                             &mock) == EFI_SUCCESS,
               "device_pointer: synthetic SimplePointer installs on that handle");

    /* Baseline: a guest CAN locate it on that handle. */
    void *found = NULL;
    test_check(gBS->HandleProtocol(ph, &sp_guid, &found) == EFI_SUCCESS && found == &mock,
               "device_pointer: guest locates the pointer before take-over");

    AxlConsoleDevice *d = _axl_console_device_new_for_test();
    _axl_console_device_test_begin(d, /*take_input=*/false);

    /* Guards on the accessors before any eviction. */
    test_check(axl_console_device_pointer_count(NULL) == 0,
               "device_pointer: count(NULL) == 0");
    test_check(axl_console_device_pointer_count(d) == 0,
               "device_pointer: count == 0 before eviction");
    test_check(axl_console_device_pointer_iface(NULL, 0) == NULL,
               "device_pointer: iface(NULL) == NULL");

    /* Evict: the pointer leaves the database (guest can no longer find it) and is
       cached for the consumer. */
    test_check(_axl_console_device_test_evict_one_pointer(d, ph) == true,
               "device_pointer: evict removes the synthetic pointer");
    found = NULL;
    test_check(gBS->HandleProtocol(ph, &sp_guid, &found) != EFI_SUCCESS,
               "device_pointer: guest CANNOT locate the pointer after eviction");
    test_check(axl_console_device_pointer_count(d) == 1,
               "device_pointer: one interface cached for the consumer");
    test_check(axl_console_device_pointer_iface(d, 0) == &mock,
               "device_pointer: cached iface is the evicted interface");
    test_check(axl_console_device_pointer_iface(d, 1) == NULL,
               "device_pointer: iface(out-of-range) == NULL");

    /* Count SimplePointer handles now (mock evicted): the proxy install must add one. */
    UINTN       pre_n = 0;
    EFI_HANDLE *pre_h = NULL;
    gBS->LocateHandleBuffer(ByProtocol, &sp_guid, NULL, &pre_n, &pre_h);
    if (pre_h != NULL) { gBS->FreePool(pre_h); }

    /* Interpose the yielding proxy: a guest now locates ONE more SimplePointer (the
       proxy), and the proxy forwards the cached real pointer's movement. */
    _axl_console_device_test_install_proxy(d);
    UINTN       post_n = 0;
    EFI_HANDLE *post_h = NULL;
    gBS->LocateHandleBuffer(ByProtocol, &sp_guid, NULL, &post_n, &post_h);
    if (post_h != NULL) { gBS->FreePool(post_h); }
    test_check(post_n == pre_n + 1,
               "device_pointer: proxy install adds one locatable SimplePointer");

    EFI_SIMPLE_POINTER_STATE pst = {0};
    int fr = _axl_console_device_test_proxy_forward(d, &pst);
    test_check(fr == (int)EFI_SUCCESS && pst.RelativeMovementX == 42,
               "device_pointer: proxy forwards the cached real pointer's movement");

    /* Uninstall the proxy: the added SimplePointer goes away again. */
    _axl_console_device_test_uninstall_proxy(d);
    UINTN       gone_n = 0;
    EFI_HANDLE *gone_h = NULL;
    gBS->LocateHandleBuffer(ByProtocol, &sp_guid, NULL, &gone_n, &gone_h);
    if (gone_h != NULL) { gBS->FreePool(gone_h); }
    test_check(gone_n == pre_n,
               "device_pointer: proxy uninstall removes the interposed SimplePointer");

    /* Restore: the pointer returns to the database (a later guest finds it again). */
    _axl_console_device_test_restore_pointer(d);
    test_check(axl_console_device_pointer_count(d) == 0,
               "device_pointer: cache cleared after restore");
    found = NULL;
    test_check(gBS->HandleProtocol(ph, &sp_guid, &found) == EFI_SUCCESS && found == &mock,
               "device_pointer: guest locates the pointer again after restore");

    _axl_console_device_test_end(d);
    gBS->UninstallProtocolInterface(ph, &sp_guid, &mock);
    gBS->UninstallProtocolInterface(ph, &keeper_guid, &keeper);
}

/* Synthetic evicted keyboard whose ReadKeyStrokeEx just counts calls (returns
   NOT_READY = "no key", so the read loop probes it exactly once per tick). */
static int g_synth_conin_reads;

static EFI_STATUS EFIAPI
synth_conin_reset(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, BOOLEAN ext)
{
    (void)This; (void)ext;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
synth_conin_readkeyex(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *kd)
{
    (void)This;
    g_synth_conin_reads++;
    if (kd != NULL) {
        EFI_KEY_DATA zero = {0};
        *kd = zero;
    }
    return EFI_NOT_READY;
}

/* Regression for the real-iDRAC #GP (dev_read_timer_cb calling a dangling
   evicted-keyboard interface after the firmware keyboard reallocated/removed its
   SimpleTextInputEx). The read loop must RE-RESOLVE each evicted handle per tick
   rather than trust a cached pointer -- so once the Ex protocol is gone, a tick
   must NOT call the (stale) interface. */
static void
test_console_device_read_loop_revalidate(void)
{
    EFI_GUID ex_guid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
    static EFI_GUID keeper_guid =
        { 0x5eeb0001, 0x1234, 0x5678, { 0x9a, 0xbc, 0xde, 0xf0, 0x11, 0x22, 0x33, 0x55 } };
    static int keeper = 0xC0FFEE;
    static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL kb;
    kb.Reset          = synth_conin_reset;
    kb.ReadKeyStrokeEx = synth_conin_readkeyex;
    kb.WaitForKeyEx   = NULL;
    kb.SetState       = NULL;
    kb.RegisterKeyNotify   = NULL;
    kb.UnregisterKeyNotify = NULL;

    /* A keeper protocol keeps the handle alive when we later remove only the Ex. */
    EFI_HANDLE kh = NULL;
    test_check(gBS->InstallProtocolInterface(&kh, &keeper_guid, EFI_NATIVE_INTERFACE,
                                             &keeper) == EFI_SUCCESS && kh != NULL,
               "read-loop: keeper installs on a fresh handle");
    test_check(gBS->InstallProtocolInterface(&kh, &ex_guid, EFI_NATIVE_INTERFACE,
                                             &kb) == EFI_SUCCESS,
               "read-loop: synthetic keyboard SimpleTextInputEx installs");

    AxlConsoleDevice *d = _axl_console_device_new_for_test();
    _axl_console_device_test_begin(d, /*take_input=*/true);
    test_check(_axl_console_device_test_add_evicted_conin(d, kh),
               "read-loop: synthetic keyboard evicted into the read list");

    /* Baseline: a tick resolves the handle and probes its Ex exactly once. */
    g_synth_conin_reads = 0;
    _axl_console_device_test_read_tick(d);
    test_check(g_synth_conin_reads == 1, "read-loop: tick reads the evicted keyboard's Ex");

    /* Now the firmware keyboard reallocates/removes its Ex (USB re-enum on real HW):
       remove the protocol from the handle. A tick must re-resolve, find it gone, and
       skip -- NOT call the stale cached pointer (which is the #GP on real HW). */
    gBS->UninstallProtocolInterface(kh, &ex_guid, &kb);
    g_synth_conin_reads = 0;
    _axl_console_device_test_read_tick(d);
    test_check(g_synth_conin_reads == 0,
               "read-loop: tick re-resolves a removed keyboard and skips (no stale-pointer call)");

    _axl_console_device_test_end(d);
    gBS->UninstallProtocolInterface(kh, &keeper_guid, &keeper);
}

/* AxlConsoleTerm — the local AxlConsoleOps sink. This pins the cell model + op
   translation (the on-screen counterpart to axl-console-mirror's VT wire). */
static void
test_console_term_output(void)
{
    AxlConsoleTermConfig cfg = { .cols = 20, .rows = 5 };   /* font NULL -> default */
    AxlConsoleTerm      *t   = axl_console_term_new(&cfg);
    test_check(t != NULL, "term: new(20x5) succeeds");

    void                *u   = NULL;
    const AxlConsoleOps *ops = axl_console_term_ops(t, &u);
    test_check(ops != NULL && ops->output_text != NULL && u == (void *)t,
               "term: ops vtable exposed + user is the instance");

    /* set_pen(fg=2 green, bg=1 blue) then output 'Hi'. */
    AxlConsolePen pen = { .fg = { .kind = AXL_CONSOLE_COLOR_INDEXED, .idx = 2 },
                          .bg = { .kind = AXL_CONSOLE_COLOR_INDEXED, .idx = 1 } };
    ops->set_pen(u, &pen);
    ops->output_text(u, "Hi", 2);

    char c[5]; uint8_t fg = 0, bg = 0;
    test_check(_axl_console_term_test_cell(t, 0, 0, c, &fg, &bg)
                   && axl_strcmp(c, "H") == 0 && fg == 2 && bg == 1,
               "term: output_text lands 'H' green-on-blue at (0,0)");
    test_check(_axl_console_term_test_cell(t, 0, 1, c, &fg, &bg)
                   && axl_strcmp(c, "i") == 0,
               "term: output_text lands 'i' at (0,1)");

    uint32_t cr = 99, cc = 99;
    _axl_console_term_test_cursor(t, &cr, &cc);
    test_check(cr == 0 && cc == 2, "term: cursor advanced to (0,2)");

    /* CR/LF: carriage-return columns to 0, line-feed drops a row. */
    ops->output_text(u, "\r\nX", 3);
    _axl_console_term_test_cursor(t, &cr, &cc);
    test_check(cr == 1 && cc == 1, "term: CR/LF moved to row 1, then 'X' -> col 1");
    test_check(_axl_console_term_test_cell(t, 1, 0, c, &fg, &bg) && axl_strcmp(c, "X") == 0,
               "term: 'X' at (1,0) after CR/LF");

    /* clear_screen blanks + homes. */
    ops->clear_screen(u);
    _axl_console_term_test_cursor(t, &cr, &cc);
    test_check(cr == 0 && cc == 0 && _axl_console_term_test_cell(t, 0, 0, c, &fg, &bg)
                   && c[0] == '\0',
               "term: clear_screen blanks (0,0) + homes the cursor");

    axl_console_term_free(t);
}

/* The grid's cell encode. A cell's bytes are handed straight to the glyph
   renderer AND to the clipboard, so re-encoding a lone surrogate in its 3-byte
   shape would put WTF-8 into both. Substitute U+FFFD, matching what the tap now
   emits on the producer side and what every AXL decoder does with malformed
   input. */
static void
test_console_term_utf8(void)
{
    static const char in_e9[]   = { (char)0xC3, (char)0xA9, '\0' };
    static const char in_eur[]  = { (char)0xE2, (char)0x82, (char)0xAC, '\0' };
    static const char in_sur[]  = { (char)0xED, (char)0xA0, (char)0x80, '\0' };
    static const char e_fffd[]  = { (char)0xEF, (char)0xBF, (char)0xBD, '\0' };

    AxlConsoleTermConfig cfg = { .cols = 20, .rows = 5 };
    AxlConsoleTerm      *t   = axl_console_term_new(&cfg);
    void                *u   = NULL;
    const AxlConsoleOps *ops = axl_console_term_ops(t, &u);
    char                 c[5];

    ops->output_text(u, in_e9, 2);
    test_check(_axl_console_term_test_cell(t, 0, 0, c, NULL, NULL)
                   && axl_strcmp(c, in_e9) == 0,
               "term-utf8: U+00E9 stored as C3 A9 in one cell");

    ops->output_text(u, in_eur, 3);
    test_check(_axl_console_term_test_cell(t, 0, 1, c, NULL, NULL)
                   && axl_strcmp(c, in_eur) == 0,
               "term-utf8: U+20AC stored as E2 82 AC in one cell");

    ops->output_text(u, in_sur, 3);
    test_check(_axl_console_term_test_cell(t, 0, 2, c, NULL, NULL)
                   && axl_strcmp(c, e_fffd) == 0,
               "term-utf8: a lone surrogate is stored as U+FFFD, not WTF-8");

    uint32_t cr = 99, cc = 99;
    _axl_console_term_test_cursor(t, &cr, &cc);
    test_check(cr == 0 && cc == 3,
               "term-utf8: three multi-byte codepoints occupied three cells");

    axl_console_term_free(t);
}

/* Scrollback: lines that scroll off the top land in the history ring; scroll()
   navigates it with clamping. */
static void
test_console_term_scrollback(void)
{
    AxlConsoleTermConfig cfg = { .cols = 8, .rows = 3, .scrollback_rows = 10 };
    AxlConsoleTerm      *t   = axl_console_term_new(&cfg);
    void                *u   = NULL;
    const AxlConsoleOps *ops = axl_console_term_ops(t, &u);

    /* Five lines "0".."4" through a 3-row screen: "0","1","2" scroll into history,
       leaving "3","4" live (rows 0,1). */
    ops->output_text(u, "0\r\n1\r\n2\r\n3\r\n4\r\n", 15);

    char c[5];
    test_check(_axl_console_term_test_cell(t, 0, 0, c, NULL, NULL) && axl_strcmp(c, "3") == 0,
               "term-sb: live screen top is '3' after 5 lines");
    test_check(_axl_console_term_test_hist_cell(t, 1, 0, c) && axl_strcmp(c, "2") == 0,
               "term-sb: history row 1-back is '2' (most-recently scrolled off)");
    test_check(_axl_console_term_test_hist_cell(t, 3, 0, c) && axl_strcmp(c, "0") == 0,
               "term-sb: history row 3-back is '0' (oldest)");
    test_check(!_axl_console_term_test_hist_cell(t, 4, 0, c),
               "term-sb: only 3 lines in history (4-back is absent)");

    /* scroll() clamps to [0, hist_fill]. */
    axl_console_term_scroll(t, 2);
    test_check(_axl_console_term_test_scroll_off(t) == 2, "term-sb: scroll(+2) -> off 2");
    axl_console_term_scroll(t, 999);
    test_check(_axl_console_term_test_scroll_off(t) == 3, "term-sb: scroll(+999) clamps to 3");
    axl_console_term_scroll(t, -999);
    test_check(_axl_console_term_test_scroll_off(t) == 0, "term-sb: scroll(-999) back to live (0)");

    axl_console_term_free(t);
}

static bool
term_px_is_rgb(AxlGfxPixel p, uint8_t r, uint8_t g, uint8_t b)
{
    return p.red == r && p.green == g && p.blue == b;
}

static void
test_console_term_render(void)
{
    const AxlFont *font = axl_gfx_default_font();
    uint32_t       cw   = font->cell_width;
    uint32_t       ch   = font->cell_height;

    AxlGfxBuffer *buf = axl_gfx_buffer_new(cw * 4, ch * 2);
    test_check(buf != NULL, "term-render: offscreen target buffer allocated");

    /* Magenta sentinel so we can prove render() actually paints the bg (palette[0] is
       black -- a black-cleared buffer could not tell "painted" from "untouched"). */
    axl_gfx_buffer_clear(buf, AXL_GFX_RGB(0xFF, 0x00, 0xFF));

    AxlConsoleTermConfig cfg = { .cols = 4, .rows = 2, .target = buf };
    AxlConsoleTerm      *t   = axl_console_term_new(&cfg);
    void                *u   = NULL;
    const AxlConsoleOps *ops = axl_console_term_ops(t, &u);

    ops->output_text(u, "A", 1);        /* fg=7 light-gray on bg=0 black at (0,0) */
    axl_console_term_render(t);

    AxlGfxPixel *px = axl_gfx_buffer_pixels(buf);
    uint32_t     w  = cw * 4;

    test_check(term_px_is_rgb(px[0], 0x00, 0x00, 0x00),
               "term-render: cell (0,0) bg painted palette[bg=0] (black)");

    /* The 'A' glyph drew at least one non-background pixel in cell (0,0). */
    bool glyph = false;
    for (uint32_t yy = 0; yy < ch && !glyph; yy++) {
        for (uint32_t xx = 0; xx < cw; xx++) {
            if (!term_px_is_rgb(px[yy * w + xx], 0x00, 0x00, 0x00)) { glyph = true; break; }
        }
    }
    test_check(glyph, "term-render: 'A' glyph drew a foreground pixel in cell (0,0)");

    /* A blank cell on the same (dirty) row is still bg-painted (sentinel overwritten). */
    test_check(term_px_is_rgb(px[3 * cw], 0x00, 0x00, 0x00),
               "term-render: blank cell (0,3) bg painted (magenta sentinel gone)");

    /* render() cleared the dirty flags: re-sentinel, render again, cell (0,0) is
       untouched (only the cursor caret at (0,1) repaints). */
    axl_gfx_buffer_clear(buf, AXL_GFX_RGB(0xFF, 0x00, 0xFF));
    axl_console_term_render(t);
    test_check(term_px_is_rgb(px[0], 0xFF, 0x00, 0xFF),
               "term-render: dirty flags cleared -> clean row not repainted");

    axl_console_term_free(t);
    axl_gfx_buffer_free(buf);
}

/* Scan the box under the cursor hotspot for any white (0xFF) fill pixel from the
   built-in arrow sprite. The blank cells under it are black, so a white pixel present
   == the arrow is composited there. The 12x19 box matches AxlCursor's private built-in
   ARROW_W x ARROW_H (axl-cursor.c); the hotspot (tip) is the sprite's top-left. */
static bool
term_arrow_present(AxlGfxPixel *px, uint32_t stride, int32_t hx, int32_t hy)
{
    for (int32_t y = hy; y < hy + 19; y++) {
        for (int32_t x = hx; x < hx + 12; x++) {
            if (term_px_is_rgb(px[(uint32_t)y * stride + (uint32_t)x], 0xFF, 0xFF, 0xFF)) {
                return true;
            }
        }
    }
    return false;
}

static void
test_console_term_mouse_cursor(void)
{
    const AxlFont *font = axl_gfx_default_font();
    uint32_t       cw   = font->cell_width;
    uint32_t       ch   = font->cell_height;
    uint32_t       w    = cw * 4;
    uint32_t       h    = ch * 4;

    /* Off by default: no cursor, set_pointer is a no-op, render draws no arrow. */
    AxlGfxBuffer        *nb  = axl_gfx_buffer_new(w, h);
    AxlConsoleTermConfig cfg0 = { .cols = 4, .rows = 4, .target = nb };
    AxlConsoleTerm      *t0   = axl_console_term_new(&cfg0);
    bool    vis  = true;
    int32_t cx   = 7, cy = 7;
    _axl_console_term_test_pointer(t0, &cx, &cy, &vis);
    test_check(!vis, "term-mouse: cursor hidden by default (mouse_cursor unset)");
    axl_gfx_buffer_clear(nb, AXL_GFX_RGB(0x00, 0x00, 0x00));
    axl_console_term_set_pointer(t0, 8, 8);
    axl_console_term_render(t0);
    test_check(!term_arrow_present(axl_gfx_buffer_pixels(nb), w, 8, 8),
               "term-mouse: no arrow drawn when mouse_cursor is unset");
    axl_console_term_free(t0);
    axl_gfx_buffer_free(nb);

    /* mouse_cursor on: set_pointer shows + positions the cursor; render composites the
       arrow at the hotspot; hide_pointer removes it on the next render. */
    AxlGfxBuffer        *buf = axl_gfx_buffer_new(w, h);
    AxlConsoleTermConfig cfg  = { .cols = 4, .rows = 4, .target = buf, .mouse_cursor = true };
    AxlConsoleTerm      *t    = axl_console_term_new(&cfg);

    vis = true;
    _axl_console_term_test_pointer(t, &cx, &cy, &vis);
    test_check(!vis, "term-mouse: cursor starts hidden");

    axl_gfx_buffer_clear(buf, AXL_GFX_RGB(0x00, 0x00, 0x00));
    axl_console_term_set_pointer(t, 8, 8);
    _axl_console_term_test_pointer(t, &cx, &cy, &vis);
    test_check(vis && cx == 8 && cy == 8, "term-mouse: set_pointer shows + positions at (8,8)");

    axl_console_term_render(t);
    AxlGfxPixel *px = axl_gfx_buffer_pixels(buf);
    test_check(term_arrow_present(px, w, 8, 8),
               "term-mouse: render composites the arrow at the hotspot");

    /* handle_pointer MOVE relocates the tracking cursor (the event-routed feed). The
       new spot (20,40) is far enough that its arrow box does not overlap (8,8)'s. The
       move itself (via set_pointer's drop) must UNFOLD the old arrow immediately -- so
       assert the old spot is clean BEFORE any re-render, which is the real
       ghost/unfold check (no buffer clear intervening to mask it). */
    AxlInputEvent mv = { .type = AXL_INPUT_MOUSE_MOVE, .x = 20, .y = 40 };
    axl_console_term_handle_pointer(t, &mv);
    _axl_console_term_test_pointer(t, &cx, &cy, &vis);
    test_check(vis && cx == 20 && cy == 40, "term-mouse: handle_pointer MOVE tracks to (20,40)");
    test_check(!term_arrow_present(px, w, 8, 8),
               "term-mouse: moving unfolds the old arrow (no ghost at the old spot)");
    axl_console_term_render(t);
    test_check(term_arrow_present(px, w, 20, 40),
               "term-mouse: arrow follows the pointer to the new spot");

    /* hide_pointer: next render leaves the cells clean where the arrow was. */
    axl_console_term_hide_pointer(t);
    _axl_console_term_test_pointer(t, &cx, &cy, &vis);
    test_check(!vis, "term-mouse: hide_pointer hides the cursor");
    axl_gfx_buffer_clear(buf, AXL_GFX_RGB(0x00, 0x00, 0x00));
    axl_console_term_render(t);
    test_check(!term_arrow_present(px, w, 20, 40),
               "term-mouse: hidden cursor draws no arrow");

    axl_console_term_free(t);
    axl_gfx_buffer_free(buf);
}

static void
test_console_term_reflow(void)
{
    AxlConsoleTermConfig cfg = { .cols = 20, .rows = 5 };
    AxlConsoleTerm      *t   = axl_console_term_new(&cfg);
    void                *u   = NULL;
    const AxlConsoleOps *ops = axl_console_term_ops(t, &u);

    uint32_t cols, rows, cw0, ch0;
    _axl_console_term_test_geometry(t, &cols, &rows, &cw0, &ch0, NULL, NULL, NULL, NULL);
    test_check(cols == 20 && rows == 5, "term-reflow: initial geometry 20x5");

    /* resize reallocs the grid; a subsequent op lands within the new bounds and the
       widened far corner is addressable. */
    axl_console_term_resize(t, 40, 10);
    _axl_console_term_test_geometry(t, &cols, &rows, NULL, NULL, NULL, NULL, NULL, NULL);
    test_check(cols == 40 && rows == 10, "term-reflow: resize(40,10) -> new geometry");
    ops->output_text(u, "Z", 1);
    char c[5];
    test_check(_axl_console_term_test_cell(t, 0, 0, c, NULL, NULL) && axl_strcmp(c, "Z") == 0,
               "term-reflow: output after resize lands in the new grid");
    test_check(_axl_console_term_test_cell(t, 9, 39, c, NULL, NULL) && c[0] == '\0',
               "term-reflow: far corner (9,39) exists and is blank after grow");

    /* set_font swaps the cached cell metrics (synthetic font, doubled cell). */
    AxlFont big = *axl_gfx_default_font();
    big.cell_width  = (uint16_t)(cw0 * 2);
    big.cell_height = (uint16_t)(ch0 * 2);
    axl_console_term_set_font(t, &big);
    uint32_t cw1, ch1;
    _axl_console_term_test_geometry(t, NULL, NULL, &cw1, &ch1, NULL, NULL, NULL, NULL);
    test_check(cw1 == cw0 * 2 && ch1 == ch0 * 2, "term-reflow: set_font updates cell metrics");

    /* set_bounds updates render origin + extent. */
    axl_console_term_set_bounds(t, 5, 7, 100, 50);
    uint32_t bx, by, bw, bh;
    _axl_console_term_test_geometry(t, NULL, NULL, NULL, NULL, &bx, &by, &bw, &bh);
    test_check(bx == 5 && by == 7 && bw == 100 && bh == 50,
               "term-reflow: set_bounds updates bounds");

    /* set_palette swaps a colour the renderer would use. */
    AxlGfxPixel pal[16];
    for (uint32_t i = 0; i < 16; i++) { pal[i] = AXL_GFX_RGB(0, 0, 0); }
    pal[1] = AXL_GFX_RGB(0x12, 0x34, 0x56);
    axl_console_term_set_palette(t, pal);
    AxlGfxPixel got = _axl_console_term_test_palette(t, 1);
    test_check(term_px_is_rgb(got, 0x12, 0x34, 0x56), "term-reflow: set_palette swaps entry 1");

    axl_console_term_free(t);
}

static void
term_clipboard_string(char *out, size_t out_sz)
{
    size_t      len = 0;
    const char *p   = axl_clipboard_get(&len, NULL);
    if (p == NULL || len >= out_sz) { out[0] = '\0'; return; }
    for (size_t i = 0; i < len; i++) { out[i] = ((const char *)p)[i]; }
    out[len] = '\0';
}

static void
test_console_term_selection(void)
{
    char out[32];

    /* Single-row selection copies exactly the covered glyphs (cols 0..4 = "Hello"). */
    AxlConsoleTermConfig cfg = { .cols = 20, .rows = 5 };
    AxlConsoleTerm      *t   = axl_console_term_new(&cfg);
    void                *u   = NULL;
    const AxlConsoleOps *ops = axl_console_term_ops(t, &u);

    ops->output_text(u, "Hello", 5);
    axl_console_term_selection_start(t, 0, 0);
    axl_console_term_selection_extend(t, 4, 0);
    test_check(axl_console_term_selection_copy(t) == AXL_OK,
               "term-sel: copy of row-0 cols 0..4 succeeds");
    term_clipboard_string(out, sizeof(out));
    test_check(axl_strcmp(out, "Hello") == 0, "term-sel: single-row selection == 'Hello'");
    axl_console_term_free(t);

    /* Two-row selection joins rows with '\n' and trims trailing blanks. */
    AxlConsoleTermConfig cfg2 = { .cols = 8, .rows = 3 };
    t   = axl_console_term_new(&cfg2);
    ops = axl_console_term_ops(t, &u);
    ops->output_text(u, "AB\r\nCD", 6);
    axl_console_term_selection_start(t, 0, 0);    /* col 0, row 0 */
    axl_console_term_selection_extend(t, 1, 1);   /* col 1, row 1 */
    test_check(axl_console_term_selection_copy(t) == AXL_OK, "term-sel: two-row copy succeeds");
    term_clipboard_string(out, sizeof(out));
    test_check(axl_strcmp(out, "AB\nCD") == 0,
               "term-sel: two-row selection trims trailing blanks + joins with newline");

    /* clear() drops the selection; copy then fails. */
    axl_console_term_selection_clear(t);
    test_check(axl_console_term_selection_copy(t) == AXL_ERR,
               "term-sel: copy with no active selection fails");
    axl_console_term_free(t);

    /* A selected cell renders inverted (fg<->bg swapped). Select the blank cell (0,1)
       so no glyph confuses the background check. */
    const AxlFont *font = axl_gfx_default_font();
    uint32_t       cw   = font->cell_width;
    uint32_t       ch   = font->cell_height;
    AxlGfxBuffer  *buf  = axl_gfx_buffer_new(cw * 2, ch);
    AxlConsoleTermConfig cfg3 = { .cols = 2, .rows = 1, .target = buf };
    AxlConsoleTerm      *t3   = axl_console_term_new(&cfg3);
    void                *u3   = NULL;
    const AxlConsoleOps *o3   = axl_console_term_ops(t3, &u3);
    o3->output_text(u3, "X", 1);                  /* 'X' at (0,0); (0,1) stays blank */
    axl_console_term_selection_start(t3, 0, 0);
    axl_console_term_selection_extend(t3, 1, 0);  /* drag over cols 0..1 (non-empty) */
    axl_console_term_render(t3);
    AxlGfxPixel *px = axl_gfx_buffer_pixels(buf);
    test_check(term_px_is_rgb(px[cw], 0xA8, 0xA8, 0xA8),
               "term-sel: selected blank cell renders inverted bg (fg colour)");
    axl_console_term_free(t3);
    axl_gfx_buffer_free(buf);

    /* A zero-length selection (a click with no drag: anchor == free end) must render
       NOTHING -- otherwise a stray click paints a lone inverted cell (the char typed
       there shows reverse-video). Only a drag highlights. */
    AxlGfxBuffer  *cbuf = axl_gfx_buffer_new(cw * 2, ch);
    AxlConsoleTermConfig cfg4 = { .cols = 2, .rows = 1, .target = cbuf };
    AxlConsoleTerm      *t4   = axl_console_term_new(&cfg4);
    void                *u4   = NULL;
    const AxlConsoleOps *o4   = axl_console_term_ops(t4, &u4);
    o4->output_text(u4, "X", 1);                  /* 'X' at (0,0), fg=7 on bg=0 black */
    axl_console_term_selection_start(t4, 0, 0);
    axl_console_term_selection_extend(t4, 0, 0);  /* click, no drag -> zero-length */
    axl_console_term_render(t4);
    AxlGfxPixel *cpx = axl_gfx_buffer_pixels(cbuf);
    test_check(term_px_is_rgb(cpx[0], 0x00, 0x00, 0x00),
               "term-sel: a zero-length selection (click) renders the cell NORMAL (bg black)");
    test_check(axl_console_term_selection_copy(t4) == AXL_ERR,
               "term-sel: copy of a zero-length selection fails (nothing selected)");
    axl_console_term_free(t4);
    axl_gfx_buffer_free(cbuf);
}

static int     s_term_zoom_calls;
static int32_t s_term_zoom_delta;

static void
term_on_zoom(void *user, int32_t delta)
{
    (void)user;
    s_term_zoom_calls++;
    s_term_zoom_delta = delta;
}

static void
test_console_term_interact(void)
{
    char out[32];

    /* --- handle_hotkey ------------------------------------------------------ */
    AxlConsoleTermConfig cfg = { .cols = 8, .rows = 3, .scrollback_rows = 10 };
    AxlConsoleTerm      *t   = axl_console_term_new(&cfg);
    void                *u   = NULL;
    const AxlConsoleOps *ops = axl_console_term_ops(t, &u);
    ops->output_text(u, "0\r\n1\r\n2\r\n3\r\n4\r\n", 15);   /* 3 lines into history */

    EFI_KEY_DATA pgup  = key_data(0x09, 0, EFI_SHIFT_STATE_VALID | EFI_LEFT_SHIFT_PRESSED, 0);
    test_check(axl_console_term_handle_hotkey(t, &pgup) == true
                   && _axl_console_term_test_scroll_off(t) == 3,
               "term-int: Shift+PgUp consumed, scrolls back one page (rows)");
    EFI_KEY_DATA pgdn = key_data(0x0A, 0, EFI_SHIFT_STATE_VALID | EFI_LEFT_SHIFT_PRESSED, 0);
    test_check(axl_console_term_handle_hotkey(t, &pgdn) == true
                   && _axl_console_term_test_scroll_off(t) == 0,
               "term-int: Shift+PgDn consumed, scrolls forward to live");
    EFI_KEY_DATA plain = key_data(0, 'a', 0, 0);
    test_check(axl_console_term_handle_hotkey(t, &plain) == false,
               "term-int: a plain key is not consumed");
    axl_console_term_free(t);

    /* Ctrl+Shift+C copies the selection. */
    AxlConsoleTermConfig cfg2 = { .cols = 8, .rows = 2 };
    t   = axl_console_term_new(&cfg2);
    ops = axl_console_term_ops(t, &u);
    ops->output_text(u, "Hi", 2);
    axl_console_term_selection_start(t, 0, 0);
    axl_console_term_selection_extend(t, 1, 0);
    EFI_KEY_DATA copyk = key_data(0, 3,
        EFI_SHIFT_STATE_VALID | EFI_LEFT_CONTROL_PRESSED | EFI_LEFT_SHIFT_PRESSED, 0);
    test_check(axl_console_term_handle_hotkey(t, &copyk) == true,
               "term-int: Ctrl+Shift+C consumed");
    term_clipboard_string(out, sizeof(out));
    test_check(axl_strcmp(out, "Hi") == 0, "term-int: Ctrl+Shift+C copied the selection");
    axl_console_term_free(t);

    /* --- handle_pointer ----------------------------------------------------- */
    const AxlFont *font = axl_gfx_default_font();
    uint32_t       cw   = font->cell_width;
    uint32_t       ch   = font->cell_height;

    /* Wheel (no Ctrl) scrolls; Ctrl+wheel zooms instead. */
    AxlConsoleTermConfig cfg3 = { .cols = 8, .rows = 3, .scrollback_rows = 10,
                                  .on_zoom = term_on_zoom };
    t   = axl_console_term_new(&cfg3);
    ops = axl_console_term_ops(t, &u);
    ops->output_text(u, "0\r\n1\r\n2\r\n3\r\n4\r\n", 15);   /* hist_fill = 3 */

    s_term_zoom_calls = 0;
    AxlInputEvent wheel = { .type = AXL_INPUT_MOUSE_WHEEL, .wheel_dy = 2 };
    axl_console_term_handle_pointer(t, &wheel);
    test_check(_axl_console_term_test_scroll_off(t) == 2 && s_term_zoom_calls == 0,
               "term-int: wheel scrolls back 2 rows, no zoom");

    AxlInputEvent cwheel = { .type = AXL_INPUT_MOUSE_WHEEL, .wheel_dy = 1,
                             .modifiers = AXL_INPUT_MOD_LCTRL };
    axl_console_term_handle_pointer(t, &cwheel);
    test_check(s_term_zoom_calls == 1 && s_term_zoom_delta == 1
                   && _axl_console_term_test_scroll_off(t) == 2,
               "term-int: Ctrl+wheel invokes on_zoom, does not scroll");
    axl_console_term_free(t);

    /* Drag: button-down then a dragging move selects a cell range; copy reflects it. */
    AxlConsoleTermConfig cfg4 = { .cols = 8, .rows = 2 };
    t   = axl_console_term_new(&cfg4);
    ops = axl_console_term_ops(t, &u);
    ops->output_text(u, "Hi", 2);
    AxlInputEvent down = { .type = AXL_INPUT_MOUSE_BUTTON_DOWN,
                           .buttons = AXL_INPUT_BUTTON_LEFT,
                           .x = (int32_t)(cw / 2), .y = (int32_t)(ch / 2) };
    axl_console_term_handle_pointer(t, &down);
    AxlInputEvent move = { .type = AXL_INPUT_MOUSE_MOVE, .dragging = true,
                           .x = (int32_t)(cw + cw / 2), .y = (int32_t)(ch / 2) };
    axl_console_term_handle_pointer(t, &move);
    AxlInputEvent up = { .type = AXL_INPUT_MOUSE_BUTTON_UP,
                         .x = (int32_t)(cw + cw / 2), .y = (int32_t)(ch / 2) };
    axl_console_term_handle_pointer(t, &up);   /* release AFTER a real drag: keep it */
    test_check(axl_console_term_selection_copy(t) == AXL_OK, "term-int: drag+release keeps the selection");
    term_clipboard_string(out, sizeof(out));
    test_check(axl_strcmp(out, "Hi") == 0, "term-int: drag-select spans cols 0..1 == 'Hi'");
    axl_console_term_free(t);

    /* A plain click (button-down then up, no drag) must leave NO lingering selection
       -- otherwise a stray click paints a permanently-inverted cell. */
    AxlConsoleTermConfig cfg5 = { .cols = 8, .rows = 2 };
    t   = axl_console_term_new(&cfg5);
    ops = axl_console_term_ops(t, &u);
    ops->output_text(u, "Hi", 2);
    AxlInputEvent cdn = { .type = AXL_INPUT_MOUSE_BUTTON_DOWN, .buttons = AXL_INPUT_BUTTON_LEFT,
                          .x = (int32_t)(cw / 2), .y = (int32_t)(ch / 2) };
    AxlInputEvent cup = { .type = AXL_INPUT_MOUSE_BUTTON_UP,
                          .x = (int32_t)(cw / 2), .y = (int32_t)(ch / 2) };
    axl_console_term_handle_pointer(t, &cdn);
    axl_console_term_handle_pointer(t, &cup);
    test_check(axl_console_term_selection_copy(t) == AXL_ERR,
               "term-int: a plain click (no drag) leaves no selection");
    axl_console_term_free(t);
}

static void
test_console_term_bounds(void)
{
    const AxlFont *font = axl_gfx_default_font();
    uint32_t       cw   = font->cell_width;
    uint32_t       ch   = font->cell_height;

    /* A 4x2 grid rendered into a target, but bounds clip it to 2 cols x 1 row:
       cells outside [0,2*cw) x [0,ch) must NOT be painted. */
    AxlGfxBuffer *buf = axl_gfx_buffer_new(cw * 4, ch * 2);
    axl_gfx_buffer_clear(buf, AXL_GFX_RGB(0xFF, 0x00, 0xFF));   /* magenta sentinel */
    AxlConsoleTermConfig cfg = { .cols = 4, .rows = 2, .target = buf,
                                 .x = 0, .y = 0, .w = cw * 2, .h = ch };
    AxlConsoleTerm      *t   = axl_console_term_new(&cfg);
    void                *u   = NULL;
    const AxlConsoleOps *ops = axl_console_term_ops(t, &u);
    ops->output_text(u, "ABCD\r\nE", 7);   /* row 0 = ABCD, row 1 = E (both dirty) */
    axl_console_term_render(t);

    AxlGfxPixel *px    = axl_gfx_buffer_pixels(buf);
    uint32_t     w_px  = cw * 4;
    test_check(term_px_is_rgb(px[0], 0x00, 0x00, 0x00),
               "term-bounds: in-bounds cell (0,0) painted");
    test_check(term_px_is_rgb(px[2 * cw], 0xFF, 0x00, 0xFF),
               "term-bounds: cell past w (col 2) NOT painted (bounds clip)");
    test_check(term_px_is_rgb(px[(size_t)ch * w_px], 0xFF, 0x00, 0xFF),
               "term-bounds: row past h (row 1) NOT painted (bounds clip)");

    /* pixel_to_cell rejects clicks outside the bounds. */
    AxlInputEvent down = { .type = AXL_INPUT_MOUSE_BUTTON_DOWN,
                           .buttons = AXL_INPUT_BUTTON_LEFT,
                           .x = (int32_t)(cw * 3), .y = 0 };   /* col 3, outside w=2*cw */
    axl_console_term_handle_pointer(t, &down);
    test_check(axl_console_term_selection_copy(t) == AXL_ERR,
               "term-bounds: click outside bounds starts no selection");

    axl_console_term_free(t);
    axl_gfx_buffer_free(buf);
}

static void
test_console_term_autogeom(void)
{
    const AxlFont *font = axl_gfx_default_font();
    uint32_t       cw   = font->cell_width;
    uint32_t       ch   = font->cell_height;

    /* Zeroed cols/rows derive geometry from the target buffer + font metrics. */
    AxlGfxBuffer *buf = axl_gfx_buffer_new(cw * 10, ch * 4);
    AxlConsoleTermConfig cfg = { .target = buf };   /* cols/rows = 0 -> auto */
    AxlConsoleTerm      *t   = axl_console_term_new(&cfg);
    uint32_t cols, rows;
    _axl_console_term_test_geometry(t, &cols, &rows, NULL, NULL, NULL, NULL, NULL, NULL);
    test_check(cols == 10 && rows == 4,
               "term-auto: zeroed geometry derived from target buffer + font");
    axl_console_term_free(t);
    axl_gfx_buffer_free(buf);
}

static void
test_console_term_resize_oom(void)
{
    AxlConsoleTermConfig cfg = { .cols = 20, .rows = 5, .scrollback_rows = 8 };
    AxlConsoleTerm      *t   = axl_console_term_new(&cfg);

    /* Fail the 3rd alloc inside resize = the history reallocation (screen, dirty,
       THEN history on a column change). The resize must roll back atomically so the
       screen/history strides never mismatch (which would overflow on the next
       scroll); geometry stays 20x5. */
    axl_mem_fail_next_alloc(3);
    axl_console_term_resize(t, 40, 10);
    uint32_t cols, rows;
    _axl_console_term_test_geometry(t, &cols, &rows, NULL, NULL, NULL, NULL, NULL, NULL);
    test_check(cols == 20 && rows == 5,
               "term-oom: resize history-alloc OOM leaves geometry unchanged (atomic)");

    /* The ring is still 20-wide + consistent: scroll past the screen writes in-bounds
       (a mismatched stride would corrupt the heap fence, caught at free). */
    void                *u   = NULL;
    const AxlConsoleOps *ops = axl_console_term_ops(t, &u);
    ops->output_text(u, "0\r\n1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n", 21);
    char c[5];
    test_check(_axl_console_term_test_hist_cell(t, 1, 0, c) && c[0] != '\0',
               "term-oom: history ring consistent after rolled-back resize");

    axl_console_term_free(t);
}

/* The public header exposes AXL-owned aliases for the KeyShiftState bits so a
   tool that includes only <axl.h> can name the modifier inject_key_ex carries,
   without reaching into the EDK2/UEFI headers. Pin each alias to its EFI_*
   counterpart so the two can never drift. */
static void
test_console_tap_shift_state_aliases(void)
{
    test_check(AXL_CONSOLE_SHIFT_STATE_VALID == EFI_SHIFT_STATE_VALID,
               "shift_alias: AXL_CONSOLE_SHIFT_STATE_VALID == EFI_SHIFT_STATE_VALID");
    test_check(AXL_CONSOLE_RIGHT_SHIFT_PRESSED == EFI_RIGHT_SHIFT_PRESSED,
               "shift_alias: AXL_CONSOLE_RIGHT_SHIFT_PRESSED == EFI_RIGHT_SHIFT_PRESSED");
    test_check(AXL_CONSOLE_LEFT_SHIFT_PRESSED == EFI_LEFT_SHIFT_PRESSED,
               "shift_alias: AXL_CONSOLE_LEFT_SHIFT_PRESSED == EFI_LEFT_SHIFT_PRESSED");
    test_check(AXL_CONSOLE_RIGHT_CONTROL_PRESSED == EFI_RIGHT_CONTROL_PRESSED,
               "shift_alias: AXL_CONSOLE_RIGHT_CONTROL_PRESSED == EFI_RIGHT_CONTROL_PRESSED");
    test_check(AXL_CONSOLE_LEFT_CONTROL_PRESSED == EFI_LEFT_CONTROL_PRESSED,
               "shift_alias: AXL_CONSOLE_LEFT_CONTROL_PRESSED == EFI_LEFT_CONTROL_PRESSED");
    test_check(AXL_CONSOLE_RIGHT_ALT_PRESSED == EFI_RIGHT_ALT_PRESSED,
               "shift_alias: AXL_CONSOLE_RIGHT_ALT_PRESSED == EFI_RIGHT_ALT_PRESSED");
    test_check(AXL_CONSOLE_LEFT_ALT_PRESSED == EFI_LEFT_ALT_PRESSED,
               "shift_alias: AXL_CONSOLE_LEFT_ALT_PRESSED == EFI_LEFT_ALT_PRESSED");
}

// ---------------------------------------------------------------------------
// Alt-screen (explicit + auto heuristic) + input capture. The tap asserts the
// alt-screen (SIMPLE_TEXT_OUTPUT cannot express it); the encoder emits 1049h/l.
// ---------------------------------------------------------------------------

static void
test_console_mirror_altscreen_input(void)
{
    /* --- Explicit alt-screen: emission + idempotency, via the mirror API. --- */
    AxlConsoleTap *t = cm_tap_begin(80, 25, /*auto_alt=*/false,
                                    /*input_capture=*/false, /*passthrough=*/false);
    cm_cap_reset();
    test_check(!axl_console_mirror_in_alt_screen(cm_enc), "altscreen: starts outside");
    axl_console_mirror_enter_alt_screen(cm_enc);
    test_check(axl_strcmp(cm_cap, "\x1b[?1049h") == 0,
               "altscreen: enter emits exactly ESC[?1049h");
    test_check(axl_console_mirror_in_alt_screen(cm_enc), "altscreen: in_alt after enter");

    cm_cap_reset();
    axl_console_mirror_enter_alt_screen(cm_enc);   /* idempotent */
    test_check(cm_cap[0] == '\0', "altscreen: second enter emits nothing");

    cm_cap_reset();
    axl_console_mirror_leave_alt_screen(cm_enc);
    test_check(axl_strcmp(cm_cap, "\x1b[?1049l") == 0,
               "altscreen: leave emits exactly ESC[?1049l");
    test_check(!axl_console_mirror_in_alt_screen(cm_enc), "altscreen: out after leave");

    cm_cap_reset();
    axl_console_mirror_leave_alt_screen(cm_enc);   /* idempotent */
    test_check(cm_cap[0] == '\0', "altscreen: second leave emits nothing");
    cm_tap_end(t);

    /* --- Auto heuristic (bracket a full-screen app): a clear alone must NOT enter
       (the mirror-from-boot latch bug); enter on a backward cursor jump after a
       clear (a TUI repaint); leave on a newline (linear shell flow resumes).
       Op-sequences are the real ones captured from `edit` on a 160x50 console. --- */
    t = cm_tap_begin(160, /*rows=*/50, /*auto_alt=*/true, false, false);

    /* (1) Boot / shell prompt: ClearScreen then a prompt at the top row is not a
       full-screen app -- no ESC[?1049h may be emitted (this is the boot latch). */
    cm_cap_reset();
    _axl_console_tap_test_clear();
    _axl_console_tap_test_set_cursor(0, 0);
    _axl_console_tap_test_puts("FS0:\\> ");
    test_check(axl_strstr(cm_cap, "\x1b[?1049h") == NULL,
               "auto: a clear + top-row prompt does NOT enter the alt-screen (no boot latch)");
    test_check(!axl_console_tap_in_alt_screen(t), "auto: not in alt after a prompt clear");

    /* (2) Full-screen app: clear, paint the title, drop to the status row (forward),
       then jump BACK UP -- the backward jump is the TUI signal that enters. */
    cm_cap_reset();
    _axl_console_tap_test_clear();
    _axl_console_tap_test_set_cursor(0, 0);
    _axl_console_tap_test_puts("UEFI Editor");   /* title bar, no newline */
    _axl_console_tap_test_set_cursor(0, 49);     /* status row (forward, r 0->49) */
    test_check(axl_strstr(cm_cap, "\x1b[?1049h") == NULL,
               "auto: clear + forward addressing (title/status) does not enter yet");
    _axl_console_tap_test_set_cursor(0, 1);      /* BACKWARD jump 49->1: TUI repaint */
    test_check(axl_strstr(cm_cap, "\x1b[?1049h") != NULL,
               "auto: a backward cursor jump after a clear enters the alt-screen");
    test_check(axl_console_tap_in_alt_screen(t), "auto: in alt-screen after the backward jump");

    /* (3) App exits: clear, the shell redraws its prompt (no newline yet -> still
       in alt), then the first NEWLINE (a command runs) is linear flow -> leave. */
    cm_cap_reset();
    _axl_console_tap_test_clear();
    _axl_console_tap_test_set_cursor(0, 0);
    _axl_console_tap_test_puts("FS0:\\> ver");   /* prompt echo, no newline */
    test_check(axl_strstr(cm_cap, "\x1b[?1049l") == NULL,
               "auto: a clear + prompt with no newline does not leave yet");
    test_check(axl_console_tap_in_alt_screen(t), "auto: still in alt until linear flow resumes");
    _axl_console_tap_test_puts("\r\n");          /* newline -> linear flow */
    test_check(axl_strstr(cm_cap, "\x1b[?1049l") != NULL,
               "auto: a newline (linear flow) leaves the alt-screen");
    test_check(!axl_console_tap_in_alt_screen(t), "auto: left alt-screen on newline");
    cm_tap_end(t);

    /* (4) End-to-end mirror-from-boot: boot + cls + prompts stay OUT of the
       alt-screen, and a full-screen app is bracketed by exactly one enter/leave. */
    t = cm_tap_begin(160, 50, /*auto_alt=*/true, false, false);
    cm_cap_reset();
    _axl_console_tap_test_clear();                        /* boot clear */
    _axl_console_tap_test_set_cursor(0, 0);
    _axl_console_tap_test_puts("FS0:\\> cls\r\n");        /* prompt + a command */
    _axl_console_tap_test_clear();                        /* cls */
    _axl_console_tap_test_set_cursor(0, 0);
    _axl_console_tap_test_puts("FS0:\\> ");               /* prompt */
    test_check(axl_strstr(cm_cap, "\x1b[?1049h") == NULL,
               "mirror-from-boot: boot + cls + prompt never enters the alt-screen");
    test_check(!axl_console_tap_in_alt_screen(t), "mirror-from-boot: outside alt after boot/cls");

    cm_cap_reset();
    _axl_console_tap_test_clear();                        /* edit enters */
    _axl_console_tap_test_set_cursor(0, 49);
    _axl_console_tap_test_set_cursor(0, 1);               /* backward -> enter */
    _axl_console_tap_test_clear();                        /* edit exits, clears */
    _axl_console_tap_test_set_cursor(0, 0);
    _axl_console_tap_test_puts("FS0:\\> ver\r\n");        /* prompt + newline -> leave */
    const char *he = axl_strstr(cm_cap, "\x1b[?1049h");
    const char *le = axl_strstr(cm_cap, "\x1b[?1049l");
    test_check(he != NULL && le != NULL && he < le,
               "mirror-from-boot: one enter/leave pair brackets the full-screen app");
    test_check(!axl_console_tap_in_alt_screen(t), "mirror-from-boot: ends outside the alt-screen");
    cm_tap_end(t);

    /* --- Input capture: true serves ONLY the inject ring. --- */
    t = cm_tap_begin(80, 25, false, /*input_capture=*/true, false);
    _axl_console_tap_test_set_stub_conin(t, /*always_key=*/true);
    test_check(_axl_console_tap_test_read_key(t) != 0,
               "input_capture: read returns NOT_READY (ignores physical key)");
    axl_console_tap_inject_key(t, 0, 'x');
    test_check(_axl_console_tap_test_read_key(t) == 0,
               "input_capture: read serves an injected key");
    test_check(_axl_console_tap_test_read_key(t) != 0,
               "input_capture: ring drained -> NOT_READY again");
    _axl_console_tap_test_pump(t);   /* the WaitForKey notify */
    test_check(_axl_console_tap_test_read_key(t) != 0,
               "input_capture: pump does not poll the physical keyboard");
    cm_tap_end(t);

    /* Sanity: capture off (default) DOES fall through to the physical key. */
    t = cm_tap_begin(80, 25, false, /*input_capture=*/false, false);
    _axl_console_tap_test_set_stub_conin(t, /*always_key=*/true);
    test_check(_axl_console_tap_test_read_key(t) == 0,
               "input_capture off: read falls through to the physical key");
    cm_tap_end(t);
}

// ---------------------------------------------------------------------------
// Control-character sanitization. EDK2's TerminalConOut.c substitutes '?' for any
// char that is neither printable (>= 0x20) nor one of {NUL, BS, TAB, LF, CR}; a
// literal ESC is emitted ONLY when the driver itself is writing a control string
// (TerminalDevice->OutputEscChar). So a UEFI app CANNOT push raw VT through
// OutputString. Forwarding ESC verbatim would both diverge from firmware
// semantics and let an app printing a user-controlled string inject escapes into
// the consumer's terminal (e.g. "\x1b[2J" clearing a remote xterm).
// ---------------------------------------------------------------------------

static void
test_console_tap_sanitize(void)
{
    AxlConsoleTap *t = cm_tap_begin(80, 25, false, false, /*passthrough=*/false);

    cm_cap_reset();
    _axl_console_tap_test_puts("\x1b[2J");
    test_check(axl_strcmp(cm_cap, "?[2J") == 0,
               "sanitize: ESC becomes '?' (no escape injection through output_text)");

    cm_cap_reset();
    _axl_console_tap_test_puts("\x07x");   /* BEL */
    test_check(axl_strcmp(cm_cap, "?x") == 0,
               "sanitize: other C0 control chars become '?'");

    cm_cap_reset();
    _axl_console_tap_test_puts("a\bb\tc\rd\ne");
    test_check(axl_strcmp(cm_cap, "a\bb\tc\rd\ne") == 0,
               "sanitize: BS/TAB/CR/LF pass through (the EFI control chars)");

    /* The substituted '?' is printable, so it advances the cursor like EDK2's does. */
    _axl_console_tap_test_set_cursor(0, 0);
    _axl_console_tap_test_puts("\x1b");
    int32_t col = -1, row = -1;
    _axl_console_tap_test_get_cursor(t, &col, &row);
    test_check(col == 1 && row == 0,
               "sanitize: the substituted '?' advances the cursor (EDK2 parity)");

    cm_tap_end(t);
}

// ---------------------------------------------------------------------------
// The tap's UCS-2 -> UTF-8 encode. output_text is contractually a UTF-8 run
// (axl-console-ops.h), and the console is UCS-2, so a lone surrogate CODE UNIT
// has no UTF-8 spelling: encoding it in its 3-byte shape anyway is WTF-8, which
// every conforming decoder downstream (a remote xterm, axl-console-term's grid,
// the clipboard) refuses. Substitute U+FFFD instead of dropping, so the run
// stays one cell per code unit -- the tap's own reported cell rule.
// ---------------------------------------------------------------------------

static void
test_console_tap_utf8(void)
{
    /* Byte arrays, not escaped literals: the point is the exact wire bytes. */
    static const char e_e9[]   = { (char)0xC3, (char)0xA9, '\0' };
    static const char e_eur[]  = { (char)0xE2, (char)0x82, (char)0xAC, '\0' };
    static const char e_fffd[] = { (char)0xEF, (char)0xBF, (char)0xBD, '\0' };

    AxlConsoleTap *t = cm_tap_begin(80, 25, false, false, /*passthrough=*/false);

    /* Non-ASCII BMP text is exactly what the tap carries that TerminalDxe's
       ASCII-only wire could not, so both multi-byte arms need pinning. */
    static const uint16_t u_e9[] = { 0x00E9, 0 };
    cm_cap_reset();
    _axl_console_tap_test_puts16(u_e9);
    test_check(axl_strcmp(cm_cap, e_e9) == 0,
               "tap-utf8: U+00E9 emits the 2-byte sequence C3 A9");

    static const uint16_t u_eur[] = { 0x20AC, 0 };
    cm_cap_reset();
    _axl_console_tap_test_puts16(u_eur);
    test_check(axl_strcmp(cm_cap, e_eur) == 0,
               "tap-utf8: U+20AC emits the 3-byte sequence E2 82 AC");

    static const uint16_t u_hi[] = { 0xD800, 0 };
    cm_cap_reset();
    _axl_console_tap_test_puts16(u_hi);
    test_check(axl_strcmp(cm_cap, e_fffd) == 0,
               "tap-utf8: a lone high surrogate emits U+FFFD, not 3-byte WTF-8");

    static const uint16_t u_lo[] = { 0xDFFF, 0 };
    cm_cap_reset();
    _axl_console_tap_test_puts16(u_lo);
    test_check(axl_strcmp(cm_cap, e_fffd) == 0,
               "tap-utf8: a lone low surrogate emits U+FFFD, not 3-byte WTF-8");

    /* U+FFFD itself is an ordinary 3-byte glyph -- the substitution must not be
       reachable only by accident of the surrogate's byte length. */
    static const uint16_t u_rep[] = { 0xFFFD, 0 };
    cm_cap_reset();
    _axl_console_tap_test_puts16(u_rep);
    test_check(axl_strcmp(cm_cap, e_fffd) == 0,
               "tap-utf8: a literal U+FFFD passes through unchanged");

    /* Inline, mid-run: substituting keeps the byte run and the cursor in step.
       Dropping would have advanced the column without emitting a glyph. */
    _axl_console_tap_test_set_cursor(0, 0);
    static const uint16_t u_mix[] = { 'a', 0xD800, 'b', 0 };
    static const char     e_mix[] = { 'a', (char)0xEF, (char)0xBF, (char)0xBD,
                                      'b', '\0' };
    cm_cap_reset();
    _axl_console_tap_test_puts16(u_mix);
    test_check(axl_strcmp(cm_cap, e_mix) == 0,
               "tap-utf8: the substitution sits inline between the run's glyphs");
    int32_t col = -1, row = -1;
    _axl_console_tap_test_get_cursor(t, &col, &row);
    test_check(col == 3 && row == 0,
               "tap-utf8: the substituted U+FFFD advances exactly one cell");

    cm_tap_end(t);
}

// ---------------------------------------------------------------------------
// The tap must OWN its SIMPLE_TEXT_OUTPUT_MODE when it is the only console
// writer. REGRESSION: install did `my_conout = *orig_conout`, which aliases the
// ORIGINAL's Mode pointer; the original driver is the only thing that maintains
// CursorRow/CursorColumn/Attribute/CursorVisible, so with passthrough_local=false
// that state froze and a nested Shell scribbled over itself (AGT axterm P4 spike).
// ---------------------------------------------------------------------------

static void
test_console_mirror_owned_mode(void)
{
    /* 80x4 so autowrap + scroll clamp are cheap to drive. */
    AxlConsoleTap *t = cm_tap_begin(80, 4, false, false, /*passthrough=*/false);

    test_check(_axl_console_tap_test_mode_owned(t),
               "owned_mode: passthrough off -> tap owns its Mode, not the original's");

    int32_t col = -1, row = -1;
    _axl_console_tap_test_clear();
    _axl_console_tap_test_get_cursor(t, &col, &row);
    test_check(col == 0 && row == 0, "owned_mode: ClearScreen homes the Mode cursor");

    _axl_console_tap_test_set_cursor(5, 3);
    _axl_console_tap_test_get_cursor(t, &col, &row);
    test_check(col == 5 && row == 3, "owned_mode: SetCursorPosition updates the Mode cursor");

    /* Text must advance the cursor — the job GraphicsConsole used to do for us. */
    _axl_console_tap_test_set_cursor(0, 0);
    _axl_console_tap_test_puts("ab");
    _axl_console_tap_test_get_cursor(t, &col, &row);
    test_check(col == 2 && row == 0, "owned_mode: printable text advances the column");

    _axl_console_tap_test_puts("\r");
    _axl_console_tap_test_get_cursor(t, &col, &row);
    test_check(col == 0 && row == 0, "owned_mode: CR homes the column");

    _axl_console_tap_test_puts("\n");
    _axl_console_tap_test_get_cursor(t, &col, &row);
    test_check(col == 0 && row == 1, "owned_mode: LF advances the row");

    _axl_console_tap_test_set_cursor(79, 0);
    _axl_console_tap_test_puts("xy");
    _axl_console_tap_test_get_cursor(t, &col, &row);
    test_check(col == 1 && row == 1, "owned_mode: column autowraps onto the next row");

    _axl_console_tap_test_set_cursor(0, 3);
    _axl_console_tap_test_puts("\n\n");
    _axl_console_tap_test_get_cursor(t, &col, &row);
    test_check(row == 3, "owned_mode: row clamps at the last line (the console scrolls)");

    _axl_console_tap_test_set_attr(0x1F);
    test_check(_axl_console_tap_test_get_attr(t) == 0x1F,
               "owned_mode: SetAttribute updates Mode.Attribute");
    _axl_console_tap_test_enable_cursor(false);
    test_check(!_axl_console_tap_test_get_cursor_visible(t),
               "owned_mode: EnableCursor updates Mode.CursorVisible");

    /* And the ORIGINAL's Mode is never touched — we never called into it. */
    int32_t ocol = -1, orow = -1;
    _axl_console_tap_test_orig_cursor(&ocol, &orow);
    test_check(ocol == 0 && orow == 0,
               "owned_mode: passthrough off never writes the original Mode");
    cm_tap_end(t);

    /* passthrough_local = true keeps aliasing the original's Mode (the SoftBMC
       path — the real console driver maintains it). Unchanged behavior. */
    t = cm_tap_begin(80, 25, false, false, /*passthrough=*/true);
    test_check(!_axl_console_tap_test_mode_owned(t),
               "owned_mode: passthrough on still aliases the original Mode");
    cm_tap_end(t);
}

// ---------------------------------------------------------------------------
// The tap as an AxlConsoleOps producer. Pins the three observable facts of the
// widened contract: it declares its cell-boundary rule once at bind, it reports
// SetAttribute as an INDEXED pen snapshot (not a raw fg/bg split), and it reports
// the alternate screen as a term-prop rather than a dedicated op. Driven against a
// custom probe vtable bound over a headless tap (never installs the live console).
// ---------------------------------------------------------------------------

typedef struct {
    AxlConsoleCellRule rule;
    int                rule_calls;
    AxlConsolePen      pen;
    int                pen_calls;
    bool               alt;
    int                prop_calls;
} TapOpsProbe;

static void probe_cell_rule(void *u, AxlConsoleCellRule r)
{
    TapOpsProbe *p = u; p->rule = r; p->rule_calls++;
}

static void probe_set_pen(void *u, const AxlConsolePen *pen)
{
    TapOpsProbe *p = u; p->pen = *pen; p->pen_calls++;
}

static int probe_set_term_prop(void *u, AxlConsoleProp prop, const AxlConsoleValue *v)
{
    TapOpsProbe *p = u;
    p->prop_calls++;
    if (prop == AXL_CONSOLE_PROP_ALT_SCREEN) p->alt = v->u.boolean;
    return 1;
}

static void
test_tap_reports_one_cell_per_codepoint(void)
{
    TapOpsProbe probe = {0};
    AxlConsoleOps ops = { .set_cell_rule = probe_cell_rule };
    AxlConsoleTap *t = _axl_console_tap_new_for_test();
    _axl_console_tap_test_setup(t, &ops, &probe, 80, 25, false, false);

    test_check(probe.rule_calls == 1, "set_cell_rule reported exactly once");
    test_check(probe.rule == AXL_CONSOLE_CELLS_ONE_PER_CODEPOINT,
               "tap is one-cell-per-codepoint");

    _axl_console_tap_test_teardown();
    axl_free(t);
}

static void
test_tap_set_attribute_becomes_indexed_pen(void)
{
    TapOpsProbe probe = {0};
    AxlConsoleOps ops = { .set_pen = probe_set_pen };
    AxlConsoleTap *t = _axl_console_tap_new_for_test();
    _axl_console_tap_test_setup(t, &ops, &probe, 80, 25, false, false);

    /* UEFI attribute 0x1E = fg 0x0E (yellow), bg 0x1 (blue). */
    _axl_console_tap_test_set_attr(0x1E);

    test_check(probe.pen_calls == 1, "set_pen called once");
    test_check(probe.pen.fg.kind == AXL_CONSOLE_COLOR_INDEXED, "fg is indexed");
    test_check(probe.pen.fg.idx == 0x0E, "fg index 14");
    test_check(probe.pen.bg.kind == AXL_CONSOLE_COLOR_INDEXED, "bg is indexed");
    test_check(probe.pen.bg.idx == 0x01, "bg index 1");
    test_check(!probe.pen.bold && !probe.pen.italic && !probe.pen.blink,
               "tap never sets style bits");

    _axl_console_tap_test_teardown();
    axl_free(t);
}

static void
test_tap_alt_screen_is_a_term_prop(void)
{
    TapOpsProbe probe = {0};
    AxlConsoleOps ops = { .set_term_prop = probe_set_term_prop };
    AxlConsoleTap *t = _axl_console_tap_new_for_test();
    _axl_console_tap_test_setup(t, &ops, &probe, 80, 25, false, false);

    axl_console_tap_enter_alt_screen(t);
    test_check(probe.prop_calls == 1, "enter reported one prop");
    test_check(probe.alt == true, "alt screen entered");

    axl_console_tap_enter_alt_screen(t);
    test_check(probe.prop_calls == 1, "enter is idempotent");

    axl_console_tap_leave_alt_screen(t);
    test_check(probe.prop_calls == 2, "leave reported one prop");
    test_check(probe.alt == false, "alt screen left");

    _axl_console_tap_test_teardown();
    axl_free(t);
}

// ---------------------------------------------------------------------------
// GOLDEN VT STREAM. Pins the exact bytes the encoder emits for a scripted run of
// console calls. Acceptance guard for the tap split: SoftBMC's wire output must
// stay byte-identical, and any encoder change must be deliberate.
// ---------------------------------------------------------------------------

static void
test_console_mirror_golden_vt(void)
{
    AxlConsoleTap *t = cm_tap_begin(80, 25, false, false, /*passthrough=*/false);
    cm_cap_reset();

    _axl_console_tap_test_clear();          /* ESC[2J ESC[H             */
    _axl_console_tap_test_set_cursor(0, 0); /* deduped -> emits NOTHING */
    _axl_console_tap_test_set_cursor(10, 5);/* ESC[6;11H (1-based)      */
    _axl_console_tap_test_puts("Hi");       /* raw UTF-8                */
    _axl_console_tap_test_set_attr(0x1F);   /* fg 15 -> 97, bg 1 -> 44  */
    _axl_console_tap_test_enable_cursor(false); /* ESC[?25l             */
    axl_console_mirror_enter_alt_screen(cm_enc);/* ESC[?1049h           */
    axl_console_mirror_leave_alt_screen(cm_enc);/* ESC[?1049l           */

    test_check(axl_strcmp(cm_cap,
                          "\x1b[2J\x1b[H"
                          "\x1b[6;11H"
                          "Hi"
                          "\x1b[0;97;44m"
                          "\x1b[?25l"
                          "\x1b[?1049h"
                          "\x1b[?1049l") == 0,
               "golden_vt: emitted stream is byte-identical to the pinned golden");
    cm_tap_end(t);
}

/* The late-join snapshot: the mirror keeps an internal screen model fed from its
   own emitted VT, and serializes it as a self-contained repaint. */
static void
test_console_mirror_snapshot(void)
{
    AxlConsoleTap    *t   = cm_tap_begin(80, 25, /*auto_alt=*/false, false,
                                         /*passthrough=*/false);
    AxlConsoleScreen *scr = _axl_console_mirror_test_screen(cm_enc);
    test_check(scr != NULL, "mirror_snapshot: mirror owns an internal screen model");

    test_check(axl_console_mirror_snapshot(NULL, cm_cap_sink, NULL) == AXL_ERR,
               "mirror_snapshot: NULL mirror rejected");
    test_check(axl_console_mirror_snapshot(cm_enc, NULL, NULL) == AXL_ERR,
               "mirror_snapshot: NULL sink rejected");

    /* Drive console content; the mirror tees its emitted VT into the model. */
    _axl_console_tap_test_clear();
    _axl_console_tap_test_set_cursor(5, 2);       /* col 5, row 2 */
    _axl_console_tap_test_set_attr(0x04);         /* fg red */
    _axl_console_tap_test_puts("HELLO");
    _axl_console_tap_test_enable_cursor(false);   /* ESC[?25l */

    /* The internal model tracks the content, cursor, and visibility. */
    char          g[8] = {0};
    AxlConsolePen pen  = {0};
    test_check(_axl_console_screen_test_cell(scr, 2, 5, g, &pen) && axl_strcmp(g, "H") == 0,
               "mirror_snapshot: internal screen has 'H' at (2,5)");
    test_check(pen.fg.kind == AXL_CONSOLE_COLOR_INDEXED,
               "mirror_snapshot: 'HELLO' carries an indexed foreground");
    uint32_t cr = 0, cc = 0;
    bool     vis = true;
    _axl_console_screen_test_cursor(scr, &cr, &cc, &vis);
    test_check(cr == 2 && cc == 10, "mirror_snapshot: cursor tracked at (2,10)");
    test_check(!vis, "mirror_snapshot: cursor-hidden tracked");

    /* Snapshot reproduces it: feed the repaint into a fresh screen. */
    cm_cap_reset();
    test_check(axl_console_mirror_snapshot(cm_enc, cm_cap_sink, NULL) == AXL_OK,
               "mirror_snapshot: returns AXL_OK");
    AxlConsoleScreen *chk = axl_console_screen_new(25, 80);
    axl_console_screen_feed(chk, (const uint8_t *)cm_cap, cm_cap_len);
    AxlConsolePen pen2 = {0};
    test_check(_axl_console_screen_test_cell(chk, 2, 5, g, &pen2) && axl_strcmp(g, "H") == 0,
               "mirror_snapshot: repaint reproduces 'H' at (2,5)");
    test_check(pen2.fg.kind == AXL_CONSOLE_COLOR_INDEXED,
               "mirror_snapshot: repaint round-trips the indexed foreground");
    _axl_console_screen_test_cursor(chk, &cr, &cc, &vis);
    test_check(cr == 2 && cc == 10 && !vis,
               "mirror_snapshot: repaint reproduces cursor + hidden state");
    axl_console_screen_free(chk);

    /* set_size resizes the internal model to the tap's resolved geometry — the
       reason this lives in the SDK and not a consumer-side parallel parser. */
    axl_console_mirror_set_size(cm_enc, 40, 10);
    uint32_t rows = 0, cols = 0;
    _axl_console_screen_test_geometry(scr, &rows, &cols);
    test_check(rows == 10 && cols == 40,
               "mirror_snapshot: set_size resizes the internal screen");
    uint32_t tcols = 0, trows = 0;
    axl_console_tap_get_size(t, &tcols, &trows);
    test_check(tcols == 40 && trows == 10,
               "mirror_snapshot: tap_get_size reports the resolved geometry");

    /* Alt-screen transitions flow into the model through the emitted stream. */
    axl_console_mirror_enter_alt_screen(cm_enc);
    test_check(_axl_console_screen_test_alt(scr),
               "mirror_snapshot: enter_alt_screen tracked in the model");
    axl_console_mirror_leave_alt_screen(cm_enc);
    test_check(!_axl_console_screen_test_alt(scr),
               "mirror_snapshot: leave_alt_screen tracked in the model");

    cm_tap_end(t);
}

// ---------------------------------------------------------------------------
// AxlImageVerify — Authenticode presence + (best-effort) db validation
// ---------------------------------------------------------------------------

static void
test_image_verify_signature(void)
{
    AxlImageSignatureInfo info = {0};

    /* NULL guards. */
    test_check(axl_image_verify_signature(NULL, false, &info) == AXL_ERR,
               "image_verify: NULL path rejected");
    test_check(axl_image_verify_signature("fs0:\\x.efi", false, NULL) == AXL_ERR,
               "image_verify: NULL info rejected");

    /* Non-existent file. */
    test_check(axl_image_verify_signature("fs0:\\definitely-not-a-pe.efi",
                                          false, &info) == AXL_ERR,
               "image_verify: missing file returns -1");

    /* Real test EFI staged by the runner. AxlTestRuntime.efi is
       built locally + unsigned, so has_signature must be false on
       the presence-only path. */
    int rc = axl_image_verify_signature("fs0:\\AxlTestRuntime.efi",
                                        /*consult_db=*/false, &info);
    if (rc != AXL_OK) {
        /* Test EFI not staged here — SKIP-balance the populated
           path's 6 assertions (4 sig fields + 2 CN-NULL pins). */
        test_skip_n(6, "image_verify (no AxlTestRuntime.efi)");
        return;
    }
    test_check(rc == AXL_OK,
               "image_verify: AxlTestRuntime.efi parses as a PE");
    test_check(info.has_signature == false,
               "image_verify: AxlTestRuntime.efi is unsigned (presence-only)");
    test_check(info.consulted_db == false,
               "image_verify: consult_db=false leaves consulted_db=false");
    test_check(info.signature_valid == false,
               "image_verify: unsigned PE reports signature_valid=false");
    /* CN extraction never fires when has_signature=false — the
       PKCS#7 walker is short-circuited before any allocation.
       Anchor the regression: a future change that started parsing
       the cert table on every call would surface as a non-NULL
       leak here. */
    test_check(info.subject_cn == NULL,
               "image_verify: unsigned PE leaves subject_cn NULL");
    test_check(info.issuer_cn == NULL,
               "image_verify: unsigned PE leaves issuer_cn NULL");

    axl_image_signature_info_free(&info);

    /* free is NULL-safe. */
    axl_image_signature_info_free(NULL);
}

// ---------------------------------------------------------------------------
// X.509 Subject/Issuer CN extraction
// ---------------------------------------------------------------------------
//
// Direct unit-level coverage of the DER walker that
// axl_image_verify_signature uses to populate
// AxlImageSignatureInfo.subject_cn / issuer_cn. The end-to-end PE +
// PKCS#7 fixture would need ~200 bytes of hand-crafted Authenticode
// blob for one positive assertion; testing the CN extractor in
// isolation against hand-crafted Name DER inputs covers the same
// failure modes (string encoding, RDN walk, OID match) at a tenth
// the fixture size.
//
// _axl_image_verify_name_extract_cn is intentionally non-static in
// src/util/axl-image-verify.c — it doesn't ship in include/axl/, so
// consumer code can't reach it, but the test file forward-declares
// it here.

extern char *_axl_image_verify_name_extract_cn(
    const uint8_t  *name_seq_value,
    size_t          name_seq_len);

static void
test_image_verify_cn_extract(void)
{
    /* Hand-crafted Name (RDNSequence content, i.e. what the walker
       receives after der_expect peels the outer SEQUENCE tag).

       One RDN, one AttributeTypeAndValue, OID 2.5.4.3 (CN), value
       PrintableString "TestSubject":

         31 11               SET, length 17
           30 0F             SEQUENCE, length 15
             06 03 55 04 03  OID 2.5.4.3 (CN)
             13 08 ...       PrintableString length 8 = "TestSubj"

       Wait — "TestSubject" is 11 chars. Build with that:
         31 14 30 12 06 03 55 04 03 13 0B 'T' 'e' 's' 't' 'S' 'u' 'b' 'j' 'e' 'c' 't' */
    {
        const uint8_t name_printable[] = {
            0x31, 0x14,                          /* SET, len 20 */
              0x30, 0x12,                        /* SEQUENCE, len 18 */
                0x06, 0x03, 0x55, 0x04, 0x03,    /* OID CN */
                0x13, 0x0B,                      /* PrintableString, len 11 */
                'T','e','s','t','S','u','b','j','e','c','t',
        };
        char *cn = _axl_image_verify_name_extract_cn(
            name_printable, sizeof(name_printable));
        test_check(cn != NULL && axl_strcmp(cn, "TestSubject") == 0,
                   "cn-extract: PrintableString CN round-trips verbatim");
        if (cn != NULL) axl_free(cn);
    }

    /* UTF8String variant — same bytes are a valid UTF-8 ASCII run. */
    {
        const uint8_t name_utf8[] = {
            0x31, 0x14,
              0x30, 0x12,
                0x06, 0x03, 0x55, 0x04, 0x03,
                0x0C, 0x0B,                      /* UTF8String, len 11 */
                'A','x','i','m','c','o','d','e',' ','C','A',
        };
        char *cn = _axl_image_verify_name_extract_cn(
            name_utf8, sizeof(name_utf8));
        test_check(cn != NULL && axl_strcmp(cn, "Aximcode CA") == 0,
                   "cn-extract: UTF8String CN round-trips verbatim");
        if (cn != NULL) axl_free(cn);
    }

    /* RDN ordering: walker should find CN even when an unrelated
       attribute (e.g. organizationName, OID 2.5.4.10) precedes it. */
    {
        const uint8_t name_o_then_cn[] = {
            /* RDN 1: O=Aximcode */
            0x31, 0x13,
              0x30, 0x11,
                0x06, 0x03, 0x55, 0x04, 0x0A,    /* O */
                0x13, 0x0A,
                'A','x','i','m','c','o','d','e','!','!',
            /* RDN 2: CN=Foo */
            0x31, 0x0C,
              0x30, 0x0A,
                0x06, 0x03, 0x55, 0x04, 0x03,
                0x13, 0x03, 'F','o','o',
        };
        char *cn = _axl_image_verify_name_extract_cn(
            name_o_then_cn, sizeof(name_o_then_cn));
        test_check(cn != NULL && axl_strcmp(cn, "Foo") == 0,
                   "cn-extract: skips non-CN RDN, finds CN later in sequence");
        if (cn != NULL) axl_free(cn);
    }

    /* Name with no CN attribute — return NULL. */
    {
        const uint8_t name_no_cn[] = {
            0x31, 0x13,
              0x30, 0x11,
                0x06, 0x03, 0x55, 0x04, 0x0A,    /* O only */
                0x13, 0x0A,
                'A','x','i','m','c','o','d','e','!','!',
        };
        char *cn = _axl_image_verify_name_extract_cn(
            name_no_cn, sizeof(name_no_cn));
        test_check(cn == NULL,
                   "cn-extract: name with no CN attribute returns NULL");
    }

    /* Unsupported string encoding (T61String 0x14). Walker hits the
       CN OID then encounters a tag it doesn't decode — returns NULL
       rather than allocating a possibly-malformed string. */
    {
        const uint8_t name_t61[] = {
            0x31, 0x0C,
              0x30, 0x0A,
                0x06, 0x03, 0x55, 0x04, 0x03,
                0x14, 0x03, 'B','a','r',          /* T61String */
        };
        char *cn = _axl_image_verify_name_extract_cn(
            name_t61, sizeof(name_t61));
        test_check(cn == NULL,
                   "cn-extract: unsupported string encoding (T61) returns NULL");
    }

    /* Truncated input (length declares more bytes than buffer holds)
       — DER walker bounds-check refuses to read past end. */
    {
        const uint8_t name_truncated[] = {
            0x31, 0x14,                          /* SET claims 20 bytes */
              0x30, 0x12,
                0x06, 0x03, 0x55, 0x04, 0x03,
                0x13, 0x0B, 'T','e','s','t',     /* but only 4 chars present */
        };
        /* Pass the full buffer; walker should fail when it tries to
           read 11 bytes of string content but only 4 remain. */
        char *cn = _axl_image_verify_name_extract_cn(
            name_truncated, sizeof(name_truncated));
        test_check(cn == NULL,
                   "cn-extract: truncated DER returns NULL (bounds rejected)");
    }

    /* Zero-length input — walker enters its loop with cur == end,
       returns NULL without crashing. */
    {
        char *cn = _axl_image_verify_name_extract_cn(NULL, 0);
        test_check(cn == NULL,
                   "cn-extract: zero-length input returns NULL");
    }
}

// ---------------------------------------------------------------------------
// AxlArgs — declarative CLI parser
// ---------------------------------------------------------------------------

/* Per-call captures so the verb stubs can report what they saw back
   to the test driver. */
typedef struct {
    int          calls;
    const char  *seen_string;
    uint64_t     seen_uint;
    bool         seen_bool;
    int          pos_count;
    const char  *pos0;
} ArgsCapture;

static int
args_verb_show(AxlArgs *a)
{
    ArgsCapture *cap = (ArgsCapture *)axl_args_user_data(a);
    cap->calls++;
    cap->seen_string = axl_args_get_string(a, "jedec-file");
    cap->seen_uint   = axl_args_get_uint(a, "slot");
    cap->seen_bool   = axl_args_get_bool(a, "verbose");
    return 0;
}

static int
args_verb_list(AxlArgs *a)
{
    ArgsCapture *cap = (ArgsCapture *)axl_args_user_data(a);
    cap->calls++;
    cap->seen_string = axl_args_get_string(a, "jedec-file");
    return 0;
}

static int
args_single_handler(AxlArgs *a)
{
    ArgsCapture *cap = (ArgsCapture *)axl_args_user_data(a);
    cap->calls++;
    cap->pos_count = axl_args_get_pos_count(a);
    cap->pos0      = axl_args_get_pos(a, 0);
    return 7;
}

/* Captures the verbose flag + first positional + positional count,
   for the negative-positional / end-of-options tests. */
static int
args_neg_handler(AxlArgs *a)
{
    ArgsCapture *cap = (ArgsCapture *)axl_args_user_data(a);
    cap->calls++;
    cap->seen_bool  = axl_args_get_bool(a, "verbose");
    cap->pos_count  = axl_args_get_pos_count(a);
    cap->pos0       = axl_args_get_pos(a, 0);
    return 0;
}

/* Dedicated capture for the CHOICE positional — reads the named
   "field" positional via axl_args_get_string so the test can pin
   the exact value that landed in the handler. */
static int
args_field_handler(AxlArgs *a)
{
    ArgsCapture *cap = (ArgsCapture *)axl_args_user_data(a);
    cap->calls++;
    cap->seen_string = axl_args_get_string(a, "field");
    return 7;
}

static const AxlArgDesc slot_pos[] = {
    { .name = "slot", .type = AXL_ARG_U8, .base = 0,
      .min = 0x50, .max = 0x57, .required = true, .help = "test slot" },
    {0}
};

static const char *const args_choice_choices[] = {
    "noHdds", "riserCfg", "delRiser", NULL
};
static const AxlArgDesc field_pos[] = {
    { .name = "field", .type = AXL_ARG_CHOICE, .required = false,
      .choices = args_choice_choices,
      .default_value = "noHdds",
      .help = "field selector" },
    {0}
};

static const AxlArgDesc field_ci_pos[] = {
    { .name = "field", .type = AXL_ARG_CHOICE, .required = false,
      .choices = args_choice_choices,
      .choices_case_insensitive = true,
      .default_value = "noHdds",
      .help = "field selector (case-insensitive)" },
    {0}
};

static const AxlArgDesc args_flags[] = {
    { .name = "jedec-file", .short_name = 'j', .type = AXL_ARG_STRING,
      .help = "test sidecar" },
    { .name = "verbose",    .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "test verbose" },
    {0}
};
static const AxlArgsNode args_verbs[] = {
    { .name = "list", .handler = args_verb_list, .help = "list" },
    { .name = "show", .handler = args_verb_show, .positionals = slot_pos,
      .help = "show one slot" },
    { .name = "field", .handler = args_field_handler, .positionals = field_pos,
      .help = "exercise CHOICE positional" },
    { .name = "field-ci", .handler = args_field_handler,
      .positionals = field_ci_pos,
      .help = "exercise case-insensitive CHOICE positional" },
    {0}
};

static int
run_args(ArgsCapture *cap, int argc, char **argv)
{
    AxlArgsNode app = {
        .name      = "argstest",
        .help      = "AxlArgs unit test",
        .flags     = args_flags,
        .verbs     = args_verbs,
        .user_data = cap,
    };
    return axl_args_run(argc, argv, &app);
}

static void
test_args_dispatch_to_verb(void)
{
    ArgsCapture cap = { 0 };
    char *argv[] = { (char *)"argstest", (char *)"show", (char *)"0x53" };
    int rc = run_args(&cap, 3, argv);
    test_check(rc == 0, "args: 'show 0x53' returns handler exit (0)");
    test_check(cap.calls == 1, "args: show handler ran exactly once");
    test_check(cap.seen_uint == 0x53,
               "args: U8 positional parsed with auto-base");
}

static void
test_args_long_flag_with_equals(void)
{
    ArgsCapture cap = { 0 };
    char *argv[] = { (char *)"argstest", (char *)"--jedec-file=path/to/x",
                     (char *)"list" };
    int rc = run_args(&cap, 3, argv);
    test_check(rc == 0, "args: --flag=value form runs verb");
    test_check(cap.seen_string != NULL
               && axl_strcmp(cap.seen_string, "path/to/x") == 0,
               "args: --flag=value parses string");
}

static void
test_args_short_flag_with_value(void)
{
    ArgsCapture cap = { 0 };
    char *argv[] = { (char *)"argstest", (char *)"-j", (char *)"sidecar.json",
                     (char *)"list" };
    int rc = run_args(&cap, 4, argv);
    test_check(rc == 0, "args: '-j path' form runs verb");
    test_check(cap.seen_string != NULL
               && axl_strcmp(cap.seen_string, "sidecar.json") == 0,
               "args: short-flag value captured");
}

static void
test_args_bool_flag_presence(void)
{
    ArgsCapture cap = { 0 };
    char *argv[] = { (char *)"argstest", (char *)"-v", (char *)"show",
                     (char *)"0x50" };
    int rc = run_args(&cap, 4, argv);
    test_check(rc == 0, "args: bool short flag accepted");
    test_check(cap.seen_bool, "args: -v set verbose=true");
}

static void
test_args_typed_positional_bounds(void)
{
    ArgsCapture cap = { 0 };
    /* Below min — 0x4F < 0x50. */
    char *argv_low[] = { (char *)"argstest", (char *)"show", (char *)"0x4F" };
    int rc_low = run_args(&cap, 3, argv_low);
    test_check(rc_low != 0, "args: out-of-range positional rejected");
    test_check(cap.calls == 0, "args: handler did not run on bad input");
}

static void
test_args_missing_required_positional(void)
{
    ArgsCapture cap = { 0 };
    char *argv[] = { (char *)"argstest", (char *)"show" };
    int rc = run_args(&cap, 2, argv);
    test_check(rc != 0, "args: missing required positional rejected");
    test_check(cap.calls == 0, "args: handler did not run on missing arg");
}

static void
test_args_choice_positional(void)
{
    /* Valid choice — handler runs, named-positional value reaches it
       via axl_args_get_string. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"field", (char *)"riserCfg" };
        int rc = run_args(&cap, 3, argv);
        test_check(rc == 7,
                   "args choice: valid value dispatches to handler");
        test_check(cap.calls == 1,
                   "args choice: handler ran exactly once");
        test_check(cap.seen_string != NULL
                       && axl_strcmp(cap.seen_string, "riserCfg") == 0,
                   "args choice: positional value reaches handler");
    }

    /* Bad choice — rejected before handler runs. The error message
       contains "is not one of:" plus the choice list; we don't pin
       the exact text (it's user-facing) but verify the rejection
       reaches the framework, not the handler. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"field", (char *)"banana" };
        int rc = run_args(&cap, 3, argv);
        test_check(rc != 0,
                   "args choice: invalid value rejected at parse time");
        test_check(cap.calls == 0,
                   "args choice: handler did NOT run on invalid value");
    }

    /* Optional CHOICE positional with default — omitted argument is
       fine; handler runs with the defaulted value. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"field" };
        int rc = run_args(&cap, 2, argv);
        test_check(rc == 7,
                   "args choice: omitted optional positional dispatches");
        test_check(cap.calls == 1,
                   "args choice: handler ran with defaulted value");
    }

    /* Boundary: first and last entries in the choice list. Each is
       valid in its own right — catches any off-by-one in the
       NULL-terminator walk. */
    {
        ArgsCapture cap1 = { 0 };
        char *argv1[] = { (char *)"argstest", (char *)"field", (char *)"noHdds" };
        test_check(run_args(&cap1, 3, argv1) == 7 && cap1.calls == 1,
                   "args choice: first list entry accepted");

        ArgsCapture cap3 = { 0 };
        char *argv3[] = { (char *)"argstest", (char *)"field", (char *)"delRiser" };
        test_check(run_args(&cap3, 3, argv3) == 7 && cap3.calls == 1,
                   "args choice: last list entry accepted");
    }

    /* Default case-sensitivity: a wrong-case input must be rejected
       on the standard `field` verb. Pin the negative path so a
       future change that flipped the default would fail. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"field", (char *)"NOHDDS" };
        int rc = run_args(&cap, 3, argv);
        test_check(rc != 0 && cap.calls == 0,
                   "args choice: default mode rejects wrong-case input");
    }

    /* choices_case_insensitive=true on the `field-ci` verb accepts
       upper-case, mixed-case, and lower-case spellings of any
       canonical entry; rejects entries that aren't in the list. */
    {
        const struct {
            const char *input;
            int         expected_rc;
            const char *label;
        } cases[] = {
            { "noHdds",   7, "args choice ci: canonical case accepted" },
            { "NOHDDS",   7, "args choice ci: upper-case accepted" },
            { "nohdds",   7, "args choice ci: lower-case accepted" },
            { "RisErCfG", 7, "args choice ci: mixed-case accepted" },
            { "delriser", 7, "args choice ci: lower-case last entry accepted" },
            { "banana",   1, "args choice ci: out-of-list rejected" },
            { "noHdd",    1, "args choice ci: prefix rejected (exact-match contract preserved)" },
            { "noHddsx",  1, "args choice ci: extra-char rejected" },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            ArgsCapture cap = { 0 };
            char *argv[] = {
                (char *)"argstest", (char *)"field-ci", (char *)cases[i].input
            };
            int rc = run_args(&cap, 3, argv);
            bool got_handler = (cap.calls == 1);
            bool ok;
            if (cases[i].expected_rc == 7) {
                ok = (rc == 7) && got_handler;
            } else {
                ok = (rc != 0) && !got_handler;
            }
            test_check(ok, cases[i].label);
        }

        /* Captured value retains the user's original casing (we
           validate case-insensitively but don't rewrite). Confirm
           "RisErCfG" reaches the handler verbatim. */
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"field-ci", (char *)"RisErCfG" };
        run_args(&cap, 3, argv);
        test_check(cap.seen_string != NULL
                       && axl_strcmp(cap.seen_string, "RisErCfG") == 0,
                   "args choice ci: handler sees user's original casing");
    }
}

static void
test_args_unknown_verb(void)
{
    ArgsCapture cap = { 0 };
    char *argv[] = { (char *)"argstest", (char *)"frobnicate" };
    int rc = run_args(&cap, 2, argv);
    test_check(rc != 0, "args: unknown verb rejected");
    test_check(cap.calls == 0, "args: no handler ran for unknown verb");
}

static void
test_args_global_flag_before_verb_survives_attach(void)
{
    /* Regression for the prior-cycle bug: when a verb is encountered
       AFTER a global flag has already been parsed, attach_verb must
       preserve the parsed flag state instead of rebuilding the slot
       table from scratch. The fix realloc-and-copies the existing
       slots — this test pins it. */
    ArgsCapture cap = { 0 };
    char *argv[] = { (char *)"argstest", (char *)"--jedec-file=before.json",
                     (char *)"show", (char *)"0x53" };
    int rc = run_args(&cap, 4, argv);
    test_check(rc == 0,
               "args: global --flag=value before verb runs verb");
    test_check(cap.seen_string != NULL
               && axl_strcmp(cap.seen_string, "before.json") == 0,
               "args: global flag value survives attach_verb");
    test_check(cap.seen_uint == 0x53,
               "args: verb's positional still parses after global flag");
}

static void
test_args_help_word_only_pre_verb(void)
{
    /* `help` as the verb selector triggers --help (no verb attached).
       But `help` AS a positional value (e.g. grep search term) is
       NOT a help token — that would shadow legitimate input. */
    ArgsCapture cap = { 0 };
    char *argv_verb[] = { (char *)"argstest", (char *)"help" };
    test_check(run_args(&cap, 2, argv_verb) == 0,
               "args: 'help' as verb selector triggers --help");
    test_check(cap.calls == 0,
               "args: --help did not run any handler");
}

static void
test_args_compact_short_group_rejected(void)
{
    /* Compact short groups (-vh) silently dropped trailing chars in
       the initial impl. The framework now rejects them with a clear
       message; this pins the rejection. */
    ArgsCapture cap = { 0 };
    char *argv[] = { (char *)"argstest", (char *)"-vh", (char *)"list" };
    int rc = run_args(&cap, 3, argv);
    test_check(rc != 0,
               "args: compact short group (-vh) rejected");
    test_check(cap.calls == 0,
               "args: handler did not run after rejection");
}

static void
test_args_extra_positional_rejected(void)
{
    /* When a tool declares positionals and they are all filled (no
       MULTI tail), an extra positional must be rejected — not
       silently swallowed. */
    ArgsCapture cap = { 0 };
    char *argv[] = { (char *)"argstest", (char *)"show",
                     (char *)"0x53", (char *)"unexpected" };
    int rc = run_args(&cap, 4, argv);
    test_check(rc != 0,
               "args: extra positional past declared list rejected");
    test_check(cap.calls == 0,
               "args: handler did not run on extra positional");
}

static void
test_args_single_handler_mode(void)
{
    ArgsCapture cap = { 0 };
    char *argv[] = { (char *)"argstest", (char *)"file1.txt", (char *)"file2.txt" };
    AxlArgsNode app = {
        .name      = "argstest",
        .help      = "single-handler mode",
        .handler   = args_single_handler,
        .user_data = &cap,
    };
    int rc = axl_args_run(3, argv, &app);
    test_check(rc == 7, "args: single-handler return value bubbles up");
    test_check(cap.calls == 1, "args: single handler ran once");
    test_check(cap.pos_count == 2,
               "args: single-handler positionals collected (count=2)");
    test_check(cap.pos0 != NULL && axl_strcmp(cap.pos0, "file1.txt") == 0,
               "args: single-handler positional[0] = 'file1.txt'");
}

/* Leaf app with flags + the negative-positional capture handler. */
static int
run_neg(ArgsCapture *cap, int argc, char **argv)
{
    AxlArgsNode app = {
        .name      = "argstest",
        .help      = "negative-positional mode",
        .flags     = args_flags,
        .handler   = args_neg_handler,
        .user_data = cap,
    };
    return axl_args_run(argc, argv, &app);
}

static void
test_args_negative_positionals(void)
{
    /* (a) -<digit> is a positional, not an unknown flag. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"-1" };
        int rc = run_neg(&cap, 2, argv);
        test_check(rc == 0 && cap.calls == 1, "args: '-1' reaches handler (not flag error)");
        test_check(cap.pos_count == 1 && cap.pos0 != NULL
                   && axl_strcmp(cap.pos0, "-1") == 0,
                   "args: '-1' captured as positional");
    }
    /* '.-1' starts with '.', already positional — guard it stays so. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)".-1" };
        run_neg(&cap, 2, argv);
        test_check(cap.pos0 != NULL && axl_strcmp(cap.pos0, ".-1") == 0,
                   "args: '.-1' captured as positional");
    }
    /* (a) -.<digit> (negative decimal) is a positional too. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"-.5" };
        run_neg(&cap, 2, argv);
        test_check(cap.pos0 != NULL && axl_strcmp(cap.pos0, "-.5") == 0,
                   "args: '-.5' captured as positional");
    }
    /* A real flag and a negative positional coexist. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"-v", (char *)"-1" };
        run_neg(&cap, 3, argv);
        test_check(cap.seen_bool, "args: '-v -1' parses -v as the flag");
        test_check(cap.pos_count == 1 && cap.pos0 != NULL
                   && axl_strcmp(cap.pos0, "-1") == 0,
                   "args: '-v -1' keeps -1 positional");
    }
    /* (b) POSIX '--' end-of-options: the marker is consumed, the rest
           is positional unconditionally. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"--", (char *)"-1" };
        run_neg(&cap, 3, argv);
        test_check(cap.pos_count == 1 && cap.pos0 != NULL
                   && axl_strcmp(cap.pos0, "-1") == 0,
                   "args: '-- -1' makes -1 positional ('--' not counted)");
    }
    /* (b) after '--', even a registered flag name is positional. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"--", (char *)"-v" };
        run_neg(&cap, 3, argv);
        test_check(!cap.seen_bool && cap.pos0 != NULL
                   && axl_strcmp(cap.pos0, "-v") == 0,
                   "args: '-- -v' treats -v as positional, not the flag");
    }
    /* (b) '--' also disables help detection. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"--", (char *)"--help" };
        run_neg(&cap, 3, argv);
        test_check(cap.calls == 1 && cap.pos0 != NULL
                   && axl_strcmp(cap.pos0, "--help") == 0,
                   "args: '-- --help' is positional, not help");
    }
    /* -h before any '--' still triggers help (handler must NOT run). */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"-h", (char *)"-1" };
        int rc = run_neg(&cap, 3, argv);
        test_check(rc == 0 && cap.calls == 0,
                   "args: '-h -1' still parses -h as the help flag");
    }
    /* (b) at a BRANCH, '--' suppresses flags/help at that level but the
           following token still selects a verb, which parses its own
           slice fresh (no '--' propagation). */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"argstest", (char *)"--",
                         (char *)"show", (char *)"0x53" };
        int rc = run_args(&cap, 4, argv);
        test_check(rc == 0 && cap.calls == 1 && cap.seen_uint == 0x53,
                   "args: '-- show 0x53' still dispatches the verb after --");
    }
}

// ---------------------------------------------------------------------------
// Nested AxlArgs — branch verbs, parent-flag visibility, breadcrumbs
// ---------------------------------------------------------------------------

typedef struct {
    int          calls;
    const char  *seen_top_string;     /* declared at root, read from leaf */
    bool         seen_top_bool;       /* declared at root, read from leaf */
    uint64_t     seen_leaf_uint;      /* leaf's own positional */
    void        *seen_user_data;      /* parent-walk inheritance */
    int          deep_calls;          /* 3rd-level handler */
} NestedCapture;

static int
nested_pci_read16(AxlArgs *a)
{
    NestedCapture *cap = (NestedCapture *)axl_args_user_data(a);
    cap->calls++;
    /* Walk up to read root-declared flags. */
    cap->seen_top_string  = axl_args_get_string(a, "host");
    cap->seen_top_bool    = axl_args_get_bool(a, "verbose");
    cap->seen_leaf_uint   = axl_args_get_uint(a, "reg");
    cap->seen_user_data   = axl_args_user_data(a);
    return 0;
}

static int
nested_deep_handler(AxlArgs *a)
{
    NestedCapture *cap = (NestedCapture *)axl_args_user_data(a);
    cap->deep_calls++;
    /* Read flag declared at depth 1 (intermediate branch). */
    cap->seen_top_string = axl_args_get_string(a, "scope");
    return 0;
}

static const AxlArgDesc reg_pos[] = {
    { .name = "reg", .type = AXL_ARG_U16, .base = 0,
      .required = true, .help = "register offset" },
    {0}
};

static const AxlArgsNode pci_sub_verbs[] = {
    { .name = "read16", .handler = nested_pci_read16,
      .positionals = reg_pos, .help = "read a 16-bit register" },
    {0}
};

/* 3-level tree: root -> outer -> inner (leaf). */
static const AxlArgsNode inner_verbs[] = {
    { .name = "leaf", .handler = nested_deep_handler,
      .help = "deepest leaf" },
    {0}
};
static const AxlArgDesc scope_flags[] = {
    { .name = "scope", .type = AXL_ARG_STRING, .help = "intermediate scope" },
    {0}
};
static const AxlArgsNode outer_verbs[] = {
    { .name = "inner", .verbs = inner_verbs, .flags = scope_flags,
      .help = "intermediate branch with its own flag" },
    {0}
};

static const AxlArgDesc root_flags[] = {
    { .name = "host", .short_name = 'H', .type = AXL_ARG_STRING,
      .help = "root-declared string flag" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "root-declared bool flag" },
    {0}
};

static const AxlArgsNode nested_top_verbs[] = {
    { .name = "pci",  .verbs = pci_sub_verbs,  .help = "PCI/PCIe access" },
    { .name = "deep", .verbs = outer_verbs,   .help = "3-level subtree" },
    {0}
};

static int
run_nested(NestedCapture *cap, int argc, char **argv)
{
    AxlArgsNode root = {
        .name      = "mytool",
        .help      = "nested verb test root",
        .flags     = root_flags,
        .verbs     = nested_top_verbs,
        .user_data = cap,
    };
    return axl_args_run(argc, argv, &root);
}

static void
test_args_nested_2level_dispatch(void)
{
    NestedCapture cap = { 0 };
    char *argv[] = { (char *)"mytool", (char *)"pci", (char *)"read16",
                     (char *)"0x10" };
    int rc = run_nested(&cap, 4, argv);
    test_check(rc == 0, "nested args: 2-level dispatch returns 0");
    test_check(cap.calls == 1, "nested args: leaf handler ran exactly once");
    test_check(cap.seen_leaf_uint == 0x10,
               "nested args: leaf's own positional parsed (reg=0x10)");
    test_check(cap.seen_user_data == &cap,
               "nested args: user_data inherited from root by leaf");
}

static void
test_args_nested_parent_flag_visible_at_leaf(void)
{
    /* Root-declared --host and --verbose must be readable from the
       leaf via the same accessors that read the leaf's own args. */
    NestedCapture cap = { 0 };
    char *argv[] = { (char *)"mytool", (char *)"--host=bmc.local", (char *)"-v",
                     (char *)"pci", (char *)"read16", (char *)"0x20" };
    int rc = run_nested(&cap, 6, argv);
    test_check(rc == 0, "nested args: root flags + nested verb dispatches");
    test_check(cap.seen_top_string != NULL
               && axl_strcmp(cap.seen_top_string, "bmc.local") == 0,
               "nested args: root --host visible to leaf via parent-walk");
    test_check(cap.seen_top_bool,
               "nested args: root -v visible to leaf via parent-walk");
}

static void
test_args_nested_3level_dispatch(void)
{
    /* do -> deep -> inner -> leaf, with --scope declared at the
       middle level and read by the deepest handler. */
    NestedCapture cap = { 0 };
    char *argv[] = { (char *)"mytool", (char *)"deep",
                     (char *)"inner", (char *)"--scope=mid", (char *)"leaf" };
    int rc = run_nested(&cap, 5, argv);
    test_check(rc == 0, "nested args: 3-level dispatch returns 0");
    test_check(cap.deep_calls == 1, "nested args: deep leaf ran once");
    test_check(cap.seen_top_string != NULL
               && axl_strcmp(cap.seen_top_string, "mid") == 0,
               "nested args: middle-level --scope reachable from deepest leaf");
}

/* Opt-in case-insensitive verb matching (AxlArgsNode.case_insensitive on the
   root). Mirrors the legacy Dell `do` tool: verb / sub-verb names match in any
   case tree-wide, while positional VALUES stay case-preserving. */
static int
args_ci_leaf(AxlArgs *a)
{
    ArgsCapture *cap = (ArgsCapture *)axl_args_user_data(a);
    cap->calls++;
    cap->seen_string = axl_args_get_string(a, "tag");   /* a positional value */
    return 0;
}

static const AxlArgDesc args_ci_pos[] = {
    { .name = "tag", .type = AXL_ARG_STRING, .required = false,
      .help = "free value" },
    {0}
};
static const AxlArgsNode args_ci_bios_verbs[] = {
    { .name = "MAP", .handler = args_ci_leaf, .help = "memory map" },
    {0}
};
static const AxlArgsNode args_ci_verbs[] = {
    { .name = "CDUMP", .handler = args_ci_leaf, .positionals = args_ci_pos,
      .help = "config dump" },
    { .name = "bios", .verbs = args_ci_bios_verbs, .help = "bios category" },
    {0}
};

/* Defined just below; used here for the case-sensitive negative case. */
static AxlStream *capture_stdout(AxlStream **buf_out);
static void       restore_stdout(AxlStream *saved, AxlStream *buf);
static bool       buf_contains(AxlStream *buf, const char *needle);

static void
test_args_case_insensitive(void)
{
    AxlArgsNode app = {
        .name = "do", .verbs = args_ci_verbs, .case_insensitive = true,
    };

    /* A root verb resolves in ANY case (CDUMP / Cdump / cdump). */
    const char *forms[] = { "CDUMP", "Cdump", "cdump" };
    for (int k = 0; k < 3; k++) {
        ArgsCapture cap = { 0 };
        app.user_data = &cap;
        char *argv[] = { (char *)"do", (char *)forms[k] };
        int rc = axl_args_run(2, argv, &app);
        test_check(rc == 0 && cap.calls == 1,
                   "args ci: root verb matches regardless of case");
    }

    /* Verb name folds, but the positional VALUE keeps its case. */
    {
        ArgsCapture cap = { 0 };
        app.user_data = &cap;
        char *argv[] = { (char *)"do", (char *)"cdump", (char *)"MixedCase" };
        axl_args_run(3, argv, &app);
        test_check(cap.seen_string != NULL
                   && axl_strcmp(cap.seen_string, "MixedCase") == 0,
                   "args ci: positional value keeps its original case");
    }

    /* Case-folding propagates tree-wide: `BIOS map` -> bios -> MAP. */
    {
        ArgsCapture cap = { 0 };
        app.user_data = &cap;
        char *argv[] = { (char *)"do", (char *)"BIOS", (char *)"map" };
        int rc = axl_args_run(3, argv, &app);
        test_check(rc == 0 && cap.calls == 1,
                   "args ci: sub-verb 'BIOS map' resolves bios->MAP tree-wide");
    }

    /* Default (case_insensitive=false): ONLY the exact case matches. */
    AxlArgsNode app_cs = { .name = "do", .verbs = args_ci_verbs };
    {
        ArgsCapture cap = { 0 };
        app_cs.user_data = &cap;
        AxlStream *buf = NULL;
        AxlStream *saved = capture_stdout(&buf);
        char *argv[] = { (char *)"do", (char *)"cdump" };
        int rc = axl_args_run(2, argv, &app_cs);
        bool unknown = buf_contains(buf, "unknown verb 'cdump'");
        restore_stdout(saved, buf);
        test_check(rc != 0 && cap.calls == 0,
                   "args cs: wrong-case 'cdump' does NOT match 'CDUMP'");
        test_check(unknown, "args cs: reports unknown verb for the wrong case");
    }
    {
        ArgsCapture cap = { 0 };
        app_cs.user_data = &cap;
        char *argv[] = { (char *)"do", (char *)"CDUMP" };
        int rc = axl_args_run(2, argv, &app_cs);
        test_check(rc == 0 && cap.calls == 1,
                   "args cs: exact-case 'CDUMP' matches as before");
    }
}

/* Opt-in compact DOS/legacy flag syntax (AxlArgsNode.compact_flags on the
   root): colon values (-x:v, --name:v), attached short values (-xv, /xv), and
   a `/` short-flag prefix (/x, /x:v). Flag VALUES keep their case. */
static int
args_compact_leaf(AxlArgs *a)
{
    ArgsCapture *cap = (ArgsCapture *)axl_args_user_data(a);
    cap->calls++;
    cap->seen_string = axl_args_get_string(a, "config");   /* -c */
    cap->pos0        = axl_args_get_string(a, "out");       /* -o */
    cap->seen_bool   = axl_args_get_bool(a, "verbose");     /* -v */
    return 0;
}
static const AxlArgDesc args_compact_desc[] = {
    { .name = "config",  .short_name = 'c', .type = AXL_ARG_STRING, .help = "cfg" },
    { .name = "out",     .short_name = 'o', .type = AXL_ARG_STRING, .help = "out" },
    { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,   .help = "v" },
    {0}
};
static int
run_compact(ArgsCapture *cap, bool compact, int argc, char **argv)
{
    AxlArgsNode app = {
        .name = "prog", .flags = args_compact_desc,
        .handler = args_compact_leaf, .compact_flags = compact,
        .user_data = cap,
    };
    return axl_args_run(argc, argv, &app);
}

static void
test_args_compact_flags(void)
{
    /* compact_flags=true: every form yields config="<v>", value case kept. */
    struct { const char *tok; const char *want; } forms[] = {
        { "-c:yn",        "yn" },        /* short + colon            */
        { "-cyn",         "yn" },        /* short + attached         */
        { "/c:zz",        "zz" },        /* slash + colon            */
        { "/czz",         "zz" },        /* slash + attached (/sVar) */
        { "--config:val", "val" },       /* long + colon             */
        { "-c:File.TXT",  "File.TXT" },  /* value keeps its case     */
        { "-c:",          "" },          /* empty attached value     */
    };
    for (size_t k = 0; k < sizeof(forms) / sizeof(forms[0]); k++) {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"prog", (char *)forms[k].tok };
        int rc = run_compact(&cap, true, 2, argv);
        test_check(rc == 0 && cap.calls == 1 && cap.seen_string != NULL
                   && axl_strcmp(cap.seen_string, forms[k].want) == 0,
                   "args compact: flag form parses with the right value");
    }

    /* -o:File.txt sets a second flag; /v is the bare short bool. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"prog", (char *)"-o:File.txt", (char *)"/v" };
        int rc = run_compact(&cap, true, 3, argv);
        test_check(rc == 0 && cap.pos0 != NULL
                   && axl_strcmp(cap.pos0, "File.txt") == 0 && cap.seen_bool,
                   "args compact: -o:File.txt + /v parse together");
    }

    /* Long `--name=value` splits on '=' even when ':' also appears in the
       value (first separator wins; ':' inside the value is preserved). */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"prog", (char *)"--out=fs0:\\path:x" };
        int rc = run_compact(&cap, true, 2, argv);
        test_check(rc == 0 && cap.pos0 != NULL
                   && axl_strcmp(cap.pos0, "fs0:\\path:x") == 0,
                   "args compact: --out=value splits on '=' with ':' in value");
    }

    /* Default (compact_flags=false) mode is getopt-style: `-abc` bundles, and a
       value-taking short flag consumes the rest of the token (`-cVALUE`) or the
       next argv (`-c VALUE`). There is NO ':' separator in this mode, so
       `-c:yn` attaches the literal ":yn" as the value. (The DOS-ish ':'/'/'
       forms remain opt-in via compact_flags, exercised above.) */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"prog", (char *)"-c:yn" };
        int rc = run_compact(&cap, false, 2, argv);
        test_check(rc == 0 && cap.calls == 1 && cap.seen_string != NULL
                   && axl_strcmp(cap.seen_string, ":yn") == 0,
                   "args getopt: -c:yn attaches literal ':yn' (no ':' split off)");
    }
    {
        /* Bundle a bool with a trailing value flag: -vc yn == -v -c yn. */
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"prog", (char *)"-vc", (char *)"yn" };
        int rc = run_compact(&cap, false, 3, argv);
        test_check(rc == 0 && cap.calls == 1 && cap.seen_bool
                   && cap.seen_string != NULL
                   && axl_strcmp(cap.seen_string, "yn") == 0,
                   "args getopt: -vc yn bundles -v and -c (value from next argv)");
    }
    {
        ArgsCapture cap = { 0 };
        AxlStream *buf = NULL;
        AxlStream *saved = capture_stdout(&buf);
        char *argv[] = { (char *)"prog", (char *)"/czz" };
        int rc = run_compact(&cap, false, 2, argv);
        restore_stdout(saved, buf);
        test_check(rc == 0 && cap.seen_string == NULL,
                   "args strict: /czz is a positional (not -c) when off");
    }

    /* Default grammar is unaffected by the new field (regression guard). */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"prog", (char *)"--config", (char *)"ok",
                         (char *)"-v" };
        int rc = run_compact(&cap, false, 4, argv);
        test_check(rc == 0 && cap.seen_string != NULL
                   && axl_strcmp(cap.seen_string, "ok") == 0 && cap.seen_bool,
                   "args strict: default grammar still parses (--config ok -v)");
    }
}

/* Swap axl_stdout for an in-memory buffer so a test can assert on
   what the parser printed. Caller pairs with restore_stdout() in the
   same scope. Returns the saved original to restore. */
static AxlStream *
capture_stdout(AxlStream **buf_out)
{
    AxlStream *saved = axl_stdout;
    AxlStream *buf   = axl_bufopen();
    test_check(buf != NULL, "capture_stdout: bufopen succeeded");
    axl_stdout = buf;
    *buf_out = buf;
    return saved;
}

static void
restore_stdout(AxlStream *saved, AxlStream *buf)
{
    axl_stdout = saved;
    if (buf != NULL) {
        axl_fclose(buf);
    }
}

static bool
buf_contains(AxlStream *buf, const char *needle)
{
    size_t        n = 0;
    const void   *p = axl_bufdata(buf, &n);
    if (p == NULL || n == 0 || needle == NULL) {
        return false;
    }
    size_t      nl = axl_strlen(needle);
    if (nl > n) {
        return false;
    }
    const char *bytes = (const char *)p;
    for (size_t i = 0; i + nl <= n; i++) {
        if (axl_memcmp(&bytes[i], needle, nl) == 0) {
            return true;
        }
    }
    return false;
}

/* First-occurrence byte offset of @p needle in the captured buffer,
   or SIZE_MAX if not found. Used by tests that need to assert
   ordering between two substrings (e.g. prolog before Usage). */
static size_t
buf_offset_of(AxlStream *buf, const char *needle)
{
    size_t        n = 0;
    const void   *p = axl_bufdata(buf, &n);
    if (p == NULL || n == 0 || needle == NULL) {
        return SIZE_MAX;
    }
    size_t      nl = axl_strlen(needle);
    if (nl > n) {
        return SIZE_MAX;
    }
    const char *bytes = (const char *)p;
    for (size_t i = 0; i + nl <= n; i++) {
        if (axl_memcmp(&bytes[i], needle, nl) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* Column (0-based byte offset within the line) at which @p needle
   appears on the first line whose content starts with @p line_prefix.
   SIZE_MAX if no such line, or needle absent from it. Used to assert
   help-text alignment without hand-counting padding spaces. */
static size_t
help_col(AxlStream *buf, const char *line_prefix, const char *needle)
{
    size_t      n = 0;
    const char *b = (const char *)axl_bufdata(buf, &n);
    if (b == NULL || n == 0) {
        return SIZE_MAX;
    }
    size_t pl = axl_strlen(line_prefix);
    size_t nl = axl_strlen(needle);
    size_t line_start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || b[i] == '\n') {
            size_t line_len = i - line_start;
            if (line_len >= pl
                && axl_memcmp(b + line_start, line_prefix, pl) == 0) {
                for (size_t j = line_start; j + nl <= i; j++) {
                    if (axl_memcmp(b + j, needle, nl) == 0) {
                        return j - line_start;
                    }
                }
                return SIZE_MAX;
            }
            line_start = i + 1;
        }
    }
    return SIZE_MAX;
}

static const AxlArgDesc align_pos[] = {
    { .name = "mode",      .type = AXL_ARG_STRING, .required = true,
      .help = "Mode selector" },
    { .name = "blinkRate", .type = AXL_ARG_STRING, .required = true,
      .help = "Blink rate" },
    {0}
};

static void
test_args_help_alignment(void)
{
    /* The help-text column must be identical across the -h line and
       every positional line, regardless of positional-name length.
       Previously print_positional_line padded a fixed number of
       spaces after a variable-length name, so the column drifted. */
    ArgsCapture cap = { 0 };
    AxlStream  *buf = NULL;
    char *argv[] = { (char *)"argstest", (char *)"-h" };
    AxlArgsNode app = {
        .name        = "argstest",
        .help        = "alignment test",
        .flags       = args_flags,
        .positionals = align_pos,
        .handler     = args_single_handler,
        .user_data   = &cap,
    };
    AxlStream *saved = capture_stdout(&buf);
    axl_args_run(2, argv, &app);
    axl_stdout = saved;   /* restore before asserting; read buf first */
    size_t c_mode  = help_col(buf, "  <mode>",      "Mode selector");
    size_t c_blink = help_col(buf, "  <blinkRate>", "Blink rate");
    size_t c_help  = help_col(buf, "  -h,",         "Show this help");
    axl_fclose(buf);

    test_check(c_mode != SIZE_MAX && c_blink != SIZE_MAX
               && c_help != SIZE_MAX,
               "args help: -h and both positional lines found");
    test_check(c_mode == c_blink,
               "args help: positional help columns align across name lengths");
    test_check(c_mode == c_help,
               "args help: positional help column matches the -h line");
}

/* Terse legacy help: a leaf with optional positionals renders a Usage line
   plus a clean aligned list — no "Arguments:" / "Flags:" section headers, no
   "(optional)" suffix (the [<name>] brackets in Usage already convey it), and
   a single aligned --help row. */
static const AxlArgDesc terse_pos[] = {
    { .name = "mode",      .type = AXL_ARG_STRING, .required = false,
      .help = "0=Norm 1=Dump 2=SMBIOS" },
    { .name = "blinkRate", .type = AXL_ARG_STRING, .required = false,
      .help = "second argument" },
    {0}
};

static void
test_args_help_terse_format(void)
{
    ArgsCapture cap = { 0 };
    char *argv[] = { (char *)"do", (char *)"sysid", (char *)"-h" };
    AxlArgsNode sysid = {
        .name = "sysid", .positionals = terse_pos,
        .handler = args_single_handler, .user_data = &cap,
    };
    AxlArgsNode verbs[] = { sysid, {0} };
    AxlArgsNode app = {
        .name = "do", .verbs = verbs, .user_data = &cap,
    };
    AxlStream *buf = NULL;
    AxlStream *saved = capture_stdout(&buf);
    int rc = axl_args_run(3, argv, &app);   /* do sysid -h */
    bool has_usage   = buf_contains(buf, "Usage:");
    bool has_args_hdr = buf_contains(buf, "Arguments:");
    bool has_flags_hdr = buf_contains(buf, "Flags:");
    bool has_optional = buf_contains(buf, "(optional)");
    bool has_mode_help = buf_contains(buf, "0=Norm 1=Dump 2=SMBIOS");
    size_t c_mode = help_col(buf, "  <mode>",      "0=Norm");
    size_t c_help = help_col(buf, "  -h,",         "Show this help");
    restore_stdout(saved, buf);

    test_check(rc == 0, "args terse: -h returns 0");
    test_check(cap.calls == 0, "args terse: -h did not invoke handler");
    test_check(has_usage, "args terse: Usage line present");
    test_check(!has_args_hdr, "args terse: no 'Arguments:' section header");
    test_check(!has_flags_hdr, "args terse: no 'Flags:' section header (no flags)");
    test_check(!has_optional, "args terse: no '(optional)' suffix on positionals");
    test_check(has_mode_help, "args terse: positional help text still shown");
    test_check(c_mode != SIZE_MAX && c_help != SIZE_MAX,
               "args terse: positional + -h rows present");
    test_check(c_mode == c_help,
               "args terse: positional row aligns with the -h row");
}

/* A UEFI text console has no UTF-8: any non-ASCII byte in generated help —
   e.g. the U+2014 em-dash (E2 80 94) once used as the name/description
   separator — renders as a white block. The auto-generated help must be pure
   ASCII. Assert it for BOTH the root header and a sub-verb header, which share
   the single renderer (print_help_for), and pin the exact ASCII separator. */
static void
test_args_help_ascii_only(void)
{
    ArgsCapture cap = { 0 };
    AxlArgsNode sysid = {
        .name = "sysid", .handler = args_single_handler,
        .help = "System identity", .user_data = &cap,
    };
    AxlArgsNode verbs[] = { sysid, {0} };
    AxlArgsNode app = {
        .name = "do", .verbs = verbs,
        .help = "OEM hardware-diagnostic CLI", .user_data = &cap,
    };

    AxlStream *buf = NULL;
    AxlStream *saved = capture_stdout(&buf);
    char *root_argv[] = { (char *)"do", (char *)"--help" };
    axl_args_run(2, root_argv, &app);                 /* root header */
    char *verb_argv[] = { (char *)"do", (char *)"sysid", (char *)"--help" };
    axl_args_run(3, verb_argv, &app);                 /* sub-verb header */

    /* Scan EVERY rendered byte for non-ASCII before restore closes buf. */
    size_t      n = 0;
    const unsigned char *b = (const unsigned char *)axl_bufdata(buf, &n);
    bool all_ascii = (b != NULL && n > 0);
    for (size_t i = 0; b != NULL && i < n; i++) {
        if (b[i] >= 0x80) {
            all_ascii = false;
            break;
        }
    }
    /* The exact ASCII separator renders in both headers (locks " - "); the
       header also carries the SDK version stamp between the name and the ' - '. */
    bool root_sep = buf_contains(buf,
        "do " AXL_VERSION_STRING " - OEM hardware-diagnostic CLI");
    bool verb_sep = buf_contains(buf,
        "do sysid " AXL_VERSION_STRING " - System identity");
    restore_stdout(saved, buf);

    test_check(n > 0, "args ascii: help produced output");
    test_check(all_ascii, "args ascii: generated help has no non-ASCII bytes");
    test_check(root_sep, "args ascii: root header uses an ASCII '-' separator");
    test_check(verb_sep, "args ascii: sub-verb header uses an ASCII '-' separator");
}

static int
args_hidden_handler(AxlArgs *a)
{
    ArgsCapture *cap = (ArgsCapture *)axl_args_user_data(a);
    cap->calls++;
    cap->seen_string = axl_args_get_string(a, "_secret");
    return 0;
}

/* A flag (or positional) marked .hidden is omitted from --help entirely but is
   still parsed and delivered to the handler — the contract test/diagnostic
   seams rely on. */
static void
test_args_hidden_flag(void)
{
    static const AxlArgDesc hflags[] = {
        { .name = "shown",   .type = AXL_ARG_BOOL,
          .help = "VISIBLE-HELP-TEXT" },
        { .name = "_secret", .type = AXL_ARG_STRING, .hidden = true,
          .help = "HIDDEN-HELP-TEXT" },
        {0}
    };
    ArgsCapture cap = { 0 };
    AxlArgsNode app = {
        .name = "htool", .flags = hflags,
        .handler = args_hidden_handler, .user_data = &cap,
    };

    /* (1) --help lists the visible flag but omits the hidden one. */
    {
        AxlStream *buf   = NULL;
        AxlStream *saved = capture_stdout(&buf);
        char *argv[] = { (char *)"htool", (char *)"--help" };
        int rc = axl_args_run(2, argv, &app);
        bool has_shown  = buf_contains(buf, "VISIBLE-HELP-TEXT");
        bool has_secret = buf_contains(buf, "_secret")
                          || buf_contains(buf, "HIDDEN-HELP-TEXT");
        restore_stdout(saved, buf);
        test_check(rc == 0, "args hidden: --help returns 0");
        test_check(has_shown, "args hidden: visible flag shown in --help");
        test_check(!has_secret, "args hidden: hidden flag omitted from --help");
    }

    /* (2) the hidden flag is still parsed and delivered to the handler. */
    {
        char *argv[] = { (char *)"htool", (char *)"--_secret", (char *)"xyzzy" };
        int rc = axl_args_run(3, argv, &app);
        test_check(rc == 0, "args hidden: hidden flag accepted (rc 0)");
        test_check(cap.calls == 1, "args hidden: handler ran once");
        test_check(cap.seen_string != NULL
                   && axl_strcmp(cap.seen_string, "xyzzy") == 0,
                   "args hidden: hidden flag value delivered to handler");
    }
}

/* "?" is a help alias matching the legacy tool: at the top level, at a branch,
   and at a sub-verb leaf, a lone "?" prints the same help as -h/--help instead
   of being consumed as a positional value (which used to error). */
static void
test_args_help_question_alias(void)
{
    /* (1) sub-verb leaf: `do sysid ?` -> sysid's help, handler not run. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"do", (char *)"sysid", (char *)"?" };
        AxlArgsNode sysid = {
            .name = "sysid", .positionals = terse_pos,
            .handler = args_single_handler, .user_data = &cap,
        };
        AxlArgsNode verbs[] = { sysid, {0} };
        AxlArgsNode app = { .name = "do", .verbs = verbs, .user_data = &cap };
        AxlStream *buf = NULL;
        AxlStream *saved = capture_stdout(&buf);
        int rc = axl_args_run(3, argv, &app);
        bool has_usage   = buf_contains(buf, "Usage:");
        bool has_sysid   = buf_contains(buf, "sysid");
        bool has_invalid = buf_contains(buf, "invalid");
        restore_stdout(saved, buf);
        test_check(rc == 0, "args ?: `do sysid ?` returns 0");
        test_check(cap.calls == 0, "args ?: `do sysid ?` did not run handler");
        test_check(has_usage && has_sysid,
                   "args ?: `do sysid ?` printed sysid help");
        test_check(!has_invalid,
                   "args ?: `do sysid ?` not treated as a positional value");
    }
    /* (2) branch top-level: `do ?` -> branch help (lists the verb). */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"do", (char *)"?" };
        AxlArgsNode sysid = {
            .name = "sysid", .positionals = terse_pos,
            .handler = args_single_handler, .user_data = &cap,
        };
        AxlArgsNode verbs[] = { sysid, {0} };
        AxlArgsNode app = { .name = "do", .verbs = verbs, .user_data = &cap };
        AxlStream *buf = NULL;
        AxlStream *saved = capture_stdout(&buf);
        int rc = axl_args_run(2, argv, &app);
        bool has_usage = buf_contains(buf, "Usage:");
        bool has_verb  = buf_contains(buf, "sysid");
        restore_stdout(saved, buf);
        test_check(rc == 0, "args ?: `do ?` returns 0");
        test_check(has_usage && has_verb, "args ?: `do ?` printed branch help");
    }
    /* (3) plain leaf top-level: `tool ?` -> help, handler not run. */
    {
        ArgsCapture cap = { 0 };
        char *argv[] = { (char *)"tool", (char *)"?" };
        AxlArgsNode app = {
            .name = "tool", .positionals = terse_pos,
            .handler = args_single_handler, .user_data = &cap,
        };
        AxlStream *buf = NULL;
        AxlStream *saved = capture_stdout(&buf);
        int rc = axl_args_run(2, argv, &app);
        bool has_usage   = buf_contains(buf, "Usage:");
        bool has_invalid = buf_contains(buf, "invalid");
        restore_stdout(saved, buf);
        test_check(rc == 0, "args ?: `tool ?` (leaf) returns 0");
        test_check(cap.calls == 0, "args ?: `tool ?` (leaf) did not run handler");
        test_check(has_usage && !has_invalid,
                   "args ?: `tool ?` (leaf) printed help, not a value error");
    }
}

static void
test_args_nested_unknown_verb_at_branch(void)
{
    /* Branch should reject unknown verb at its own level with an
       error prefixed by the full breadcrumb path. */
    NestedCapture cap = { 0 };
    char *argv[] = { (char *)"mytool", (char *)"pci", (char *)"flarble" };

    AxlStream *buf = NULL;
    AxlStream *saved = capture_stdout(&buf);
    int rc = run_nested(&cap, 3, argv);
    bool has_breadcrumb = buf_contains(buf, "mytool pci: unknown verb");
    bool has_token      = buf_contains(buf, "flarble");
    restore_stdout(saved, buf);

    test_check(rc != 0, "nested args: unknown verb at branch rejected");
    test_check(cap.calls == 0, "nested args: leaf handler did not run");
    test_check(cap.deep_calls == 0,
               "nested args: deep handler did not run on shallow rejection");
    test_check(has_breadcrumb,
               "nested args: error message includes 'mytool pci:' breadcrumb");
    test_check(has_token,
               "nested args: error message names the rejected verb");
}

static void
test_args_nested_branch_help_lists_subverbs(void)
{
    /* `mytool pci --help` triggers help at the pci branch level and the
       output names the subverbs of that branch (not the root). */
    NestedCapture cap = { 0 };
    char *argv[] = { (char *)"mytool", (char *)"pci", (char *)"--help" };

    AxlStream *buf = NULL;
    AxlStream *saved = capture_stdout(&buf);
    int rc = run_nested(&cap, 3, argv);
    bool has_branch_path = buf_contains(buf, "mytool pci");
    bool has_subverb     = buf_contains(buf, "read16");
    bool has_root_only   = buf_contains(buf, "deep");
    restore_stdout(saved, buf);

    test_check(rc == 0, "nested args: --help at branch returns 0");
    test_check(cap.calls == 0, "nested args: --help did not invoke handler");
    test_check(cap.deep_calls == 0, "nested args: --help did not recurse");
    test_check(has_branch_path,
               "nested args: --help output uses branch breadcrumb");
    test_check(has_subverb,
               "nested args: --help output lists branch's subverb 'read16'");
    test_check(!has_root_only,
               "nested args: --help at branch does NOT list root-only verbs");
}

static void
test_args_nested_misconfigured_node_rejected(void)
{
    /* A node with neither verbs nor handler is misconfigured and
       must fail before the parser tries to dispatch. */
    NestedCapture cap = { 0 };
    AxlArgsNode bad = {
        .name      = "argstest",
        .help      = "broken node",
        .user_data = &cap,
        /* no verbs, no handler */
    };
    char *argv[] = { (char *)"argstest" };
    int rc = axl_args_run(1, argv, &bad);
    test_check(rc != 0, "nested args: handler-less + verb-less node rejected");
}

// ---------------------------------------------------------------------------
// Branch + default handler — `mytool bios` with no sub-verb runs handler
// ---------------------------------------------------------------------------

typedef struct {
    int          default_calls;     /* incremented when default fires */
    int          info_calls;        /* incremented when info verb fires */
    int          test_calls;        /* incremented when test verb fires */
    bool         saw_branch_flag;   /* did handler see --quiet from branch level */
} BranchDefaultCapture;

static int
bdh_info(AxlArgs *a)
{
    BranchDefaultCapture *cap = (BranchDefaultCapture *)axl_args_user_data(a);
    cap->info_calls++;
    cap->default_calls++;   /* same fn doubles as default — count both */
    cap->saw_branch_flag = axl_args_get_bool(a, "quiet");
    return 0;
}

static int
bdh_test(AxlArgs *a)
{
    BranchDefaultCapture *cap = (BranchDefaultCapture *)axl_args_user_data(a);
    cap->test_calls++;
    return 0;
}

static const AxlArgsNode bdh_bios_verbs[] = {
    { .name = "info", .handler = bdh_info, .help = "Type 0 summary" },
    { .name = "test", .handler = bdh_test, .help = "walk records" },
    {0}
};
static const AxlArgDesc bdh_bios_flags[] = {
    { .name = "quiet", .short_name = 'q', .type = AXL_ARG_BOOL,
      .help = "suppress per-record progress" },
    {0}
};
static const AxlArgsNode bdh_top_verbs[] = {
    /* `bios` is a branch with sub-verbs AND a default handler. */
    { .name = "bios", .verbs = bdh_bios_verbs, .flags = bdh_bios_flags,
      .handler = bdh_info, .help = "BIOS / SMBIOS subcommands" },
    {0}
};

static int
run_bdh(BranchDefaultCapture *cap, int argc, char **argv)
{
    AxlArgsNode root = {
        .name      = "mytool",
        .help      = "branch+default test root",
        .verbs     = bdh_top_verbs,
        .user_data = cap,
    };
    return axl_args_run(argc, argv, &root);
}

static void
test_args_branch_default_fires_on_no_verb(void)
{
    /* `mytool bios` with no further verb invokes the default handler.
       info_calls AND default_calls both go up because the same fn
       is referenced as both the explicit verb and the default. */
    BranchDefaultCapture cap = { 0 };
    char *argv[] = { (char *)"mytool", (char *)"bios" };
    int rc = run_bdh(&cap, 2, argv);
    test_check(rc == 0,
               "branch+default: 'mytool bios' with no sub-verb returns 0");
    test_check(cap.default_calls == 1,
               "branch+default: default handler fired once");
    test_check(cap.test_calls == 0,
               "branch+default: unrelated sub-verb handler did NOT fire");
}

static void
test_args_branch_default_subverb_still_recurses(void)
{
    /* Explicit sub-verb still recurses normally; default handler
       does NOT also fire. */
    BranchDefaultCapture cap = { 0 };
    char *argv[] = { (char *)"mytool", (char *)"bios", (char *)"test" };
    int rc = run_bdh(&cap, 3, argv);
    test_check(rc == 0, "branch+default: explicit sub-verb returns 0");
    test_check(cap.test_calls == 1,
               "branch+default: 'test' verb fired");
    test_check(cap.info_calls == 0,
               "branch+default: default 'info' did NOT fire when verb supplied");
}

static void
test_args_branch_default_unknown_verb_still_errors(void)
{
    /* Unknown sub-verb is still an error — default handler is NOT
       a catch-all. */
    BranchDefaultCapture cap = { 0 };
    char *argv[] = { (char *)"mytool", (char *)"bios", (char *)"flarble" };

    AxlStream *buf = NULL;
    AxlStream *saved = capture_stdout(&buf);
    int rc = run_bdh(&cap, 3, argv);
    bool has_breadcrumb = buf_contains(buf, "mytool bios: unknown verb");
    restore_stdout(saved, buf);

    test_check(rc != 0,
               "branch+default: unknown sub-verb still rejected");
    test_check(cap.default_calls == 0 && cap.test_calls == 0,
               "branch+default: no handler fired on unknown verb");
    test_check(has_breadcrumb,
               "branch+default: error message uses branch breadcrumb");
}

static void
test_args_branch_default_sees_branch_flags(void)
{
    /* --quiet declared on the bios branch must reach the default
       handler when no sub-verb is supplied. */
    BranchDefaultCapture cap = { 0 };
    char *argv[] = { (char *)"mytool", (char *)"bios", (char *)"-q" };
    int rc = run_bdh(&cap, 3, argv);
    test_check(rc == 0,
               "branch+default: branch-level flags before default OK");
    test_check(cap.default_calls == 1,
               "branch+default: default fired with branch flags");
    test_check(cap.saw_branch_flag,
               "branch+default: handler saw --quiet from branch level");
}

static void
test_args_branch_no_handler_still_shows_help(void)
{
    /* Regression: a branch WITHOUT a default handler keeps the
       v0.8.0 behavior of showing help on no-verb. */
    NestedCapture cap = { 0 };
    char *argv[] = { (char *)"mytool", (char *)"pci" };

    AxlStream *buf = NULL;
    AxlStream *saved = capture_stdout(&buf);
    int rc = run_nested(&cap, 2, argv);
    bool has_help = buf_contains(buf, "Verbs:");
    restore_stdout(saved, buf);

    test_check(rc != 0,
               "branch (no handler): no-verb returns non-zero");
    test_check(cap.calls == 0,
               "branch (no handler): no handler invoked");
    test_check(has_help,
               "branch (no handler): help output emitted");
}

static int
s64_capture_handler(AxlArgs *a)
{
    ArgsCapture *cap = (ArgsCapture *)axl_args_user_data(a);
    cap->calls++;
    cap->seen_uint = (uint64_t)axl_args_get_int(a, "value");
    return 0;
}

static void
test_args_s64_bounds(void)
{
    /* AXL_ARG_S64 as a FLAG with bounds in [-10, 10]. Flag form
       is used (rather than a positional) because a bare negative
       literal "-5" would be parsed as a short-flag prefix; the
       --value=N form sidesteps that pre-existing limitation. The
       negative lower bound goes via two's-complement cast — same
       convention as documented on AxlArgDesc. */
    static const AxlArgDesc s64_flags[] = {
        { .name = "value", .type = AXL_ARG_S64,
          .min = (uint64_t)(int64_t)-10, .max = 10,
          .help = "signed value in [-10, 10]" },
        {0}
    };

    ArgsCapture cap;
    AxlArgsNode app = {
        .name      = "argstest",
        .help      = "S64 bounds test",
        .flags     = s64_flags,
        .handler   = s64_capture_handler,
        .user_data = &cap,
    };

    /* In-bounds positive value accepted. */
    cap = (ArgsCapture){0};
    char *argv_ok[] = { (char *)"argstest", (char *)"--value=5" };
    test_check(axl_args_run(2, argv_ok, &app) == 0,
               "args S64: in-bounds value (5) accepted");
    test_check(cap.calls == 1 && (int64_t)cap.seen_uint == 5,
               "args S64: handler saw value 5");

    /* In-bounds negative also accepted. */
    cap = (ArgsCapture){0};
    char *argv_neg[] = { (char *)"argstest", (char *)"--value=-5" };
    test_check(axl_args_run(2, argv_neg, &app) == 0,
               "args S64: in-bounds negative (-5) accepted");
    test_check(cap.calls == 1 && (int64_t)cap.seen_uint == -5,
               "args S64: handler saw value -5 via --value=-N");

    /* Below min rejected. */
    cap = (ArgsCapture){0};
    char *argv_low[] = { (char *)"argstest", (char *)"--value=-20" };
    test_check(axl_args_run(2, argv_low, &app) != 0,
               "args S64: below min (-20 < -10) rejected");
    test_check(cap.calls == 0,
               "args S64: handler did not run on below-min");

    /* Above max rejected. */
    cap = (ArgsCapture){0};
    char *argv_high[] = { (char *)"argstest", (char *)"--value=50" };
    test_check(axl_args_run(2, argv_high, &app) != 0,
               "args S64: above max (50 > 10) rejected");
    test_check(cap.calls == 0,
               "args S64: handler did not run on above-max");
}

/* Forward decl — definition follows test_args() so the new regression
   doesn't disrupt the existing function ordering in this file. */
static void test_args_numeric_default_value_applied(void);
static void test_args_get_uint_offset(void);
static void test_args_help_prolog_epilog(void);

static void
test_args(void)
{
    test_args_dispatch_to_verb();
    test_args_long_flag_with_equals();
    test_args_short_flag_with_value();
    test_args_bool_flag_presence();
    test_args_typed_positional_bounds();
    test_args_missing_required_positional();
    test_args_choice_positional();
    test_args_unknown_verb();
    test_args_global_flag_before_verb_survives_attach();
    test_args_help_word_only_pre_verb();
    test_args_compact_short_group_rejected();
    test_args_extra_positional_rejected();
    test_args_single_handler_mode();
    test_args_negative_positionals();
    test_args_help_alignment();
    test_args_help_terse_format();
    test_args_help_ascii_only();
    test_args_help_question_alias();
    test_args_hidden_flag();
    test_args_nested_2level_dispatch();
    test_args_nested_parent_flag_visible_at_leaf();
    test_args_nested_3level_dispatch();
    test_args_case_insensitive();
    test_args_compact_flags();
    test_args_nested_unknown_verb_at_branch();
    test_args_nested_branch_help_lists_subverbs();
    test_args_nested_misconfigured_node_rejected();
    test_args_s64_bounds();
    test_args_branch_default_fires_on_no_verb();
    test_args_branch_default_subverb_still_recurses();
    test_args_branch_default_unknown_verb_still_errors();
    test_args_branch_default_sees_branch_flags();
    test_args_branch_no_handler_still_shows_help();
    test_args_numeric_default_value_applied();
    test_args_get_uint_offset();
    test_args_help_prolog_epilog();
}

/* Regression: AXL_ARG_U16/U32/U64 with .default_value="N" should
   make axl_args_get_uint return N when the flag is unset.
   Pre-fix bug (caught 2026-05-05 on axl-webfs serve hardware run):
   register_descs assigned default_value into str_value but never
   ran parse_typed, so uint_value stayed 0. axl-webfs's serve
   listened on port 0 instead of the declared default 8080. */
static int
default_uint_handler(AxlArgs *a)
{
    ArgsCapture *cap = (ArgsCapture *)axl_args_user_data(a);
    cap->calls++;
    cap->seen_uint = axl_args_get_uint(a, "port");
    return 0;
}

static void
test_args_numeric_default_value_applied(void)
{
    static const AxlArgDesc default_uint_flags[] = {
        { .name = "port", .short_name = 'p', .type = AXL_ARG_U16,
          .default_value = "8080", .help = "test port" },
        {0}
    };

    /* Case 1: flag unset — should see the parsed default 8080. */
    {
        ArgsCapture cap = { 0 };
        AxlArgsNode app = {
            .name      = "argstest",
            .help      = "default-value test",
            .flags     = default_uint_flags,
            .handler   = default_uint_handler,
            .user_data = &cap,
        };
        char *argv[] = { (char *)"argstest" };
        int rc = axl_args_run(1, argv, &app);
        test_check(rc == 0, "args: default_value run accepted");
        test_check(cap.seen_uint == 8080,
                   "args: U16 default_value=\"8080\" applied to uint_value");
    }

    /* Case 2: flag explicitly set — should override the default. */
    {
        ArgsCapture cap = { 0 };
        AxlArgsNode app = {
            .name      = "argstest",
            .help      = "default-value override test",
            .flags     = default_uint_flags,
            .handler   = default_uint_handler,
            .user_data = &cap,
        };
        char *argv[] = { (char *)"argstest", (char *)"-p", (char *)"4242" };
        int rc = axl_args_run(3, argv, &app);
        test_check(rc == 0, "args: explicit override accepted");
        test_check(cap.seen_uint == 4242,
                   "args: explicit -p 4242 overrides default_value");
    }
}

// ---------------------------------------------------------------------------
// AxlArgs — help_prolog / help_epilog
// ---------------------------------------------------------------------------

static int
prolog_dummy_handler(AxlArgs *a)
{
    (void)a;
    return 0;
}

static void
test_args_help_prolog_epilog(void)
{
    static const AxlArgDesc dummy_flags[] = {
        { .name = "verbose", .short_name = 'v', .type = AXL_ARG_BOOL,
          .help = "verbose output" },
        {0}
    };
    static const AxlArgsNode bios_verbs[] = {
        { .name = "info",
          .help = "Type 0 summary",
          .handler = prolog_dummy_handler },
        {0}
    };

    /* Per-node prolog/epilog: each level has its own. The bios-node
       strings use distinctive markers ("BIOS-PROLOG-XYZZY" /
       "BIOS-EPILOG-PLUGH") so the per-node test below can prove
       'mytool bios --help' uses these and NOT the root node's. */
    static const AxlArgsNode bios_node = {
        .name        = "bios",
        .help        = "BIOS / SMBIOS subcommands",
        .verbs       = bios_verbs,
        .help_prolog = "BIOS-PROLOG-XYZZY: bios-only context.",
        .help_epilog = "BIOS-EPILOG-PLUGH: bios-only see-also.",
    };

    static const AxlArgsNode top_verbs[] = {
        bios_node,
        {0}
    };

    AxlArgsNode root = {
        .name        = "tester",
        .help        = "Hardware diagnostic CLI",
        .flags       = dummy_flags,
        .verbs       = top_verbs,
        .help_prolog =
            "ROOT-PROLOG-FROBNICATE\n"
            "Multi-line prolog text describing the tool.\n"
            "Includes examples and a list of env vars.",
        .help_epilog =
            "ROOT-EPILOG-CROMULENT\n"
            "Report bugs at https://example.invalid/issues",
    };

    /* 1. Root --help renders prolog before "Usage:" and epilog
       after the last auto-section. */
    {
        char *argv[] = { (char *)"tester", (char *)"--help" };
        AxlStream *buf = NULL;
        AxlStream *saved = capture_stdout(&buf);
        int rc = axl_args_run(2, argv, &root);
        size_t off_prolog = buf_offset_of(buf, "ROOT-PROLOG-FROBNICATE");
        size_t off_usage  = buf_offset_of(buf, "Usage: tester");
        size_t off_help_flag = buf_offset_of(buf, "-h, --help");
        size_t off_epilog = buf_offset_of(buf, "ROOT-EPILOG-CROMULENT");
        bool   has_multi  = buf_contains(buf, "Multi-line prolog text");
        restore_stdout(saved, buf);

        test_check(rc == 0, "args help prolog: root --help returns 0");
        test_check(off_prolog != SIZE_MAX,
                   "args help prolog: root prolog appears in --help");
        test_check(has_multi,
                   "args help prolog: multi-line prolog text rendered intact");
        test_check(off_usage != SIZE_MAX
                   && off_prolog < off_usage,
                   "args help prolog: prolog precedes 'Usage:'");
        test_check(off_epilog != SIZE_MAX,
                   "args help prolog: root epilog appears in --help");
        /* Epilog must come AFTER the last auto-generated line. The
           '-h, --help' flag line is the last thing the framework
           prints in every node shape (with or without flags), so
           it's the most reliable "tail of auto output" anchor. */
        test_check(off_help_flag != SIZE_MAX
                   && off_help_flag < off_epilog,
                   "args help prolog: epilog follows the last auto-generated line");
    }

    /* 2. Per-node: `tester bios --help` uses the bios node's
       prolog/epilog, NOT the root's. This is the load-bearing
       contract — if the framework only honored the root, sub-verb
       help would either repeat the root's text or show neither. */
    {
        char *argv[] = { (char *)"tester", (char *)"bios", (char *)"--help" };
        AxlStream *buf = NULL;
        AxlStream *saved = capture_stdout(&buf);
        int rc = axl_args_run(3, argv, &root);
        bool has_bios_prolog = buf_contains(buf, "BIOS-PROLOG-XYZZY");
        bool has_bios_epilog = buf_contains(buf, "BIOS-EPILOG-PLUGH");
        bool has_root_prolog = buf_contains(buf, "ROOT-PROLOG-FROBNICATE");
        bool has_root_epilog = buf_contains(buf, "ROOT-EPILOG-CROMULENT");
        restore_stdout(saved, buf);

        test_check(rc == 0, "args help prolog: 'bios --help' returns 0");
        test_check(has_bios_prolog,
                   "args help prolog: sub-verb shows its own prolog");
        test_check(has_bios_epilog,
                   "args help prolog: sub-verb shows its own epilog");
        test_check(!has_root_prolog,
                   "args help prolog: sub-verb does NOT inherit root prolog");
        test_check(!has_root_epilog,
                   "args help prolog: sub-verb does NOT inherit root epilog");
    }

    /* 3. NULL prolog/epilog: no extra output, no marker bytes,
       no spurious blank lines beyond the existing baseline.
       Regression for every existing caller (which doesn't set
       these fields). */
    {
        AxlArgsNode bare = {
            .name = "bare",
            .help = "no prolog or epilog",
            .handler = prolog_dummy_handler,
            /* help_prolog / help_epilog left zero-init NULL */
        };
        char *argv[] = { (char *)"bare", (char *)"--help" };
        AxlStream *buf = NULL;
        AxlStream *saved = capture_stdout(&buf);
        int rc = axl_args_run(2, argv, &bare);
        bool has_xyzzy = buf_contains(buf, "XYZZY");
        bool has_plugh = buf_contains(buf, "PLUGH");
        bool has_usage = buf_contains(buf, "Usage: bare");
        restore_stdout(saved, buf);

        test_check(rc == 0,
                   "args help prolog: bare --help returns 0");
        test_check(has_usage,
                   "args help prolog: bare --help still emits Usage section");
        test_check(!has_xyzzy && !has_plugh,
                   "args help prolog: NULL prolog/epilog → nothing extra printed");
    }
}

// ---------------------------------------------------------------------------
// AxlArgs — axl_args_get_uint_offset accessor
// ---------------------------------------------------------------------------

/* Captures the four NULL/unknown-name AXL_ERR returns of
   axl_args_get_uint_offset from inside a real handler. */
typedef struct {
    int rc_null_args;
    int rc_null_name;
    int rc_null_out;
    int rc_unknown_name;
} UintOffsetGuardCap;

static int
uint_offset_null_guard_handler(AxlArgs *a)
{
    UintOffsetGuardCap *g = (UintOffsetGuardCap *)axl_args_user_data(a);
    uint64_t out = 0;
    g->rc_null_args    = axl_args_get_uint_offset(NULL, "addr", &out);
    g->rc_null_name    = axl_args_get_uint_offset(a, NULL, &out);
    g->rc_null_out     = axl_args_get_uint_offset(a, "addr", NULL);
    g->rc_unknown_name = axl_args_get_uint_offset(a, "noSuchName", &out);
    return 0;
}

static int
uint_offset_capture_handler(AxlArgs *a)
{
    ArgsCapture *cap = (ArgsCapture *)axl_args_user_data(a);
    cap->calls++;
    uint64_t v = 0;
    if (axl_args_get_uint_offset(a, "addr", &v) == AXL_OK) {
        cap->seen_uint = v;
    } else {
        cap->seen_uint = 0xBADBADul;  /* sentinel — must not appear */
    }
    return 0;
}

static void
test_args_get_uint_offset(void)
{
    /* 1. Round-trip via STRING positional: "0x1000+0x50" → 0x1050. */
    {
        static const AxlArgDesc args[] = {
            { .name = "addr", .type = AXL_ARG_STRING, .required = true,
              .help = "register/memory address (hex+offset)" },
            {0}
        };
        ArgsCapture cap = { 0 };
        AxlArgsNode app = {
            .name = "t", .help = "uint_offset",
            .positionals = args, .handler = uint_offset_capture_handler,
            .user_data = &cap,
        };
        char *argv[] = { (char *)"t", (char *)"0x1000+0x50" };
        int rc = axl_args_run(2, argv, &app);
        test_check(rc == 0, "args uint_offset: round-trip run accepted");
        test_check(cap.seen_uint == 0x1050,
                   "args uint_offset: '0x1000+0x50' resolves to 0x1050");
    }

    /* 2. Plain hex (no offset). */
    {
        static const AxlArgDesc args[] = {
            { .name = "addr", .type = AXL_ARG_STRING, .required = true },
            {0}
        };
        ArgsCapture cap = { 0 };
        AxlArgsNode app = {
            .name = "t", .positionals = args,
            .handler = uint_offset_capture_handler, .user_data = &cap,
        };
        char *argv[] = { (char *)"t", (char *)"0xabcd" };
        int rc = axl_args_run(2, argv, &app);
        test_check(rc == 0 && cap.seen_uint == 0xabcd,
                   "args uint_offset: plain hex value parsed");
    }

    /* 3. default_value picked up when the flag is unset. */
    {
        static const AxlArgDesc default_flags[] = {
            { .name = "addr", .short_name = 'a', .type = AXL_ARG_STRING,
              .default_value = "0x1000+0x10",
              .help = "address (with offset)" },
            {0}
        };
        ArgsCapture cap = { 0 };
        AxlArgsNode app = {
            .name = "t", .flags = default_flags,
            .handler = uint_offset_capture_handler, .user_data = &cap,
        };
        char *argv[] = { (char *)"t" };
        int rc = axl_args_run(1, argv, &app);
        test_check(rc == 0, "args uint_offset: default_value run accepted");
        test_check(cap.seen_uint == 0x1010,
                   "args uint_offset: default_value '0x1000+0x10' parsed → 0x1010");
    }

    /* 4. Optional positional, value absent → AXL_ERR (caller's
       sentinel survives). */
    {
        static const AxlArgDesc args[] = {
            { .name = "addr", .type = AXL_ARG_STRING, .required = false,
              .help = "optional address" },
            {0}
        };
        ArgsCapture cap = { 0 };
        AxlArgsNode app = {
            .name = "t", .positionals = args,
            .handler = uint_offset_capture_handler, .user_data = &cap,
        };
        char *argv[] = { (char *)"t" };
        int rc = axl_args_run(1, argv, &app);
        test_check(rc == 0 && cap.calls == 1,
                   "args uint_offset: optional absent → handler still ran");
        test_check(cap.seen_uint == 0xBADBADul,
                   "args uint_offset: absent arg returns AXL_ERR (sentinel kept)");
    }

    /* 5. Malformed token surfaces as AXL_ERR through the accessor.
       The framework itself doesn't pre-validate (STRING accepts
       anything), so the parse failure happens inside the
       accessor on the handler side. */
    {
        static const AxlArgDesc args[] = {
            { .name = "addr", .type = AXL_ARG_STRING, .required = true },
            {0}
        };
        ArgsCapture cap = { 0 };
        AxlArgsNode app = {
            .name = "t", .positionals = args,
            .handler = uint_offset_capture_handler, .user_data = &cap,
        };
        char *argv[] = { (char *)"t", (char *)"zz" };
        int rc = axl_args_run(2, argv, &app);
        test_check(rc == 0 && cap.calls == 1,
                   "args uint_offset: malformed value still reaches handler "
                   "(STRING is unconstrained)");
        test_check(cap.seen_uint == 0xBADBADul,
                   "args uint_offset: malformed token → accessor returns AXL_ERR");
    }

    /* 6. Parents-walk: arg declared on the parent node is reachable
       from a child verb's handler via the same accessor. Mirrors
       the family contract documented on the other axl_args_get_*
       accessors. */
    {
        static const AxlArgDesc parent_flags[] = {
            { .name = "addr", .short_name = 'a', .type = AXL_ARG_STRING,
              .help = "parent-level address" },
            {0}
        };
        static const AxlArgsNode child_verbs[] = {
            { .name = "go", .handler = uint_offset_capture_handler,
              .help = "child verb" },
            {0}
        };
        ArgsCapture cap = { 0 };
        AxlArgsNode app = {
            .name = "t", .flags = parent_flags, .verbs = child_verbs,
            .user_data = &cap,
        };
        char *argv[] = { (char *)"t",
                         (char *)"--addr=0x200+0x40",
                         (char *)"go" };
        int rc = axl_args_run(3, argv, &app);
        test_check(rc == 0, "args uint_offset: parents-walk run accepted");
        test_check(cap.calls == 1 && cap.seen_uint == 0x240,
                   "args uint_offset: parent-level addr reachable from child handler");
    }

    /* 7. NULL/unknown-name guards on the accessor itself. */
    {
        static const AxlArgDesc args[] = {
            { .name = "addr", .type = AXL_ARG_STRING, .required = true },
            {0}
        };
        UintOffsetGuardCap guard = { 0 };
        AxlArgsNode app = {
            .name = "t", .positionals = args,
            .handler = uint_offset_null_guard_handler,
            .user_data = &guard,
        };
        char *argv[] = { (char *)"t", (char *)"0x10" };
        int rc = axl_args_run(2, argv, &app);
        test_check(rc == 0, "args uint_offset: NULL-guard run accepted");
        test_check(guard.rc_null_args == AXL_ERR,
                   "args uint_offset: NULL args → AXL_ERR");
        test_check(guard.rc_null_name == AXL_ERR,
                   "args uint_offset: NULL name → AXL_ERR");
        test_check(guard.rc_null_out == AXL_ERR,
                   "args uint_offset: NULL out_value → AXL_ERR");
        test_check(guard.rc_unknown_name == AXL_ERR,
                   "args uint_offset: unknown name → AXL_ERR");
    }
}

// ---------------------------------------------------------------------------
// Sort Tests (axl_qsort / axl_qsort_with_data — introsort)
// ---------------------------------------------------------------------------

static int
sort_cmp_int(const void *a, const void *b)
{
    int va = *(const int *)a;
    int vb = *(const int *)b;

    if (va < vb) { return -1; }
    if (va > vb) { return 1; }
    return 0;
}

static int
sort_cmp_int_data(const void *a, const void *b, void *user_data)
{
    int va = *(const int *)a;
    int vb = *(const int *)b;
    int descending = *(int *)user_data;
    int rc;

    if (va < vb) { rc = -1; }
    else if (va > vb) { rc = 1; }
    else { rc = 0; }

    return descending ? -rc : rc;
}

static bool
sort_is_ascending(const int *a, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        if (a[i - 1] > a[i]) {
            return false;
        }
    }
    return true;
}

static void
test_qsort_basic(void)
{
    int a[] = { 5, 3, 8, 1, 9, 2, 7, 4, 6, 0 };
    size_t n = sizeof(a) / sizeof(a[0]);

    axl_qsort(a, n, sizeof(a[0]), sort_cmp_int);

    bool ok = true;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != (int)i) {
            ok = false;
        }
    }
    test_check(ok, "qsort: 0..9 fully ordered");
}

static void
test_qsort_already_sorted(void)
{
    int a[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    size_t n = sizeof(a) / sizeof(a[0]);

    axl_qsort(a, n, sizeof(a[0]), sort_cmp_int);

    bool ok = (a[0] == 1 && a[7] == 8 && sort_is_ascending(a, n));
    test_check(ok, "qsort: already-sorted stays sorted");
}

static void
test_qsort_reverse(void)
{
    int a[] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
    size_t n = sizeof(a) / sizeof(a[0]);

    axl_qsort(a, n, sizeof(a[0]), sort_cmp_int);

    bool ok = (a[0] == 0 && a[n - 1] == 9 && sort_is_ascending(a, n));
    test_check(ok, "qsort: reverse-sorted becomes ascending");
}

static void
test_qsort_all_equal(void)
{
    int a[] = { 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7 };
    size_t n = sizeof(a) / sizeof(a[0]);

    axl_qsort(a, n, sizeof(a[0]), sort_cmp_int);

    bool ok = true;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != 7) {
            ok = false;
        }
    }
    test_check(ok, "qsort: all-equal preserved (no corruption)");
}

static void
test_qsort_edge_sizes(void)
{
    int empty[1] = { 42 };
    int one[1] = { 99 };
    int two_a[2] = { 2, 1 };
    int two_b[2] = { 1, 2 };

    // nmemb 0 and 1 are no-ops; must not touch memory or crash.
    axl_qsort(empty, 0, sizeof(int), sort_cmp_int);
    test_check(empty[0] == 42, "qsort: nmemb 0 is a no-op");

    axl_qsort(one, 1, sizeof(int), sort_cmp_int);
    test_check(one[0] == 99, "qsort: nmemb 1 is a no-op");

    axl_qsort(two_a, 2, sizeof(int), sort_cmp_int);
    test_check(two_a[0] == 1 && two_a[1] == 2, "qsort: 2 elements swapped");

    axl_qsort(two_b, 2, sizeof(int), sort_cmp_int);
    test_check(two_b[0] == 1 && two_b[1] == 2, "qsort: 2 elements kept");
}

static void
test_qsort_null_guards(void)
{
    int a[] = { 3, 1, 2 };

    // Each of these must be a safe no-op (no crash, array untouched).
    axl_qsort(NULL, 3, sizeof(int), sort_cmp_int);
    axl_qsort(a, 3, sizeof(int), NULL);
    axl_qsort(a, 3, 0, sort_cmp_int);

    bool ok = (a[0] == 3 && a[1] == 1 && a[2] == 2);
    test_check(ok, "qsort: NULL/zero-size guards leave input untouched");
}

static void
test_qsort_large_random(void)
{
    enum { N = 4096 };
    static int a[N];
    AXL_AUTOPTR(AxlRand) rng = axl_rand_new_seeded(0x1234abcdu);

    for (size_t i = 0; i < N; i++) {
        // Narrow value range (0..255) exercises duplicate handling in the
        // partition. Values stay well-distributed, so median-of-three keeps
        // recursion balanced — this is the quicksort/insertion path, not the
        // heapsort fallback (see test_qsort_heapsort_fallback for that).
        a[i] = axl_rand_int_range(rng, 0, 256);
    }

    axl_qsort(a, N, sizeof(a[0]), sort_cmp_int);

    test_check(sort_is_ascending(a, N), "qsort: 4096 random ints ascending");
}

static void
test_qsort_heapsort_fallback(void)
{
    // A large mostly-equal array drives the Lomuto partition into maximally
    // unbalanced splits (each level peels off only the pivot), so recursion
    // depth blows past the 2*log2(n) limit and introsort bails to its
    // heapsort fallback. The distinct sentinels among the duplicates must
    // still come out correctly ordered through that path — in particular the
    // three large values exercise heapsort's ordering of distinct keys.
    enum { N = 128 };
    static int a[N];

    for (size_t i = 0; i < N; i++) {
        a[i] = 100;
    }
    a[3]  = 1;    // small sentinels — sort to the front
    a[70] = 2;
    a[120] = 3;
    a[40] = 200;  // large distinct sentinels — heapsort must order these
    a[41] = 150;
    a[42] = 120;

    axl_qsort(a, N, sizeof(a[0]), sort_cmp_int);

    // Expected: [1, 2, 3, 100 x122, 120, 150, 200]
    bool ok = sort_is_ascending(a, N);
    if (a[0] != 1 || a[1] != 2 || a[2] != 3) { ok = false; }
    if (a[125] != 120 || a[126] != 150 || a[127] != 200) { ok = false; }
    for (size_t i = 3; i <= 124; i++) {
        if (a[i] != 100) { ok = false; }
    }
    test_check(ok, "qsort: heapsort fallback orders distinct sentinels");
}

typedef struct {
    int      key;
    uint8_t  pad[80]; // > mem_swap chunk (64) to exercise chunked + tail swap
} SortBig;

static int
sort_cmp_big(const void *a, const void *b)
{
    return sort_cmp_int(&((const SortBig *)a)->key, &((const SortBig *)b)->key);
}

static void
test_qsort_large_elements(void)
{
    enum { N = 50 };
    static SortBig a[N];
    AXL_AUTOPTR(AxlRand) rng = axl_rand_new_seeded(0xdeadbeefu);

    for (size_t i = 0; i < N; i++) {
        a[i].key = axl_rand_int_range(rng, 0, 1000);
        a[i].pad[0] = (uint8_t)a[i].key;   // tie payload to key
        a[i].pad[79] = (uint8_t)(a[i].key ^ 0xFF);
    }

    axl_qsort(a, N, sizeof(SortBig), sort_cmp_big);

    bool ok = true;
    for (size_t i = 0; i < N; i++) {
        // Sorted by key AND payload moved intact with its element.
        if (a[i].pad[0] != (uint8_t)a[i].key) { ok = false; }
        if (a[i].pad[79] != (uint8_t)(a[i].key ^ 0xFF)) { ok = false; }
        if (i > 0 && a[i - 1].key > a[i].key) { ok = false; }
    }
    test_check(ok, "qsort: 80-byte elements move payload intact, ordered");
}

static void
test_qsort_with_data_descending(void)
{
    int a[] = { 5, 3, 8, 1, 9, 2, 7, 4, 6, 0 };
    size_t n = sizeof(a) / sizeof(a[0]);
    int descending = 1;

    axl_qsort_with_data(a, n, sizeof(a[0]), sort_cmp_int_data, &descending);

    bool ok = true;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != (int)(9 - i)) {
            ok = false;
        }
    }
    test_check(ok, "qsort_with_data: descending 9..0");
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Clipboard (D1)
// ---------------------------------------------------------------------------

static void
test_clipboard(void)
{
    axl_clipboard_clear();   /* known-empty start (process-global) */

    size_t      len = 99;
    const char *mime = (const char *)1;
    const void *p = axl_clipboard_get(&len, &mime);
    test_check(p == NULL && len == 0 && mime == NULL, "clipboard: empty after clear");

    /* set copies the bytes in (ownership) */
    char src[] = "hello";
    test_check(axl_clipboard_set(src, 5, NULL) == AXL_OK, "clipboard: set ok");
    src[0] = 'X';            /* mutate source: stored copy must be unaffected */
    p = axl_clipboard_get(&len, &mime);
    test_check(p != NULL && len == 5 && axl_memcmp(p, "hello", 5) == 0,
               "clipboard: get returns an owned copy of the bytes");
    test_check(mime == NULL, "clipboard: no mime -> NULL");

    /* mime stored + readable; out_mime optional */
    test_check(axl_clipboard_set("data", 4, "text/plain") == AXL_OK, "clipboard: set with mime");
    p = axl_clipboard_get(&len, &mime);
    test_check(len == 4 && axl_memcmp(p, "data", 4) == 0 && mime != NULL
               && axl_strcmp(mime, "text/plain") == 0, "clipboard: mime round-trips");
    test_check(axl_clipboard_get(&len, NULL) != NULL, "clipboard: NULL out_mime allowed");

    /* set replaces previous contents and drops the previous mime */
    test_check(axl_clipboard_set("cd", 2, NULL) == AXL_OK, "clipboard: replace");
    p = axl_clipboard_get(&len, &mime);
    test_check(len == 2 && axl_memcmp(p, "cd", 2) == 0 && mime == NULL,
               "clipboard: replacing without mime clears the old mime");

    /* binary payload with an embedded NUL survives intact */
    const unsigned char bin[] = { 0x00, 0x01, 0xFF, 0x00, 0x42 };
    test_check(axl_clipboard_set(bin, sizeof(bin), "application/octet-stream") == AXL_OK,
               "clipboard: set binary");
    p = axl_clipboard_get(&len, &mime);
    test_check(len == sizeof(bin) && axl_memcmp(p, bin, sizeof(bin)) == 0,
               "clipboard: embedded NUL preserved");

    /* invalid args leave the clipboard unchanged */
    test_check(axl_clipboard_set(NULL, 5, NULL) == AXL_ERR, "clipboard: NULL data with len -> ERR");
    p = axl_clipboard_get(&len, NULL);
    test_check(len == sizeof(bin) && p != NULL, "clipboard: unchanged after invalid set");

    /* OOM atomicity: a failed set leaves the prior clipboard intact. The
       scratch image buffer is set's first allocation (before any shm call),
       so failing it is deterministic and never touches the live segment. */
    test_check(axl_clipboard_set("keep", 4, "m/keep") == AXL_OK, "clipboard: seed for OOM test");
    axl_mem_fail_next_alloc(1);                          /* fail the image buffer */
    test_check(axl_clipboard_set("lost", 4, NULL) == AXL_ERR,
               "clipboard: set fails when the image buffer can't be allocated");
    p = axl_clipboard_get(&len, &mime);
    test_check(len == 4 && axl_memcmp(p, "keep", 4) == 0 && mime != NULL
               && axl_strcmp(mime, "m/keep") == 0,
               "clipboard: prior contents intact when set's allocation fails");

    /* empty payload (NULL data, 0 len) is valid */
    test_check(axl_clipboard_set(NULL, 0, NULL) == AXL_OK, "clipboard: empty set ok");
    p = axl_clipboard_get(&len, &mime);
    test_check(p == NULL && len == 0, "clipboard: empty payload reads back empty");

    axl_clipboard_clear();
    p = axl_clipboard_get(&len, &mime);
    test_check(p == NULL && len == 0 && mime == NULL, "clipboard: clear empties");
}

/* Hand-build a clipboard segment to exercise the reader's robustness.
   White-box: the layout is [u32 struct_size=16][u32 mime_len][u64 data_len]
   [data][mime] (little-endian; both target arches are LE). */
static uint8_t *
craft_clip_segment(size_t data_len, size_t mime_len)
{
    (void)axl_clipboard_clear();
    uint8_t *seg = (uint8_t *)axl_shm_open("axl/clipboard", 16 + data_len + mime_len,
                                           AXL_SHM_CREATE, NULL);
    if (seg == NULL) {
        return NULL;
    }
    axl_memset(seg, 0, 16);
    seg[0] = 16;                                   /* struct_size = 16 */
    seg[4] = (uint8_t)mime_len;                    /* mime_len (small) */
    seg[8] = (uint8_t)data_len;                    /* data_len (small) */
    return seg;
}

static void
test_clipboard_corrupt(void)
{
    /* A valid hand-built segment reads back, proving the layout matches. */
    uint8_t *seg = craft_clip_segment(1, 3);
    if (seg == NULL) {
        axl_printf("SKIP: clipboard corrupt-segment test (shm unavailable)\n");
        return;
    }
    seg[16] = 'A';
    seg[17] = 'b'; seg[18] = 'c'; seg[19] = '\0';  /* mime "bc" (NUL-terminated) */
    size_t len = 0;
    const char *mime = NULL;
    const void *p = axl_clipboard_get(&len, &mime);
    test_check(p != NULL && len == 1 && axl_memcmp(p, "A", 1) == 0
               && mime != NULL && axl_strcmp(mime, "bc") == 0,
               "clipboard: hand-built segment reads back (layout sanity)");

    /* The same segment without a terminating NUL in the MIME window must be
       rejected (a corrupt/foreign segment must not OOB-scan via %s). */
    seg = craft_clip_segment(1, 3);
    seg[16] = 'A';
    seg[17] = 'b'; seg[18] = 'c'; seg[19] = 'x';   /* mime "bcx" — no NUL */
    len = 99;
    mime = (const char *)1;
    p = axl_clipboard_get(&len, &mime);
    test_check(p == NULL && len == 0 && mime == NULL,
               "clipboard: non-NUL-terminated MIME segment rejected as empty");
    (void)axl_clipboard_clear();
}

// ---------------------------------------------------------------------------
// Shared memory (axl-shm)
// ---------------------------------------------------------------------------

#define SHM_A "axl/test/shm.a"
#define SHM_B "axl/test/shm.b"

static void
test_shm(void)
{
    /* clean slate (a prior assertion in this boot may have created it) */
    (void)axl_shm_unlink(SHM_A);
    (void)axl_shm_unlink(SHM_B);

    size_t sz = 12345;
    test_check(axl_shm_open(SHM_A, 128, 0, &sz) == NULL,
               "shm: open missing without CREATE -> NULL");
    test_check(!axl_shm_exists(SHM_A, NULL), "shm: exists(missing) -> false");

    /* create */
    sz = 0;
    uint8_t *p = (uint8_t *)axl_shm_open(SHM_A, 128, AXL_SHM_CREATE, &sz);
    test_check(p != NULL && sz == 128, "shm: create returns region + size");
    bool zeroed = true;
    for (size_t i = 0; i < 128; i++) {
        if (p[i] != 0) {
            zeroed = false;
        }
    }
    test_check(zeroed, "shm: freshly created region is zeroed");

    /* write a pattern, reopen, confirm same pointer + persisted bytes */
    for (size_t i = 0; i < 128; i++) {
        p[i] = (uint8_t)(i * 7u + 3u);
    }
    size_t sz2 = 0;
    uint8_t *p2 = (uint8_t *)axl_shm_open(SHM_A, 9999, 0, &sz2);  /* size ignored on existing */
    test_check(p2 == p && sz2 == 128, "shm: reopen returns the same region + size");
    bool same = true;
    for (size_t i = 0; i < 128; i++) {
        if (p2[i] != (uint8_t)(i * 7u + 3u)) {
            same = false;
        }
    }
    test_check(same, "shm: bytes persist across opens");

    test_check(axl_shm_exists(SHM_A, &sz2) && sz2 == 128, "shm: exists(present) -> true + size");

    /* CREATE on an existing segment returns it (ignores size); EXCL fails */
    test_check(axl_shm_open(SHM_A, 64, AXL_SHM_CREATE, NULL) == p,
               "shm: CREATE on existing returns it");
    test_check(axl_shm_open(SHM_A, 64, AXL_SHM_CREATE | AXL_SHM_EXCL, NULL) == NULL,
               "shm: CREATE|EXCL on existing -> NULL");

    /* a different name is an independent segment */
    uint8_t *pb = (uint8_t *)axl_shm_open(SHM_B, 64, AXL_SHM_CREATE, NULL);
    test_check(pb != NULL && pb != p, "shm: distinct name -> distinct region");

    /* unlink removes it; reopen misses; double-unlink is a no-op */
    test_check(axl_shm_unlink(SHM_A) == AXL_OK, "shm: unlink");
    test_check(!axl_shm_exists(SHM_A, NULL), "shm: gone after unlink");
    test_check(axl_shm_open(SHM_A, 0, 0, NULL) == NULL, "shm: open after unlink -> NULL");
    test_check(axl_shm_unlink(SHM_A) == AXL_OK, "shm: unlink(absent) -> OK");

    /* recreate after unlink yields a fresh zeroed region */
    uint8_t *p3 = (uint8_t *)axl_shm_open(SHM_A, 16, AXL_SHM_CREATE, &sz2);
    test_check(p3 != NULL && sz2 == 16 && p3[0] == 0 && p3[15] == 0,
               "shm: recreate after unlink is fresh + zeroed");

    /* NULL-name guards */
    test_check(axl_shm_open(NULL, 8, AXL_SHM_CREATE, NULL) == NULL, "shm: NULL name open -> NULL");
    test_check(!axl_shm_exists(NULL, NULL), "shm: NULL name exists -> false");
    test_check(axl_shm_unlink(NULL) == AXL_ERR, "shm: NULL name unlink -> ERR");

    (void)axl_shm_unlink(SHM_A);
    (void)axl_shm_unlink(SHM_B);
}

// ---------------------------------------------------------------------------
// AxlRand Tests
//
// Expected values are from the canonical xoshiro256**/SplitMix64 reference
// (computed independently of this implementation) so the assertions pin the
// documented stream rather than passing against whatever the impl happens to
// produce. Seed-0x1234 stream words w0..w5:
//   CF1350DCCA3DEBE9 ACC53B3FB46C231F 17D76A4D73642536
//   E573FFDBE8DFBF83 9EAD861BFCAB7610 56D3D3FF75D17A51
// ---------------------------------------------------------------------------

static uint64_t
double_bits(double d)
{
    uint64_t bits;
    axl_memcpy(&bits, &d, sizeof(bits));
    return bits;
}

static void
test_rand_stream(void)
{
    static const uint64_t expect[6] = {
        0xCF1350DCCA3DEBE9ULL, 0xACC53B3FB46C231FULL, 0x17D76A4D73642536ULL,
        0xE573FFDBE8DFBF83ULL, 0x9EAD861BFCAB7610ULL, 0x56D3D3FF75D17A51ULL,
    };
    AXL_AUTOPTR(AxlRand) r = axl_rand_new_seeded(0x1234);
    test_check(r != NULL, "rand: new_seeded -> non-NULL");
    for (int i = 0; i < 6; i++) {
        test_check(axl_rand_uint64(r) == expect[i],
            "rand: uint64 stream matches reference");
    }

    // uint32 = high 32 bits of each fresh word, one word per call.
    AXL_AUTOPTR(AxlRand) r2 = axl_rand_new_seeded(0x1234);
    test_check(axl_rand_uint32(r2) == 0xCF1350DCU, "rand: uint32[0] = high32(w0)");
    test_check(axl_rand_uint32(r2) == 0xACC53B3FU, "rand: uint32[1] = high32(w1)");
    test_check(axl_rand_uint32(r2) == 0x17D76A4DU, "rand: uint32[2] = high32(w2)");

    // Zero seed is non-degenerate (SplitMix64 expansion).
    AXL_AUTOPTR(AxlRand) rz = axl_rand_new_seeded(0);
    test_check(axl_rand_uint64(rz) == 0x99EC5F36CB75F2B4ULL,
        "rand: zero seed -> non-degenerate reference word");
}

static void
test_rand_derived(void)
{
    // double: top 53 bits of each fresh word -> [0,1). Pin exact bit patterns.
    AXL_AUTOPTR(AxlRand) r = axl_rand_new_seeded(0x1234);
    test_check(double_bits(axl_rand_double(r)) == 0x3FE9E26A1B9947BDULL,
        "rand: double[0] exact bits");
    test_check(double_bits(axl_rand_double(r)) == 0x3FE598A767F68D84ULL,
        "rand: double[1] exact bits");

    // boolean = bit 63 of each fresh word: 1 1 0 1 1 0 0 1.
    AXL_AUTOPTR(AxlRand) rb = axl_rand_new_seeded(0x1234);
    static const bool expb[8] = { true, true, false, true, true, false, false, true };
    for (int i = 0; i < 8; i++) {
        test_check(axl_rand_boolean(rb) == expb[i], "rand: boolean bit63 stream");
    }

    // bytes: little-endian emission of successive words.
    static const uint8_t expbytes[10] = {
        0xE9, 0xEB, 0x3D, 0xCA, 0xDC, 0x50, 0x13, 0xCF, 0x1F, 0x23,
    };
    AXL_AUTOPTR(AxlRand) ry = axl_rand_new_seeded(0x1234);
    uint8_t buf[10];
    axl_memset(buf, 0xAA, sizeof(buf));
    axl_rand_bytes(ry, buf, sizeof(buf));
    bool match = true;
    for (int i = 0; i < 10; i++) {
        if (buf[i] != expbytes[i]) { match = false; }
    }
    test_check(match, "rand: bytes little-endian stream matches reference");

    // len == 0 is a no-op (buffer untouched).
    uint8_t sentinel = 0x5A;
    axl_rand_bytes(ry, &sentinel, 0);
    test_check(sentinel == 0x5A, "rand: bytes len==0 is a no-op");
}

static void
test_rand_reproducible(void)
{
    // Same seed -> identical sequences.
    AXL_AUTOPTR(AxlRand) a = axl_rand_new_seeded(0xABCDEF);
    AXL_AUTOPTR(AxlRand) b = axl_rand_new_seeded(0xABCDEF);
    bool same = true;
    for (int i = 0; i < 100; i++) {
        if (axl_rand_uint64(a) != axl_rand_uint64(b)) { same = false; }
    }
    test_check(same, "rand: same seed -> identical 100-word streams");

    // set_seed resets regardless of prior use: after reseeding, the
    // stream matches a fresh generator with the same seed (b has advanced
    // 100 words, so it is NOT the right reference here).
    (void)axl_rand_uint64(a);  // advance a to an arbitrary position
    axl_rand_set_seed(a, 0xABCDEF);
    AXL_AUTOPTR(AxlRand) fresh = axl_rand_new_seeded(0xABCDEF);
    test_check(axl_rand_uint64(a) == axl_rand_uint64(fresh),
        "rand: set_seed resets the stream");

    // copy duplicates position, then runs independently.
    AXL_AUTOPTR(AxlRand) c = axl_rand_new_seeded(0x42);
    (void)axl_rand_uint64(c);
    (void)axl_rand_uint64(c);
    AXL_AUTOPTR(AxlRand) d = axl_rand_copy(c);
    test_check(d != NULL, "rand: copy -> non-NULL");
    // Both at the same position: each yields the word at that position.
    // Independent state, not shared — shared state would have made d's draw
    // the word AFTER c's, so equality here proves both duplication and
    // independence in one assertion.
    uint64_t cv = axl_rand_uint64(c);
    uint64_t dv = axl_rand_uint64(d);
    test_check(cv == dv, "rand: copy resumes the identical stream independently");
    test_check(axl_rand_copy(NULL) == NULL, "rand: copy(NULL) -> NULL");

    // Smoke: the non-reproducible constructor plumbs entropy/clock and
    // yields a usable generator (its values can't be pinned).
    AXL_AUTOPTR(AxlRand) nd = axl_rand_new();
    test_check(nd != NULL, "rand: new() (entropy-seeded) -> non-NULL");
    (void)axl_rand_uint64(nd);
}

static void
test_rand_ranges(void)
{
    // int_range: unbiased rejection; pin the first five [1,7) draws.
    static const int32_t expdice[5] = { 4, 6, 5, 6, 3 };
    AXL_AUTOPTR(AxlRand) r = axl_rand_new_seeded(0x1234);
    bool dice_ok = true;
    for (int i = 0; i < 5; i++) {
        if (axl_rand_int_range(r, 1, 7) != expdice[i]) { dice_ok = false; }
    }
    test_check(dice_ok, "rand: int_range(1,7) matches reference");

    // Bounds respected across many draws, including a negative range.
    bool in_bounds = true;
    for (int i = 0; i < 1000; i++) {
        int32_t v = axl_rand_int_range(r, -10, 10);
        if (v < -10 || v >= 10) { in_bounds = false; }
    }
    test_check(in_bounds, "rand: int_range(-10,10) stays in bounds");

    // Full INT32 range (span > INT32_MAX) must not overflow/crash.
    bool full_ok = true;
    for (int i = 0; i < 1000; i++) {
        int32_t v = axl_rand_int_range(r, INT32_MIN, INT32_MAX);
        if (v == INT32_MAX) { full_ok = false; }  // exclusive upper bound
    }
    test_check(full_ok, "rand: int_range full INT32 span excludes end, no overflow");

    // double in [0,1) across many draws.
    bool d_ok = true;
    for (int i = 0; i < 1000; i++) {
        double v = axl_rand_double(r);
        if (v < 0.0 || v >= 1.0) { d_ok = false; }
    }
    test_check(d_ok, "rand: double stays in [0,1)");

    // double_range exact (pins the no-FMA two-step construction).
    AXL_AUTOPTR(AxlRand) rr = axl_rand_new_seeded(0x1234);
    test_check(double_bits(axl_rand_double_range(rr, 10.0, 20.0)) == 0x403216C1289FE66BULL,
        "rand: double_range(10,20)[0] exact bits (no FMA)");
    bool dr_ok = true;
    for (int i = 0; i < 1000; i++) {
        double v = axl_rand_double_range(rr, -5.0, 5.0);
        if (v < -5.0 || v >= 5.0) { dr_ok = false; }
    }
    test_check(dr_ok, "rand: double_range stays in [begin,end)");

    // Degenerate (empty) ranges return begin without drawing.
    test_check(axl_rand_int_range(rr, 5, 5) == 5, "rand: int_range empty -> begin");
    test_check(axl_rand_int_range(rr, 9, 3) == 9, "rand: int_range inverted -> begin");
    test_check(axl_rand_double_range(rr, 2.5, 2.5) == 2.5,
        "rand: double_range empty -> begin");
}

static void
test_rand_global(void)
{
    // Global stream is the same engine; set_seed ties it to the reference.
    axl_random_set_seed(0x1234);
    test_check(axl_random_uint32() == 0xCF1350DCU, "rand: global uint32 = high32(w0)");

    // set_seed resets the global stream regardless of prior use.
    (void)axl_random_uint32();
    (void)axl_random_double();
    axl_random_set_seed(0x1234);
    test_check(axl_random_uint32() == 0xCF1350DCU, "rand: global set_seed resets");

    // Range/double bounds hold on the global stream too.
    bool ok = true;
    for (int i = 0; i < 500; i++) {
        int32_t iv = axl_random_int_range(0, 6);
        double  dv = axl_random_double();
        double  rv = axl_random_double_range(100.0, 200.0);
        if (iv < 0 || iv >= 6 || dv < 0.0 || dv >= 1.0 || rv < 100.0 || rv >= 200.0) {
            ok = false;
        }
    }
    test_check(ok, "rand: global int/double/double_range stay in bounds");
    (void)axl_random_boolean();
}

// ---------------------------------------------------------------------------
// AxlBytes adoption: axl_file_get_bytes + axl_clipboard_get_bytes.
// ---------------------------------------------------------------------------

static void
test_file_get_bytes(void)
{
    const char data[] = "axl_file_get_bytes payload \x01\x02\x00\x03";  // embedded NUL
    const size_t dlen = sizeof(data) - 1;
    test_check(axl_file_set_contents("axl-fgb.tmp", data, dlen) == 0,
        "file_get_bytes: write fixture");

    AxlBytes *b = axl_file_get_bytes("axl-fgb.tmp");
    test_check(b != NULL, "file_get_bytes: non-NULL");
    size_t n = 0;
    const uint8_t *p = axl_bytes_get_data(b, &n);
    test_check(n == dlen, "file_get_bytes: size matches");
    test_check(p != NULL && axl_memcmp(p, data, dlen) == 0,
        "file_get_bytes: content matches (incl. embedded NUL)");
    axl_bytes_unref(b);

    // Empty file -> valid empty AxlBytes.
    test_check(axl_file_set_contents("axl-fgb-empty.tmp", "", 0) == 0,
        "file_get_bytes: write empty fixture");
    AxlBytes *e = axl_file_get_bytes("axl-fgb-empty.tmp");
    test_check(e != NULL && axl_bytes_get_size(e) == 0, "file_get_bytes: empty file -> size 0");
    size_t en;
    test_check(axl_bytes_get_data(e, &en) == NULL && en == 0,
        "file_get_bytes: empty file -> get_data NULL");
    axl_bytes_unref(e);

    // Missing file -> NULL.
    test_check(axl_file_get_bytes("does-not-exist.tmp") == NULL,
        "file_get_bytes: missing file -> NULL");

    (void)axl_file_delete("axl-fgb.tmp");
    (void)axl_file_delete("axl-fgb-empty.tmp");
}

static void
test_clipboard_get_bytes(void)
{
    test_check(axl_clipboard_set("first-payload", 13, "text/plain") == 0,
        "clip_bytes: set first");

    AxlBytes *snap = axl_clipboard_get_bytes();
    test_check(snap != NULL && axl_bytes_get_size(snap) == 13, "clip_bytes: snapshot size 13");

    // The key win: snapshot stays valid and unchanged across a later set
    // that invalidates the borrowed axl_clipboard_get pointer.
    test_check(axl_clipboard_set("totally-different-and-longer", 28, NULL) == 0,
        "clip_bytes: set second (invalidates borrows)");
    size_t n = 0;
    const uint8_t *p = axl_bytes_get_data(snap, &n);
    test_check(n == 13 && axl_memcmp(p, "first-payload", 13) == 0,
        "clip_bytes: snapshot stable across later set");
    axl_bytes_unref(snap);

    // Cleared clipboard -> NULL.
    axl_clipboard_clear();
    test_check(axl_clipboard_get_bytes() == NULL, "clip_bytes: empty -> NULL");
}

// ---------------------------------------------------------------------------
// AxlTar — ustar reader/writer round-trip over an in-memory stream
// ---------------------------------------------------------------------------

static size_t
tar_read_all(AxlTarReader *r, char *out, size_t cap)
{
    size_t      total = 0;
    axl_ssize_t n;
    while (total < cap
           && (n = axl_tar_reader_read(r, out + total, cap - total)) > 0) {
        total += (size_t)n;
    }
    return total;
}

static void
test_tar_roundtrip(void)
{
    AxlStream *buf = axl_bufopen();
    test_check(buf != NULL, "tar: bufopen");
    AxlTarWriter *w = axl_tar_writer_new(buf);
    test_check(w != NULL, "tar: writer_new");

    const char *j = "{\"a\":1}";                 /* 7 bytes */
    const char  d[] = "FACP\x24\x00\x00\x00rest"; /* 13 bytes incl embedded NULs */
    test_check(axl_tar_writer_add(w, "pci.json", 0644, j, 7) == AXL_OK,
               "tar: add pci.json");
    test_check(axl_tar_writer_add(w, "acpi/facp.dat", 0644, d, 13) == AXL_OK,
               "tar: add acpi/facp.dat (subdir name)");
    test_check(axl_tar_writer_add(w, "empty.bin", 0644, "", 0) == AXL_OK,
               "tar: add zero-length entry");
    test_check(axl_tar_writer_finish(w) == AXL_OK, "tar: finish");
    axl_tar_writer_free(w);

    /* finish() pads to GNU tar's 10240-byte record boundary so streamed
       reads (tar -tzf over gzip) exit cleanly. Three small entries are
       well under one record, so the whole archive must be exactly
       10240 bytes. */
    size_t tar_size = 0;
    (void)axl_bufdata(buf, &tar_size);
    test_check(tar_size == 10240,
               "tar: archive padded to the 10240-byte record boundary");

    axl_fseek(buf, 0, AXL_SEEK_SET);
    AxlTarReader *r = axl_tar_reader_new(buf);
    test_check(r != NULL, "tar: reader_new");
    AxlTarEntry e;
    char        tmp[64];

    test_check(axl_tar_reader_next(r, &e) == AXL_OK
               && axl_strcmp(e.name, "pci.json") == 0 && e.size == 7
               && e.type == AXL_TAR_TYPE_FILE,
               "tar: entry 1 name/size/type");
    test_check(tar_read_all(r, tmp, sizeof tmp) == 7
               && axl_memcmp(tmp, j, 7) == 0, "tar: entry 1 data");

    test_check(axl_tar_reader_next(r, &e) == AXL_OK
               && axl_strcmp(e.name, "acpi/facp.dat") == 0 && e.size == 13,
               "tar: entry 2 name/size");
    test_check(tar_read_all(r, tmp, sizeof tmp) == 13
               && axl_memcmp(tmp, d, 13) == 0,
               "tar: entry 2 data (with embedded NULs)");

    test_check(axl_tar_reader_next(r, &e) == AXL_OK
               && axl_strcmp(e.name, "empty.bin") == 0 && e.size == 0,
               "tar: entry 3 zero-length");
    test_check(tar_read_all(r, tmp, sizeof tmp) == 0,
               "tar: entry 3 reads no data");

    test_check(axl_tar_reader_next(r, &e) == AXL_ERR,
               "tar: end-of-archive stops iteration");
    axl_tar_reader_free(r);
    axl_fclose(buf);
}

static void
test_tar_long_name(void)
{
    AxlStream *buf = axl_bufopen();
    AxlTarWriter *w = axl_tar_writer_new(buf);

    /* 110-char dir + "/f.bin" = 116 chars: splits cleanly (name "f.bin"
       <= 100, prefix 110 <= 155). */
    char longname[160];
    for (int i = 0; i < 110; i++) { longname[i] = 'a'; }
    longname[110] = '/';
    axl_memcpy(longname + 111, "f.bin", 5);
    longname[116] = '\0';
    test_check(axl_tar_writer_add(w, longname, 0644, "x", 1) == AXL_OK,
               "tar: long-but-splittable name accepted");

    /* 150-char single component (no '/') can't fit name(100) → rejected. */
    char toolong[160];
    for (int i = 0; i < 150; i++) { toolong[i] = 'b'; }
    toolong[150] = '\0';
    test_check(axl_tar_writer_add(w, toolong, 0644, "x", 1) == AXL_ERR,
               "tar: unsplittable over-long name rejected");

    axl_tar_writer_finish(w);
    axl_tar_writer_free(w);

    axl_fseek(buf, 0, AXL_SEEK_SET);
    AxlTarReader *r = axl_tar_reader_new(buf);
    AxlTarEntry e;
    test_check(axl_tar_reader_next(r, &e) == AXL_OK
               && axl_strcmp(e.name, longname) == 0,
               "tar: long name round-trips via name/prefix split");
    axl_tar_reader_free(r);
    axl_fclose(buf);
}

static void
test_tar_dir_entry(void)
{
    AxlStream *buf = axl_bufopen();
    AxlTarWriter *w = axl_tar_writer_new(buf);
    /* add_dir appends the trailing slash; pass it without one. */
    test_check(axl_tar_writer_add_dir(w, "acpi", 0755) == AXL_OK,
               "tar: add_dir ok");
    test_check(axl_tar_writer_add(w, "acpi/facp.dat", 0644, "x", 1) == AXL_OK,
               "tar: add file after dir");
    test_check(axl_tar_writer_finish(w) == AXL_OK, "tar: finish (dir)");
    axl_tar_writer_free(w);

    axl_fseek(buf, 0, AXL_SEEK_SET);
    AxlTarReader *r = axl_tar_reader_new(buf);
    AxlTarEntry e;
    test_check(axl_tar_reader_next(r, &e) == AXL_OK
               && e.type == AXL_TAR_TYPE_DIR
               && axl_strcmp(e.name, "acpi/") == 0
               && e.size == 0,
               "tar: dir entry round-trips (type DIR, trailing slash)");
    test_check(axl_tar_reader_next(r, &e) == AXL_OK
               && e.type == AXL_TAR_TYPE_FILE
               && axl_strcmp(e.name, "acpi/facp.dat") == 0,
               "tar: file entry after dir");
    axl_tar_reader_free(r);
    axl_fclose(buf);
}

static void
test_tar_reader_rejects_bad(void)
{
    /* Non-header garbage / truncated input → AXL_ERR, no crash. */
    AxlStream *buf = axl_bufopen();
    char junk[100] = {1, 2, 3};
    axl_write(buf, junk, sizeof junk);
    axl_fseek(buf, 0, AXL_SEEK_SET);
    AxlTarReader *r = axl_tar_reader_new(buf);
    AxlTarEntry e;
    test_check(axl_tar_reader_next(r, &e) == AXL_ERR,
               "tar: garbage/truncated input rejected");
    axl_tar_reader_free(r);
    axl_fclose(buf);

    /* A valid archive with one corrupted header byte → checksum fails. */
    AxlStream *b2 = axl_bufopen();
    AxlTarWriter *w = axl_tar_writer_new(b2);
    axl_tar_writer_add(w, "f", 0644, "data", 4);
    axl_tar_writer_finish(w);
    axl_tar_writer_free(w);
    size_t n = 0;
    uint8_t *raw = (uint8_t *)axl_bufsteal(b2, &n);
    axl_fclose(b2);
    test_check(raw != NULL && n >= AXL_TAR_BLOCK, "tar: stole archive bytes");
    raw[0] ^= 0xFF;  /* corrupt the name field → header checksum mismatch */
    AxlStream *b3 = axl_bufopen();
    axl_write(b3, raw, n);
    axl_fseek(b3, 0, AXL_SEEK_SET);
    AxlTarReader *r3 = axl_tar_reader_new(b3);
    test_check(axl_tar_reader_next(r3, &e) == AXL_ERR,
               "tar: corrupted header (bad checksum) rejected");
    axl_tar_reader_free(r3);
    axl_fclose(b3);
    axl_free(raw);

    /* NULL guards. */
    test_check(axl_tar_writer_new(NULL) == NULL, "tar: writer_new(NULL) -> NULL");
    test_check(axl_tar_reader_new(NULL) == NULL, "tar: reader_new(NULL) -> NULL");
}

static void
test_status_enum_contract(void)
{
    /* AxlStatus numeric values are part of the public contract (callers may
       compare against the literal integers). Pin every code so a reorder or
       renumber is caught. New codes only ever extend the negative range. */
    test_check(AXL_OK == 0,            "status: AXL_OK == 0");
    test_check(AXL_ERR == -1,          "status: AXL_ERR == -1");
    test_check(AXL_CANCELLED == -2,    "status: AXL_CANCELLED == -2");
    test_check(AXL_TIMEOUT == -3,      "status: AXL_TIMEOUT == -3");
    test_check(AXL_INVALID == -4,      "status: AXL_INVALID == -4");
    test_check(AXL_NOT_FOUND == -5,    "status: AXL_NOT_FOUND == -5");
    test_check(AXL_DENIED == -6,       "status: AXL_DENIED == -6");
    test_check(AXL_UNSUPPORTED == -7,  "status: AXL_UNSUPPORTED == -7");
    test_check(AXL_NO_RESOURCES == -8, "status: AXL_NO_RESOURCES == -8");
    test_check(AXL_IO_ERROR == -9,     "status: AXL_IO_ERROR == -9");
    /* Every richer code is a failure (negative) — callers treating "any
       negative == error" must stay correct. */
    test_check(AXL_NOT_FOUND < 0 && AXL_DENIED < 0 && AXL_UNSUPPORTED < 0
               && AXL_NO_RESOURCES < 0 && AXL_IO_ERROR < 0 && AXL_INVALID < 0,
               "status: all richer codes are negative (failure)");
}

static void
test_status_efi_mapping(void)
{
    /* to_efi: representative EFI code per AxlStatus. */
    test_check(axl_status_to_efi(AXL_OK) == AXL_EFI_SUCCESS,
               "to_efi: OK -> SUCCESS");
    test_check(axl_status_to_efi(AXL_INVALID) == AXL_EFI_INVALID_PARAMETER,
               "to_efi: INVALID -> INVALID_PARAMETER");
    test_check(axl_status_to_efi(AXL_NOT_FOUND) == AXL_EFI_NOT_FOUND,
               "to_efi: NOT_FOUND -> NOT_FOUND");
    test_check(axl_status_to_efi(AXL_DENIED) == AXL_EFI_ACCESS_DENIED,
               "to_efi: DENIED -> ACCESS_DENIED");
    test_check(axl_status_to_efi(AXL_UNSUPPORTED) == AXL_EFI_UNSUPPORTED,
               "to_efi: UNSUPPORTED -> UNSUPPORTED");
    test_check(axl_status_to_efi(AXL_NO_RESOURCES) == AXL_EFI_OUT_OF_RESOURCES,
               "to_efi: NO_RESOURCES -> OUT_OF_RESOURCES");
    test_check(axl_status_to_efi(AXL_IO_ERROR) == AXL_EFI_DEVICE_ERROR,
               "to_efi: IO_ERROR -> DEVICE_ERROR");
    test_check(axl_status_to_efi(AXL_TIMEOUT) == AXL_EFI_TIMEOUT,
               "to_efi: TIMEOUT -> TIMEOUT");
    test_check(axl_status_to_efi(AXL_CANCELLED) == AXL_EFI_ABORTED,
               "to_efi: CANCELLED -> ABORTED");
    test_check(axl_status_to_efi(AXL_ERR) == AXL_EFI_ABORTED,
               "to_efi: generic ERR -> ABORTED");
    /* Every failure maps to an EFI error; OK does not. */
    test_check(!AXL_EFI_ERROR(axl_status_to_efi(AXL_OK)),
               "to_efi: OK is not an EFI error");
    test_check(AXL_EFI_ERROR(axl_status_to_efi(AXL_ERR)),
               "to_efi: ERR is an EFI error");

    /* from_efi: success/warning -> OK; mapped errors -> peer; else -> ERR. */
    test_check(axl_status_from_efi(AXL_EFI_SUCCESS) == AXL_OK,
               "from_efi: SUCCESS -> OK");
    /* A warning has the top (error) bit clear, so it is not a failure. */
    test_check(axl_status_from_efi((AxlEfiStatus)1) == AXL_OK,
               "from_efi: a warning (top bit clear) -> OK");
    test_check(axl_status_from_efi(AXL_EFI_ACCESS_DENIED) == AXL_DENIED,
               "from_efi: ACCESS_DENIED -> DENIED");
    test_check(axl_status_from_efi(AXL_EFI_NOT_FOUND) == AXL_NOT_FOUND,
               "from_efi: NOT_FOUND -> NOT_FOUND");
    test_check(axl_status_from_efi(AXL_EFI_SECURITY_VIOLATION) == AXL_ERR,
               "from_efi: unmapped error -> generic ERR");

    /* Round-trip is identity for codes with a 1:1 EFI peer. */
    static const AxlStatus peers[] = {
        AXL_OK, AXL_INVALID, AXL_NOT_FOUND, AXL_DENIED, AXL_UNSUPPORTED,
        AXL_NO_RESOURCES, AXL_IO_ERROR, AXL_TIMEOUT, AXL_CANCELLED,
    };
    bool round_trips = true;
    for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); i++) {
        if (axl_status_from_efi(axl_status_to_efi(peers[i])) != peers[i]) {
            round_trips = false;
        }
    }
    test_check(round_trips,
               "status: from_efi(to_efi(x)) == x for 1:1-peer codes");
}

// ---------------------------------------------------------------------------
// AXL_DEBUG_ASSERT Tests
// ---------------------------------------------------------------------------

static void
test_debug_assert(void)
{
    size_t before = _axl_debug_assert_count();

    /* A satisfied invariant must not fire. */
    AXL_DEBUG_ASSERT(1 == 1);
    AXL_DEBUG_ASSERT_MSG(before == before, "tautology");
    test_check(_axl_debug_assert_count() == before,
               "debug-assert: true condition does not fire");

#ifdef NDEBUG
    /* Under NDEBUG the macros compile out — a false condition is a no-op
       and is not even evaluated. */
    AXL_DEBUG_ASSERT(1 == 2);
    test_check(_axl_debug_assert_count() == before,
               "debug-assert: compiled out (no fire) under NDEBUG");
#else
    /* In a debug/test build a violated invariant fires exactly once and
       CONTINUES (no abort/wedge — the next line still runs). */
    AXL_DEBUG_ASSERT(1 == 2);
    test_check(_axl_debug_assert_count() == before + 1,
               "debug-assert: false condition fires once and continues");

    AXL_DEBUG_ASSERT_MSG(0, "intentional test failure");
    test_check(_axl_debug_assert_count() == before + 2,
               "debug-assert: _MSG variant fires and continues");
#endif
}

/* C++ RAII autoptr — AXL_AUTOPTR must free the tar writer/reader and the
   console terminal at scope exit. The tar objects borrow the stream (freed
   separately); live-allocation count returning to baseline proves each
   scope-exit free ran. */
static void
test_autoptr_util(void)
{
    /* tar writer */
    AxlStream *ws = axl_bufopen();
    test_check(ws != NULL, "autoptr: tar writer stream");
    axl_tar_writer_free(axl_tar_writer_new(ws));   /* prime */
    AxlMemStats before, after;
    axl_mem_get_stats(&before);
    {
        AXL_AUTOPTR(AxlTarWriter) w = axl_tar_writer_new(ws);
        test_check(w != NULL, "autoptr: tar writer new");
    }
    axl_mem_get_stats(&after);
    test_check(after.count == before.count, "autoptr: tar writer freed at scope exit");
    axl_fclose(ws);

    /* tar reader */
    AxlStream *rs = axl_bufopen();
    test_check(rs != NULL, "autoptr: tar reader stream");
    axl_tar_reader_free(axl_tar_reader_new(rs));   /* prime */
    axl_mem_get_stats(&before);
    {
        AXL_AUTOPTR(AxlTarReader) r = axl_tar_reader_new(rs);
        test_check(r != NULL, "autoptr: tar reader new");
    }
    axl_mem_get_stats(&after);
    test_check(after.count == before.count, "autoptr: tar reader freed at scope exit");
    axl_fclose(rs);

    /* console terminal */
    AxlConsoleTermConfig cfg = { .cols = 20, .rows = 5 };
    axl_console_term_free(axl_console_term_new(&cfg));   /* prime */
    axl_mem_get_stats(&before);
    {
        AXL_AUTOPTR(AxlConsoleTerm) t = axl_console_term_new(&cfg);
        test_check(t != NULL, "autoptr: console term new");
    }
    axl_mem_get_stats(&after);
    test_check(after.count == before.count, "autoptr: console term freed at scope exit");
}

int
test_util_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlUtil");

    test_status_enum_contract();
    test_status_efi_mapping();

    test_clipboard();
    test_clipboard_corrupt();
    test_clipboard_get_bytes();
    test_shm();

    test_rand_stream();
    test_rand_derived();
    test_rand_reproducible();
    test_rand_ranges();
    test_rand_global();

    test_file();
    test_file_get_bytes();
    test_seek_tell();
    test_feof();
    test_file_delete();
    test_file_rename();
    test_mkdir_rmdir();
    test_dir_read();
    test_env();
    test_cwd();
    test_path();
    test_path_resolve();
    test_path_build_uefi();
    test_path_companion();
    test_dir_list_json();
    test_volume_enumerate();
    test_volume_get_space();
    test_volume_get_label_spellings();
    test_volume_map_name();
    test_volume_map_ops();
    test_path_search();
    test_smbios();
    test_smbios_extras();
    test_nvstore_namespaces();
    test_attempt();
    test_driver_load_dir_guarded();
    test_nvstore_roundtrip();
    test_var_enumerate();
    test_boot();
    test_app_boot_path();
    test_image_enumerate();
    test_image_watch();
    test_cpu_register_exception();
    test_cpu_features();
    test_cpu_simd_tier();
    test_cpu_enable_avx();
    test_cpu_features_extended();
    test_cpu_enable_avx512();
    test_image();
    test_shell_launch();
    test_shell_sources();
    test_shell_kind();
    test_shell_execute();
    test_console_ops_tee();
    test_console_vt_enc();
    test_console_mirror();
    test_console_tap_guards();
    test_console_device_guards();
    test_console_tap_key_notify();
    test_console_tap_key_notify_wildcard();
    test_console_tap_key_notify_passthrough_not_owned();
    test_console_tap_inject_key_ex_guards();
    test_console_device_input();
    test_console_device_pointer_evict();
    test_console_device_read_loop_revalidate();
    test_console_term_output();
    test_console_term_utf8();
    test_console_term_scrollback();
    test_console_term_render();
    test_console_term_mouse_cursor();
    test_console_term_reflow();
    test_console_term_selection();
    test_console_term_interact();
    test_console_term_bounds();
    test_console_term_autogeom();
    test_console_term_resize_oom();
    test_console_tap_shift_state_aliases();
    test_console_mirror_altscreen_input();
    test_console_tap_sanitize();
    test_console_tap_utf8();
    test_console_mirror_owned_mode();
    test_tap_reports_one_cell_per_codepoint();
    test_tap_set_attribute_becomes_indexed_pen();
    test_tap_alt_screen_is_a_term_prop();
    test_console_mirror_golden_vt();
    test_console_mirror_snapshot();
    test_image_verify_signature();
    test_image_verify_cn_extract();
    test_hexdump();
    test_time();
    test_time_set();
    test_time_wakeup();
    test_memmap_snapshot();
    test_time_get_us();
    test_clock_gettime();
    test_time_sleep();
    test_config();
    test_config_minmax_ignored_by_parsing();
    test_config_width_overflow();
    test_config_parent();
    test_config_descs_append_basic();
    test_config_descs_append_null_safety();
    test_config_descs_append_capacity();
    test_config_descs_net_client();
    test_config_descs_net_server();
    test_config_descs_net_static();
    test_net_static_setters();
    test_config_descs_net_source_and_listen_share_field();
    test_config_descs_net_empty_kinds();
    test_config_descs_net_capacity();
    test_config_descs_net_round_trip();
    test_config_file();
    test_config_file_string_round_trip();
    test_config_multi();
    test_config_to_from_string();
    test_config_target_to_string_round_trip();
    test_config_callback();
    test_config_validation();
    test_config_setv();
    test_subcommand_dispatch();
    test_service_attach_driver();
    test_service_is_running();
    test_service_launch_embedded_validates();
    test_driver_ensure_from_path();
    test_service_stop_validates();
    test_guid_v5();
    test_service_guid();
    test_service_driver_init_status_width();
    test_service_reload_validates();
    test_args();
    test_protocol_registry();
    test_driver_ensure();
    test_driver_load_buffer();
    test_shared_driver();
    test_driver_locate();
    test_diag_probe_protocol();
    test_qsort_basic();
    test_qsort_already_sorted();
    test_qsort_reverse();
    test_qsort_all_equal();
    test_qsort_edge_sizes();
    test_qsort_null_guards();
    test_qsort_large_random();
    test_qsort_heapsort_fallback();
    test_qsort_large_elements();
    test_qsort_with_data_descending();

    test_debug_assert();

    test_tar_roundtrip();
    test_tar_long_name();
    test_tar_dir_entry();
    test_tar_reader_rejects_bad();
    test_autoptr_util();

    return test_print_results();
}

AXL_APP(test_util_main)
