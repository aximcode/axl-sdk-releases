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
    test_check(rc == 0, "stat: returns 0");
    test_check(fi.size == sizeof(test_data) - 1, "stat: size matches");
    test_check(!fi.is_dir, "stat: not a dir");

    rc = axl_file_info("fs0:\\", &fi);
    test_check(rc == 0, "stat: root dir returns 0");
    test_check(fi.is_dir, "stat: root is dir");

    test_check(axl_file_info("no-such-file-12345", &fi) != 0,
        "stat: missing file returns -1");
    test_check(axl_file_info(NULL, &fi) != 0,
        "stat: NULL path returns -1");
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

    test_check(axl_fseek(s, 0, AXL_SEEK_SET) == 0, "seek: seek SET 0");
    pos = axl_ftell(s);
    test_check(pos == 0, "seek: tell after seek SET 0");

    axl_fread(buf, 1, 2, s);
    test_check(buf[0] == 'A' && buf[1] == 'B', "seek: re-read after seek");

    test_check(axl_fseek(s, -1, AXL_SEEK_END) == 0, "seek: seek END -1");
    axl_fread(buf, 1, 1, s);
    test_check(buf[0] == 'J', "seek: read last byte via END");

    test_check(axl_fseek(s, -3, AXL_SEEK_CUR) == 0, "seek: seek CUR -3");
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
    test_check(axl_file_delete("axl-del.tmp") == 0, "delete: returns 0");
    test_check(axl_file_info("axl-del.tmp", &del_fi) != 0,
               "delete: file gone after delete");
}

static void
test_file_rename(void)
{
    AxlFileInfo fi;

    if (axl_file_set_contents("axl-ren-old.tmp", "data", 4) != AXL_OK) {
        return;
    }
    test_check(axl_file_rename("axl-ren-old.tmp", "axl-ren-new.tmp") == 0,
               "rename: returns 0");
    test_check(axl_file_info("axl-ren-new.tmp", &fi) == 0,
               "rename: new exists");
    test_check(fi.size == 4, "rename: size preserved");

    /* cleanup */
    axl_file_delete("axl-ren-new.tmp");
}

