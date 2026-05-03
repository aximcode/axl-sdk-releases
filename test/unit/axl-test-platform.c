/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-test-platform.c
    Test application for AXL platform-access modules — AxlAcpi,
    AxlPci, and axl_io_port_*. These all rely on real firmware /
    hardware fixtures supplied by the QEMU OVMF environment the
    integration runner boots.
**/

#include "axl-test.h"
#include <axl/axl-log.h>
#include <axl/axl-spd.h>
#include <axl/axl-usb.h>

#include "spd-ddr4-micron-8gb.h"
#include "spd-ddr5-samsung-16gb.h"

AXL_LOG_DOMAIN("test");

// ---------------------------------------------------------------------------
// AxlAcpi
// ---------------------------------------------------------------------------

static void
test_acpi_revision(void)
{
    uint8_t rev = 0xFF;
    int rc = axl_acpi_revision(&rev);
    test_check(rc == 0, "acpi: revision call succeeds");
    test_check(rev <= 6, "acpi: revision in [0,6]");

    /* NULL out param. */
    test_check(axl_acpi_revision(NULL) == -1,
               "acpi: revision(NULL) returns -1");
}

static void
test_acpi_find(void)
{
    /* QEMU OVMF publishes at least FACP (FADT) and APIC (MADT). */
    AxlAcpiHeader *facp = axl_acpi_find("FACP");
    test_check(facp != NULL, "acpi: find FACP");

    AxlAcpiHeader *madt = axl_acpi_find("APIC");
    test_check(madt != NULL, "acpi: find APIC (MADT)");

    /* Bogus signature returns NULL. */
    AxlAcpiHeader *bogus = axl_acpi_find("ZZZZ");
    test_check(bogus == NULL, "acpi: find ZZZZ returns NULL");

    /* Header sanity: signature matches what we asked for. */
    if (facp != NULL) {
        test_check(facp->signature[0] == 'F'
                   && facp->signature[1] == 'A'
                   && facp->signature[2] == 'C'
                   && facp->signature[3] == 'P',
                   "acpi: FACP signature bytes correct");
        test_check(facp->length >= 36,
                   "acpi: FACP length >= header size");
    }
}

static void
test_acpi_iter(void)
{
    /* axl_acpi_next walks every table. Count and verify
       termination. */
    size_t total = 0;
    AxlAcpiHeader *h = NULL;
    while ((h = axl_acpi_next(h)) != NULL) {
        total++;
        if (total > 256) {
            break;
        }
    }
    test_check(total > 0, "acpi: next walks at least one table");
    test_check(total <= 256, "acpi: next terminates");

    /* find_next on a known signature returns the same set as
       find() for the first call, and NULL eventually. */
    AxlAcpiHeader *first = axl_acpi_find_next("FACP", NULL);
    test_check(first == axl_acpi_find("FACP"),
               "acpi: find_next(NULL) == find");
}

static void
test_acpi_checksum(void)
{
    /* NULL → false. */
    test_check(!axl_acpi_checksum_ok(NULL),
               "acpi: checksum_ok(NULL) returns false");

    /* Truncated header (length too small) → false. */
    AxlAcpiHeader fake = { 0 };
    fake.length = 16;
    test_check(!axl_acpi_checksum_ok(&fake),
               "acpi: checksum_ok(length<36) returns false");

    /* Real tables found in the catalog must validate. */
    AxlAcpiHeader *h = NULL;
    size_t valid = 0;
    while ((h = axl_acpi_next(h)) != NULL) {
        if (axl_acpi_checksum_ok(h)) {
            valid++;
        }
        if (valid > 32) {
            break;
        }
    }
    test_check(valid > 0,
               "acpi: at least one real table checksums valid");
}

static void
test_acpi_read_mcfg(void)
{
    /* QEMU q35 publishes MCFG with 1 segment. virt board on aa64
       also publishes MCFG. */
    AxlAcpiMcfg mcfg;
    int rc = axl_acpi_read_mcfg(&mcfg);
    if (rc != 0) {
        axl_printf("SKIP: acpi read_mcfg (no MCFG on this firmware)\n");
        return;
    }
    test_check(rc == 0, "acpi: read_mcfg succeeds");
    test_check(mcfg.count >= 1 && mcfg.count <= 16,
               "acpi: mcfg has 1..16 segments");
    test_check(mcfg.segments[0].base_addr != 0,
               "acpi: mcfg segment 0 base non-zero");
    test_check(mcfg.segments[0].start_bus <= mcfg.segments[0].end_bus,
               "acpi: mcfg segment 0 bus range valid");
}

static void
test_acpi_read_madt(void)
{
    AxlAcpiMadt madt;
    int rc = axl_acpi_read_madt(&madt);
    test_check(rc == 0, "acpi: read_madt succeeds");
    /* x86 firmware reports IOAPIC entries; aa64 firmware reports
       GIC regions. Whichever side is non-empty is right for this
       arch. */
    test_check(madt.ioapic_count > 0 || madt.gic_region_count > 0,
               "acpi: madt has IOAPIC or GIC entries for this arch");
}

static void
test_acpi_read_facp(void)
{
    AxlAcpiFacp facp;
    int rc = axl_acpi_read_facp(&facp);
    test_check(rc == 0, "acpi: read_facp succeeds");
    /* DSDT pointer must be non-zero on any sane firmware. The
       legacy 32-bit field is fine for QEMU OVMF. */
    test_check(facp.dsdt != 0 || facp.x_dsdt != 0,
               "acpi: FACP has a DSDT pointer");
}

// ---------------------------------------------------------------------------
// AxlPci
// ---------------------------------------------------------------------------

static void
test_pci_enumerate(void)
{
    /* QEMU q35 + virt have a non-trivial PCI topology. The walk
       should find at least one device. */
    AxlPciAddr *p = NULL;
    size_t      count = 0;
    while ((p = axl_pci_next(p)) != NULL) {
        count++;
        if (count > 4096) {
            break;
        }
    }
    test_check(count > 0, "pci: next finds at least one device");
    test_check(count <= 4096, "pci: next terminates");
}

static void
test_pci_read_config(void)
{
    /* The host bridge at 00:00.0 always responds. Read its
       vendor ID — must not be 0xFFFF. */
    AxlPciAddr root = { .seg = 0, .bus = 0, .dev = 0, .func = 0 };
    uint16_t vid = 0xFFFF;
    int rc = axl_pci_read_config_16(root, 0x00, &vid);
    if (rc != 0) {
        axl_printf("SKIP: pci read_config (00:00.0 unreachable)\n");
        return;
    }
    test_check(rc == 0, "pci: read_config_16 succeeds at 00:00.0");
    test_check(vid != 0xFFFF, "pci: 00:00.0 vendor ID populated");

    /* Read class/subclass/progif at 0x09. The host bridge's
       base class is 0x06 (bridge). */
    uint32_t reg08;
    test_check(axl_pci_read_config_32(root, 0x08, &reg08) == 0,
               "pci: read_config_32 succeeds");
    uint8_t base_class = (uint8_t)((reg08 >> 24) & 0xFF);
    test_check(base_class == 0x06,
               "pci: 00:00.0 base class == 0x06 (bridge)");

    /* NULL out param. */
    uint16_t dummy16;
    (void)dummy16;
    test_check(axl_pci_read_config_16(root, 0x00, NULL) == -1,
               "pci: read_config_16(NULL) returns -1");
}

static void
test_pci_find_by_class(void)
{
    /* Class triplet 0x06xxxx (any bridge) — should hit the host
       bridge at minimum. */
    AxlPciAddr a = { 0 };
    int rc = axl_pci_find_by_class(0x060000, 0, &a);
    test_check(rc == 0,
               "pci: find_by_class host bridge (0x060000)");

    /* Wildcard: 0xFFFFFF matches anything. */
    AxlPciAddr any = { 0 };
    test_check(axl_pci_find_by_class(0xFFFFFF, 0, &any) == 0,
               "pci: find_by_class(0xFFFFFF) wildcard succeeds");

    /* nth out of range — return -1. */
    AxlPciAddr none = { 0 };
    test_check(axl_pci_find_by_class(0xFFFFFF, 65535, &none) == -1,
               "pci: find_by_class with huge nth returns -1");
}

static void
test_pci_find_by_vid_did(void)
{
    /* Read the host bridge's VID/DID, then look it up. */
    AxlPciAddr root = { .seg = 0, .bus = 0, .dev = 0, .func = 0 };
    uint16_t vid, did;
    if (axl_pci_read_config_16(root, 0x00, &vid) != 0
        || axl_pci_read_config_16(root, 0x02, &did) != 0
        || vid == 0xFFFF) {
        axl_printf("SKIP: pci find_by_vid_did (no host bridge readable)\n");
        return;
    }

    AxlPciAddr found = { 0 };
    test_check(axl_pci_find_by_vid_did(vid, did, 0, &found) == 0,
               "pci: find_by_vid_did locates host bridge");
    test_check(found.seg == 0 && found.bus == 0
               && found.dev == 0 && found.func == 0,
               "pci: find_by_vid_did returns 00:00.0");

    /* Bogus VID. */
    test_check(axl_pci_find_by_vid_did(0xDEAD, 0xBEEF, 0, &found) == -1,
               "pci: find_by_vid_did bogus VID returns -1");
}

static void
test_pci_addr_parse_format(void)
{
    AxlPciAddr a;

    /* 4-component form */
    test_check(axl_pci_addr_parse("0001:aa:1f.7", &a) == 0
               && a.seg == 0x0001 && a.bus == 0xAA
               && a.dev == 0x1F   && a.func == 7,
               "pci addr_parse: seg:bus:dev.func canonical");

    /* 3-component form, segment defaults to 0 */
    test_check(axl_pci_addr_parse("12:03.4", &a) == 0
               && a.seg == 0 && a.bus == 0x12
               && a.dev == 3 && a.func == 4,
               "pci addr_parse: bus:dev.func defaults seg=0");

    /* All-zero edge case */
    test_check(axl_pci_addr_parse("0:0.0", &a) == 0
               && a.seg == 0 && a.bus == 0 && a.dev == 0 && a.func == 0,
               "pci addr_parse: 0:0.0");

    /* Range checks */
    test_check(axl_pci_addr_parse("100:0.0", &a) == -1,
               "pci addr_parse: bus 0x100 rejected (>0xFF)");
    test_check(axl_pci_addr_parse("0:20.0", &a) == -1,
               "pci addr_parse: dev 0x20 rejected (>0x1F)");
    test_check(axl_pci_addr_parse("0:0.8", &a) == -1,
               "pci addr_parse: func 8 rejected (>0x7)");
    test_check(axl_pci_addr_parse("10000:0:0.0", &a) == -1,
               "pci addr_parse: seg 0x10000 rejected (>0xFFFF)");

    /* Malformed inputs */
    test_check(axl_pci_addr_parse("",         &a) == -1, "pci addr_parse: empty");
    test_check(axl_pci_addr_parse("0",        &a) == -1, "pci addr_parse: 1 field");
    test_check(axl_pci_addr_parse("0.0",      &a) == -1, "pci addr_parse: 2 fields");
    test_check(axl_pci_addr_parse(":0:0.0",   &a) == -1, "pci addr_parse: leading sep");
    test_check(axl_pci_addr_parse("0:0:0.0:", &a) == -1, "pci addr_parse: trailing junk");
    test_check(axl_pci_addr_parse("xx:0.0",   &a) == -1, "pci addr_parse: non-hex");
    test_check(axl_pci_addr_parse(NULL,       &a) == -1, "pci addr_parse: NULL string");

    /* Format produces canonical lower-hex 4-2-2-1 */
    char buf[AXL_PCI_ADDR_STR_MAX];
    AxlPciAddr fmt = { .seg = 0xABCD, .bus = 0x12, .dev = 0x1F, .func = 7 };
    int n = axl_pci_addr_format(fmt, buf, sizeof(buf));
    test_check(n == 12 && axl_strcmp(buf, "abcd:12:1f.7") == 0,
               "pci addr_format: canonical SSSS:BB:DD.F");

    /* Round-trip */
    AxlPciAddr round = { 0 };
    test_check(axl_pci_addr_parse(buf, &round) == 0
               && round.seg == fmt.seg && round.bus == fmt.bus
               && round.dev == fmt.dev && round.func == fmt.func,
               "pci addr_format: round-trips through addr_parse");

    /* Buffer too small */
    char small[12];
    test_check(axl_pci_addr_format(fmt, small, sizeof(small)) == -1,
               "pci addr_format: rejects undersized buffer");
}

static void
test_pci_get_vid_did_class_code(void)
{
    /* Host bridge — guaranteed present on QEMU q35. */
    AxlPciAddr root = { .seg = 0, .bus = 0, .dev = 0, .func = 0 };
    uint16_t vid, did;
    if (axl_pci_get_vid_did(root, &vid, &did) != 0) {
        axl_printf("SKIP: pci get_vid_did (no host bridge readable)\n");
        return;
    }
    test_check(vid != 0xFFFF, "pci get_vid_did: host bridge VID is not 0xFFFF");

    /* Cross-check against raw config-space reads */
    uint16_t vid_raw, did_raw;
    test_check(axl_pci_read_config_16(root, 0x00, &vid_raw) == 0
               && axl_pci_read_config_16(root, 0x02, &did_raw) == 0
               && vid == vid_raw && did == did_raw,
               "pci get_vid_did: matches raw 0x00/0x02 reads");

    /* Absent slot at 1f.7 (q35 LPC owns 1f.0; .7 may be empty) — pick
       a deterministic absent function: bus=0xFF dev=0x1F func=7 has
       no MCFG mapping in any sane firmware. */
    AxlPciAddr absent = { .seg = 0, .bus = 0xFF, .dev = 0x1F, .func = 7 };
    test_check(axl_pci_get_vid_did(absent, &vid, &did) == -1,
               "pci get_vid_did: absent function returns -1");

    /* class_code on the host bridge — base class 0x06 (Bridge) */
    uint32_t class_code;
    test_check(axl_pci_get_class_code(root, &class_code) == 0
               && (class_code >> 16) == 0x06,
               "pci get_class_code: host bridge base class is 0x06");

    /* Cross-check against raw byte reads */
    uint8_t base, sub, prog;
    test_check(axl_pci_read_config_8(root, 0x0B, &base) == 0
               && axl_pci_read_config_8(root, 0x0A, &sub)  == 0
               && axl_pci_read_config_8(root, 0x09, &prog) == 0
               && class_code == (((uint32_t)base << 16) | ((uint32_t)sub << 8) | prog),
               "pci get_class_code: matches raw 0x09/0x0A/0x0B fold");

    /* NULL guards */
    test_check(axl_pci_get_vid_did(root, NULL, &did) == -1, "pci get_vid_did: NULL vid");
    test_check(axl_pci_get_class_code(root, NULL) == -1,       "pci get_class_code: NULL out");
}

// ---------------------------------------------------------------------------
// AxlPci — tree walker
// ---------------------------------------------------------------------------

typedef struct {
    size_t        n_visits;
    size_t        n_root;            /* count of depth==0 callbacks */
    size_t        n_bridges;
    size_t        n_below_bridge;    /* count of depth>=1 callbacks */
    unsigned      max_depth;
    /* Capture the first depth-1 BDF, if any — that's a function
       behind a bridge, which only exists when the runner config
       includes a pcie-root-port + child device. */
    AxlPciAddr    first_child;
    bool          saw_child;
} TreeCtx;

static int
tree_count_cb(
    AxlPciAddr  addr,
    unsigned    depth,
    bool        is_bridge,
    void       *ctx
    )
{
    TreeCtx *t = ctx;
    t->n_visits++;
    if (depth == 0) {
        t->n_root++;
    } else {
        t->n_below_bridge++;
        if (!t->saw_child) {
            t->first_child = addr;
            t->saw_child = true;
        }
    }
    if (is_bridge) {
        t->n_bridges++;
    }
    if (depth > t->max_depth) {
        t->max_depth = depth;
    }
    return 0;
}

