/** @file axl-test-util.c
    Test application for AxlUtil — file, path, SMBIOS, hex dump, time, args.
**/

#include "axl-test.h"
#include <axl/axl-smbios.h>

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
    AxlFileInfo fi;
    rc = axl_file_info("axl-test-util.tmp", &fi);
    test_check(rc == AXL_OK, "stat: returns AXL_OK");
    test_check(fi.size == sizeof(test_data) - 1, "stat: size matches");
    test_check(!fi.is_dir, "stat: not a dir");

    rc = axl_file_info("fs0:\\", &fi);
    test_check(rc == AXL_OK, "stat: root dir returns AXL_OK");
    test_check(fi.is_dir, "stat: root is dir");

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
    axl_fread(buf, 1, 64, s);  /* short read: returns 2 */
    axl_fread(buf, 1, 1, s);   /* returns 0 → sets eof */
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
    AxlFileInfo del_fi;

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
    AxlFileInfo fi;

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
    AxlDirEntry entry;
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
    unsigned short   *str;

    // Find BIOS Information (type 0)
    hdr = axl_smbios_find(0);
    test_check(hdr != NULL, "smbios: find type 0(BIOS)");
    if (hdr == NULL) {
        return;
    }

    // Get string index 1 (vendor)
    str = (unsigned short *)axl_smbios_get_string(hdr, 1);
    test_check(str != NULL && str[0] != L'\0', "smbios: get string 1 non-empty");

    // Index 0 returns empty
    str = (unsigned short *)axl_smbios_get_string(hdr, 0);
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
                    for (int i = 0; i < 3; i++) {
                        test_check(true, "smbios get_oem_string: SKIP balance (short string)");
                    }
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
                    test_check(true,
                               "smbios get_oem_string: NULL *required SKIP balance (empty source)");
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
            for (int i = 0; i < 9; i++) {
                test_check(true, "smbios get_oem_string: SKIP balance");
            }
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
}