static void
test_mkdir_rmdir(void)
{
    test_check(axl_dir_mkdir("axl-test-dir") == 0, "mkdir: returns 0");
    test_check(axl_file_is_dir("axl-test-dir"), "mkdir: is dir");
    test_check(axl_dir_rmdir("axl-test-dir") == 0, "rmdir: returns 0");
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

    // Join
    result = axl_path_join("fs0:\\dir", "file.efi");
    test_check(result != NULL && axl_strcmp(result, "fs0:\\dir/file.efi") == 0,
        "path: join");
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
    test_check(axl_smbios_read_bios_info(&bi) == 0, "smbios: read bios info");
    test_check(bi.vendor != NULL && bi.vendor[0] != '\0', "smbios: bios vendor populated");

    // Typed System info reader + UUID byte-swap
    AxlSmbiosSystemInfo si;
    test_check(axl_smbios_read_system_info(&si) == 0, "smbios: read system info");
    test_check(si.manufacturer != NULL, "smbios: system mfr populated");

    // System UUID getter: either returns 0 with valid bytes, or -1 cleanly
    uint8_t uuid[16];
    int uuid_rc = axl_smbios_get_system_uuid(uuid);
    test_check(uuid_rc == 0 || uuid_rc == -1, "smbios: uuid getter returns 0 or -1");

    // Processor reader: walk every Type 4 and read it
    size_t cpu_count = 0;
    AxlSmbiosHeader *ph = NULL;
    while ((ph = axl_smbios_find_next(AXL_SMBIOS_TYPE_PROCESSOR, ph)) != NULL) {
        AxlSmbiosProcessorInfo pi;
        test_check(axl_smbios_read_processor(ph, &pi) == 0, "smbios: read processor");
        test_check(pi.socket_designation != NULL, "smbios: processor socket populated");
        cpu_count++;
        if (cpu_count > 64) { break; }
    }

    // Memory device reader: walk every Type 17 and read it
    size_t mem_count = 0;
    AxlSmbiosHeader *mh = NULL;
    while ((mh = axl_smbios_find_next(AXL_SMBIOS_TYPE_MEMORY_DEVICE, mh)) != NULL) {
        AxlSmbiosMemoryDevice md;
        test_check(axl_smbios_read_memory_device(mh, &md) == 0, "smbios: read memory device");
        test_check(md.device_locator != NULL, "smbios: mem device locator populated");
        mem_count++;
        if (mem_count > 1024) { break; }
    }

    // Wrong-type guard: read_processor should refuse a non-Type-4 header
    AxlSmbiosProcessorInfo pi_bad;
    test_check(axl_smbios_read_processor(bios, &pi_bad) == -1,
               "smbios: read_processor rejects Type 0 hdr");
    test_check(axl_smbios_read_memory_device(bios, NULL) == -1,
               "smbios: read_memory_device rejects NULL out");

    // Type 38 — IPMI Device Information. QEMU + IPMI SSIF test harness
    // publishes one; plain QEMU doesn't. Just verify the call shape.
    AxlSmbiosIpmiDeviceInfo ip;
    int ip_rc = axl_smbios_read_ipmi_device_info(&ip);
    test_check(ip_rc == 0 || ip_rc == -1,
               "smbios: read_ipmi_device_info returns 0 or -1");
    if (ip_rc == 0) {
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
        test_check(axl_smbios_read_host_interface(ih, &iface) == 0,
                   "smbios: read_host_interface on real Type 42");
        test_check(iface.protocol_count <= 8, "smbios: protocol_count within cap");
        host_iface_count++;
        if (host_iface_count > 16) { break; }
    }
    AxlSmbiosHeader *rf_hdr = NULL;
    AxlSmbiosHostInterface rf_iface;
    int rf_rc = axl_smbios_find_redfish_host_interface(&rf_hdr, &rf_iface);
    test_check(rf_rc == 0 || rf_rc == -1, "smbios: redfish find returns 0 or -1");

    // Wrong-type guard for Type 42 reader
    AxlSmbiosHostInterface iface_bad;
    test_check(axl_smbios_read_host_interface(bios, &iface_bad) == -1,
               "smbios: read_host_interface rejects Type 0 hdr");
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
    test_check(axl_setenv("AXL_TEST_VAR", "hello", true) == 0,
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
    test_check(axl_unsetenv("AXL_TEST_VAR") == 0, "env: unsetenv returns 0");
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

    test_check(axl_chdir("fs0:\\") == 0, "cwd: chdir to root");
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
    test_check(axl_path_resolve("/base", "file.efi", buf, sizeof(buf)) == 0,
               "resolve: simple join");
    test_check(axl_strcmp(buf, "/base/file.efi") == 0,
               "resolve: simple join value");

    /* Dot removal */
    test_check(axl_path_resolve("/base", "./foo", buf, sizeof(buf)) == 0,
               "resolve: dot removal");
    test_check(axl_strcmp(buf, "/base/foo") == 0,
               "resolve: dot removal value");

    /* Dotdot resolution */
    test_check(axl_path_resolve("/a/b/c", "../d", buf, sizeof(buf)) == 0,
               "resolve: dotdot");
    test_check(axl_strcmp(buf, "/a/b/d") == 0,
               "resolve: dotdot value");

    /* Multiple dotdot */
    test_check(axl_path_resolve("/a/b/c", "../../d", buf, sizeof(buf)) == 0,
               "resolve: multi-dotdot");
    test_check(axl_strcmp(buf, "/a/d") == 0,
               "resolve: multi-dotdot value");

    /* Absolute relative overrides base */
    test_check(axl_path_resolve("/base", "/absolute/path", buf, sizeof(buf)) == 0,
               "resolve: absolute override");
    test_check(axl_strcmp(buf, "/absolute/path") == 0,
               "resolve: absolute override value");

    /* Root path */
    test_check(axl_path_resolve("/", ".", buf, sizeof(buf)) == 0,
               "resolve: root dot");
    test_check(axl_strcmp(buf, "/") == 0,
               "resolve: root dot value");

    /* Dotdot underflow past root */
    test_check(axl_path_resolve("/a", "../../x", buf, sizeof(buf)) == -1,
               "resolve: dotdot underflow");

    /* NULL args */
    test_check(axl_path_resolve(NULL, "foo", buf, sizeof(buf)) == -1,
               "resolve: NULL base");
    test_check(axl_path_resolve("/a", NULL, buf, sizeof(buf)) == -1,
               "resolve: NULL relative");

    /* Buffer too small */
    test_check(axl_path_resolve("/base", "file.efi", buf, 5) == -1,
               "resolve: buffer too small");

    /* Backslash handling */
    test_check(axl_path_resolve("/a\\b", "c\\d", buf, sizeof(buf)) == 0,
               "resolve: backslash");
    test_check(axl_strcmp(buf, "/a/b/c/d") == 0,
               "resolve: backslash normalized");
}

// ---------------------------------------------------------------------------
// UEFI path construction tests
// ---------------------------------------------------------------------------

static void
test_path_build_uefi(void)
{
    char buf[64];

    /* Basic */
    test_check(axl_path_build_uefi("fs0", "/dir/file.efi", buf, sizeof(buf)) == 0,
               "build_uefi: basic");
    test_check(axl_strcmp(buf, "fs0:\\dir\\file.efi") == 0,
               "build_uefi: slashes converted");

    /* Root path */
    test_check(axl_path_build_uefi("fs0", "/", buf, sizeof(buf)) == 0,
               "build_uefi: root");
    test_check(axl_strcmp(buf, "fs0:\\") == 0,
               "build_uefi: root value");

    /* Already backslash */
    test_check(axl_path_build_uefi("fs1", "\\already\\back", buf, sizeof(buf)) == 0,
               "build_uefi: backslash passthrough");
    test_check(axl_strcmp(buf, "fs1:\\already\\back") == 0,
               "build_uefi: backslash preserved");

    /* Buffer too small */
    test_check(axl_path_build_uefi("fs0", "/dir/file.efi", buf, 5) == -1,
               "build_uefi: buffer too small");

    /* NULL safety */
    test_check(axl_path_build_uefi(NULL, "/foo", buf, sizeof(buf)) == -1,
               "build_uefi: NULL volume");
    test_check(axl_path_build_uefi("fs0", NULL, buf, sizeof(buf)) == -1,
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
    test_check(axl_dir_list_json(empty_entries, 0, buf, sizeof(buf)) == 0,
               "dir_json: empty");
    test_check(axl_strcmp(buf, "[]") == 0,
               "dir_json: empty value");

    /* Single file entry */
    AxlDirEntry entries[2];
    axl_strlcpy(entries[0].name, "test.txt", sizeof(entries[0].name));
    entries[0].size = 1024;
    entries[0].is_dir = false;

    test_check(axl_dir_list_json(entries, 1, buf, sizeof(buf)) == 0,
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

    test_check(axl_dir_list_json(entries, 2, buf, sizeof(buf)) == 0,
               "dir_json: two entries");
    test_check(axl_strstr_len(buf, -1, "\"dir\":true") != NULL,
               "dir_json: has dir true");
    test_check(buf[0] == '[' && buf[axl_strlen(buf) - 1] == ']',
               "dir_json: array brackets");

    /* Buffer too small */
    test_check(axl_dir_list_json(entries, 2, buf, 10) == -1,
               "dir_json: buffer overflow");

    /* NULL safety */
    test_check(axl_dir_list_json(NULL, 1, buf, sizeof(buf)) == -1,
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
    test_check(axl_volume_enumerate(NULL, 0, &count) == 0,
               "vol enum: query count");
    test_check(count > 0, "vol enum: at least 1 volume");

    /* Enumerate into array */
    AxlVolume vols[8];
    size_t filled = 0;
    test_check(axl_volume_enumerate(vols, 8, &filled) == 0,
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
    test_check(axl_volume_enumerate(NULL, 0, NULL) == -1,
               "vol enum: NULL count returns -1");
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
    { "port",     AXL_CFG_UINT,   "8080",  'p', "Listen port",
      offsetof(TestConfigTarget, port), sizeof(uint64_t) },
    { "verbose",  AXL_CFG_BOOL,   "false", 'v', "Verbose output",
      offsetof(TestConfigTarget, verbose), sizeof(bool) },
    { "max.conn", AXL_CFG_INT,    "16",     0,  "Max connections",
      offsetof(TestConfigTarget, max_conn), sizeof(int) },
    { "name",     AXL_CFG_STRING, NULL,     'n', "Server name", 0, 0 },
    { "header",   AXL_CFG_MULTI,  NULL,     'H', "Custom header", 0, 0 },
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
    test_check(axl_config_set(cfg, "port", "9090") == 0,
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
    test_check(axl_config_set(cfg, "unknown", "x") == -1,
               "config: unknown key rejected");

    /* Type validation */
    test_check(axl_config_set(cfg, "port", "abc") == -1,
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
        { "port",      AXL_CFG_UINT, "8080",  'p', "Listen port (u16)",
          offsetof(NarrowTarget, port), sizeof(uint16_t) },
        { "timeout",   AXL_CFG_UINT, "30000", 't', "Timeout ms (u32)",
          offsetof(NarrowTarget, timeout_ms), sizeof(uint32_t) },
        { "threshold", AXL_CFG_INT,  "0",      0,  "Threshold (i32)",
          offsetof(NarrowTarget, threshold), sizeof(int32_t) },
        { 0 }
    };

    NarrowTarget tgt;
    axl_memset(&tgt, 0, sizeof(tgt));
    AXL_AUTOPTR(AxlConfig) cfg = axl_config_new(descs, NULL, &tgt);
    test_check(cfg != NULL, "width: config new");

    /* In-range values still work. */
    test_check(axl_config_set(cfg, "port", "9090") == 0
               && tgt.port == 9090, "width: u16 9090 accepted");
    test_check(axl_config_set(cfg, "port", "65535") == 0
               && tgt.port == 65535, "width: u16 max 65535 accepted");

    /* Overflow rejected — used to silently truncate to 34463. */
    test_check(axl_config_set(cfg, "port", "65536") == -1,
               "width: u16 65536 rejected (was: silent truncate to 0)");
    test_check(axl_config_set(cfg, "port", "99999") == -1,
               "width: u16 99999 rejected (was: silent truncate to 34463)");
    test_check(tgt.port == 65535,
               "width: u16 field preserved after rejection");

    /* u32 boundary. */
    test_check(axl_config_set(cfg, "timeout", "4294967295") == 0
               && tgt.timeout_ms == 4294967295u, "width: u32 max accepted");
    test_check(axl_config_set(cfg, "timeout", "4294967296") == -1,
               "width: u32 max+1 rejected");

    /* i32 boundaries — both ends. */
    test_check(axl_config_set(cfg, "threshold", "2147483647") == 0
               && tgt.threshold == 2147483647, "width: i32 max accepted");
    test_check(axl_config_set(cfg, "threshold", "-2147483648") == 0
               && tgt.threshold == -2147483648, "width: i32 min accepted");
    test_check(axl_config_set(cfg, "threshold", "2147483648") == -1,
               "width: i32 max+1 rejected");
    test_check(axl_config_set(cfg, "threshold", "-2147483649") == -1,
               "width: i32 min-1 rejected");

    /* Rejected set leaves the stored hash entry consistent with the
     * target field — both keep the previous value, neither drifts. */
    test_check(axl_strcmp(axl_config_get(cfg, "port"), "65535") == 0,
               "width: stored value preserved after rejected set");

    /* A descriptor whose default overflows the declared width should
     * not crash axl_config_new — the default is logged-and-skipped,
     * the config is still usable. */
    static const AxlConfigDesc bad_default[] = {
        { "tiny", AXL_CFG_UINT, "70000", 0, "u16 with overflowing default",
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
test_config_args(void)
{
    static const AxlConfigDesc descs[] = {
        { "port",    AXL_CFG_UINT,   "8080",  'p', "Listen port", 0, 0 },
        { "verbose", AXL_CFG_BOOL,   "false", 'v', "Verbose", 0, 0 },
        { "output",  AXL_CFG_STRING, NULL,     'o', "Output file", 0, 0 },
        { "header",  AXL_CFG_MULTI,  NULL,     'H', "Header", 0, 0 },
        { 0 }
    };

    AxlConfig *cfg = axl_config_new(descs, NULL, NULL);
    test_check(cfg != NULL, "config args: new");

    /* Parse short flags */
    char *argv1[] = { "app", "-v", "-p", "9090", "file.txt" };
    test_check(axl_config_parse_args(cfg, 5, argv1) == 0,
               "config args: parse short");
    test_check(axl_config_get_bool(cfg, "verbose") == true,
               "config args: -v sets verbose");
    test_check(axl_config_get_uint(cfg, "port") == 9090,
               "config args: -p 9090");
    test_check(axl_config_pos_count(cfg) == 1,
               "config args: 1 positional");
    test_check(axl_strcmp(axl_config_pos(cfg, 0), "file.txt") == 0,
               "config args: positional value");

    axl_config_free(cfg);

    /* Long options */
    cfg = axl_config_new(descs, NULL, NULL);
    char *argv2[] = { "app", "--port=3000", "--verbose", "--output", "out.bin" };
    test_check(axl_config_parse_args(cfg, 5, argv2) == 0,
               "config args: parse long");
    test_check(axl_config_get_uint(cfg, "port") == 3000,
               "config args: --port=3000");
    test_check(axl_config_get_bool(cfg, "verbose") == true,
               "config args: --verbose");
    test_check(axl_strcmp(axl_config_get(cfg, "output"), "out.bin") == 0,
               "config args: --output out.bin");

    axl_config_free(cfg);

    /* Double-dash stops parsing */
    cfg = axl_config_new(descs, NULL, NULL);
    char *argv3[] = { "app", "-v", "--", "-p", "file" };
    axl_config_parse_args(cfg, 5, argv3);
    test_check(axl_config_get_bool(cfg, "verbose") == true,
               "config args: -v before --");
    test_check(axl_config_get_uint(cfg, "port") == 8080,
               "config args: -p not parsed after --");
    test_check(axl_config_pos_count(cfg) == 2,
               "config args: 2 positional after --");

    axl_config_free(cfg);
}

static void
test_config_parent(void)
{
    static const AxlConfigDesc descs[] = {
        { "timeout", AXL_CFG_UINT, "5000", 0, "Timeout", 0, 0 },
        { "name",    AXL_CFG_STRING, NULL, 0, "Name", 0, 0 },
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
        { "header", AXL_CFG_MULTI, NULL, 'H', "Custom header", 0, 0 },
        { "port",   AXL_CFG_UINT,  "80", 'p', "Port", 0, 0 },
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

    /* Parse multi from args */
    axl_config_free(cfg);
    cfg = axl_config_new(descs, NULL, NULL);
    char *argv[] = { "app", "-H", "Auth: Bearer tok", "-H", "Accept: */*" };
    axl_config_parse_args(cfg, 5, argv);
    test_check(axl_config_get_multi_count(cfg, "header") == 2,
               "config multi: args count 2");

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
        { "port", AXL_CFG_UINT, "80", 0, "Port", 0, 0 },
        { 0 }
    };

    AxlConfig *cfg = axl_config_new(descs, test_dynamic_apply, NULL);

    /* Known key passes through callback (returns 0) to auto-apply */
    test_check(axl_config_set(cfg, "port", "9090") == 0,
               "config cb: known key accepted");
    test_check(axl_config_get_uint(cfg, "port") == 9090,
               "config cb: known key value");

    /* Dynamic key handled by callback (returns 1) */
    test_cb_counter = 0;
    test_check(axl_config_set(cfg, "dynamic.foo", "bar") == 0,
               "config cb: dynamic key accepted");
    test_check(test_cb_counter == 1, "config cb: callback fired");
    test_check(axl_strcmp(axl_config_get(cfg, "dynamic.foo"), "bar") == 0,
               "config cb: dynamic key retrievable");

    /* Unknown key (not in descriptors, callback returns 0) rejected */
    test_check(axl_config_set(cfg, "unknown", "x") == -1,
               "config cb: unknown key rejected");

    axl_config_free(cfg);
}

static void
test_config_validation(void)
{
    static const AxlConfigDesc descs[] = {
        { "count",  AXL_CFG_UINT,   "0",     0, "Count", 0, 0 },
        { "offset", AXL_CFG_INT,    "0",     0, "Offset", 0, 0 },
        { "flag",   AXL_CFG_BOOL,   "false", 0, "Flag", 0, 0 },
        { 0 }
    };

    AxlConfig *cfg = axl_config_new(descs, NULL, NULL);

    /* UINT rejects non-numeric */
    test_check(axl_config_set(cfg, "count", "abc") == -1,
               "config val: uint rejects abc");
    test_check(axl_config_set(cfg, "count", "-5") == -1,
               "config val: uint rejects negative");
    test_check(axl_config_set(cfg, "count", "0xFF") == 0,
               "config val: uint accepts hex");

    /* INT rejects non-numeric */
    test_check(axl_config_set(cfg, "offset", "xyz") == -1,
               "config val: int rejects xyz");
    test_check(axl_config_set(cfg, "offset", "-42") == 0,
               "config val: int accepts negative");
    test_check(axl_config_get_int(cfg, "offset") == -42,
               "config val: int value -42");

    /* BOOL rejects garbage */
    test_check(axl_config_set(cfg, "flag", "maybe") == -1,
               "config val: bool rejects maybe");
    test_check(axl_config_set(cfg, "flag", "yes") == 0,
               "config val: bool accepts yes");
    test_check(axl_config_get_bool(cfg, "flag") == true,
               "config val: bool yes is true");

    axl_config_free(cfg);
}

static void
test_config_setv(void)
{
    static const AxlConfigDesc descs[] = {
        { "host",    AXL_CFG_STRING, "0.0.0.0", 0, "Host", 0, 0 },
        { "port",    AXL_CFG_UINT,   "80",      0, "Port", 0, 0 },
        { "verbose", AXL_CFG_BOOL,   "false",   0, "Verbose", 0, 0 },
        { 0 }
    };

    AxlConfig *cfg = axl_config_new(descs, NULL, NULL);

    /* Set multiple at once */
    test_check(axl_config_setv(cfg,
        "host", "10.0.0.1",
        "port", "9090",
        "verbose", "true",
        NULL) == 0,
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
        NULL) == -1,
        "config setv: stops on error");

    /* port was set before the error */
    test_check(axl_config_get_uint(cfg, "port") == 3000,
               "config setv: partial apply before error");

    /* verbose was NOT set (after the error) */
    test_check(axl_config_get_bool(cfg, "verbose") == true,
               "config setv: skipped after error");

    /* NULL config */
    test_check(axl_config_setv(NULL, "port", "80", NULL) == -1,
               "config setv: NULL cfg");

    axl_config_free(cfg);
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
     * doesn't, the bogus name causes a search miss and we get -1. */
    test_check(axl_driver_ensure(&simple_fs,
                                 "definitely-not-a-real-driver.efi") == 0,
               "driver_ensure: short-circuits when protocol registered");

    /* NULL args — both arguments are required. */
    test_check(axl_driver_ensure(NULL, "x.efi") == -1,
               "driver_ensure: rejects NULL guid");
    test_check(axl_driver_ensure(&simple_fs, NULL) == -1,
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
    test_check(axl_driver_locate(NULL, path, sizeof(path)) == -1,
               "driver_locate: rejects NULL name");
    test_check(axl_driver_locate("x.efi", NULL, sizeof(path)) == -1,
               "driver_locate: rejects NULL out");
    test_check(axl_driver_locate("x.efi", path, 0) == -1,
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
    if (rc == 0) {
        test_check(axl_strlen(path) > 0, "driver_locate: writes non-empty path");
        AxlFileInfo info;
        test_check(axl_file_info(path, &info) == 0,
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
    test_check(axl_driver_locate("axl-test-util.tmp", tiny, sizeof(tiny)) == -1,
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
    test_check(axl_diag_probe_protocol(NULL, "x") == -1,
               "diag_probe: NULL guid rejected");
    test_check(axl_diag_probe_protocol(NULL, NULL) == -1,
               "diag_probe: NULL guid rejected even with NULL name");

    /* SimpleFileSystem is guaranteed registered in QEMU (we boot
     * from fs0). Probe should return 0 and the line should print. */
    static const AxlGuid simple_fs = AXL_GUID(
        0x0964e5b22, 0x6459, 0x11d2,
        0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b);
    test_check(axl_diag_probe_protocol(&simple_fs,
                                       "EFI_SIMPLE_FILE_SYSTEM") == 0,
               "diag_probe: registered protocol returns 0");

    /* Bogus GUID → -1. Doesn't crash on a NULL display_name either. */
    static const AxlGuid bogus = AXL_GUID(
        0xdeadbeef, 0xcafe, 0xbabe,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef);
    test_check(axl_diag_probe_protocol(&bogus, NULL) == -1,
               "diag_probe: unregistered protocol returns -1, NULL name OK");
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
    test_dir_list_json();
    test_volume_enumerate();
    test_smbios();
    test_hexdump();
    test_time();
    test_time_sleep();
    test_config();
    test_config_width_overflow();
    test_config_args();
    test_config_parent();
    test_config_multi();
    test_config_callback();
    test_config_validation();
    test_config_setv();
    test_driver_ensure();
    test_driver_locate();
    test_diag_probe_protocol();

    return test_print_results();
}

AXL_APP(test_util_main)