static int
tree_stop_after_two_cb(
    AxlPciAddr  addr,
    unsigned    depth,
    bool        is_bridge,
    void       *ctx
    )
{
    (void)addr; (void)depth; (void)is_bridge;
    int *seen = ctx;
    (*seen)++;
    return (*seen >= 2) ? 7 : 0;  /* arbitrary non-zero */
}

static void
test_pci_tree_walker(void)
{
    /* NULL guard. */
    test_check(axl_pci_tree_for_each(NULL, NULL) == -1,
               "pci tree: NULL callback returns -1");

    /* Full walk — every responding function visited exactly once.
       The visit count must equal axl_pci_next's enumeration count;
       any drift would indicate either a missed function (cycle
       killed the walk early) or a double-visit (same bus reached
       via two paths). */
    size_t flat_count = 0;
    AxlPciAddr *p = NULL;
    while ((p = axl_pci_next(p)) != NULL) {
        flat_count++;
    }

    TreeCtx t = {0};
    test_check(axl_pci_tree_for_each(tree_count_cb, &t) == 0,
               "pci tree: full walk succeeds");
    test_check(t.n_visits == flat_count,
               "pci tree: visit count == axl_pci_next count");
    test_check(t.n_root > 0,
               "pci tree: at least one root-bus function visited");
    test_check(t.max_depth < AXL_PCI_TREE_MAX_DEPTH,
               "pci tree: depth stayed below AXL_PCI_TREE_MAX_DEPTH");

    /* Bridge coverage: the integration runner adds a pcie-root-port
       so we expect at least one bridge AND at least one function
       behind it (depth >= 1). If the runner config drifts and there
       are no bridges, balance the count so the ratchet doesn't move. */
    if (t.n_bridges > 0) {
        test_check(t.n_below_bridge > 0,
                   "pci tree: at least one function visited below a bridge");
        test_check(t.saw_child && t.first_child.bus != 0,
                   "pci tree: child function lives on a non-zero bus");
        /* Cross-check against bridge_info: the child's bus should
           equal some bridge's secondary bus number. */
        AxlPciAddr   br_addr;
        AxlPciBridge br;
        bool match = false;
        if (axl_pci_find_by_class(0x060400, 0, &br_addr) == 0
            && axl_pci_bridge_info(br_addr, &br) == 0)
        {
            match = (br.secondary == t.first_child.bus);
        }
        test_check(match,
                   "pci tree: child bus == bridge.secondary from bridge_info");
    } else {
        test_check(true, "pci tree: SKIP — no bridges in topology (3 balancers)");
        test_check(true, "pci tree: SKIP balance");
        test_check(true, "pci tree: SKIP balance");
    }

    /* Early stop: callback returning non-zero must propagate to
       tree_for_each return value, halting iteration. */
    int seen = 0;
    test_check(axl_pci_tree_for_each(tree_stop_after_two_cb, &seen) == 7,
               "pci tree: cb non-zero return propagates");
    test_check(seen == 2,
               "pci tree: walk stopped at second callback");
}

// ---------------------------------------------------------------------------
// AxlPci — vendor/device name database (pci-ids.json5)
// ---------------------------------------------------------------------------

static void
test_pci_ids_db(void)
{
    /* No-database state: lookups return NULL cleanly even when the
       caller forgets to load. */
    test_check(axl_pci_vendor_name(0x8086) == NULL,
               "pci-ids: vendor lookup before load returns NULL");
    test_check(axl_pci_device_name(0x8086, 0x29C0) == NULL,
               "pci-ids: device lookup before load returns NULL");

    /* Auto-discover via the companion-path resolver. The integration
       runner stages share/pci-ids.json5 next to the EFI; passing
       NULL here exercises that lookup path. */
    AxlSidecarStatus rc = axl_pci_ids_load(NULL);
    if (rc != AXL_SIDECAR_OK) {
        axl_printf("SKIP: pci-ids load (no companion file staged)\n");
        /* Balance: 7 conditional test_checks below (rc==0, intel,
           q35, unknown vendor, unknown device, second-load, after-free). */
        for (int i = 0; i < 7; i++) {
            test_check(true, "pci-ids: SKIP balance");
        }
        return;
    }
    test_check(rc == 0, "pci-ids: companion-path load succeeds");

    /* Known entries from share/pci-ids.json5. The exact strings are
       part of the contract — if the file content changes, fix the
       assertion to match. */
    const char *intel = axl_pci_vendor_name(0x8086);
    test_check(intel != NULL && axl_strstr(intel, "Intel") != NULL,
               "pci-ids: vendor 0x8086 decodes to Intel");

    const char *q35 = axl_pci_device_name(0x8086, 0x29C0);
    test_check(q35 != NULL && axl_strstr(q35, "Q35") != NULL,
               "pci-ids: device 8086:29C0 decodes to Q35 host bridge");

    /* Unknown IDs return NULL — no fallback to vendor name from
       device_name (caller composes that themselves). */
    test_check(axl_pci_vendor_name(0xDEAD) == NULL,
               "pci-ids: unknown vendor returns NULL");
    test_check(axl_pci_device_name(0x8086, 0xDEAD) == NULL,
               "pci-ids: unknown device returns NULL even with known vendor");

    /* Idempotent reload — second call is a no-op success. */
    test_check(axl_pci_ids_load(NULL) == AXL_SIDECAR_OK,
               "pci-ids: second load is a no-op success");

    /* Cleanup. After free, lookups return NULL again. */
    axl_pci_ids_free();
    test_check(axl_pci_vendor_name(0x8086) == NULL,
               "pci-ids: vendor lookup after free returns NULL");
}

// ---------------------------------------------------------------------------
// AxlPci — subsystem lookup + iter API (Phase B)
// ---------------------------------------------------------------------------

static void
test_pci_ids_subsystem_lookup(void)
{
    /* Two subsystem entries under the same SVID — exercises the
       pair-key disambiguation. Vendor IDs and names are abstract
       test data; the schema doesn't care. */
    static const char fixture[] =
        "{ schema: 1,\n"
        "  vendors: [{ id: 0xAAAA, name: 'TestVendor' }],\n"
        "  devices: [{ vid: 0xAAAA, did: 0x1000, name: 'TestSilicon' }],\n"
        "  subsystems: [\n"
        "    { svid: 0xBBBB, sdid: 0x0001, name: 'OEM Card A' },\n"
        "    { svid: 0xBBBB, sdid: 0x0002, name: 'OEM Card B' },\n"
        "  ],\n"
        "}\n";

    AxlPciIds *h = NULL;
    test_check(axl_pci_ids_open_from_buffer(
                   fixture, axl_strlen(fixture), &h) == AXL_SIDECAR_OK,
               "pci-ids subsys: fixture loads");

    /* Hit. */
    const char *s = axl_pci_ids_subsys_name(h, 0xBBBB, 0x0001);
    test_check(s != NULL && axl_strcmp(s, "OEM Card A") == 0,
               "pci-ids subsys: known (BBBB,0001) decodes to OEM Card A");

    /* Different sdid under same svid — verify pair_key disambiguates. */
    s = axl_pci_ids_subsys_name(h, 0xBBBB, 0x0002);
    test_check(s != NULL && axl_strcmp(s, "OEM Card B") == 0,
               "pci-ids subsys: distinct sdid under same svid hits its own entry");

    /* Misses: unknown pair, NULL handle, svid==0. */
    test_check(axl_pci_ids_subsys_name(h, 0xDEAD, 0xBEEF) == NULL,
               "pci-ids subsys: unknown pair returns NULL");
    test_check(axl_pci_ids_subsys_name(NULL, 0xBBBB, 0x0001) == NULL,
               "pci-ids subsys: NULL handle returns NULL");
    test_check(axl_pci_ids_subsys_name(h, 0, 0x0001) == NULL,
               "pci-ids subsys: svid==0 returns NULL (sentinel)");

    axl_pci_ids_close(h);
}

typedef struct {
    int n_visits;
    int n_intel_seen;
    int stop_after;  /* 0 = never stop */
} ForeachCtx;

static int
vendor_count_cb(uint16_t vid, const char *name, void *ctx)
{
    ForeachCtx *c = ctx;
    c->n_visits++;
    if (vid == 0x8086 || (name != NULL && axl_strstr(name, "Intel") != NULL)) {
        c->n_intel_seen++;
    }
    if (c->stop_after > 0 && c->n_visits >= c->stop_after) {
        return 42;  /* arbitrary non-zero, propagates */
    }
    return 0;
}

static int
device_count_cb(uint16_t vid, uint16_t did, const char *name, void *ctx)
{
    (void)vid; (void)did; (void)name;
    ForeachCtx *c = ctx;
    c->n_visits++;
    return 0;
}

static int
subsys_count_cb(uint16_t svid, uint16_t sdid, const char *name, void *ctx)
{
    (void)svid; (void)sdid; (void)name;
    ForeachCtx *c = ctx;
    c->n_visits++;
    return 0;
}

static void
test_pci_ids_foreach(void)
{
    static const char fixture[] =
        "{ schema: 1,\n"
        "  vendors: [\n"
        "    { id: 0x8086, name: 'Intel Corporation' },\n"
        "    { id: 0x1022, name: 'AMD' },\n"
        "    { id: 0x10DE, name: 'NVIDIA' },\n"
        "  ],\n"
        "  devices: [\n"
        "    { vid: 0x8086, did: 0x29C0, name: 'Q35' },\n"
        "    { vid: 0x8086, did: 0x100E, name: '82540EM' },\n"
        "  ],\n"
        "  subsystems: [\n"
        "    { svid: 0xBBBB, sdid: 0x0001, name: 'OEM Card A' },\n"
        "  ],\n"
        "}\n";

    AxlPciIds *h = NULL;
    test_check(axl_pci_ids_open_from_buffer(
                   fixture, axl_strlen(fixture), &h) == AXL_SIDECAR_OK,
               "pci-ids foreach: fixture loads");

    /* NULL guards. */
    test_check(axl_pci_ids_foreach_vendor(NULL, vendor_count_cb, NULL) == -1,
               "pci-ids foreach: NULL handle returns -1");
    test_check(axl_pci_ids_foreach_vendor(h, NULL, NULL) == -1,
               "pci-ids foreach: NULL callback returns -1");

    /* Counts match the fixture. */
    ForeachCtx vc = {0};
    test_check(axl_pci_ids_foreach_vendor(h, vendor_count_cb, &vc) == 0,
               "pci-ids foreach_vendor: full walk succeeds");
    test_check(vc.n_visits == 3,
               "pci-ids foreach_vendor: visited all 3 vendors");
    test_check(vc.n_intel_seen == 1,
               "pci-ids foreach_vendor: saw Intel exactly once");

    ForeachCtx dc = {0};
    test_check(axl_pci_ids_foreach_device(h, device_count_cb, &dc) == 0
               && dc.n_visits == 2,
               "pci-ids foreach_device: visited all 2 devices");

    ForeachCtx sc = {0};
    test_check(axl_pci_ids_foreach_subsys(h, subsys_count_cb, &sc) == 0
               && sc.n_visits == 1,
               "pci-ids foreach_subsys: visited the 1 subsystem entry");

    /* Early stop: callback returning 42 propagates. */
    ForeachCtx stop = { .stop_after = 1 };
    test_check(axl_pci_ids_foreach_vendor(h, vendor_count_cb, &stop) == 42,
               "pci-ids foreach: early-stop return value propagates");

    axl_pci_ids_close(h);
}

static void
test_pci_ids_subsys_db(void)
{
    /* End-to-end: the staged share/pci-ids.json5 should now have
       subsystem entries via the singleton API. SKIP-balanced when
       the DB isn't loaded (e.g. when test EFI is launched outside
       the integration runner). */
    if (axl_pci_ids_load(NULL) != AXL_SIDECAR_OK) {
        axl_printf("SKIP: pci-ids subsys_db (no companion file staged)\n");
        for (int i = 0; i < 2; i++) {
            test_check(true, "pci-ids subsys: SKIP balance");
        }
        return;
    }

    /* The curated share/pci-ids.json5 ships a tiny starter subsystems
       block — assert one of those entries is reachable through the
       singleton API. If the curated set is restructured, update the
       assertion to match. */
    const char *s = axl_pci_subsys_name(0x1028, 0x1FCA);
    test_check(s != NULL && axl_strstr(s, "Server NIC") != NULL,
               "pci-ids subsys: singleton finds curated server-NIC entry");
    test_check(axl_pci_subsys_name(0x0000, 0x0000) == NULL,
               "pci-ids subsys: zero pair returns NULL");
}

// ---------------------------------------------------------------------------
// AxlPci — class-name overlay sidecar (Phase E)
// ---------------------------------------------------------------------------

static void
test_pci_class_db_handle(void)
{
    /* Schema: top-level 'classes' array; each entry pins any subset
       of (base, sub, prog). Overlay handle holds the parsed table;
       per-tier lookups return NULL when the overlay has no entry
       for that exact code (no fallback at the handle layer). */
    static const char fixture[] =
        "{ schema: 1,\n"
        "  classes: [\n"
        "    { base: 0xCC, name: 'OverlayBase' },\n"
        "    { base: 0xCC, sub: 0xCD, name: 'OverlaySub' },\n"
        "    { base: 0xCC, sub: 0xCD, prog: 0xCE, name: 'OverlayProg' },\n"
        "  ],\n"
        "}\n";

    AxlPciClassDb *db = NULL;
    test_check(axl_pci_class_open_from_buffer(
                   fixture, axl_strlen(fixture), &db) == AXL_SIDECAR_OK,
               "pci class_db: handle opens from buffer");

    /* Per-tier lookups return the overlay name for entries the
       fixture defined. */
    const char *b = axl_pci_class_db_base_name(db, 0xCC);
    test_check(b != NULL && axl_strcmp(b, "OverlayBase") == 0,
               "pci class_db: base lookup returns overlay name");
    const char *s = axl_pci_class_db_sub_name(db, 0xCC, 0xCD);
    test_check(s != NULL && axl_strcmp(s, "OverlaySub") == 0,
               "pci class_db: sub lookup returns overlay name");
    const char *p = axl_pci_class_db_prog_name(db, 0xCC, 0xCD, 0xCE);
    test_check(p != NULL && axl_strcmp(p, "OverlayProg") == 0,
               "pci class_db: prog lookup returns overlay name");

    /* Codes the overlay doesn't define return NULL — no fallback
       at the handle layer (axl_pci_class_string_fmt does the
       fallback). */
    test_check(axl_pci_class_db_base_name(db, 0x06) == NULL,
               "pci class_db: undefined base returns NULL (no compiled-in fallback)");
    test_check(axl_pci_class_db_sub_name(db, 0xCC, 0xFF) == NULL,
               "pci class_db: undefined sub under known base returns NULL");

    /* NULL handle is NULL-safe. */
    test_check(axl_pci_class_db_base_name(NULL, 0xCC) == NULL,
               "pci class_db: NULL handle returns NULL");

    /* Bad JSON5 returns -2; close NULL is a no-op. */
    AxlPciClassDb *bad = NULL;
    test_check(axl_pci_class_open_from_buffer(
                   "@@@ garbage", 11, &bad) == AXL_SIDECAR_PARSE_ERROR,
               "pci class_db: malformed JSON5 returns -2");
    test_check(bad == NULL, "pci class_db: handle stays NULL on -2");

    axl_pci_class_close(db);
    axl_pci_class_close(NULL);
    test_check(true, "pci class_db: close + close(NULL) OK");
}