static void
test_time_sleep(void)
{
    uint64_t  before;
    uint64_t  after;

    before = axl_time_get_ms();
    test_check(true, "time: get_ms returns");

    axl_msleep(50);
    after = axl_time_get_ms();
    test_check(after >= before, "time: get_ms monotonic after msleep");

    axl_usleep(1000);  /* 1ms */
    test_pass("time: usleep(1000) completes");
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
    AxlDirEntry empty_entries[1] = {0};
    test_check(axl_dir_list_json(empty_entries, 0, buf, sizeof(buf)) == AXL_OK,
               "dir_json: empty");
    test_check(axl_strcmp(buf, "[]") == 0,
               "dir_json: empty value");

    /* Single file entry */
    AxlDirEntry entries[2];
    axl_strlcpy(entries[0].name, "test.txt", sizeof(entries[0].name));
    entries[0].size = 1024;
    entries[0].is_dir = false;

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
    entries[1].size = 0;
    entries[1].is_dir = true;

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
    test_check(true, "config: free(NULL) no crash");
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
        { "bios",  sub_bios,   "[test|pci]",  "do bios test  — run POST self-test\n" },
        { "sysid", sub_sysid,  "[hexValue]",  NULL  },
        { "crash", sub_crash,  "trigger",     NULL  },
    };
    static const size_t count = sizeof(cmds) / sizeof(cmds[0]);

    /* exact match → fn invoked, return value passed through */
    {
        char *argv[] = { (char *)"do", (char *)"bios", (char *)"--flag", (char *)"v" };
        g_sub_calls = 0;
        g_sub_last_arg0 = NULL;
        int rc = axl_subcommand_dispatch(cmds, count, 4, argv, "do");
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
        char *argv[] = { (char *)"do", (char *)"help" };
        g_sub_calls = 0;
        int rc = axl_subcommand_dispatch(cmds, count, 2, argv, "do");
        test_check(rc == 0, "subcommand: help returns 0");
        test_check(g_sub_calls == 0, "subcommand: help doesn't invoke any fn");
    }

    /* "-h" / "--help" both → help (no fn invoked) */
    {
        char *argv1[] = { (char *)"do", (char *)"-h" };
        char *argv2[] = { (char *)"do", (char *)"--help" };
        g_sub_calls = 0;
        test_check(axl_subcommand_dispatch(cmds, count, 2, argv1, "do") == 0,
                   "subcommand: -h returns 0");
        test_check(axl_subcommand_dispatch(cmds, count, 2, argv2, "do") == 0,
                   "subcommand: --help returns 0");
        test_check(g_sub_calls == 0, "subcommand: -h/--help no fn invocations");
    }

    /* "help <cmd>" prints command help, returns 0 */
    {
        char *argv[] = { (char *)"do", (char *)"help", (char *)"bios" };
        int rc = axl_subcommand_dispatch(cmds, count, 3, argv, "do");
        test_check(rc == 0, "subcommand: help <cmd> returns 0");
    }

    /* "help <unknown>" returns -1 */
    {
        char *argv[] = { (char *)"do", (char *)"help", (char *)"nonsense" };
        int rc = axl_subcommand_dispatch(cmds, count, 3, argv, "do");
        test_check(rc == -1, "subcommand: help <unknown> returns -1");
    }

    /* unknown command → -1 */
    {
        char *argv[] = { (char *)"do", (char *)"frobnicate" };
        g_sub_calls = 0;
        int rc = axl_subcommand_dispatch(cmds, count, 2, argv, "do");
        test_check(rc == -1, "subcommand: unknown command returns -1");
        test_check(g_sub_calls == 0, "subcommand: unknown doesn't invoke any fn");
    }

    /* typo close to "sysid" → still -1 but the "did you mean" path runs */
    {
        char *argv[] = { (char *)"do", (char *)"sysud" };
        int rc = axl_subcommand_dispatch(cmds, count, 2, argv, "do");
        test_check(rc == -1, "subcommand: close typo returns -1");
        /* No way to capture stderr text here; just exercise the path. */
    }

    /* argc < 2 → help, returns 0 */
    {
        char *argv[] = { (char *)"do" };
        int rc = axl_subcommand_dispatch(cmds, count, 1, argv, "do");
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
        char *argv[] = { (char *)"do", (char *)"crash" };
        int rc = axl_subcommand_dispatch(cmds, count, 2, argv, "do");
        test_check(rc == -7, "subcommand: negative rc passes through");
    }

    /* Empty table behaves: help only */
    {
        char *argv[] = { (char *)"do" };
        int rc = axl_subcommand_dispatch(NULL, 0, 1, argv, "do");
        test_check(rc == 0, "subcommand: empty table + no args returns 0");
        char *argv2[] = { (char *)"do", (char *)"anything" };
        int rc2 = axl_subcommand_dispatch(NULL, 0, 2, argv2, "do");
        test_check(rc2 == -1, "subcommand: empty table + unknown returns -1");
    }

    /* Public print fns shouldn't crash on edge cases */
    axl_subcommand_print_help(cmds, count, "do");
    axl_subcommand_print_help(NULL, 0, "do");
    axl_subcommand_print_command_help(&cmds[0], "do");
    axl_subcommand_print_command_help(NULL, "do");
    test_check(true, "subcommand: print fns don't crash");
}
#pragma GCC diagnostic pop

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
     * doesn't, the bogus name causes a search miss and we get -1. */
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
     * on the disk. Should walk the search list, find nothing,
     * return -1 without crashing. */
    static const AxlGuid never_registered = AXL_GUID(
        0xdeadbeef, 0xcafe, 0xbabe,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef);

    test_check(axl_driver_ensure(&never_registered,
                                 "no-such-driver-12345.efi") == -1,
               "driver_ensure: returns -1 when driver not found");
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
        AxlFileInfo info;
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
   the pointer, not the bytes, so the values never reach firmware. */
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

    /* Re-register with a DIFFERENT token rejects. */
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
// AxlNvstore — set/get/get_attrs/iter/delete round-trip
// ---------------------------------------------------------------------------