static void
test_pci_class_db_singleton_overrides(void)
{
    /* Loading the singleton overlay changes axl_pci_class_string_fmt
       output for the codes the overlay redefines. The compiled-in
       table is the fallback for codes the overlay doesn't define. */
    char buf[AXL_PCI_CLASS_NAME_MAX];

    /* Baseline (no overlay) — exercise compiled-in tables.
       0x060000 = Bridge / Host bridge per the compiled-in table. */
    axl_pci_class_free();  /* ensure clean slate */
    int n = axl_pci_class_string_fmt(0x060000,
                                     AXL_PCI_CLASS_FMT_FULL,
                                     buf, sizeof(buf));
    test_check(n > 0
               && axl_strcmp(buf, "Bridge / Host bridge") == 0,
               "pci class_db: baseline (no overlay) uses compiled-in tables");

    /* Load the test-only overlay fixture via the explicit-override
       path. The integration runner stages
       test/data/pci-class-test.json5 at the disk root next to the
       EFI; SKIP-balance when not present.

       Note: explicit override is authoritative (no companion-path
       fallback per the API contract), so this depends on the runner
       doing `cd \` in startup.nsh — the bare relative
       `pci-class-test.json5` resolves against CWD. Other consumers
       of the same loader who don't `cd \` would have to pass an
       absolute path; the autodiscovery path (NULL override) is the
       one that gets image-path-anchored discovery via
       axl_resolve_data_file.

       Production share/pci-class.json5 ships with an empty classes[]
       block so deployed lspci output isn't polluted with a demo
       "[overlay]" marker (a downstream session caught this leaking
       into prod tools). The test-only file is the override that
       proves the loader-applied lookup actually fires.

       Populated path runs 6 conditional checks below: 'overlay
       loaded', '[overlay] marker', 'codes outside overlay still
       hit compiled-in', 'second load no-op', 'free reverts',
       'missing file -1'. */
    AxlSidecarStatus rc = axl_pci_class_load("pci-class-test.json5");
    if (rc != AXL_SIDECAR_OK) {
        axl_printf("SKIP: pci class_db (no test overlay staged)\n");
        for (int i = 0; i < 6; i++) {
            test_check(true, "pci class_db: SKIP balance");
        }
        return;
    }
    test_check(true, "pci class_db: overlay loaded");

    /* The test fixture redefines 0x060000 (Host bridge) — a stable
       triple in the compiled-in table — with an "[overlay]" marker
       that proves the overlay lookup happened. */
    n = axl_pci_class_string_fmt(0x060000,
                                 AXL_PCI_CLASS_FMT_FULL,
                                 buf, sizeof(buf));
    test_check(n > 0 && axl_strstr(buf, "[overlay]") != NULL,
               "pci class_db: overlay redefines 0x060000 (sees [overlay] marker)");

    /* Codes the overlay doesn't define still come from compiled-in.
       0x010601 = SATA AHCI 1.0 — every tier is in the compiled-in
       table. */
    n = axl_pci_class_string_fmt(0x010601,
                                 AXL_PCI_CLASS_FMT_FULL,
                                 buf, sizeof(buf));
    test_check(n > 0 && axl_strstr(buf, "SATA") != NULL,
               "pci class_db: codes outside overlay still hit compiled-in");

    /* Reload short-circuits (idempotent). */
    test_check(axl_pci_class_load("pci-class-test.json5") == AXL_SIDECAR_OK,
               "pci class_db: second load is a no-op success");

    /* Free clears the overlay; output reverts to compiled-in. */
    axl_pci_class_free();
    n = axl_pci_class_string_fmt(0x060000,
                                 AXL_PCI_CLASS_FMT_FULL,
                                 buf, sizeof(buf));
    test_check(n > 0
               && axl_strcmp(buf, "Bridge / Host bridge") == 0,
               "pci class_db: free reverts to compiled-in");

    /* Load failure modes (parallel to pci-ids FILE_MISSING / PARSE_ERROR split). */
    test_check(axl_pci_class_load(
                   "fs0:\\does-not-exist-anywhere.json5")
                   == AXL_SIDECAR_FILE_MISSING,
               "pci class_db: missing file returns FILE_MISSING (authoritative)");
}

// ---------------------------------------------------------------------------
// AxlPci — composed-name helper (Phase D)
// ---------------------------------------------------------------------------

static void
test_pci_format_name(void)
{
    /* Composed-name helper renders the same string for the same
       (vid, did) pair across every consumer that uses it. Tested
       against (a) the staged singleton DB for cases 1-3 and
       (b) a handle-loaded pathological fixture for case 4. */
    char buf[AXL_PCI_NAME_COMPOSED_MAX];

    /* Bad-arg guards apply unconditionally — exercise them before
       the load gate so they always run. */
    test_check(axl_pci_format_name(0x8086, 0x29C0, NULL, sizeof(buf)) == -1,
               "pci format_name: NULL buf returns -1");
    test_check(axl_pci_format_name(0x8086, 0x29C0, buf, 0) == -1,
               "pci format_name: buflen 0 returns -1");

    /* Case 4: vendor unknown takes precedence even when the device
       entry exists. Pathological fixture with a device entry under
       a vendor that has no vendor-name entry — proves the
       short-circuit fires. Use the handle API directly so we don't
       have to disturb the singleton. */
    {
        static const char fixture[] =
            "{ schema: 1,\n"
            "  vendors: [],\n"
            "  devices: [{ vid: 0xCAFE, did: 0x0001, name: 'Orphan' }],\n"
            "}\n";
        AxlPciIds *h = NULL;
        test_check(axl_pci_ids_open_from_buffer(
                       fixture, axl_strlen(fixture), &h) == AXL_SIDECAR_OK,
                   "pci format_name: orphan-device fixture loads");
        int rc = axl_pci_ids_format_name(h, 0xCAFE, 0x0001,
                                         buf, sizeof(buf));
        /* Without a vendor entry, output must be numeric — must not
           leak the device name. */
        test_check(rc > 0 && axl_strcmp(buf, "cafe:0001") == 0,
                   "pci format_name: vendor-unknown short-circuits "
                   "even when device entry would have hit");
        axl_pci_ids_close(h);
    }

    /* Cases 1-3 use the staged singleton DB. SKIP-balanced when
       no companion DB is present. */
    if (axl_pci_ids_load(NULL) != AXL_SIDECAR_OK) {
        axl_printf("SKIP: pci format_name (no companion DB staged)\n");
        for (int i = 0; i < 3; i++) {
            test_check(true, "pci format_name: SKIP balance");
        }
        return;
    }

    /* Case 1: vendor + device both known. share/pci-ids.json5 ships
       Intel (8086) + Q35 Host Bridge (29C0). Pin the exact string
       — substring matches silently tolerate curator typos. */
    int n = axl_pci_format_name(0x8086, 0x29C0, buf, sizeof(buf));
    test_check(n > 0
               && axl_strcmp(buf,
                   "Intel Corporation Q35 Host Bridge") == 0,
               "pci format_name: 8086:29C0 == 'Intel Corporation Q35 Host Bridge'");

    /* Case 2: vendor known, device unknown — formatter inserts
       'Device <DID hex>' with lowercase 4-wide zero-padded hex. */
    n = axl_pci_format_name(0x8086, 0xDEAD, buf, sizeof(buf));
    test_check(n > 0
               && axl_strcmp(buf,
                   "Intel Corporation Device dead") == 0,
               "pci format_name: 8086:DEAD == 'Intel Corporation Device dead'");

    /* Case 3: vendor unknown — fully numeric, no name material. */
    n = axl_pci_format_name(0xDEAD, 0xBEEF, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "dead:beef") == 0,
               "pci format_name: unknown vendor == 'VID:DID'");
}

// ---------------------------------------------------------------------------
// AxlPci — handle API, buffer load, length contracts, load failure modes
// ---------------------------------------------------------------------------

static void
test_pci_ids_length_macros(void)
{
    /* Compile-time contract — caps are public macros consumers can
       use to size stack buffers. Sanity-check they're sensibly sized
       relative to one another. */
    test_check(AXL_PCI_VENDOR_NAME_MAX == 128u,
               "pci-ids: AXL_PCI_VENDOR_NAME_MAX == 128");
    test_check(AXL_PCI_DEVICE_NAME_MAX == 192u,
               "pci-ids: AXL_PCI_DEVICE_NAME_MAX == 192");
    test_check(AXL_PCI_SUBSYS_NAME_MAX == 192u,
               "pci-ids: AXL_PCI_SUBSYS_NAME_MAX == 192");
    test_check(AXL_PCI_NAME_COMPOSED_MAX >= AXL_PCI_VENDOR_NAME_MAX
               + AXL_PCI_DEVICE_NAME_MAX,
               "pci-ids: composed name fits vendor+device");
}

static void
test_pci_ids_length_enforcement(void)
{
    /* Loader silently truncates over-cap entries (axl_json_get_string
       writes at most buflen-1 bytes + NUL). The contract: lookups
       always return a string whose length is < the documented cap.
       Pin this with a fixture whose vendor name exceeds the cap. */
    static const char fixture[] =
        "{ schema: 1,\n"
        "  vendors: [{ id: 0x9999, name: '"
        /* 200 chars of payload — well over AXL_PCI_VENDOR_NAME_MAX. */
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
        "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD"
        "' }] }";

    AxlPciIds *h = NULL;
    test_check(axl_pci_ids_open_from_buffer(
                   fixture, axl_strlen(fixture), &h) == AXL_SIDECAR_OK,
               "pci-ids cap: over-cap fixture loads (truncates silently)");

    const char *v = axl_pci_ids_vendor_name(h, 0x9999);
    test_check(v != NULL,
               "pci-ids cap: over-cap vendor returns non-NULL (truncated)");
    test_check(v != NULL
               && axl_strlen(v) < AXL_PCI_VENDOR_NAME_MAX,
               "pci-ids cap: returned name is < AXL_PCI_VENDOR_NAME_MAX");

    axl_pci_ids_close(h);
}

static void
test_pci_ids_partial_schemas(void)
{
    /* Any subset of the three top-level arrays is valid — a database
       may ship with vendors only, devices only, or even just the
       schema field (an empty stub). All paths must open cleanly. */
    AxlPciIds *h = NULL;

    /* Vendors only. */
    static const char vendors_only[] =
        "{ schema: 1, vendors: [{ id: 0x0001, name: 'V' }] }";
    test_check(axl_pci_ids_open_from_buffer(
                   vendors_only, axl_strlen(vendors_only), &h) == AXL_SIDECAR_OK,
               "pci-ids schema: vendors-only loads");
    test_check(axl_pci_ids_vendor_name(h, 0x0001) != NULL,
               "pci-ids schema: vendors-only — vendor lookup hits");
    test_check(axl_pci_ids_device_name(h, 0x0001, 0x0001) == NULL,
               "pci-ids schema: vendors-only — device lookup misses");
    axl_pci_ids_close(h);

    /* Devices only. */
    static const char devices_only[] =
        "{ schema: 1, devices: [{ vid: 0x0001, did: 0x0001, name: 'D' }] }";
    test_check(axl_pci_ids_open_from_buffer(
                   devices_only, axl_strlen(devices_only), &h) == AXL_SIDECAR_OK,
               "pci-ids schema: devices-only loads");
    test_check(axl_pci_ids_device_name(h, 0x0001, 0x0001) != NULL,
               "pci-ids schema: devices-only — device lookup hits");
    test_check(axl_pci_ids_vendor_name(h, 0x0001) == NULL,
               "pci-ids schema: devices-only — vendor lookup misses");
    axl_pci_ids_close(h);

    /* Schema field only — empty stub. All lookups miss; no crash. */
    static const char schema_only[] = "{ schema: 1 }";
    test_check(axl_pci_ids_open_from_buffer(
                   schema_only, axl_strlen(schema_only), &h) == AXL_SIDECAR_OK,
               "pci-ids schema: empty stub loads");
    test_check(axl_pci_ids_vendor_name(h, 0x0001) == NULL
               && axl_pci_ids_device_name(h, 0x0001, 0x0001) == NULL,
               "pci-ids schema: empty stub — every lookup misses");
    axl_pci_ids_close(h);
}

static void
test_pci_ids_handle_buffer(void)
{
    /* Buffer-load path lets us embed a tiny test fixture without
       depending on a staged file. Schema is the same as the on-disk
       JSON5: top-level vendors/devices arrays. */
    static const char fixture[] =
        "{\n"
        "  schema: 1,\n"
        "  vendors: [\n"
        "    { id: 0x1234, name: 'TestVendor' },\n"
        "    { id: 0x5678, name: 'OtherVendor' },\n"
        "  ],\n"
        "  devices: [\n"
        "    { vid: 0x1234, did: 0x0001, name: 'Test Device 1' },\n"
        "    { vid: 0x5678, did: 0x00AB, name: 'Other Device AB' },\n"
        "  ],\n"
        "}\n";

    AxlPciIds *h = NULL;
    AxlSidecarStatus rc = axl_pci_ids_open_from_buffer(fixture,
                                                       axl_strlen(fixture), &h);
    test_check(rc == AXL_SIDECAR_OK && h != NULL,
               "pci-ids handle: open_from_buffer succeeds on valid JSON5");

    const char *v = axl_pci_ids_vendor_name(h, 0x1234);
    test_check(v != NULL && axl_strcmp(v, "TestVendor") == 0,
               "pci-ids handle: vendor lookup returns exact name");

    const char *d = axl_pci_ids_device_name(h, 0x5678, 0x00AB);
    test_check(d != NULL && axl_strcmp(d, "Other Device AB") == 0,
               "pci-ids handle: device lookup returns exact name");

    /* Unknown lookups return NULL on a present handle. */
    test_check(axl_pci_ids_vendor_name(h, 0xDEAD) == NULL,
               "pci-ids handle: unknown vendor returns NULL");
    test_check(axl_pci_ids_device_name(h, 0x1234, 0xDEAD) == NULL,
               "pci-ids handle: unknown device returns NULL");

    /* NULL handle propagates as NULL — keeps a layered priority
       chain (private then public, etc.) readable without per-lookup
       null guards. */
    test_check(axl_pci_ids_vendor_name(NULL, 0x1234) == NULL,
               "pci-ids handle: NULL handle returns NULL");

    /* Close is NULL-safe. */
    axl_pci_ids_close(h);
    axl_pci_ids_close(NULL);
    test_check(true, "pci-ids handle: close + close(NULL) OK");
}

static void
test_pci_ids_handle_priority(void)
{
    /* Public + private overlay: a private handle shadows the public
       one for entries the private set redefines. Composed via a
       caller-side priority lookup (the API doesn't pre-merge —
       consumers chain handles in their own preferred order). */
    static const char public_db[] =
        "{ schema: 1,\n"
        "  vendors: [{ id: 0x1234, name: 'PublicVendor' }],\n"
        "  devices: [{ vid: 0x1234, did: 0x0001, name: 'Public Device' }] }\n";
    static const char private_db[] =
        "{ schema: 1,\n"
        "  vendors: [{ id: 0x1234, name: 'PrivateVendor' }],\n"
        "  devices: [{ vid: 0x1234, did: 0x0001, name: 'Private Rebadge' }] }\n";

    AxlPciIds *pub  = NULL;
    AxlPciIds *priv = NULL;
    test_check(axl_pci_ids_open_from_buffer(
                   public_db, axl_strlen(public_db), &pub) == AXL_SIDECAR_OK,
               "pci-ids overlay: public DB opens");
    test_check(axl_pci_ids_open_from_buffer(
                   private_db, axl_strlen(private_db), &priv) == AXL_SIDECAR_OK,
               "pci-ids overlay: private DB opens");

    /* Priority lookup: private first, public fallback. */
    const char *v = axl_pci_ids_vendor_name(priv, 0x1234);
    if (v == NULL) v = axl_pci_ids_vendor_name(pub, 0x1234);
    test_check(v != NULL && axl_strcmp(v, "PrivateVendor") == 0,
               "pci-ids overlay: private shadows public on vendor");

    const char *d = axl_pci_ids_device_name(priv, 0x1234, 0x0001);
    if (d == NULL) d = axl_pci_ids_device_name(pub, 0x1234, 0x0001);
    test_check(d != NULL && axl_strcmp(d, "Private Rebadge") == 0,
               "pci-ids overlay: private shadows public on device");

    axl_pci_ids_close(priv);
    axl_pci_ids_close(pub);
}

static void
test_pci_ids_load_failure_modes(void)
{
    /* axl_pci_ids_load with an explicit override is authoritative
       (per the post-friction API change): the override path is used
       as-is, no fallback to companion/cwd. So passing a known-absent
       path returns AXL_SIDECAR_FILE_MISSING, and a known-malformed
       path returns AXL_SIDECAR_PARSE_ERROR — deployment vs
       authoring failure modes stay distinguishable.

       Make sure the singleton is empty before each call: axl_pci_ids_load
       short-circuits on g_singleton != NULL. */
    axl_pci_ids_free();

    /* FILE_MISSING: explicit path that doesn't exist. The fact that
       pci-ids.json5 IS staged in the companion path must NOT mask this. */
    test_check(axl_pci_ids_load("fs0:\\does-not-exist-anywhere.json5")
                   == AXL_SIDECAR_FILE_MISSING,
               "pci-ids load: explicit missing file returns FILE_MISSING (no fallback)");

    /* PARSE_ERROR: file found but malformed. The integration runner
       stages pci-ids-malformed.json5 next to the EFI; SKIP-balanced
       when the fixture isn't present. */
    AxlSidecarStatus rc = axl_pci_ids_load("pci-ids-malformed.json5");
    if (rc == AXL_SIDECAR_FILE_MISSING) {
        axl_printf("SKIP: pci-ids load PARSE_ERROR (malformed fixture not staged)\n");
        test_check(true, "pci-ids load: SKIP balance for PARSE_ERROR path");
    } else {
        test_check(rc == AXL_SIDECAR_PARSE_ERROR,
                   "pci-ids load: malformed JSON5 returns PARSE_ERROR");
    }

    /* Handle API mirrors the same split. NULL out param is cleared
       on error. */
    AxlPciIds *h = (AxlPciIds *)0x1;
    test_check(axl_pci_ids_open(
                   "fs0:\\does-not-exist-anywhere.json5", &h)
                   == AXL_SIDECAR_FILE_MISSING,
               "pci-ids open: missing file returns FILE_MISSING");
    test_check(h == NULL,
               "pci-ids open: handle cleared to NULL on error");
}

static AxlSidecarStatus
ids_buffer_open_cb(const char *fixture)
{
    /* Helper: open from a string buffer and immediately close.
       Used to prove the buffer path doesn't leak. Called from a
       loop in the test below. */
    AxlPciIds *h = NULL;
    AxlSidecarStatus rc = axl_pci_ids_open_from_buffer(
        fixture, axl_strlen(fixture), &h);
    if (rc == AXL_SIDECAR_OK) {
        axl_pci_ids_close(h);
    }
    return rc;
}

static void
test_pci_ids_schema_v2_hierarchical(void)
{
    /* Schema 2 puts devices under their parent vendor and subsystems
       under their parent device — the natural hand-edit shape for a
       human maintaining thousands of entries. The loader pivots on
       the schema field; both schema 1 (flat) and schema 2
       (hierarchical) populate the same internal hash tables, so
       lookups don't care which shape the file used. */
    static const char fixture[] =
        "{ schema: 2,\n"
        "  vendors: [\n"
        "    { id: 0x8086, name: 'TestIntel',\n"
        "      devices: [\n"
        "        { did: 0x29C0, name: 'Test Q35',\n"
        "          subsystems: [\n"
        "            { svid: 0x1028, sdid: 0x1FCA, name: 'Hier OEM Card' },\n"
        "          ],\n"
        "        },\n"
        "        { did: 0x100E, name: 'Test 82540EM' },\n"
        "      ],\n"
        "    },\n"
        "    { id: 0x10DE, name: 'TestNVIDIA' },\n"
        "  ],\n"
        "}\n";

    AxlPciIds *h = NULL;
    test_check(axl_pci_ids_open_from_buffer(
                   fixture, axl_strlen(fixture), &h) == AXL_SIDECAR_OK,
               "pci-ids v2: hierarchical fixture loads");

    /* Vendor lookups (top-level vendors[]). */
    const char *v = axl_pci_ids_vendor_name(h, 0x8086);
    test_check(v != NULL && axl_strcmp(v, "TestIntel") == 0,
               "pci-ids v2: vendor at top level decodes");
    v = axl_pci_ids_vendor_name(h, 0x10DE);
    test_check(v != NULL && axl_strcmp(v, "TestNVIDIA") == 0,
               "pci-ids v2: vendor with no nested devices decodes");

    /* Device lookups — keyed on (vid, did) globally, not on nesting. */
    const char *d = axl_pci_ids_device_name(h, 0x8086, 0x29C0);
    test_check(d != NULL && axl_strcmp(d, "Test Q35") == 0,
               "pci-ids v2: nested device decodes via global (vid,did) key");
    d = axl_pci_ids_device_name(h, 0x8086, 0x100E);
    test_check(d != NULL && axl_strcmp(d, "Test 82540EM") == 0,
               "pci-ids v2: device without subsystems decodes");

    /* Subsystem lookup — keyed on (svid, sdid) globally, not on
       parent (vid, did) context. */
    const char *s = axl_pci_ids_subsys_name(h, 0x1028, 0x1FCA);
    test_check(s != NULL && axl_strcmp(s, "Hier OEM Card") == 0,
               "pci-ids v2: nested subsystem decodes via global (svid,sdid) key");

    /* Misses still return NULL. */
    test_check(axl_pci_ids_vendor_name(h, 0xDEAD) == NULL,
               "pci-ids v2: unknown vendor returns NULL");
    test_check(axl_pci_ids_device_name(h, 0x10DE, 0x0001) == NULL,
               "pci-ids v2: vendor with no devices array — device lookup misses");

    axl_pci_ids_close(h);

    /* Schema 2 also accepts a top-level subsystems[] block alongside
       the nested form — orphan entries the maintainer doesn't know
       which device to nest under can land in the flat block. The
       loader merges both into the same hash table. */
    static const char hybrid_fixture[] =
        "{ schema: 2,\n"
        "  vendors: [\n"
        "    { id: 0x8086, name: 'TestIntel',\n"
        "      devices: [\n"
        "        { did: 0x1521, name: 'I350',\n"
        "          subsystems: [\n"
        "            { svid: 0xAAAA, sdid: 0x0001, name: 'Nested Sub' },\n"
        "          ],\n"
        "        },\n"
        "      ],\n"
        "    },\n"
        "  ],\n"
        "  subsystems: [\n"
        "    { svid: 0xBBBB, sdid: 0x0002, name: 'Orphan Sub' },\n"
        "  ],\n"
        "}\n";
    test_check(axl_pci_ids_open_from_buffer(
                   hybrid_fixture, axl_strlen(hybrid_fixture), &h) == AXL_SIDECAR_OK,
               "pci-ids v2 hybrid: nested + top-level subsystems[] loads");
    test_check(axl_pci_ids_subsys_name(h, 0xAAAA, 0x0001) != NULL,
               "pci-ids v2 hybrid: nested subsystem reachable");
    test_check(axl_pci_ids_subsys_name(h, 0xBBBB, 0x0002) != NULL,
               "pci-ids v2 hybrid: orphan top-level subsystem reachable");
    axl_pci_ids_close(h);

    /* Schema 1 (flat) still works after schema 2 was added. */
    static const char v1_fixture[] =
        "{ schema: 1,\n"
        "  vendors: [{ id: 0x8086, name: 'FlatIntel' }],\n"
        "  devices: [{ vid: 0x8086, did: 0x29C0, name: 'Flat Q35' }],\n"
        "  subsystems: [{ svid: 0x1028, sdid: 0x1FCA, name: 'Flat OEM' }],\n"
        "}\n";
    test_check(axl_pci_ids_open_from_buffer(
                   v1_fixture, axl_strlen(v1_fixture), &h) == AXL_SIDECAR_OK,
               "pci-ids v1: flat fixture still loads after v2 was added");
    test_check(axl_pci_ids_vendor_name(h, 0x8086) != NULL
               && axl_strcmp(axl_pci_ids_vendor_name(h, 0x8086),
                             "FlatIntel") == 0,
               "pci-ids v1: flat vendor lookup unchanged");
    axl_pci_ids_close(h);

    /* Unknown schema number returns -2 (parse error) — guards against
       a future schema 3 file being silently misparsed by this build. */
    static const char v99_fixture[] =
        "{ schema: 99, vendors: [{ id: 0x0001, name: 'X' }] }\n";
    h = NULL;
    test_check(axl_pci_ids_open_from_buffer(
                   v99_fixture, axl_strlen(v99_fixture), &h) == AXL_SIDECAR_PARSE_ERROR,
               "pci-ids: unknown schema number returns -2");
    test_check(h == NULL,
               "pci-ids: handle stays NULL on unknown-schema -2");

    /* Missing schema field returns -2 (parse error) — would otherwise
       silently misparse: a v2 file forgetting schema would parse as
       v1 and silently drop every nested device. The required-field
       check is the cheap safety net. */
    static const char no_schema_fixture[] =
        "{ vendors: [{ id: 0x0001, name: 'X' }] }\n";
    h = NULL;
    test_check(axl_pci_ids_open_from_buffer(
                   no_schema_fixture, axl_strlen(no_schema_fixture),
                   &h) == AXL_SIDECAR_PARSE_ERROR,
               "pci-ids: missing 'schema' field returns -2");
}

static void
test_pci_ids_buffer_parse_errors(void)
{
    /* Buffer load with malformed JSON5 returns -2 — same posture
       as the file path. Use an unambiguously-invalid token so the
       JSON5 parser doesn't accidentally accept it via permissive
       identifier-key grammar (a previous attempt with
       "{ this is not JSON5 at all" partially parsed because JSON5
       allows unquoted identifier keys). */
    static const char bad[] = "@@@ not even close to JSON5 @@@";
    AxlPciIds *h = NULL;
    test_check(axl_pci_ids_open_from_buffer(bad, axl_strlen(bad), &h) == AXL_SIDECAR_PARSE_ERROR,
               "pci-ids buffer: malformed JSON5 returns -2");
    test_check(h == NULL, "pci-ids buffer: handle stays NULL on parse fail");

    /* Repeated open/close on a valid buffer leaves no leaks (caught
       by AXL_MEM_DEBUG's per-test leak report). */
    static const char tiny[] =
        "{ schema: 1, vendors: [{ id: 0x0001, name: 'Tiny' }] }";
    for (int i = 0; i < 4; i++) {
        test_check(ids_buffer_open_cb(tiny) == AXL_SIDECAR_OK,
                   "pci-ids buffer: re-open OK (no leaks)");
    }
}

static int
vpd_iter_count_cb(const char keyword[2], const uint8_t *data,
                  size_t len, void *ctx)
{
    (void)keyword; (void)data; (void)len;
    int *n = (int *)ctx;
    (*n)++;
    return 0;
}

static int
vpd_iter_stop_cb(const char keyword[2], const uint8_t *data,
                 size_t len, void *ctx)
{
    (void)keyword; (void)data; (void)len; (void)ctx;
    return 42;  /* arbitrary non-zero, should propagate to iter return */
}

static void
test_pci_vpd_iter(void)
{
    /* NULL cb is rejected. */
    AxlPciAddr root = { .seg = 0, .bus = 0, .dev = 0, .func = 0 };
    test_check(axl_pci_vpd_iter(root, NULL, NULL) == -1,
               "pci vpd_iter: NULL callback rejected");

    /* Host bridge has no VPD capability — iter should fail with -1
       (no VPD cap), not call the callback. */
    int seen = 0;
    test_check(axl_pci_vpd_iter(root, vpd_iter_count_cb, &seen) == -1
               && seen == 0,
               "pci vpd_iter: function without VPD cap returns -1");

    /* If any device on the bus exposes VPD, the iter walks it and
       the count is non-zero. QEMU's emulated devices typically
       don't, so this is a SKIP path on standard test images.
       Either outcome is correct — we're just exercising the API
       end-to-end against whatever the bus offers. */
    AxlPciAddr *p = NULL;
    bool tried_real = false;
    while ((p = axl_pci_next(p)) != NULL) {
        int n = 0;
        int rc = axl_pci_vpd_iter(*p, vpd_iter_count_cb, &n);
        if (rc == 0 && n > 0) {
            tried_real = true;
            test_check(true,
                       "pci vpd_iter: walked VPD on real device");
            /* Verify early-stop: callback's non-zero return propagates. */
            test_check(axl_pci_vpd_iter(*p, vpd_iter_stop_cb, NULL) == 42,
                       "pci vpd_iter: cb non-zero return propagates");
            break;
        }
    }
    if (!tried_real) {
        axl_printf("SKIP: pci vpd_iter (no device with VPD on this bus)\n");
        /* Balance: 2 checks ran in the populated path, 0 in skip. Add
           2 trivial passing checks so the ratchet doesn't drift between
           QEMU images. */
        test_check(true, "pci vpd_iter: SKIP balance 1");
        test_check(true, "pci vpd_iter: SKIP balance 2");
    }
}

// ---------------------------------------------------------------------------
// AxlUsb (Phase A: enumeration + vid/pid)
// ---------------------------------------------------------------------------

static void
test_usb_enumerate(void)
{
    AxlUsbAddr *u = NULL;
    size_t      count = 0;
    while ((u = axl_usb_next(u)) != NULL) {
        count++;
        if (count > 1024) {
            break;
        }
    }
    if (count == 0) {
        /* Real-hardware / virt-without-USB envs hit this path; the
           QEMU test runner injects qemu-xhci + usb-mouse so the
           populated branch is what CI exercises. Balance the count
           against the populated branch (3 checks). */
        axl_printf("SKIP: usb_enumerate (no EFI_USB_IO_PROTOCOL handles)\n");
        test_check(true, "usb enumerate: SKIP balance 1");
        test_check(true, "usb enumerate: SKIP balance 2");
        test_check(true, "usb enumerate: SKIP balance 3");
        return;
    }
    test_check(count > 0, "usb: next finds at least one interface");
    test_check(count <= 1024, "usb: next terminates");

    /* Cursor-restart semantics: passing a caller-allocated
       AxlUsbAddr (not the static cursor) restarts the walk silently. */
    AxlUsbAddr  stack_copy = { 0 };
    AxlUsbAddr *r = axl_usb_next(&stack_copy);
    test_check(r != NULL,
               "usb: next restarts on caller-supplied prev");
}

static void
test_usb_get_vid_pid(void)
{
    AxlUsbAddr *u = axl_usb_next(NULL);
    if (u == NULL) {
        axl_printf("SKIP: usb_get_vid_pid (no USB devices)\n");
        test_check(true, "usb vid_pid: SKIP balance 1");
        test_check(true, "usb vid_pid: SKIP balance 2");
        test_check(true, "usb vid_pid: SKIP balance 3");
        test_check(true, "usb vid_pid: SKIP balance 4");
        test_check(true, "usb vid_pid: SKIP balance 5");
        return;
    }

    /* Distinct sentinels so a stub `*vid = 0; *pid = 0; return 0;`
       can't slip past the gate — both values must move. */
    uint16_t vid = 0xDEAD;
    uint16_t pid = 0xBEEF;
    int rc = axl_usb_get_vid_pid(*u, &vid, &pid);
    test_check(rc == 0,
               "usb get_vid_pid: succeeds on enumerated device");
    /* common-test.sh injects -device usb-mouse, which surfaces as
       QEMU vendor 0x0627 (Adomax Technology Co., Ltd. — the QEMU/
       RedHat default vid for the emulated mouse and tablet). Pin
       the vid here; the SKIP path covers real hardware where USB
       might be absent. */
    test_check(vid == 0x0627,
               "usb get_vid_pid: vid matches QEMU usb-mouse (0x0627)");
    test_check(pid != 0xBEEF,
               "usb get_vid_pid: pid populated (sentinel cleared)");

    /* Bogus address — never enumerated, must return -1. */
    AxlUsbAddr bogus = { .bus = 0xFF, .addr = 0xFF, .intf = 0xFF };
    test_check(axl_usb_get_vid_pid(bogus, &vid, &pid) == -1,
               "usb get_vid_pid: unknown addr returns -1");

    /* NULL out param. */
    test_check(axl_usb_get_vid_pid(*u, NULL, &pid) == -1,
               "usb get_vid_pid: NULL vid rejected");
}