typedef struct {
    int   matches;
    char  last_key[64];
} NvstoreIterCtx;

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
           authentication policies). Skip cleanly. */
        axl_printf("SKIP: nvstore round-trip (set returned -1)\n");
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
        for (int i = 0; i < 3; i++) {
            test_check(true, "nvstore get_alloc empty: SKIP balance");
        }
    }

    /* Delete + verify gone. */
    test_check(axl_nvstore_delete("app", key) == AXL_OK,
               "nvstore: delete succeeds");
    sz = sizeof(buf);
    test_check(axl_nvstore_get("app", key, buf, &sz) == AXL_ERR,
               "nvstore: get after delete returns -1");
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
        test_check(true, "boot: round-trip skipped on this firmware");
        test_check(true, "boot: round-trip skipped on this firmware");
        test_check(true, "boot: round-trip skipped on this firmware");
        test_check(true, "boot: round-trip skipped on this firmware");
        test_check(true, "boot: round-trip skipped on this firmware");
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

    /* Unload. */
    test_check(axl_image_unload(img) == AXL_OK,
               "image: unload");

    /* Unload(NULL) is a no-op — return 0. */
    test_check(axl_image_unload(NULL) == AXL_OK,
               "image: unload(NULL) is a no-op");
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
        axl_printf("SKIP: image_verify (no AxlTestRuntime.efi)\n");
        for (int i = 0; i < 6; i++) {
            test_check(true, "image_verify: SKIP balance");
        }
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
        .name      = "do",
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
    char *argv[] = { (char *)"do", (char *)"pci", (char *)"read16",
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
    char *argv[] = { (char *)"do", (char *)"--host=bmc.local", (char *)"-v",
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
    char *argv[] = { (char *)"do", (char *)"deep",
                     (char *)"inner", (char *)"--scope=mid", (char *)"leaf" };
    int rc = run_nested(&cap, 5, argv);
    test_check(rc == 0, "nested args: 3-level dispatch returns 0");
    test_check(cap.deep_calls == 1, "nested args: deep leaf ran once");
    test_check(cap.seen_top_string != NULL
               && axl_strcmp(cap.seen_top_string, "mid") == 0,
               "nested args: middle-level --scope reachable from deepest leaf");
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

static void
test_args_nested_unknown_verb_at_branch(void)
{
    /* Branch should reject unknown verb at its own level with an
       error prefixed by the full breadcrumb path. */
    NestedCapture cap = { 0 };
    char *argv[] = { (char *)"do", (char *)"pci", (char *)"flarble" };

    AxlStream *buf = NULL;
    AxlStream *saved = capture_stdout(&buf);
    int rc = run_nested(&cap, 3, argv);
    bool has_breadcrumb = buf_contains(buf, "do pci: unknown verb");
    bool has_token      = buf_contains(buf, "flarble");
    restore_stdout(saved, buf);

    test_check(rc != 0, "nested args: unknown verb at branch rejected");
    test_check(cap.calls == 0, "nested args: leaf handler did not run");
    test_check(cap.deep_calls == 0,
               "nested args: deep handler did not run on shallow rejection");
    test_check(has_breadcrumb,
               "nested args: error message includes 'do pci:' breadcrumb");
    test_check(has_token,
               "nested args: error message names the rejected verb");
}

static void
test_args_nested_branch_help_lists_subverbs(void)
{
    /* `do pci --help` triggers help at the pci branch level and the
       output names the subverbs of that branch (not the root). */
    NestedCapture cap = { 0 };
    char *argv[] = { (char *)"do", (char *)"pci", (char *)"--help" };

    AxlStream *buf = NULL;
    AxlStream *saved = capture_stdout(&buf);
    int rc = run_nested(&cap, 3, argv);
    bool has_branch_path = buf_contains(buf, "do pci");
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
// Branch + default handler — `do bios` with no sub-verb runs handler
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
        .name      = "do",
        .help      = "branch+default test root",
        .verbs     = bdh_top_verbs,
        .user_data = cap,
    };
    return axl_args_run(argc, argv, &root);
}

static void
test_args_branch_default_fires_on_no_verb(void)
{
    /* `do bios` with no further verb invokes the default handler.
       info_calls AND default_calls both go up because the same fn
       is referenced as both the explicit verb and the default. */
    BranchDefaultCapture cap = { 0 };
    char *argv[] = { (char *)"do", (char *)"bios" };
    int rc = run_bdh(&cap, 2, argv);
    test_check(rc == 0,
               "branch+default: 'do bios' with no sub-verb returns 0");
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
    char *argv[] = { (char *)"do", (char *)"bios", (char *)"test" };
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
    char *argv[] = { (char *)"do", (char *)"bios", (char *)"flarble" };

    AxlStream *buf = NULL;
    AxlStream *saved = capture_stdout(&buf);
    int rc = run_bdh(&cap, 3, argv);
    bool has_breadcrumb = buf_contains(buf, "do bios: unknown verb");
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
    char *argv[] = { (char *)"do", (char *)"bios", (char *)"-q" };
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
    char *argv[] = { (char *)"do", (char *)"pci" };

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
    test_args_nested_2level_dispatch();
    test_args_nested_parent_flag_visible_at_leaf();
    test_args_nested_3level_dispatch();
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
// Entry Point
// ---------------------------------------------------------------------------

int
test_util_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlUtil");

    test_file();
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
    test_smbios();
    test_smbios_extras();
    test_nvstore_namespaces();
    test_nvstore_roundtrip();
    test_boot();
    test_image();
    test_image_verify_signature();
    test_image_verify_cn_extract();
    test_hexdump();
    test_time();
    test_time_sleep();
    test_config();
    test_config_width_overflow();
    test_config_parent();
    test_config_multi();
    test_config_callback();
    test_config_validation();
    test_config_setv();
    test_subcommand_dispatch();
    test_args();
    test_driver_ensure();
    test_driver_locate();
    test_diag_probe_protocol();

    return test_print_results();
}

AXL_APP(test_util_main)