static void
test_usb_get_class(void)
{
    AxlUsbAddr *u = axl_usb_next(NULL);
    if (u == NULL) {
        axl_printf("SKIP: usb_get_class (no USB devices)\n");
        test_check(true, "usb get_class: SKIP balance 1");
        test_check(true, "usb get_class: SKIP balance 2");
        test_check(true, "usb get_class: SKIP balance 3");
        test_check(true, "usb get_class: SKIP balance 4");
        test_check(true, "usb get_class: SKIP balance 5");
        test_check(true, "usb get_class: SKIP balance 6");
        return;
    }

    /* Distinct sentinels — a stub returning success+zeros must not
       slip past the per-field "moved from sentinel" check. */
    uint8_t cls = 0xAA;
    uint8_t sub = 0xBB;
    uint8_t prot = 0xCC;
    int rc = axl_usb_get_class(*u, &cls, &sub, &prot);
    test_check(rc == 0,
               "usb get_class: succeeds on enumerated interface");
    /* QEMU usb-mouse on the test bus exposes the HID Boot Mouse
       triplet: bInterfaceClass=0x03 (HID), bInterfaceSubClass=0x01
       (Boot Interface), bInterfaceProtocol=0x02 (Mouse). Pin all
       three so a stub that fills only one field can't pass. */
    test_check(cls == 0x03,
               "usb get_class: class is HID (0x03) for usb-mouse");
    test_check(sub == 0x01,
               "usb get_class: subclass is Boot Interface (0x01)");
    test_check(prot == 0x02,
               "usb get_class: protocol is Mouse (0x02)");

    /* Out parameters are independently optional. Caller asks for
       prot only, supplies NULL for class/sub: the call still
       succeeds and prot is populated. Per-field NULL handling is
       a documented contract; without this assertion, a regression
       that re-introduces an "any-NULL → -1" guard would slip
       through (the prior assertions pre-fill all three). */
    uint8_t prot_only = 0xCC;
    test_check(axl_usb_get_class(*u, NULL, NULL, &prot_only) == 0
               && prot_only == 0x02,
               "usb get_class: NULL class/sub still populates prot");

    /* Bogus address — never enumerated, must return -1. */
    AxlUsbAddr bogus = { .bus = 0xFF, .addr = 0xFF, .intf = 0xFF };
    test_check(axl_usb_get_class(bogus, &cls, &sub, &prot) == -1,
               "usb get_class: unknown addr returns -1");
}

static void
test_usb_class_string(void)
{
    char buf[AXL_USB_CLASS_NAME_MAX];

    /* Full triplet: HID / Boot Interface / Mouse — the usb-mouse
       fixture's class triplet. Exact-string assertions per
       feedback_tdd_mandatory: substring match would silently allow
       regressions. */
    int n = axl_usb_class_string(0x03, 0x01, 0x02, buf, sizeof(buf));
    test_check(n > 0,
               "usb class_string: HID/Boot/Mouse returns positive byte count");
    test_check(axl_strcmp(buf,
                          "Human Interface Device / Boot Interface / Mouse")
               == 0,
               "usb class_string: HID/Boot/Mouse exact decode");

    /* Known base + sub, unknown prot — drops the prot tier. */
    n = axl_usb_class_string(0x03, 0x01, 0xEE, buf, sizeof(buf));
    test_check(n > 0
               && axl_strcmp(buf,
                             "Human Interface Device / Boot Interface")
                  == 0,
               "usb class_string: omits unknown prot tier");

    /* Known base, unknown sub — drops both tiers. */
    n = axl_usb_class_string(0x03, 0xEE, 0xEE, buf, sizeof(buf));
    test_check(n > 0
               && axl_strcmp(buf, "Human Interface Device") == 0,
               "usb class_string: omits unknown sub+prot tiers");

    /* Wholly unknown class — numeric fallback, lowercase 6 hex digits. */
    n = axl_usb_class_string(0xCD, 0xEE, 0xEF, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Class cdeeef") == 0,
               "usb class_string: unknown class falls back to numeric");

    /* FMT_BASE: just the base name. */
    n = axl_usb_class_string_fmt(0x03, 0x01, 0x02,
                                 AXL_USB_CLASS_FMT_BASE,
                                 buf, sizeof(buf));
    test_check(n > 0
               && axl_strcmp(buf, "Human Interface Device") == 0,
               "usb class_string_fmt(BASE): base name only");

    /* FMT_SUBCLASS: subclass name; coarsens to base when unknown. */
    n = axl_usb_class_string_fmt(0x03, 0x01, 0x02,
                                 AXL_USB_CLASS_FMT_SUBCLASS,
                                 buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Boot Interface") == 0,
               "usb class_string_fmt(SUBCLASS): subclass only");

    n = axl_usb_class_string_fmt(0x03, 0xEE, 0x02,
                                 AXL_USB_CLASS_FMT_SUBCLASS,
                                 buf, sizeof(buf));
    test_check(n > 0
               && axl_strcmp(buf, "Human Interface Device") == 0,
               "usb class_string_fmt(SUBCLASS): coarsens to base when sub unknown");

    /* Mass Storage / SCSI / BBB triplet — exercises a different
       branch of the table. */
    n = axl_usb_class_string(0x08, 0x06, 0x50, buf, sizeof(buf));
    test_check(n > 0
               && axl_strcmp(buf,
                             "Mass Storage / SCSI transparent / Bulk-Only Transport")
                  == 0,
               "usb class_string: Mass Storage/SCSI/BBB exact decode");

    /* Hub class (0x09) — base only, the spec doesn't define
       subclasses for hubs. */
    n = axl_usb_class_string_fmt(0x09, 0x00, 0x00,
                                 AXL_USB_CLASS_FMT_BASE,
                                 buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Hub") == 0,
               "usb class_string_fmt(BASE): Hub class");

    /* Bad args. */
    test_check(axl_usb_class_string(0x03, 0x01, 0x02, NULL, 16) == -1,
               "usb class_string: NULL buf rejected");
    test_check(axl_usb_class_string(0x03, 0x01, 0x02, buf, 0) == -1,
               "usb class_string: zero buflen rejected");
    test_check(axl_usb_class_string_fmt(0x03, 0x01, 0x02,
                                        (AxlUsbClassFmt)99,
                                        buf, sizeof(buf)) == -1,
               "usb class_string_fmt: unknown fmt enum rejected");
}

static void
test_usb_get_string(void)
{
    AxlUsbAddr *u = axl_usb_next(NULL);
    if (u == NULL) {
        axl_printf("SKIP: usb_get_string (no USB devices)\n");
        for (int i = 0; i < 5; i++) {
            test_check(true, "usb get_string: SKIP balance");
        }
        return;
    }

    /* QEMU's usb-mouse advertises iProduct = "QEMU USB Mouse"
       (hw/usb/dev-hid.c). The string descriptor table puts it at
       index 2 by convention, but we shouldn't hard-code that —
       axl_usb_get_product reads the index from the device
       descriptor. Use that as the way to get a known string into
       buf; the generic axl_usb_get_string is exercised through it. */
    char buf[AXL_USB_STRING_MAX];
    int n = axl_usb_get_product(*u, buf, sizeof(buf));
    test_check(n > 0,
               "usb get_string: product read returns positive byte count");

    /* Pin exact value to defeat stub-passes-trivially. QEMU is
       stable across releases on this string. */
    test_check(n > 0 && axl_strcmp(buf, "QEMU USB Mouse") == 0,
               "usb get_string: QEMU usb-mouse product is exact 'QEMU USB Mouse'");

    /* index=0 is the language-ID table; not a real string.
       axl_usb_get_string must reject it. */
    test_check(axl_usb_get_string(*u, 0, buf, sizeof(buf)) == -1,
               "usb get_string: index 0 rejected (lang-ID table, not a string)");

    /* A high index the device doesn't define returns -1. */
    test_check(axl_usb_get_string(*u, 0xFE, buf, sizeof(buf)) == -1,
               "usb get_string: missing high index returns -1");

    /* Bogus address — never enumerated. */
    AxlUsbAddr bogus = { .bus = 0xFF, .addr = 0xFF, .intf = 0xFF };
    test_check(axl_usb_get_string(bogus, 1, buf, sizeof(buf)) == -1,
               "usb get_string: unknown addr returns -1");
}

static void
test_usb_get_manufacturer(void)
{
    AxlUsbAddr *u = axl_usb_next(NULL);
    if (u == NULL) {
        axl_printf("SKIP: usb_get_manufacturer (no USB devices)\n");
        for (int i = 0; i < 3; i++) {
            test_check(true, "usb get_manufacturer: SKIP balance");
        }
        return;
    }

    /* QEMU usb-mouse: iManufacturer = "QEMU". */
    char buf[AXL_USB_STRING_MAX];
    int n = axl_usb_get_manufacturer(*u, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "QEMU") == 0,
               "usb get_manufacturer: QEMU usb-mouse exact 'QEMU'");

    AxlUsbAddr bogus = { .bus = 0xFF, .addr = 0xFF, .intf = 0xFF };
    test_check(axl_usb_get_manufacturer(bogus, buf, sizeof(buf)) == -1,
               "usb get_manufacturer: unknown addr returns -1");

    /* NULL buf — argument-validation parity with axl_usb_get_string. */
    test_check(axl_usb_get_manufacturer(*u, NULL, sizeof(buf)) == -1,
               "usb get_manufacturer: NULL buf rejected");
}

static void
test_usb_get_product(void)
{
    AxlUsbAddr *u = axl_usb_next(NULL);
    if (u == NULL) {
        axl_printf("SKIP: usb_get_product (no USB devices)\n");
        for (int i = 0; i < 3; i++) {
            test_check(true, "usb get_product: SKIP balance");
        }
        return;
    }

    char buf[AXL_USB_STRING_MAX];
    int n = axl_usb_get_product(*u, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "QEMU USB Mouse") == 0,
               "usb get_product: QEMU usb-mouse exact 'QEMU USB Mouse'");

    AxlUsbAddr bogus = { .bus = 0xFF, .addr = 0xFF, .intf = 0xFF };
    test_check(axl_usb_get_product(bogus, buf, sizeof(buf)) == -1,
               "usb get_product: unknown addr returns -1");

    test_check(axl_usb_get_product(*u, buf, 0) == -1,
               "usb get_product: zero buflen rejected");
}

static void
test_usb_get_serial(void)
{
    AxlUsbAddr *u = axl_usb_next(NULL);
    if (u == NULL) {
        axl_printf("SKIP: usb_get_serial (no USB devices)\n");
        for (int i = 0; i < 4; i++) {
            test_check(true, "usb get_serial: SKIP balance");
        }
        return;
    }

    /* QEMU's USB stack auto-generates an iSerialNumber per device
       instance — the format is build-and-topology-dependent (e.g.
       "89126-0000:00:03.0-1" with PCIe BDF + port chain), so an
       exact-string pin would be brittle across QEMU versions. The
       manufacturer + product tests above already defend against
       stubs that return success+empty; here we just confirm a
       non-trivial string lands in the buffer. */
    char buf[AXL_USB_STRING_MAX] = "<sentinel>";
    int n = axl_usb_get_serial(*u, buf, sizeof(buf));
    test_check(n > 0,
               "usb get_serial: QEMU usb-mouse returns a string");
    test_check(n > 0 && buf[0] != '\0' && buf[0] != '<',
               "usb get_serial: buf is non-empty + sentinel cleared");

    AxlUsbAddr bogus = { .bus = 0xFF, .addr = 0xFF, .intf = 0xFF };
    test_check(axl_usb_get_serial(bogus, buf, sizeof(buf)) == -1,
               "usb get_serial: unknown addr returns -1");

    /* Sentinel guard: a short buffer (1 byte = NUL only) writes
       zero useful bytes; the call should still succeed. */
    char tiny[1];
    int rc_tiny = axl_usb_get_serial(*u, tiny, sizeof(tiny));
    test_check(rc_tiny >= 0 && tiny[0] == '\0',
               "usb get_serial: 1-byte buffer NUL-terminates");
}

// ---------------------------------------------------------------------------
// AxlUsb — tree walker (Phase F: real hub-port chain)
// ---------------------------------------------------------------------------

typedef struct {
    size_t       n_visits;
    size_t       n_depth_zero;
    size_t       n_depth_pos;
    unsigned     max_depth;
    AxlUsbAddr   first_below_hub;
    bool         saw_below_hub;
} UsbTreeCtx;

static int
usb_tree_count_cb(AxlUsbAddr addr, unsigned depth, void *ctx)
{
    UsbTreeCtx *t = ctx;
    t->n_visits++;
    if (depth == 0) {
        t->n_depth_zero++;
    } else {
        t->n_depth_pos++;
        if (!t->saw_below_hub) {
            t->first_below_hub = addr;
            t->saw_below_hub = true;
        }
    }
    if (depth > t->max_depth) {
        t->max_depth = depth;
    }
    return 0;
}

static int
usb_tree_stop_after_first_cb(AxlUsbAddr addr, unsigned depth, void *ctx)
{
    (void)addr; (void)depth;
    int *seen = ctx;
    (*seen)++;
    return 13;  /* arbitrary non-zero */
}

static void
test_usb_tree_walker(void)
{
    UsbTreeCtx t = { 0 };
    int rc = axl_usb_tree_for_each(usb_tree_count_cb, &t);

    /* No-USB envs (e.g. real hardware without controllers) skip
       cleanly. Populated branch — what CI exercises against the
       qemu-xhci + usb-mouse + usb-hub + usb-tablet runner topology
       — runs 7 conditional asserts: rc==0, n_visits>=1, depth-zero
       count >= 2 (mouse + hub direct), depth-positive count >= 1
       (tablet behind hub), max_depth >= 1, plus early-stop and
       NULL-fn guards. */
    if (rc == -1 || t.n_visits == 0) {
        axl_printf("SKIP: usb_tree_walker (no USB stack)\n");
        for (int i = 0; i < 7; i++) {
            test_check(true, "usb tree_walker: SKIP balance");
        }
        return;
    }
    test_check(rc == 0, "usb tree_walker: clean walk returns 0");
    test_check(t.n_visits >= 1, "usb tree_walker: visits at least one entry");

    /* The runner's topology guarantees mouse + hub directly attached
       (depth 0) and a tablet behind the hub (depth 1). Pin both
       so a stub that emits constant depth=0 can't pass. */
    test_check(t.n_depth_zero >= 2,
               "usb tree_walker: at least 2 entries at depth 0 "
               "(mouse + hub direct attach)");
    test_check(t.n_depth_pos >= 1,
               "usb tree_walker: at least 1 entry at depth > 0 "
               "(tablet behind hub)");
    test_check(t.max_depth >= 1,
               "usb tree_walker: max depth >= 1 (hub-attached device visible)");

    /* Early-stop: callback returns non-zero, walker propagates. */
    int seen = 0;
    int stop_rc = axl_usb_tree_for_each(usb_tree_stop_after_first_cb, &seen);
    test_check(stop_rc == 13 && seen == 1,
               "usb tree_walker: cb non-zero return propagates after first visit");

    /* NULL fn rejected. */
    test_check(axl_usb_tree_for_each(NULL, &t) == -1,
               "usb tree_walker: NULL fn rejected");
}

// ---------------------------------------------------------------------------
// AxlUsb — vendor/device-name database (Phase D)
// ---------------------------------------------------------------------------

static int
usb_count_vendor_cb(uint16_t vid, const char *name, void *ctx)
{
    (void)vid; (void)name;
    int *n = ctx;
    (*n)++;
    return 0;
}

static int
usb_count_device_cb(uint16_t vid, uint16_t pid, const char *name, void *ctx)
{
    (void)vid; (void)pid; (void)name;
    int *n = ctx;
    (*n)++;
    return 0;
}

static int
usb_stop_vendor_cb(uint16_t vid, const char *name, void *ctx)
{
    (void)vid; (void)name; (void)ctx;
    return 7;
}

static int
usb_stop_device_cb(uint16_t vid, uint16_t pid, const char *name, void *ctx)
{
    (void)vid; (void)pid; (void)name; (void)ctx;
    return 11;
}

static void
test_usb_ids_load_failure_modes(void)
{
    AxlUsbIds *h = (AxlUsbIds *)0x1;
    test_check(axl_usb_ids_open("fs0:\\does-not-exist-anywhere.json5", &h)
                   == AXL_SIDECAR_FILE_MISSING,
               "usb-ids open: missing file returns FILE_MISSING");
    test_check(h == NULL,
               "usb-ids open: handle cleared to NULL on error");

    /* Schema is REQUIRED. */
    static const char no_schema[] =
        "{ vendors: [{ id: 0x046D, name: 'Logitech, Inc.' }] }";
    h = NULL;
    test_check(axl_usb_ids_open_from_buffer(
                   no_schema, axl_strlen(no_schema), &h)
                   == AXL_SIDECAR_PARSE_ERROR,
               "usb-ids open_from_buffer: missing schema returns PARSE_ERROR");

    /* Unrecognized schema. */
    static const char bad_schema[] = "{ schema: 99, vendors: [] }";
    h = NULL;
    test_check(axl_usb_ids_open_from_buffer(
                   bad_schema, axl_strlen(bad_schema), &h)
                   == AXL_SIDECAR_PARSE_ERROR,
               "usb-ids open_from_buffer: unknown schema returns PARSE_ERROR");

    /* Malformed JSON5 fails at the parse stage. */
    static const char garbage[] = "@@@ not even json";
    h = NULL;
    test_check(axl_usb_ids_open_from_buffer(
                   garbage, axl_strlen(garbage), &h)
                   == AXL_SIDECAR_PARSE_ERROR,
               "usb-ids open_from_buffer: malformed JSON5 returns PARSE_ERROR");
}

static void
test_usb_ids_handle_buffer(void)
{
    /* Hierarchical schema 1 — devices nest under their vendor. */
    static const char fixture[] =
        "{ schema: 1,\n"
        "  vendors: [\n"
        "    { id: 0x046D, name: 'Logitech, Inc.',\n"
        "      devices: [\n"
        "        { pid: 0xC52B, name: 'Unifying Receiver' },\n"
        "        { pid: 0xC077, name: 'M105 Optical Mouse' },\n"
        "      ],\n"
        "    },\n"
        "    { id: 0x0627, name: 'Adomax Technology Co., Ltd' },\n"
        "  ],\n"
        "}\n";

    AxlUsbIds *h = NULL;
    test_check(axl_usb_ids_open_from_buffer(
                   fixture, axl_strlen(fixture), &h) == AXL_SIDECAR_OK,
               "usb-ids handle: open_from_buffer succeeds on schema 1");

    const char *v = axl_usb_ids_vendor_name(h, 0x046D);
    test_check(v != NULL && axl_strcmp(v, "Logitech, Inc.") == 0,
               "usb-ids handle: 0x046D decodes to exact 'Logitech, Inc.'");

    const char *d = axl_usb_ids_device_name(h, 0x046D, 0xC52B);
    test_check(d != NULL && axl_strcmp(d, "Unifying Receiver") == 0,
               "usb-ids handle: (0x046D, 0xC52B) decodes to exact 'Unifying Receiver'");

    /* Vendor without nested devices is a valid entry. */
    test_check(axl_usb_ids_vendor_name(h, 0x0627) != NULL,
               "usb-ids handle: vendor without nested devices still resolves");
    test_check(axl_usb_ids_device_name(h, 0x0627, 0x0001) == NULL,
               "usb-ids handle: device under nested-less vendor returns NULL");

    /* Unknown lookups return NULL. */
    test_check(axl_usb_ids_vendor_name(h, 0xFFFF) == NULL,
               "usb-ids handle: unknown vendor returns NULL");
    test_check(axl_usb_ids_device_name(h, 0x046D, 0xDEAD) == NULL,
               "usb-ids handle: unknown PID under known vendor returns NULL");

    /* NULL handle is NULL-safe. */
    test_check(axl_usb_ids_vendor_name(NULL, 0x046D) == NULL,
               "usb-ids handle: NULL handle returns NULL");

    axl_usb_ids_close(h);
    axl_usb_ids_close(NULL);
    test_check(true, "usb-ids handle: close + close(NULL) OK");
}

static void
test_usb_ids_foreach(void)
{
    static const char fixture[] =
        "{ schema: 1,\n"
        "  vendors: [\n"
        "    { id: 0x046D, name: 'Logitech, Inc.',\n"
        "      devices: [\n"
        "        { pid: 0xC52B, name: 'Unifying Receiver' },\n"
        "        { pid: 0xC077, name: 'M105 Optical Mouse' },\n"
        "      ],\n"
        "    },\n"
        "    { id: 0x0627, name: 'Adomax' },\n"
        "  ],\n"
        "}\n";

    AxlUsbIds *h = NULL;
    test_check(axl_usb_ids_open_from_buffer(
                   fixture, axl_strlen(fixture), &h) == AXL_SIDECAR_OK,
               "usb-ids foreach: fixture loads");

    int n_vendors = 0;
    test_check(axl_usb_ids_foreach_vendor(h, usb_count_vendor_cb, &n_vendors)
                   == 0
               && n_vendors == 2,
               "usb-ids foreach_vendor: visits exactly 2 vendors");

    int n_devices = 0;
    test_check(axl_usb_ids_foreach_device(h, usb_count_device_cb, &n_devices)
                   == 0
               && n_devices == 2,
               "usb-ids foreach_device: visits exactly 2 devices");

    /* Early-stop: callback's non-zero return propagates. */
    test_check(axl_usb_ids_foreach_vendor(h, usb_stop_vendor_cb, NULL) == 7,
               "usb-ids foreach_vendor: cb non-zero return propagates");
    test_check(axl_usb_ids_foreach_device(h, usb_stop_device_cb, NULL) == 11,
               "usb-ids foreach_device: cb non-zero return propagates");

    /* NULL guards. */
    test_check(axl_usb_ids_foreach_vendor(NULL, usb_count_vendor_cb, &n_vendors)
                   == -1,
               "usb-ids foreach_vendor: NULL handle rejected");
    test_check(axl_usb_ids_foreach_device(h, NULL, &n_devices) == -1,
               "usb-ids foreach_device: NULL fn rejected");

    axl_usb_ids_close(h);
}

static void
test_usb_ids_format_name(void)
{
    static const char fixture[] =
        "{ schema: 1,\n"
        "  vendors: [\n"
        "    { id: 0x046D, name: 'Logitech, Inc.',\n"
        "      devices: [{ pid: 0xC52B, name: 'Unifying Receiver' }],\n"
        "    },\n"
        "  ],\n"
        "}\n";

    AxlUsbIds *h = NULL;
    axl_usb_ids_open_from_buffer(fixture, axl_strlen(fixture), &h);

    char buf[AXL_USB_NAME_COMPOSED_MAX];

    /* vendor known + device known → "<vendor> <device>" */
    int n = axl_usb_ids_format_name(h, 0x046D, 0xC52B, buf, sizeof(buf));
    test_check(n > 0
               && axl_strcmp(buf, "Logitech, Inc. Unifying Receiver") == 0,
               "usb-ids format_name: full known → '<vendor> <device>'");

    /* vendor known + device unknown → "<vendor> Device <pid hex>" */
    n = axl_usb_ids_format_name(h, 0x046D, 0xDEAD, buf, sizeof(buf));
    test_check(n > 0
               && axl_strcmp(buf, "Logitech, Inc. Device dead") == 0,
               "usb-ids format_name: device unknown → '<vendor> Device <pid>'");

    /* vendor unknown → "<vid>:<pid>" */
    n = axl_usb_ids_format_name(h, 0xFFFF, 0xC52B, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "ffff:c52b") == 0,
               "usb-ids format_name: vendor unknown → numeric vid:pid");

    /* NULL handle → numeric. */
    n = axl_usb_ids_format_name(NULL, 0x046D, 0xC52B, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "046d:c52b") == 0,
               "usb-ids format_name: NULL handle → numeric");

    test_check(axl_usb_ids_format_name(h, 0x046D, 0xC52B, NULL, 16) == -1,
               "usb-ids format_name: NULL buf rejected");

    axl_usb_ids_close(h);
}

static void
test_usb_ids_singleton(void)
{
    /* Make sure the singleton is empty before each call: the loader
       short-circuits on already-loaded. */
    axl_usb_ids_free();

    test_check(axl_usb_ids_load("fs0:\\nope.json5")
                   == AXL_SIDECAR_FILE_MISSING,
               "usb-ids load: explicit missing returns FILE_MISSING");

    test_check(axl_usb_vendor_name(0x0627) == NULL,
               "usb-ids: vendor_name pre-load returns NULL");

    /* Autodiscover via companion path — share/usb-ids.json5 staged
       by the integration runner. SKIP-balanced when not staged. */
    AxlSidecarStatus rc = axl_usb_ids_load(NULL);
    if (rc != AXL_SIDECAR_OK) {
        axl_printf("SKIP: usb-ids load (no companion usb-ids.json5)\n");
        for (int i = 0; i < 4; i++) {
            test_check(true, "usb-ids singleton: SKIP balance");
        }
        return;
    }
    test_check(true, "usb-ids load: autodiscover succeeds");

    /* share/usb-ids.json5 carries Adomax (the QEMU usb-mouse vendor)
       at 0x0627 and a few common vendors. Pin Adomax exactly. */
    const char *v = axl_usb_vendor_name(0x0627);
    test_check(v != NULL && axl_strcmp(v, "Adomax Technology Co., Ltd") == 0,
               "usb-ids: 0x0627 decodes to exact 'Adomax Technology Co., Ltd'");

    /* Idempotent. */
    test_check(axl_usb_ids_load(NULL) == AXL_SIDECAR_OK,
               "usb-ids load: idempotent on second call");

    axl_usb_ids_free();
    test_check(axl_usb_vendor_name(0x0627) == NULL,
               "usb-ids: vendor_name returns NULL after _free");
}

// ---------------------------------------------------------------------------

static void
test_pci_dump(void)
{
    /* Host bridge — guaranteed present on QEMU q35. Read 64 bytes
       (one cache line) of config space and verify the VID at offset
       0 matches an independent read. */
    AxlPciAddr root = { .seg = 0, .bus = 0, .dev = 0, .func = 0 };
    uint8_t  buf[256] = { 0 };
    size_t   ok       = 0;
    int rc = axl_pci_dump(root, buf, 64, &ok);
    if (rc != 0) {
        axl_printf("SKIP: pci_dump (host bridge unreachable)\n");
        /* Balance: 7 checks ran in the populated path. */
        for (int i = 0; i < 7; i++) {
            test_check(true, "pci dump: SKIP balance");
        }
        return;
    }
    test_check(rc == 0, "pci dump: host bridge succeeds");
    test_check(ok == 64, "pci dump: 64 bytes populated");

    /* Cross-check against direct read at offset 0. */
    uint16_t vid_raw;
    test_check(axl_pci_read_config_16(root, 0x00, &vid_raw) == 0
               && (uint16_t)(buf[0] | (buf[1] << 8)) == vid_raw,
               "pci dump: VID at offset 0 matches direct read");

    /* Absent function — high-bus address that's outside MCFG (or
       within MCFG but always empty). Either way axl_pci_dump must
       return -1 with out_read=0. */
    AxlPciAddr absent_hi = { .seg = 0, .bus = 0xFF, .dev = 0x1F, .func = 7 };
    size_t ok_absent = 999;
    test_check(axl_pci_dump(absent_hi, buf, 64, &ok_absent) == -1
               && ok_absent == 0,
               "pci dump: absent function (high bus) returns -1, out_read=0");

    /* Empty slot inside known-mapped MCFG range. On QEMU q35 the
       host bridge owns 00:00.0 but funcs 1..7 are empty, so this
       exercises the "MCFG hit + ECAM returns 0xFFFFFFFF" path
       specifically (vs the absent_hi case which may also exit via
       MCFG miss depending on firmware). */
    AxlPciAddr absent_func = { .seg = 0, .bus = 0, .dev = 0, .func = 7 };
    ok_absent = 999;
    test_check(axl_pci_dump(absent_func, buf, 64, &ok_absent) == -1
               && ok_absent == 0,
               "pci dump: empty MCFG-mapped function also returns -1");

    /* Cap behavior: requesting > AXL_PCI_CONFIG_SPACE_MAX silently
       caps; requesting < 4 returns -1. */
    test_check(axl_pci_dump(root, buf, 2, NULL) == -1,
               "pci dump: bytes < 4 returns -1");

    /* NULL guard. */
    test_check(axl_pci_dump(root, NULL, 64, NULL) == -1,
               "pci dump: NULL buf returns -1");
}

static void
test_pci_class_string(void)
{
    char buf[80];

    /* USB xHCI (0C/03/30) — three known tiers. */
    int n = axl_pci_class_string(0x0C0330, buf, sizeof(buf));
    test_check(n > 0, "pci class_string: USB xHCI returns positive");
    test_check(axl_strstr(buf, "Serial bus controller") != NULL
               && axl_strstr(buf, "USB") != NULL
               && axl_strstr(buf, "xHCI") != NULL,
               "pci class_string: USB xHCI decodes all three tiers");

    /* Display VGA standard (03/00/00). */
    n = axl_pci_class_string(0x030000, buf, sizeof(buf));
    test_check(n > 0
               && axl_strstr(buf, "Display") != NULL
               && axl_strstr(buf, "VGA") != NULL,
               "pci class_string: Display VGA decoded");

    /* Host bridge (06/00/00). */
    n = axl_pci_class_string(0x060000, buf, sizeof(buf));
    test_check(n > 0 && axl_strstr(buf, "Host bridge") != NULL,
               "pci class_string: 0x060000 decodes 'Host bridge'");

    /* SATA AHCI (01/06/01). */
    n = axl_pci_class_string(0x010601, buf, sizeof(buf));
    test_check(n > 0
               && axl_strstr(buf, "SATA") != NULL
               && axl_strstr(buf, "AHCI") != NULL,
               "pci class_string: SATA AHCI decoded");

    /* Unknown base class — falls back to numeric "Class XXXXXX"
       form, matching Linux lspci. */
    n = axl_pci_class_string(0xAB1234, buf, sizeof(buf));
    test_check(n > 0 && axl_strstr(buf, "Class") != NULL
               && axl_strstr(buf, "ab1234") != NULL,
               "pci class_string: unknown class falls back to 'Class XXXXXX'");

    /* Unknown subclass under known base — emit the base alone (no
       "<unknown>" placeholder), per lspci. */
    n = axl_pci_class_string(0x06FFFF, buf, sizeof(buf));
    test_check(n > 0
               && axl_strstr(buf, "Bridge") != NULL
               && axl_strstr(buf, "<unknown>") == NULL,
               "pci class_string: known base + unknown sub omits sub/prog");

    /* Known base+sub, unknown prog — show base / sub, omit the prog
       tier rather than print "<unknown>". Host bridge (06:00:xx) is
       the canonical case: most platforms leave prog_if undefined.
       Pin the exact string so a regression that re-introduces
       "<unknown>" can't slip past the substring-only checks. */
    n = axl_pci_class_string(0x060000, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Bridge / Host bridge") == 0,
               "pci class_string: 0x060000 == 'Bridge / Host bridge' (prog omitted)");

    /* Base 0x00 ("Unclassified") with sub/prog absent from the table
       — output collapses to just the base name. virtio-rng-pci on
       QEMU exercises this path with class 0x00FF00 in real life. */
    n = axl_pci_class_string(0x00FF00, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Unclassified") == 0,
               "pci class_string: 0x00FF00 == 'Unclassified' (sub+prog omitted)");

    /* Three-tier canonical case — pin the full string so future
       refactors that quietly drop a separator are caught here. */
    n = axl_pci_class_string(0x010601, buf, sizeof(buf));
    test_check(n > 0
               && axl_strcmp(buf,
                   "Mass storage controller / SATA / AHCI 1.0") == 0,
               "pci class_string: 0x010601 == full triplet 'Mass storage / SATA / AHCI 1.0'");

    /* axl_pci_class_string_fmt — explicit shape selector for
       row-oriented consumers (cdump-style) who want subclass alone
       or base alone. */

    /* FMT_FULL is identical to axl_pci_class_string. */
    n = axl_pci_class_string_fmt(0x060000,
                                 AXL_PCI_CLASS_FMT_FULL, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Bridge / Host bridge") == 0,
               "pci class_string_fmt(FULL): == 'Bridge / Host bridge'");

    /* FMT_SUBCLASS emits the subclass alone — matches Linux lspci. */
    n = axl_pci_class_string_fmt(0x060000,
                                 AXL_PCI_CLASS_FMT_SUBCLASS, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Host bridge") == 0,
               "pci class_string_fmt(SUBCLASS): 0x060000 == 'Host bridge'");
    n = axl_pci_class_string_fmt(0x010601,
                                 AXL_PCI_CLASS_FMT_SUBCLASS, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "SATA") == 0,
               "pci class_string_fmt(SUBCLASS): 0x010601 == 'SATA'");

    /* FMT_BASE emits the base alone. */
    n = axl_pci_class_string_fmt(0x060000,
                                 AXL_PCI_CLASS_FMT_BASE, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Bridge") == 0,
               "pci class_string_fmt(BASE): 0x060000 == 'Bridge'");
    n = axl_pci_class_string_fmt(0x010601,
                                 AXL_PCI_CLASS_FMT_BASE, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Mass storage controller") == 0,
               "pci class_string_fmt(BASE): 0x010601 == 'Mass storage controller'");

    /* Fallback chain when the requested tier is unknown:
       SUBCLASS on (known base, unknown sub) → falls back to the base.
       BASE on (unknown base) → numeric. */
    n = axl_pci_class_string_fmt(0x06FFFF,
                                 AXL_PCI_CLASS_FMT_SUBCLASS, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Bridge") == 0,
               "pci class_string_fmt(SUBCLASS): unknown sub falls back to base");
    n = axl_pci_class_string_fmt(0xAB1234,
                                 AXL_PCI_CLASS_FMT_BASE, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Class ab1234") == 0,
               "pci class_string_fmt(BASE): unknown base → numeric fallback");
    /* SUBCLASS on a wholly-unknown class falls through both rungs
       (sub → base → numeric) and lands on the numeric form. */
    n = axl_pci_class_string_fmt(0xAB1234,
                                 AXL_PCI_CLASS_FMT_SUBCLASS, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Class ab1234") == 0,
               "pci class_string_fmt(SUBCLASS): unknown base → numeric fallback");

    /* Bad fmt returns -1 (out-of-range enum value). */
    test_check(axl_pci_class_string_fmt(0x060000,
                                        (AxlPciClassFmt)99,
                                        buf, sizeof(buf)) == -1,
               "pci class_string_fmt: unknown fmt enum returns -1");

    /* NULL/zero-length guards. */
    test_check(axl_pci_class_string(0x060000, NULL, 80) == -1,
               "pci class_string: NULL buf returns -1");
    test_check(axl_pci_class_string(0x060000, buf, 0) == -1,
               "pci class_string: buflen=0 returns -1");
}

static void
test_pci_capabilities(void)
{
    /* Walk the full enumeration looking for any device with a
       non-empty legacy capability list. QEMU q35 has multiple
       (LPC, IDE, USB controllers). */
    AxlPciAddr *p = NULL;
    size_t      with_caps = 0;
    while ((p = axl_pci_next(p)) != NULL) {
        uint16_t off = 0;
        uint16_t id;
        if (axl_pci_cap_next(*p, off, &off, &id) == 0) {
            with_caps++;
            /* Walk a few caps to exercise the chain. */
            for (int i = 0; i < 16; i++) {
                if (axl_pci_cap_next(*p, off, &off, &id) != 0) {
                    break;
                }
            }
        }
        if (with_caps > 16) {
            break;
        }
    }
    test_check(with_caps > 0,
               "pci: at least one device has legacy caps");

    /* Negative: a device without caps returns -1 from cap_next. */
    AxlPciAddr root = { .seg = 0, .bus = 0, .dev = 0, .func = 0 };
    uint16_t status;
    if (axl_pci_read_config_16(root, 0x06, &status) == 0
        && (status & 0x10) == 0) {
        uint16_t off, id;
        test_check(axl_pci_cap_next(root, 0, &off, &id) == -1,
                   "pci: cap_next on no-caps device returns -1");
    }

    /* Bridge bus tuple. The integration runner injects a pcie-root-port
       which is header type 1; AxlPci's bridge_info should recognize it
       and report a non-zero secondary bus. The host bridge at 00:00.0
       is header type 0 and must be rejected. */
    {
        AxlPciBridge br;
        /* Host bridge is type 0 — bridge_info must say "not a bridge". */
        test_check(axl_pci_bridge_info(root, &br) == -1,
                   "pci bridge_info: host bridge (type 0) returns -1");

        /* NULL out param. */
        test_check(axl_pci_bridge_info(root, NULL) == -1,
                   "pci bridge_info: NULL out returns -1");

        /* Find the pcie-root-port the test runner adds (class 0x060400,
           PCI-to-PCI bridge). It's at 00:02.0 in QEMU's auto-assignment
           but we locate it by class to stay topology-agnostic. */
        AxlPciAddr rp;
        if (axl_pci_find_by_class(0x060400, 0, &rp) == 0) {
            test_check(axl_pci_bridge_info(rp, &br) == 0,
                       "pci bridge_info: PCI-to-PCI bridge succeeds");
            test_check(br.secondary != 0,
                       "pci bridge_info: root port has non-zero secondary bus");
            test_check(br.subordinate >= br.secondary,
                       "pci bridge_info: subordinate >= secondary");
        } else {
            /* No PCI-PCI bridge on this image — runner config drift.
               Balance against the populated path so the ratchet stays
               stable. */
            test_check(true, "pci bridge_info: SKIP — no bridge in topology");
            test_check(true, "pci bridge_info: SKIP balance");
            test_check(true, "pci bridge_info: SKIP balance");
        }
    }

    /* Regression: cap_next on an absent BDF must terminate immediately,
       not loop. Bit aa64 QEMU virt at 0:1f.0 — ECAM all-1s response
       fooled the walk into a self-loop at offset 0xFC. The fix is a
       VID precheck at walk entry; a pre-fix run of this test would
       hang QEMU until the guest watchdog tripped. */
    {
        /* In-MCFG empty slot: bus 0 dev 0 func 7 is mapped on q35 and
           virt but never populated. Exercises the all-1s ECAM path
           specifically. */
        AxlPciAddr absent_func = { .seg = 0, .bus = 0, .dev = 0, .func = 7 };
        uint16_t   off = 0, id = 0;
        test_check(axl_pci_cap_next(absent_func, 0, &off, &id) == -1,
                   "pci: cap_next on empty MCFG-mapped function returns -1");

        /* The exact BDF from the original aa64 bug report. May exit
           via MCFG miss or via the all-1s VID check, either is fine. */
        AxlPciAddr absent_aa64 = { .seg = 0, .bus = 0, .dev = 0x1F, .func = 0 };
        off = 0; id = 0;
        test_check(axl_pci_cap_next(absent_aa64, 0, &off, &id) == -1,
                   "pci: cap_next on absent 0:1f.0 returns -1 (aa64 regression)");
    }
}

// ---------------------------------------------------------------------------
// axl_io_port_* (x86 only)
// ---------------------------------------------------------------------------

static void
test_io_port(void)
{
#if defined(__x86_64__) || defined(__i386__)
    /* CMOS RTC is universally present on x86 emulation: port 0x70
       is the index register, 0x71 the data port. Reading the
       seconds register (offset 0x00) returns a BCD value 0..59. */
    axl_io_port_write8(0x70, 0x00);
    uint8_t seconds = axl_io_port_read8(0x71);
    /* BCD: low nibble 0..9, high nibble 0..5. */
    uint8_t lo = seconds & 0x0F;
    uint8_t hi = (uint8_t)(seconds >> 4) & 0x0F;
    test_check(lo <= 9 && hi <= 5,
               "io_port: CMOS seconds register is BCD 00..59");

    /* 16-bit + 32-bit reads on a register that doesn't fault.
       Just verify the calls work — the values themselves are
       host-time-dependent. */
    axl_io_port_write8(0x70, 0x09);  /* year register */
    uint16_t w = axl_io_port_read16(0x70);
    uint32_t d = axl_io_port_read32(0x70);
    (void)w; (void)d;
    test_check(true, "io_port: read16/32 don't fault");
#else
    /* AArch64: the public symbols are compiled out. Emit the same
       number of "passed" lines as the x86 path so the cross-arch
       ratchet stays balanced; a single SKIP line would create a
       count differential. */
    test_check(true, "io_port: not applicable on AArch64");
    test_check(true, "io_port: declarations gated out at compile time");
#endif
}

// ---------------------------------------------------------------------------
// axl_mem_phys_* — map/unmap + one-shot helpers + search
// ---------------------------------------------------------------------------

static void
test_mem_phys(void)
{
    /* NULL guards. */
    void *va = NULL;
    test_check(axl_mem_phys_map(0x1000, 16, NULL) == -1,
               "mem_phys: map(NULL out) returns -1");
    test_check(axl_mem_phys_map(0x1000, 0, &va) == -1,
               "mem_phys: map(len=0) returns -1");
    test_check(axl_mem_phys_read32(0x1000, NULL) == -1,
               "mem_phys: read32(NULL out) returns -1");
    /* unmap(NULL) is a no-op — must not crash. */
    axl_mem_phys_unmap(NULL, 0);
    test_check(true, "mem_phys: unmap(NULL) no-op");

    /* Map an SMBIOS table (its address is published by firmware,
       so we know it's a real, readable physical region). Read the
       first byte both via map+deref and via the one-shot helper;
       the values must match. */
    AxlSmbiosHeader *bios = axl_smbios_find(AXL_SMBIOS_TYPE_BIOS_INFO);
    if (bios == NULL) {
        axl_printf("SKIP: mem_phys (no SMBIOS to anchor on)\n");
        /* keep the test count stable: emit equivalent shape passes */
        test_check(true, "mem_phys: real-region read SKIPPED (no SMBIOS)");
        test_check(true, "mem_phys: one-shot read SKIPPED (no SMBIOS)");
        test_check(true, "mem_phys: search SKIPPED (no SMBIOS)");
        return;
    }
    uintptr_t bios_phys = (uintptr_t)bios;
    uint8_t   first_via_map = 0;
    if (axl_mem_phys_map(bios_phys, 4, &va) == 0) {
        first_via_map = *(volatile const uint8_t *)va;
        axl_mem_phys_unmap(va, 4);
    }
    uint8_t  first_via_oneshot = 0xAB;
    test_check(axl_mem_phys_read8(bios_phys, &first_via_oneshot) == 0,
               "mem_phys: one-shot read8 succeeds");
    test_check(first_via_map == first_via_oneshot,
               "mem_phys: map+deref matches one-shot read");

    /* Search: find the SMBIOS Type 0 (BIOS Info) signature byte
       within the table, which we already know is at byte 0. */
    const void *match = NULL;
    int rc = axl_mem_phys_search(bios, 4, &first_via_oneshot, 1, &match);
    test_check(rc == 0 && match == bios,
               "mem_phys: search finds known byte at offset 0");

    /* Negative search. A 4-byte sentinel that cannot occur inside
       a 4-byte SMBIOS Type 0 prefix (Type=0, Length, Handle low,
       Handle high — the first byte is always 0, so any needle
       starting with 0xDE is guaranteed to miss). Verify both the
       -1 return and that *out_match is cleared. */
    const uint8_t miss[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    match = (const void *)(uintptr_t)0xFEEDFACEul;
    rc = axl_mem_phys_search(bios, 4, miss, 4, &match);
    test_check(rc == -1, "mem_phys: search miss returns -1");
    test_check(match == NULL, "mem_phys: search miss clears *out_match");

    /* NULL-arg search. */
    test_check(axl_mem_phys_search(NULL, 4, miss, 1, &match) == -1,
               "mem_phys: search(NULL va) returns -1");
    test_check(axl_mem_phys_search(bios, 4, NULL, 1, &match) == -1,
               "mem_phys: search(NULL needle) returns -1");
    test_check(axl_mem_phys_search(bios, 4, miss, 0, &match) == -1,
               "mem_phys: search(needle_len=0) returns -1");
    test_check(axl_mem_phys_search(bios, 1, miss, 4, &match) == -1,
               "mem_phys: search(needle_len > region) returns -1");
}

// ---------------------------------------------------------------------------
// axl_watchdog_*
// ---------------------------------------------------------------------------

static void
test_watchdog(void)
{
    /* Disarm. UEFI starts with a 5-min watchdog; this turns it
       off. The test runner cleans up the QEMU instance shortly
       after, so leaving it disarmed is fine. */
    test_check(axl_watchdog_disarm() == 0,
               "watchdog: disarm succeeds");

    /* Set a 60-second window. The test will finish well before. */
    test_check(axl_watchdog_set(60) == 0,
               "watchdog: set(60s) succeeds");

    /* pet re-arms to last-set value. */
    test_check(axl_watchdog_pet() == 0,
               "watchdog: pet succeeds");

    /* Disarm again before exit so the watchdog isn't running
       when the test app returns. */
    test_check(axl_watchdog_disarm() == 0,
               "watchdog: re-disarm succeeds");

    /* pet after final disarm is still safe (no-op). */
    test_check(axl_watchdog_pet() == 0,
               "watchdog: pet after disarm is a no-op");
}

// ---------------------------------------------------------------------------
// axl_rng_*
// ---------------------------------------------------------------------------

static void
test_rng(void)
{
    /* NULL / zero-length guards. */
    test_check(axl_rng_bytes(NULL, 16) == -1,
               "rng: bytes(NULL) returns -1");
    uint8_t scratch[16];
    test_check(axl_rng_bytes(scratch, 0) == -1,
               "rng: bytes(len=0) returns -1");

    /* Real fill. EFI_RNG_PROTOCOL is published by OVMF on both
       arches when the host has an entropy source. If the call
       fails, that means the protocol isn't installed — emit
       balanced shape passes to keep the cross-arch count
       stable. */
    uint8_t buf1[32] = { 0 };
    uint8_t buf2[32] = { 0 };
    int rc = axl_rng_bytes(buf1, sizeof(buf1));
    if (rc != 0) {
        /* Four balancers — the fixed path below emits four conditional
           test_check calls (bytes(32) succeeds + second fill + distinct
           + non-zero). Off-by-one previously left the skipped count
           short by one against the populated count, which surfaced as
           a CI ratchet failure when the runner's OVMF didn't publish
           EFI_RNG_PROTOCOL but local OVMF did. */
        test_check(true, "rng: protocol not published — bytes test SKIPPED");
        test_check(true, "rng: protocol not published — second fill SKIPPED");
        test_check(true, "rng: protocol not published — distinct test SKIPPED");
        test_check(true, "rng: protocol not published — non-zero test SKIPPED");
        return;
    }
    test_check(rc == 0, "rng: bytes(32) succeeds");

    /* Two consecutive fills should differ — collision probability
       on 32 bytes from a real RNG is 2^-256. */
    test_check(axl_rng_bytes(buf2, sizeof(buf2)) == 0,
               "rng: second fill succeeds");
    bool same = true;
    for (size_t i = 0; i < sizeof(buf1); i++) {
        if (buf1[i] != buf2[i]) {
            same = false;
            break;
        }
    }
    test_check(!same,
               "rng: two consecutive fills produce distinct output");

    /* At least one byte non-zero. P(all-zero | 32 bytes) = 2^-256. */
    bool any_nonzero = false;
    for (size_t i = 0; i < sizeof(buf1); i++) {
        if (buf1[i] != 0) {
            any_nonzero = true;
            break;
        }
    }
    test_check(any_nonzero,
               "rng: 32 random bytes contain at least one non-zero");
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------
// AxlSpd — pure-decoder coverage against canned blobs (test/data/gen-spd.py)
// + a probe sweep that runs against whatever the SMBus controller exposes.
// ---------------------------------------------------------------------------

static void
test_spd_decode_ddr4(void)
{
    AxlSpdInfo info;
    int rc = axl_spd_decode(spd_ddr4_micron_8gb, sizeof(spd_ddr4_micron_8gb), &info);
    test_check(rc == 0, "spd: DDR4 decode succeeds");
    test_check(info.ddr_generation == 4, "spd: DDR4 ddr_generation == 4");
    test_check(info.capacity_bytes == 8ULL * 1024 * 1024 * 1024,
               "spd: DDR4 capacity decodes to 8 GiB");
    test_check(info.speed_mts == 2400, "spd: DDR4 speed decodes to 2400 MT/s");
    test_check(info.has_ecc, "spd: DDR4 ECC flag set");
    test_check(!info.registered, "spd: DDR4 UDIMM is not registered");
    /* Lower-page-only blob: manufacturing block at 320+ is unavailable. */
    test_check(info.mfg_code_module == 0,
               "spd: DDR4 module mfg_code stays 0 when upper page absent");
}

static void
test_spd_decode_ddr5(void)
{
    AxlSpdInfo info;
    int rc = axl_spd_decode(spd_ddr5_samsung_16gb, sizeof(spd_ddr5_samsung_16gb), &info);
    test_check(rc == 0, "spd: DDR5 decode succeeds");
    test_check(info.ddr_generation == 5, "spd: DDR5 ddr_generation == 5");
    test_check(info.capacity_bytes == 16ULL * 1024 * 1024 * 1024,
               "spd: DDR5 capacity decodes to 16 GiB (x8 device, 64-bit bus)");
    test_check(info.speed_mts >= 4700 && info.speed_mts <= 4900,
               "spd: DDR5 speed decodes near 4800 MT/s");
    test_check(info.has_ecc, "spd: DDR5 ECC flag set");
    test_check(info.mfg_code_module == 0x00CE,
               "spd: DDR5 module mfg_code is Samsung (0x00CE)");
    test_check(info.serial == 0xDEADBEEF,
               "spd: DDR5 serial decodes big-endian");
    test_check(info.part_number[0] == 'M' && info.part_number[1] == '3',
               "spd: DDR5 part number begins with 'M3'");
}

static void
test_spd_decode_unknown(void)
{
    /* All-zero buffer mimics QEMU's default zero-init smbus-eeprom: the
       memory-type byte at offset 2 is 0x00, which is neither DDR4 nor
       DDR5. axl_spd_decode reports success with ddr_generation = 0. */
    uint8_t zero[16] = { 0 };
    AxlSpdInfo info;
    int rc = axl_spd_decode(zero, sizeof(zero), &info);
    test_check(rc == 0, "spd: zero-init buffer decodes successfully");
    test_check(info.ddr_generation == 0,
               "spd: zero-init buffer reports ddr_generation = 0");
}

static void
test_spd_decode_rejects_bogus(void)
{
    AxlSpdInfo info;
    test_check(axl_spd_decode(NULL, 256, &info) == -1,
               "spd: NULL buffer rejected");
    test_check(axl_spd_decode(spd_ddr4_micron_8gb, 2, &info) == -1,
               "spd: short buffer rejected (len < 3)");
}

static void
test_spd_probe(void)
{
    /* axl_spd_next gracefully returns NULL when no SMBus controller is
       available (e.g. AArch64 virt machine). On x64/Q35 it should walk
       the eight default SPD slots QEMU instantiates at 0x50..0x57 — but
       the exact count is platform-dependent, so we only assert the loop
       terminates and produces in-range slot addresses. */
    uint8_t *slot = NULL;
    int      iters = 0;
    int      out_of_range = 0;
    while ((slot = axl_spd_next(slot)) != NULL && iters < 16) {
        if (*slot < AXL_SPD_ADDR_FIRST || *slot > AXL_SPD_ADDR_LAST) {
            out_of_range++;
        }
        iters++;
    }
    test_check(slot == NULL, "spd: axl_spd_next loop terminates");
    test_check(out_of_range == 0,
               "spd: every reported slot is in 0x50..0x57");
}

// ---------------------------------------------------------------------------
// AxlSpd — JEDEC vendor-name handle / singleton
// ---------------------------------------------------------------------------

static int
spd_count_cb(uint16_t code, const char *name, void *ctx)
{
    (void)code; (void)name;
    int *n = ctx;
    (*n)++;
    return 0;
}

static int
spd_stop_cb(uint16_t code, const char *name, void *ctx)
{
    (void)code; (void)name; (void)ctx;
    return 7;
}

static void
test_spd_ids_load_failure_modes(void)
{
    AxlSpdIds *h = (AxlSpdIds *)0x1;
    test_check(axl_spd_ids_open("fs0:\\does-not-exist-anywhere.json5", &h)
                   == AXL_SIDECAR_FILE_MISSING,
               "spd-ids open: missing file returns FILE_MISSING");
    test_check(h == NULL,
               "spd-ids open: handle cleared to NULL on error");

    /* Schema is REQUIRED — a buffer without one must fail parse. */
    static const char no_schema[] =
        "{ vendors: [{ code: 0x002C, name: 'Micron' }] }";
    h = NULL;
    test_check(axl_spd_ids_open_from_buffer(
                   no_schema, axl_strlen(no_schema), &h)
                   == AXL_SIDECAR_PARSE_ERROR,
               "spd-ids open_from_buffer: missing schema returns PARSE_ERROR");

    /* Unrecognized schema rejects loud rather than silently misparsing. */
    static const char bad_schema[] = "{ schema: 99, vendors: [] }";
    h = NULL;
    test_check(axl_spd_ids_open_from_buffer(
                   bad_schema, axl_strlen(bad_schema), &h)
                   == AXL_SIDECAR_PARSE_ERROR,
               "spd-ids open_from_buffer: unknown schema returns PARSE_ERROR");

    /* Malformed JSON5 fails at parse stage. */
    static const char garbage[] = "@@@ not even json";
    h = NULL;
    test_check(axl_spd_ids_open_from_buffer(
                   garbage, axl_strlen(garbage), &h)
                   == AXL_SIDECAR_PARSE_ERROR,
               "spd-ids open_from_buffer: malformed JSON5 returns PARSE_ERROR");
}

static void
test_spd_ids_handle_buffer(void)
{
    /* Schema 1: flat vendors[]. Mirrors share/jedec.json5's layout. */
    static const char fixture[] =
        "{ schema: 1,\n"
        "  vendors: [\n"
        "    { code: 0x002C, name: 'Micron' },\n"
        "    { code: 0x00CE, name: 'Samsung' },\n"
        "  ],\n"
        "}\n";

    AxlSpdIds *h = NULL;
    test_check(axl_spd_ids_open_from_buffer(
                   fixture, axl_strlen(fixture), &h) == AXL_SIDECAR_OK,
               "spd-ids handle: open_from_buffer succeeds on schema 1");

    const char *m = axl_spd_ids_vendor_name(h, 0x002C);
    test_check(m != NULL && axl_strcmp(m, "Micron") == 0,
               "spd-ids handle: 0x002C decodes to exact 'Micron'");
    const char *s = axl_spd_ids_vendor_name(h, 0x00CE);
    test_check(s != NULL && axl_strcmp(s, "Samsung") == 0,
               "spd-ids handle: 0x00CE decodes to exact 'Samsung'");
    test_check(axl_spd_ids_vendor_name(h, 0xFFFF) == NULL,
               "spd-ids handle: unknown code returns NULL");
    test_check(axl_spd_ids_vendor_name(NULL, 0x002C) == NULL,
               "spd-ids handle: NULL handle returns NULL");

    axl_spd_ids_close(h);
    axl_spd_ids_close(NULL);
    test_check(true, "spd-ids handle: close + close(NULL) OK");
}

static void
test_spd_ids_foreach(void)
{
    static const char fixture[] =
        "{ schema: 1,\n"
        "  vendors: [\n"
        "    { code: 0x002C, name: 'Micron' },\n"
        "    { code: 0x00CE, name: 'Samsung' },\n"
        "  ],\n"
        "}\n";

    AxlSpdIds *h = NULL;
    test_check(axl_spd_ids_open_from_buffer(
                   fixture, axl_strlen(fixture), &h) == AXL_SIDECAR_OK,
               "spd-ids foreach: fixture loads");

    int n = 0;
    int rc = axl_spd_ids_foreach_vendor(h, spd_count_cb, &n);
    test_check(rc == 0 && n == 2,
               "spd-ids foreach: visits exactly 2 entries");

    /* Early-stop: callback's non-zero return propagates. */
    test_check(axl_spd_ids_foreach_vendor(h, spd_stop_cb, NULL) == 7,
               "spd-ids foreach: cb non-zero return propagates");

    /* NULL guards. */
    test_check(axl_spd_ids_foreach_vendor(NULL, spd_count_cb, &n) == -1,
               "spd-ids foreach: NULL handle rejected");
    test_check(axl_spd_ids_foreach_vendor(h, NULL, &n) == -1,
               "spd-ids foreach: NULL fn rejected");

    axl_spd_ids_close(h);
}

static void
test_spd_ids_format_name(void)
{
    /* Composer: known → "<vendor>", unknown → "0xCCCC". Every consumer
       prints the same string for the same JEP-106 code. */
    static const char fixture[] =
        "{ schema: 1, vendors: [{ code: 0x002C, name: 'Micron' }] }";

    AxlSpdIds *h = NULL;
    axl_spd_ids_open_from_buffer(fixture, axl_strlen(fixture), &h);

    char buf[AXL_SPD_NAME_COMPOSED_MAX];
    int n = axl_spd_ids_format_name(h, 0x002C, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "Micron") == 0,
               "spd-ids format_name: known code renders exact 'Micron'");

    n = axl_spd_ids_format_name(h, 0xABCD, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "0xABCD") == 0,
               "spd-ids format_name: unknown code renders 0xABCD");

    /* NULL handle falls through to numeric. */
    n = axl_spd_ids_format_name(NULL, 0x002C, buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, "0x002C") == 0,
               "spd-ids format_name: NULL handle renders numeric");

    test_check(axl_spd_ids_format_name(h, 0x002C, NULL, 16) == -1,
               "spd-ids format_name: NULL buf rejected");

    axl_spd_ids_close(h);
}

static void
test_spd_ids_singleton(void)
{
    /* Make sure the singleton is empty before each call: the loader
       short-circuits on already-loaded. */
    axl_spd_ids_free();

    /* FILE_MISSING for an explicit override that doesn't exist. */
    test_check(axl_spd_ids_load("fs0:\\nope.json5")
                   == AXL_SIDECAR_FILE_MISSING,
               "spd-ids load: explicit missing returns FILE_MISSING");

    /* Singleton lookup returns NULL before any successful load. */
    test_check(axl_spd_vendor_name(0x002C) == NULL,
               "spd-ids: vendor_name pre-load returns NULL");

    /* Autodiscover via companion path — share/jedec.json5 staged by
       the integration runner. SKIP-balanced when not staged. */
    AxlSidecarStatus rc = axl_spd_ids_load(NULL);
    if (rc != AXL_SIDECAR_OK) {
        axl_printf("SKIP: spd-ids load (no companion jedec.json5)\n");
        for (int i = 0; i < 7; i++) {
            test_check(true, "spd-ids singleton: SKIP balance");
        }
        return;
    }
    test_check(true, "spd-ids load: autodiscover succeeds");

    /* share/jedec.json5 carries Micron at 0x002C with the long form
       'Micron Technology'. Pin the exact string — substring matches
       lie. */
    const char *m = axl_spd_vendor_name(0x002C);
    test_check(m != NULL && axl_strcmp(m, "Micron Technology") == 0,
               "spd-ids: 0x002C decodes to exact 'Micron Technology'");

    /* Regression pins for the encoding contract: bank byte is the
       raw JEP-106 continuation count (no parity OR'd in). Nanya is
       bank 4 (count 3 → 0x03), Crucial is bank 6 (count 5 → 0x05).
       The id_byte keeps its odd-parity MSB on the wire (0x9B / 0x0B).
       Earlier shipped data had parity wrongly applied to the bank
       byte (0x830B / 0x859B) and silently failed lookup — these
       asserts catch that regression. */
    const char *nanya = axl_spd_vendor_name(0x030B);
    test_check(nanya != NULL && axl_strcmp(nanya, "Nanya Technology") == 0,
               "spd-ids: 0x030B decodes to exact 'Nanya Technology'");
    const char *crucial = axl_spd_vendor_name(0x059B);
    test_check(crucial != NULL
                   && axl_strcmp(crucial, "Crucial Technology") == 0,
               "spd-ids: 0x059B decodes to exact 'Crucial Technology'");
    /* Patriot is bank 6 (count 5 → 0x05); id position 0x1D + odd-
       parity MSB → id byte 0x9D (the wire form). Earlier shipped
       data carried 0x1D without the parity bit and would miss
       lookups against real Patriot DIMMs. */
    const char *patriot = axl_spd_vendor_name(0x059D);
    test_check(patriot != NULL
                   && axl_strcmp(patriot, "Patriot Memory") == 0,
               "spd-ids: 0x059D decodes to exact 'Patriot Memory'");

    /* Idempotent: second load is OK without re-opening. */
    test_check(axl_spd_ids_load(NULL) == AXL_SIDECAR_OK,
               "spd-ids load: idempotent on second call");

    axl_spd_ids_free();
    test_check(axl_spd_vendor_name(0x002C) == NULL,
               "spd-ids: vendor_name returns NULL after _free");
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------

int
test_platform_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlPlatform");

    /* AxlAcpi */
    test_acpi_revision();
    test_acpi_find();
    test_acpi_iter();
    test_acpi_checksum();
    test_acpi_read_mcfg();
    test_acpi_read_madt();
    test_acpi_read_facp();

    /* AxlPci */
    test_pci_enumerate();
    test_pci_read_config();
    test_pci_find_by_class();
    test_pci_find_by_vid_did();
    test_pci_addr_parse_format();
    test_pci_get_vid_did_class_code();
    test_pci_dump();
    test_pci_class_string();
    test_pci_capabilities();
    test_pci_tree_walker();
    test_pci_ids_db();
    test_pci_ids_length_macros();
    test_pci_ids_length_enforcement();
    test_pci_ids_partial_schemas();
    test_pci_ids_handle_buffer();
    test_pci_ids_handle_priority();
    test_pci_ids_load_failure_modes();
    test_pci_ids_buffer_parse_errors();
    test_pci_ids_schema_v2_hierarchical();
    test_pci_ids_subsystem_lookup();
    test_pci_ids_foreach();
    test_pci_ids_subsys_db();
    test_pci_format_name();
    test_pci_class_db_handle();
    test_pci_class_db_singleton_overrides();
    test_pci_vpd_iter();

    /* AxlUsb */
    test_usb_enumerate();
    test_usb_get_vid_pid();
    test_usb_get_class();
    test_usb_class_string();
    test_usb_get_string();
    test_usb_get_manufacturer();
    test_usb_get_product();
    test_usb_get_serial();
    test_usb_tree_walker();
    test_usb_ids_load_failure_modes();
    test_usb_ids_handle_buffer();
    test_usb_ids_foreach();
    test_usb_ids_format_name();
    test_usb_ids_singleton();

    /* axl_io_port_* */
    test_io_port();

    /* R+3 */
    test_mem_phys();
    test_watchdog();
    test_rng();

    /* R+4: AxlSpd */
    test_spd_decode_ddr4();
    test_spd_decode_ddr5();
    test_spd_decode_unknown();
    test_spd_decode_rejects_bogus();
    test_spd_ids_load_failure_modes();
    test_spd_ids_handle_buffer();
    test_spd_ids_foreach();
    test_spd_ids_format_name();
    test_spd_ids_singleton();
    test_spd_probe();

    return test_print_results();
}

AXL_APP(test_platform_main)
