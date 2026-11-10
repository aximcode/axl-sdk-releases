/** @file axl-test-driver.c
    Unit tests for the AXL driver-authoring surface (axl-driver.h).

    Phase 1 covers the protocol-publishing primitive
    axl_protocol_install / axl_protocol_uninstall: the AXL-typed wrappers
    a Type-A resident driver uses to publish a protocol interface without
    a gBS-> drop-down. Tests install a sentinel interface under a private
    test GUID, then confirm via the firmware's own LocateProtocol that it
    really landed in the protocol database (and is gone after uninstall).

    AxlGuid is binary-compatible with EFI_GUID and AxlHandle is
    EFI_HANDLE, so the test casts between them to cross-check the AXL
    surface against the raw firmware view.
**/

#include "axl-test.h"
#include <axl/axl-driver.h>
#include <axl/axl-driver-deps.h>
#include <axl/axl-driver-info.h>
#include <axl/axl-image.h>
#include <axl/axl-image-verify.h>
#include <axl/axl-log.h>
#include <axl/axl-runtime.h>
#include <axl/axl-service.h>
#include <axl/axl-shared-driver.h>

//
// <uefi/axl-uefi.h> declares gBS and the EFI types used to cross-check
// the install against the firmware's own protocol database. Never
// include backend-internal headers from tests.
//
#include <uefi/axl-uefi.h>

// ---------------------------------------------------------------------------
// Private test GUIDs + sentinel interfaces (file scope: UEFI is single-process)
// ---------------------------------------------------------------------------

static const AxlGuid TEST_PROTO_GUID = {
    0xa1b2c3d4, 0xe5f6, 0x4789, { 0x8a, 0x0b, 0x1c, 0x2d, 0x3e, 0x4f, 0x50, 0x61 }
};
static const AxlGuid TEST_PROTO_GUID2 = {
    0xa1b2c3d4, 0xe5f6, 0x4789, { 0x8a, 0x0b, 0x1c, 0x2d, 0x3e, 0x4f, 0x50, 0x62 }
};

typedef struct {
    uint32_t magic;
} TestIface;

static TestIface g_iface  = { 0xCAFEF00D };
static TestIface g_iface2 = { 0xBADC0FFE };

// ---------------------------------------------------------------------------
// AxlDriverDeps: sidecar parse + transitive walk (pure, firmware-free)
// ---------------------------------------------------------------------------

// Two-level tree: Nic/Nic2 -> Comp -> SubComp; Solo is self-contained.
static const char DEPS_JSON[] =
    "{ schema: 1, drivers: ["
    " { name: 'Nic.efi',  requires: [ 'Comp.efi' ] },"
    " { name: 'Nic2.efi', requires: [ 'Comp.efi' ] },"
    " { name: 'Comp.efi', requires: [ 'SubComp.efi' ] },"
    " { name: 'Solo.efi' } ] }";

// A recording visitor: load() appends "name<parent", enter() can prune a name.
#define DEP_REC_MAX 16
static char        g_dep_rec[DEP_REC_MAX][96];
static size_t      g_dep_nrec;
static const char *g_dep_skip;     // enter() returns false for this name (prune)
static const char *g_dep_resident; // enter() returns false for this name (already-loaded reuse)

static void
dep_rec_reset(void)
{
    g_dep_nrec = 0;
    g_dep_skip = NULL;
    g_dep_resident = NULL;
}

static bool
dep_rec_enter(const char *name, const char *parent, void *ctx)
{
    (void)parent;
    (void)ctx;
    if (g_dep_skip != NULL && axl_strcmp(name, g_dep_skip) == 0) {
        return false;
    }
    if (g_dep_resident != NULL && axl_strcmp(name, g_dep_resident) == 0) {
        return false;
    }
    return true;
}

static void
dep_rec_load(const char *name, const char *parent, void *ctx)
{
    (void)ctx;
    if (g_dep_nrec < DEP_REC_MAX) {
        axl_snprintf(g_dep_rec[g_dep_nrec], sizeof g_dep_rec[0], "%s<%s",
                     name, parent != NULL ? parent : "");
        g_dep_nrec++;
    }
}

static void
test_deps_parse(void)
{
    AxlDriverDeps deps;
    AxlSidecarStatus rc = axl_driver_deps_load_buffer(DEPS_JSON, sizeof DEPS_JSON - 1,
                                                      "deptest", &deps);
    test_check(rc == AXL_SIDECAR_OK, "deps: valid buffer parses OK");
    test_check(deps.n_rows == 4, "deps: four driver rows parsed");

    const AxlDriverDepRow *nic = axl_driver_deps_lookup(&deps, "Nic.efi");
    test_check(nic != NULL, "deps: lookup finds a declaring driver");
    test_check(nic != NULL && nic->n_needs == 1
                   && axl_strcmp(nic->needs[0], "Comp.efi") == 0,
               "deps: Nic.efi requires exactly Comp.efi");

    const AxlDriverDepRow *comp = axl_driver_deps_lookup(&deps, "Comp.efi");
    test_check(comp != NULL && comp->n_needs == 1
                   && axl_strcmp(comp->needs[0], "SubComp.efi") == 0,
               "deps: Comp.efi requires exactly SubComp.efi (mid-tree)");

    const AxlDriverDepRow *solo = axl_driver_deps_lookup(&deps, "Solo.efi");
    test_check(solo != NULL && solo->n_needs == 0,
               "deps: Solo.efi is a declared, self-contained row");

    test_check(axl_driver_deps_lookup(&deps, "Ghost.efi") == NULL,
               "deps: lookup of an undeclared driver is NULL");

    // is_required: dependency drivers (incl. the mid-tree + leaf) are required;
    // NIC picks + self-contained are not.
    test_check(axl_driver_deps_is_required(&deps, "Comp.efi"),
               "deps: Comp.efi is required (mid-tree node)");
    test_check(axl_driver_deps_is_required(&deps, "SubComp.efi"),
               "deps: SubComp.efi is required (transitive leaf)");
    test_check(!axl_driver_deps_is_required(&deps, "Nic.efi"),
               "deps: Nic.efi is not a dependency");
    test_check(!axl_driver_deps_is_required(&deps, "Solo.efi"),
               "deps: Solo.efi is not a dependency");
}

static void
test_deps_walk_order(void)
{
    AxlDriverDeps deps;
    axl_driver_deps_load_buffer(DEPS_JSON, sizeof DEPS_JSON - 1, "deptest", &deps);
    AxlDriverDepVisitor v = { dep_rec_enter, dep_rec_load, NULL };

    // Post-order: SubComp (needed by Comp) before Comp (needed by Nic).
    // Nic itself is NOT loaded by the walk.
    dep_rec_reset();
    axl_driver_deps_walk(&deps, "Nic.efi", &v);
    test_check(g_dep_nrec == 2, "deps walk: two dependencies brought resident");
    test_check(g_dep_nrec == 2 && axl_strcmp(g_dep_rec[0], "SubComp.efi<Comp.efi") == 0,
               "deps walk: transitive leaf loaded first, attributed to Comp");
    test_check(g_dep_nrec == 2 && axl_strcmp(g_dep_rec[1], "Comp.efi<Nic.efi") == 0,
               "deps walk: dependency loaded after its subtree, attributed to Nic");

    // Solo declares nothing -> walk is a no-op.
    dep_rec_reset();
    axl_driver_deps_walk(&deps, "Solo.efi", &v);
    test_check(g_dep_nrec == 0, "deps walk: self-contained target loads no dependencies");
}

static void
test_deps_walk_prune(void)
{
    AxlDriverDeps deps;
    axl_driver_deps_load_buffer(DEPS_JSON, sizeof DEPS_JSON - 1, "deptest", &deps);
    AxlDriverDepVisitor v = { dep_rec_enter, dep_rec_load, NULL };

    // enter() false on Comp prunes Comp AND its subtree (SubComp): the
    // quarantine/skip mechanism must not load a skipped node's children.
    dep_rec_reset();
    g_dep_skip = "Comp.efi";
    axl_driver_deps_walk(&deps, "Nic.efi", &v);
    test_check(g_dep_nrec == 0,
               "deps walk: enter()=false prunes the node and its whole subtree");

    // Cross-walk reuse: a dependency already resident (enter()=false) is not
    // reloaded, mirroring a sweep that loads Comp once for Nic then skips it
    // for Nic2. SubComp is under the pruned Comp, so it too is skipped.
    dep_rec_reset();
    g_dep_resident = "Comp.efi";
    axl_driver_deps_walk(&deps, "Nic2.efi", &v);
    test_check(g_dep_nrec == 0,
               "deps walk: an already-resident dependency is not reloaded");
}

static void
test_deps_walk_cycle(void)
{
    // A mis-declared cycle (A -> B -> A) must terminate, not recurse forever.
    static const char cyc[] =
        "{ schema: 1, drivers: ["
        " { name: 'N.efi', requires: [ 'A.efi' ] },"
        " { name: 'A.efi', requires: [ 'B.efi' ] },"
        " { name: 'B.efi', requires: [ 'A.efi' ] } ] }";
    AxlDriverDeps deps;
    AxlSidecarStatus rc = axl_driver_deps_load_buffer(cyc, sizeof cyc - 1, "deptest", &deps);
    test_check(rc == AXL_SIDECAR_OK, "deps cycle: buffer parses OK");

    AxlDriverDepVisitor v = { dep_rec_enter, dep_rec_load, NULL };
    dep_rec_reset();
    axl_driver_deps_walk(&deps, "N.efi", &v);   // must return (no infinite recursion)
    test_check(g_dep_nrec == 2, "deps cycle: each node loaded once, walk terminates");
    test_check(g_dep_nrec == 2 && axl_strcmp(g_dep_rec[0], "B.efi<A.efi") == 0,
               "deps cycle: subtree loaded before the cyclic parent");
    test_check(g_dep_nrec == 2 && axl_strcmp(g_dep_rec[1], "A.efi<N.efi") == 0,
               "deps cycle: cyclic back-edge does not re-load the visited node");
}

static void
test_deps_walk_diamond(void)
{
    // Diamond: Top -> {L, R}, and both L and R -> D. The shared node D must
    // load EXACTLY ONCE (the visited set is shared across the whole walk, not
    // per-branch), and it must not be mistaken for a cycle. Post-order puts D
    // before both its consumers. This pins the safety property the cycle guard
    // provides on a converging-but-acyclic graph.
    static const char diamond[] =
        "{ schema: 1, drivers: ["
        " { name: 'Top.efi', requires: [ 'L.efi', 'R.efi' ] },"
        " { name: 'L.efi',   requires: [ 'D.efi' ] },"
        " { name: 'R.efi',   requires: [ 'D.efi' ] },"
        " { name: 'D.efi' } ] }";
    AxlDriverDeps deps;
    AxlSidecarStatus rc = axl_driver_deps_load_buffer(diamond, sizeof diamond - 1,
                                                      "deptest", &deps);
    test_check(rc == AXL_SIDECAR_OK, "deps diamond: buffer parses OK");

    AxlDriverDepVisitor v = { dep_rec_enter, dep_rec_load, NULL };
    dep_rec_reset();
    axl_driver_deps_walk(&deps, "Top.efi", &v);

    // D loaded once (not once per consumer), and before both L and R.
    size_t d_loads = 0, d_idx = 0, l_idx = 0, r_idx = 0;
    for (size_t i = 0; i < g_dep_nrec; i++) {
        if (axl_strncmp(g_dep_rec[i], "D.efi<", 6) == 0) { d_loads++; d_idx = i; }
        else if (axl_strncmp(g_dep_rec[i], "L.efi<", 6) == 0) { l_idx = i; }
        else if (axl_strncmp(g_dep_rec[i], "R.efi<", 6) == 0) { r_idx = i; }
    }
    test_check(d_loads == 1, "deps diamond: shared node D loaded exactly once");
    test_check(d_loads == 1 && d_idx < l_idx && d_idx < r_idx,
               "deps diamond: D loaded before both consumers L and R");
}

static void
test_deps_walk_missing_dep(void)
{
    // A dependency with no row of its own is a leaf: entered and loaded, its
    // (absent) subtree a no-op.
    static const char miss[] =
        "{ schema: 1, drivers: [ { name: 'Nic.efi', requires: [ 'Ghost.efi' ] } ] }";
    AxlDriverDeps deps;
    axl_driver_deps_load_buffer(miss, sizeof miss - 1, "deptest", &deps);

    AxlDriverDepVisitor v = { dep_rec_enter, dep_rec_load, NULL };
    dep_rec_reset();
    axl_driver_deps_walk(&deps, "Nic.efi", &v);
    test_check(g_dep_nrec == 1 && axl_strcmp(g_dep_rec[0], "Ghost.efi<Nic.efi") == 0,
               "deps walk: an undeclared dependency is still loaded as a leaf");
}

static void
test_deps_parse_errors(void)
{
    AxlDriverDeps deps;

    // Malformed JSON5 -> PARSE_ERROR, table left empty.
    AxlSidecarStatus rc = axl_driver_deps_load_buffer("{ this is not valid ]",
                                                      21, "deptest", &deps);
    test_check(rc == AXL_SIDECAR_PARSE_ERROR, "deps: malformed JSON5 -> PARSE_ERROR");
    test_check(deps.n_rows == 0, "deps: malformed parse leaves the table empty");

    // Over-cap requires list: only AXL_DRIVER_DEPS_PER_NODE are kept.
    static const char many[] =
        "{ schema: 1, drivers: [ { name: 'Nic.efi',"
        " requires: [ 'a.efi','b.efi','c.efi','d.efi','e.efi','f.efi' ] } ] }";
    rc = axl_driver_deps_load_buffer(many, sizeof many - 1, "deptest", &deps);
    test_check(rc == AXL_SIDECAR_OK, "deps: over-cap requires still parses OK");
    const AxlDriverDepRow *nic = axl_driver_deps_lookup(&deps, "Nic.efi");
    test_check(nic != NULL && nic->n_needs == AXL_DRIVER_DEPS_PER_NODE,
               "deps: requires list is capped at AXL_DRIVER_DEPS_PER_NODE");
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void
test_install_fresh_handle(void)
{
    AxlHandle h = NULL;   /* fresh-handle path */
    int r = axl_protocol_install(&TEST_PROTO_GUID, &g_iface, &h);
    test_check(r == AXL_OK, "install: fresh handle returns AXL_OK");
    test_check(h != NULL, "install: fresh handle was allocated (non-NULL)");

    /* Cross-check against the firmware's own view. */
    void      *found = NULL;
    EFI_STATUS s = gBS->LocateProtocol((EFI_GUID *)&TEST_PROTO_GUID, NULL, &found);
    test_check(!EFI_ERROR(s) && found == &g_iface,
               "install: protocol locatable and returns the installed interface");

    /* Install a SECOND protocol on the SAME handle — handle unchanged. */
    AxlHandle h2 = h;
    int r2 = axl_protocol_install(&TEST_PROTO_GUID2, &g_iface2, &h2);
    test_check(r2 == AXL_OK && h2 == h,
               "install: second protocol on existing handle keeps the handle");

    /* Uninstall the first; it must no longer be locatable. */
    int u = axl_protocol_uninstall(h, &TEST_PROTO_GUID, &g_iface);
    test_check(u == AXL_OK, "uninstall: returns AXL_OK");
    found = NULL;
    s = gBS->LocateProtocol((EFI_GUID *)&TEST_PROTO_GUID, NULL, &found);
    test_check(EFI_ERROR(s), "uninstall: protocol no longer locatable");

    /* Clean up the second. */
    test_check(axl_protocol_uninstall(h, &TEST_PROTO_GUID2, &g_iface2) == AXL_OK,
               "uninstall: second protocol returns AXL_OK");
}

static void
test_install_null_safety(void)
{
    AxlHandle h = NULL;
    test_check(axl_protocol_install(&TEST_PROTO_GUID, &g_iface, NULL) == AXL_ERR,
               "install: NULL handle pointer returns AXL_ERR");
    test_check(axl_protocol_install(NULL, &g_iface, &h) == AXL_ERR,
               "install: NULL guid returns AXL_ERR");
    test_check(axl_protocol_install(&TEST_PROTO_GUID, NULL, &h) == AXL_ERR,
               "install: NULL iface returns AXL_ERR");
    test_check(h == NULL, "install: rejected calls leave the handle untouched");

    test_check(axl_protocol_uninstall(NULL, &TEST_PROTO_GUID, &g_iface) == AXL_ERR,
               "uninstall: NULL handle returns AXL_ERR");
    test_check(axl_protocol_uninstall(&h, NULL, &g_iface) == AXL_ERR,
               "uninstall: NULL guid returns AXL_ERR");
}

// ---------------------------------------------------------------------------
// Buffer-load identity + app-dir-restricted load: safe-negative validation.
//
// Only the errors AXL's own validation produces BEFORE any firmware call are
// asserted here (NULL / 0 / a name that could escape the app directory, and a
// well-formed-but-absent sibling). A real LoadImage of a bogus blob is NOT
// exercised — on UEFI that faults rather than returning cleanly
// (feedback_uefi_firmware_test_hazards). The positive round-trip + the exact
// synthesized device-path string live in the QEMU integration test
// (test-driver-identity-qemu.sh).
// ---------------------------------------------------------------------------

static void
test_buffer_identity_and_sibling(void)
{
    AxlDriverHandle h     = (AxlDriverHandle)0x1;  // sentinel: must be cleared on reject
    unsigned char   blob[4] = { 'M', 'Z', 0, 0 };

    // axl_driver_load_buffer_with_image_info argument validation.
    test_check(axl_driver_load_buffer_with_image_info(NULL, 4, NULL, &h) == AXL_ERR,
               "load_buffer_with_image_info: NULL buffer returns AXL_ERR");
    test_check(axl_driver_load_buffer_with_image_info(blob, 0, NULL, &h) == AXL_ERR,
               "load_buffer_with_image_info: zero length returns AXL_ERR");
    test_check(axl_driver_load_buffer_with_image_info(blob, 4, NULL, NULL) == AXL_ERR,
               "load_buffer_with_image_info: NULL out_handle returns AXL_ERR");

    // axl_driver_load_sibling argument validation.
    h = (AxlDriverHandle)0x1;
    test_check(axl_driver_load_sibling(NULL, &h) == AXL_ERR,
               "load_sibling: NULL file_name returns AXL_ERR");
    test_check(h == NULL,
               "load_sibling: rejected call clears *out_handle");
    test_check(axl_driver_load_sibling("doDriver.efi", NULL) == AXL_ERR,
               "load_sibling: NULL out_handle returns AXL_ERR");

    // Path-escape guard: a bare basename only — no separator or drive prefix.
    test_check(axl_driver_load_sibling("sub/doDriver.efi", &h) == AXL_INVALID,
               "load_sibling: '/' in name returns AXL_INVALID");
    test_check(axl_driver_load_sibling("sub\\doDriver.efi", &h) == AXL_INVALID,
               "load_sibling: backslash in name returns AXL_INVALID");
    test_check(axl_driver_load_sibling("fs0:doDriver.efi", &h) == AXL_INVALID,
               "load_sibling: ':' in name returns AXL_INVALID");

    // A well-formed bare name not present in the app's own directory:
    // resolution succeeds, the file lookup does not. (The test EFI's
    // directory has no 'definitely-not-here.efi'.)
    test_check(axl_driver_load_sibling("definitely-not-here.efi", &h) == AXL_NOT_FOUND,
               "load_sibling: well-formed missing sibling returns AXL_NOT_FOUND");
}

/* A load must never load the CALLING image: a driver that shares the
   running image's file name would otherwise load (and, through ensure,
   start) the image itself -- the test binary re-running inside itself.
   test-axl.sh stages the unit binaries at fs0:\, so this binary is
   fs0:\AxlTestDriver.efi. */
static void
test_self_load_refused(void)
{
    /* Every spelling below names THIS file; a driver load of it is refused.
       Each is first shown to name an existing file, so an AXL_ERR is the
       refusal and not a path that did not resolve (unrefused, the load of
       the running image succeeds -- that is what RED showed). */
    static const struct {
        const char *path;
        const char *what;
    } spellings[] = {
        { "fs0:\\AxlTestDriver.efi",             "its own path" },
        { "FS0:/axltestdriver.EFI",              "another case and '/'" },
        { "fs0:\\selfdir\\..\\AxlTestDriver.efi", "a '..' segment" },
        { "fs0:\\.\\AxlTestDriver.efi",          "a '.' segment" },
        { "fs0:\\AxlTestDriver.efi.",            "a trailing dot (FAT trims it)" },
        { NULL,                                  "its 8.3 alias" },   /* found below */
    };
    /* The 8.3 alias depends on what else the disk holds (every AxlTest*.efi
       shares the stem, and the numeric tail grows: AXLTES~9, then
       AXLTE~10), so it is found by CONTENT: the alias whose bytes are this
       image's. */
    char   alias[32] = "";
    void  *me        = NULL;
    size_t me_len    = 0;
    if (axl_file_get_contents("fs0:\\AxlTestDriver.efi", &me, &me_len) == AXL_OK) {
        for (unsigned n = 1; n <= 99 && alias[0] == '\0'; n++) {
            char   cand[32];
            void  *b = NULL;
            size_t bl = 0;
            axl_snprintf(cand, sizeof(cand),
                         n < 10 ? "fs0:\\AXLTES~%u.EFI" : "fs0:\\AXLTE~%u.EFI", n);
            if (axl_file_get_contents(cand, &b, &bl) == AXL_OK && bl == me_len
                && axl_memcmp(b, me, me_len) == 0) {
                axl_strlcpy(alias, cand, sizeof(alias));
            }
            axl_free(b);
        }
    }
    axl_free(me);
    axl_dir_mkdir("fs0:\\selfdir");
    for (size_t i = 0; i < sizeof(spellings) / sizeof(spellings[0]); i++) {
        const char *sp = spellings[i].path != NULL ? spellings[i].path : alias;
        AxlFsEntry  e;
        bool exists = sp[0] != '\0' && axl_file_info(sp, &e) == AXL_OK
                      && !axl_fs_entry_is_dir(&e);
        AxlDriverHandle h  = (AxlDriverHandle)0x1;
        int             rc = exists ? axl_driver_load(sp, &h) : AXL_OK;
        char name[128];
        axl_snprintf(name, sizeof(name),
                     "self load: axl_driver_load of the running image by %s is refused",
                     spellings[i].what);
        test_check(exists && rc == AXL_ERR && h == NULL, name);
        if (rc == AXL_OK && h != NULL && h != (AxlDriverHandle)0x1) {
            axl_driver_unload(h);
        }
    }

    AxlDriverHandle h  = (AxlDriverHandle)0x1;
    int             rc = axl_driver_load_sibling("AxlTestDriver.efi", &h);
    test_check(rc == AXL_ERR && h == NULL,
               "self load: axl_driver_load_sibling of the image's own name is refused");
    if (rc == AXL_OK && h != NULL) {
        axl_driver_unload(h);
    }

    /* ensure with the image's own name and a protocol nothing publishes:
       the search must not load-and-start this binary; with no other copy it
       finds none. NOTE for whoever sees this suite misbehave: if this guard
       regresses, the check does not FAIL cleanly -- ensure STARTS this test
       binary, which runs the whole suite again inside itself (about thirty
       levels deep, then out of memory or the harness timeout). A recursion
       in AxlTestDriver means this check failed. */
    static const AxlGuid nobody = {
        0x5e1f10ad, 0x0bad, 0x4c0d, { 0x9a, 0x11, 0x5e, 0x1f, 0x10, 0xad, 0x00, 0x01 }
    };
    test_check(axl_driver_ensure(&nobody, "AxlTestDriver.efi") == AXL_NOT_FOUND,
               "self load: axl_driver_ensure with the image's own name finds nothing");

    /* An APPLICATION load of its own path is the caller's choice and works:
       a self-update probes the file it just staged over itself, and a
       re-exec runs it (SoftBMC's upgrade does both). Loaded, not started. */
    AxlImage *img = NULL;
    rc = axl_image_load("fs0:\\AxlTestDriver.efi", &img);
    test_check(rc == AXL_OK && img != NULL,
               "self load: axl_image_load of the caller's own path still loads");
    if (img != NULL) {
        axl_image_unload(img);
    }

    /* Controls: the same bytes elsewhere are not this file and load -- under
       another name, and under the SAME name in another directory (which a
       basename comparison would wrongly refuse). */
    static const char *const copies[] = {
        "fs0:\\axl-self-copy.tmp",
        "fs0:\\selfdir\\AxlTestDriver.efi",
    };
    void  *bytes  = NULL;
    size_t nbytes = 0;
    bool   read   = axl_file_get_contents("fs0:\\AxlTestDriver.efi", &bytes,
                                          &nbytes) == AXL_OK;
    for (size_t i = 0; i < sizeof(copies) / sizeof(copies[0]); i++) {
        h  = NULL;
        rc = AXL_ERR;
        if (read && axl_file_set_contents(copies[i], bytes, nbytes) == AXL_OK) {
            rc = axl_driver_load(copies[i], &h);
        }
        char name[128];
        axl_snprintf(name, sizeof(name),
                     "self load: a copy at %s loads (control)", copies[i]);
        test_check(rc == AXL_OK && h != NULL, name);
        if (h != NULL) {
            axl_driver_unload(h);
        }
        axl_file_delete(copies[i]);
    }
    axl_free(bytes);
    axl_dir_rmdir("fs0:\\selfdir");
}

/* The running image's identity is resolved ONCE per process: on BMC
   virtual media every component open is a slow USB round trip, and the
   search checks every candidate. Counted by the debug line the resolution
   emits. Earlier tests may already have resolved it, so the window allows
   at most one -- uncached, it is one per check. */
static int g_self_resolves;

static void
self_resolve_capture(int level, const char *domain, const char *message,
                     const AxlRealtime *stamp, void *data)
{
    (void)level;
    (void)stamp;
    (void)data;
    static const char prefix[] = "driver: the running image resolved as ";
    if (domain != NULL && axl_strcmp(domain, "driver") == 0
        && axl_strncmp(message, prefix, sizeof(prefix) - 1) == 0) {
        g_self_resolves++;
    }
}

static void
test_self_identity_cached(void)
{
    g_self_resolves = 0;
    axl_log_set_domain_level("driver", AXL_LOG_DEBUG);
    bool hooked = axl_log_add_handler(self_resolve_capture, NULL) == AXL_OK;
    int  refused = 0;
    for (int i = 0; i < 4; i++) {
        AxlDriverHandle h = NULL;
        if (axl_driver_load("fs0:\\AxlTestDriver.efi", &h) == AXL_ERR && h == NULL) {
            refused++;
        } else if (h != NULL) {
            axl_driver_unload(h);
        }
    }
    axl_log_remove_handler(self_resolve_capture);
    axl_log_set_domain_level("driver", -1);
    test_check(hooked && refused == 4 && g_self_resolves <= 1,
               "self load: the running image is resolved once per process, not per check");
}

// ---------------------------------------------------------------------------
// Type-B Driver Model binding — a synthetic "bus" protocol on a test-created
// controller, driven by the firmware's real ConnectController / Disconnect.
// ---------------------------------------------------------------------------

static const AxlGuid SYNTH_BIND_GUID = {
    0xb1c2d3e4, 0xf5a6, 0x47b8, { 0x9c, 0x0d, 0x2e, 0x3f, 0x40, 0x51, 0x62, 0x73 }
};
static TestIface g_synth_iface = { 0x5151D135 };

static int       g_bind_supported = 0;
static int       g_bind_start     = 0;
static int       g_bind_stop      = 0;
static AxlHandle g_bind_ctrl      = NULL;
static void     *g_bind_iface     = NULL;

static bool bind_supported(AxlHandle ctrl, void *ctx)
{
    (void)ctrl; (void)ctx; g_bind_supported++; return true;
}
static int bind_start(AxlHandle ctrl, void *iface, void *ctx)
{
    (void)ctx; g_bind_start++; g_bind_ctrl = ctrl; g_bind_iface = iface;
    return AXL_OK;
}
static int bind_stop(AxlHandle ctrl, void *ctx)
{
    (void)ctrl; (void)ctx; g_bind_stop++; return AXL_OK;
}

static void
test_driver_binding(void)
{
    /* A synthetic controller: a fresh handle carrying SYNTH_BIND_GUID. */
    AxlHandle controller = NULL;
    test_check(axl_protocol_install(&SYNTH_BIND_GUID, &g_synth_iface,
                                    &controller) == AXL_OK && controller != NULL,
               "binding: synthetic controller created");

    /* Install a Type-B binding that manages SYNTH_BIND_GUID controllers. */
    AxlDriverBinding db = {
        .name      = "axl-test-binding",
        .binds     = &SYNTH_BIND_GUID,
        .supported = bind_supported,
        .start     = bind_start,
        .stop      = bind_stop,
        .ctx       = NULL,
    };
    test_check(axl_driver_binding_install(&db) == AXL_OK, "binding: install OK");

    /* v1 is one binding per image handle: a second install is rejected
       (the firmware refuses a duplicate EFI_DRIVER_BINDING_PROTOCOL). */
    test_check(axl_driver_binding_install(&db) == AXL_ERR,
               "binding: second install on the image handle rejected (v1)");

    /* The firmware's ConnectController drives Supported -> Start. */
    EFI_STATUS s = gBS->ConnectController((EFI_HANDLE)controller, NULL, NULL, TRUE);
    test_check(!EFI_ERROR(s), "binding: ConnectController succeeded");
    test_check(g_bind_supported >= 1, "binding: Supported was consulted");
    test_check(g_bind_start == 1, "binding: Start fired exactly once");
    test_check(g_bind_ctrl == controller, "binding: Start got the controller handle");
    test_check(g_bind_iface == &g_synth_iface, "binding: Start got the bound interface");

    /* axl_driver_disconnect_handle drives Stop and releases the BY_DRIVER
       open (the AXL wrapper over the firmware's DisconnectController). */
    test_check(axl_driver_disconnect_handle(controller) == AXL_OK,
               "binding: axl_driver_disconnect_handle succeeded");
    test_check(g_bind_stop == 1, "binding: Stop fired on disconnect");

    /* Re-connect / re-disconnect: Start/Stop fire again. */
    s = gBS->ConnectController((EFI_HANDLE)controller, NULL, NULL, TRUE);
    test_check(!EFI_ERROR(s) && g_bind_start == 2,
               "binding: re-ConnectController fires Start again");
    test_check(axl_driver_disconnect_handle(controller) == AXL_OK,
               "binding: re-disconnect via handle succeeded");
    test_check(g_bind_stop == 2, "binding: re-Disconnect fires Stop again");

    /* Clean up the synthetic controller. The uninstall must succeed — if Stop
       failed to CloseProtocol the BY_DRIVER open, this would return AXL_ERR
       (the proto still open), catching that regression. */
    test_check(axl_protocol_uninstall(controller, &SYNTH_BIND_GUID,
                                      &g_synth_iface) == AXL_OK,
               "binding: synthetic controller uninstalled cleanly after Stop");

    /* Explicit teardown — the path a Type-B *driver* uses from its unload
       callback. The axl_atexit hook is a safety net on both exit paths now
       (axl_driver_cleanup drains it on driver unload), but it runs after the
       unload callback and so cannot disconnect or report. Proven to really
       remove the binding by a fresh install succeeding again afterward, then
       a final uninstall to leave clean. */
    test_check(axl_driver_binding_uninstall() == AXL_OK,
               "binding: explicit uninstall succeeds");
    test_check(axl_driver_binding_install(&db) == AXL_OK,
               "binding: re-install after uninstall succeeds (image slot freed)");
    test_check(axl_driver_binding_uninstall() == AXL_OK,
               "binding: second uninstall cleans up");
    test_check(axl_driver_binding_uninstall() == AXL_ERR,
               "binding: uninstall with nothing installed returns AXL_ERR");
}

// axl_driver_disconnect_handle's argument + NOT_FOUND contract, independent
// of a live binding (the bound-driver Stop path is covered by
// test_driver_binding above).
static void
test_disconnect_handle_contract(void)
{
    test_check(axl_driver_disconnect_handle(NULL) == AXL_ERR,
               "disconnect_handle: NULL handle returns AXL_ERR");

    /* A handle with no driver managing it disconnects cleanly (the firmware
       returns EFI_SUCCESS for an unmanaged controller; the wrapper also maps
       EFI_NOT_FOUND to AXL_OK as a defensive symmetry with connect_handle). */
    AxlHandle h = NULL;
    test_check(axl_protocol_install(&TEST_PROTO_GUID, &g_iface, &h) == AXL_OK,
               "disconnect_handle: scratch handle created");
    test_check(axl_driver_disconnect_handle(h) == AXL_OK,
               "disconnect_handle: nothing bound returns AXL_OK");
    test_check(axl_protocol_uninstall(h, &TEST_PROTO_GUID, &g_iface) == AXL_OK,
               "disconnect_handle: scratch handle cleaned up");
}

// ---------------------------------------------------------------------------
// AxlDriverInfo (P1) — the `drivers` view: enumerate loaded DriverBinding
// drivers with names, version, #managed, network classification. Read-only;
// exercised against the live OVMF/AAVMF driver set (dozens of DriverBinding
// drivers, including the firmware network stack).
// ---------------------------------------------------------------------------

static AxlDriverInfo g_drv_list[256];

static void
test_driver_info(void)
{
    /* NULL count is rejected. */
    test_check(axl_driver_list_loaded(NULL, 0, NULL) == AXL_ERR,
               "driver_info: list_loaded(NULL count) returns AXL_ERR");

    /* Count-only mode (out == NULL): the platform has DriverBinding drivers. */
    size_t total = 0;
    test_check(axl_driver_list_loaded(NULL, 0, &total) == AXL_OK,
               "driver_info: count-only returns AXL_OK");
    test_check(total > 0,
               "driver_info: at least one DriverBinding driver is loaded");

    /* Populate. */
    size_t n = 0;
    test_check(axl_driver_list_loaded(g_drv_list, 256, &n) == AXL_OK,
               "driver_info: populate returns AXL_OK");
    test_check(n == total,
               "driver_info: populated count matches the count-only total");

    size_t cap_written = (n < 256) ? n : 256;
    bool   any_name = false, any_handle = true, any_network = false;
    uint32_t total_managed = 0;
    for (size_t i = 0; i < cap_written; i++) {
        if (g_drv_list[i].name[0] != '\0') {
            any_name = true;
        }
        if (g_drv_list[i].handle == NULL) {
            any_handle = false;
        }
        if (g_drv_list[i].is_network) {
            any_network = true;
        }
        total_managed += g_drv_list[i].num_devices;
    }
    test_check(any_handle,
               "driver_info: every entry carries a DriverBinding handle");
    test_check(any_name,
               "driver_info: at least one driver has a ComponentName2 name");
    test_check(total_managed > 0,
               "driver_info: drivers manage at least one controller total");
    test_check(any_network,
               "driver_info: at least one network driver is classified");

    /* Truncation contract: cap smaller than the total still reports the full
       count and writes only `cap` entries. */
    size_t n2 = 0;
    test_check(axl_driver_list_loaded(g_drv_list, 1, &n2) == AXL_OK,
               "driver_info: truncated call returns AXL_OK");
    test_check(n2 == total,
               "driver_info: count is the full total even when cap < total");
}

// ---------------------------------------------------------------------------
// AxlDriverInfo (P1a/P2/P3/P5) — name a handle, PCI<->handle, bound query,
// targeted bind. Exercised against the live PCI topology.
// ---------------------------------------------------------------------------

static void
test_driver_discovery(void)
{
    char nbuf[128];

    /* P1a — handle name. */
    test_check(axl_handle_name(NULL, nbuf, sizeof(nbuf)) == AXL_ERR,
               "handle_name: NULL handle returns AXL_ERR");
    size_t ndrv = 0;
    axl_driver_list_loaded(g_drv_list, 256, &ndrv);
    bool named = false;
    for (size_t i = 0; i < ndrv && i < 256; i++) {
        if (axl_handle_name(g_drv_list[i].handle, nbuf, sizeof(nbuf)) == AXL_OK
            && nbuf[0] != '\0') {
            named = true;
            break;
        }
    }
    test_check(named, "handle_name: names a loaded driver handle");

    /* P2/P5 — a bogus PCI address resolves to nothing. */
    AxlPciAddr bogus = { .seg = 0xFFFF, .bus = 0xFF, .dev = 0x1F, .func = 7 };
    AxlHandle  bh    = (AxlHandle)(uintptr_t)0x1;
    test_check(axl_pci_to_handle(bogus, &bh) == AXL_ERR && bh == NULL,
               "pci_to_handle: bogus address returns AXL_ERR + clears out");
    test_check(axl_pci_to_handle(bogus, NULL) == AXL_ERR,
               "pci_to_handle: NULL out returns AXL_ERR");
    bool bd = true;
    test_check(axl_pci_driver_bound(bogus, &bd, NULL, 0) == AXL_ERR,
               "pci_driver_bound: bogus address returns AXL_ERR");
    test_check(axl_pci_driver_bound(bogus, NULL, NULL, 0) == AXL_ERR,
               "pci_driver_bound: NULL bound returns AXL_ERR");

    /* P3 — NULL controller guard. */
    test_check(axl_driver_bind(NULL, NULL) == AXL_ERR,
               "driver_bind: NULL controller returns AXL_ERR");

    /* P2/P5 — a real PCI function resolves to its controller handle. */
    AxlPciAddr *p    = NULL;
    AxlHandle   ctrl = NULL;
    AxlPciAddr  ctrl_addr = {0};
    while ((p = axl_pci_next(p)) != NULL) {
        if (axl_pci_to_handle(*p, &ctrl) == AXL_OK) {
            ctrl_addr = *p;
            break;
        }
    }
    test_check(ctrl != NULL,
               "pci_to_handle: a real PCI function resolves to a handle");
    if (ctrl != NULL) {
        /* P2 — the bound query succeeds. */
        bool bound = false;
        char dn[64];
        test_check(axl_pci_driver_bound(ctrl_addr, &bound, dn, sizeof(dn))
                       == AXL_OK,
                   "pci_driver_bound: real device query returns AXL_OK");

        /* P1a — the resolved controller also names (device-path fallback when
           it carries no ComponentName2). */
        test_check(axl_handle_name(ctrl, nbuf, sizeof(nbuf)) == AXL_OK
                       && nbuf[0] != '\0',
                   "handle_name: names a controller handle (device-path)");
    }

    /* P3 — find a PCI controller that IS bound (the PCI bus driver manages
       bridges, so at least one exists on both arches) and re-bind it:
       already-connected => verified-managed => AXL_OK. This exercises the
       success path without depending on connecting an unbound device. */
    AxlPciAddr *q = NULL;
    AxlHandle   bound_ctrl = NULL;
    while ((q = axl_pci_next(q)) != NULL) {
        AxlHandle h = NULL;
        bool      b = false;
        if (axl_pci_to_handle(*q, &h) == AXL_OK
            && axl_pci_driver_bound(*q, &b, NULL, 0) == AXL_OK && b) {
            bound_ctrl = h;
            break;
        }
    }
    test_check(bound_ctrl != NULL,
               "pci_driver_bound: at least one bound PCI controller exists");
    if (bound_ctrl != NULL) {
        test_check(axl_driver_bind(bound_ctrl, NULL) == AXL_OK,
                   "driver_bind: re-connect an already-bound controller is AXL_OK");
    }

    /* P4 — the network base-class constant, and raw SNP/NII handle
       enumeration by name (the name table now carries "nii"). Count may be 0
       on a NIC-less platform; the point is the name resolves (AXL_OK). */
    test_check(AXL_PCI_CLASS_NETWORK == 0x02,
               "p4: AXL_PCI_CLASS_NETWORK is the network base class");
    void  **snp_h = NULL;
    size_t  snp_n = 0;
    test_check(axl_protocol_enumerate("simple-network", &snp_h, &snp_n) == AXL_OK,
               "p4: enumerate(\"simple-network\") resolves the SNP name");
    axl_free(snp_h);
    void  **nii_h = NULL;
    size_t  nii_n = 0;
    test_check(axl_protocol_enumerate("nii", &nii_h, &nii_n) == AXL_OK,
               "p4: enumerate(\"nii\") resolves the NII name");
    axl_free(nii_h);
}

// ---------------------------------------------------------------------------
// AxlDriverInfo (P1b/P1c) — generic handle + protocol enumeration: the Devices
// tab and the per-NIC protocol-stack view. axl_handle_list,
// axl_handle_protocols, axl_net_protocol_name, axl_handle_drivers,
// axl_handle_children. Exercised against the live OVMF/AAVMF handle database
// (with the network stack connected — the unit boot runs connect -r + DHCP).
// ---------------------------------------------------------------------------

/* Canonical UEFI protocol GUIDs (the test owns these literals so it can both
   filter by them and round-trip them through axl_net_protocol_name). */
static const AxlGuid GUID_DEVICE_PATH =
    AXL_GUID(0x09576e91, 0x6d3f, 0x11d2,
             0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b);
static const AxlGuid GUID_SIMPLE_NETWORK =
    AXL_GUID(0xA19832B9, 0xAC25, 0x11D3,
             0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D);
static const AxlGuid GUID_IP4CONFIG2 =
    AXL_GUID(0x5b446ed1, 0xe30b, 0x4faa,
             0x87, 0x1a, 0x36, 0x54, 0xec, 0xa3, 0x60, 0x80);
/* A non-protocol GUID identifier — its canonical name keeps the "_GUID". */
static const AxlGuid GUID_ACPI_TABLE =
    AXL_GUID(0x8868e871, 0xe4f1, 0x11d3,
             0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81);
/* A GUID no protocol uses — for the unknown-GUID name path. */
static const AxlGuid GUID_BOGUS =
    AXL_GUID(0xdeadbeef, 0x1234, 0x5678,
             0x9a, 0xbc, 0xde, 0xf0, 0x11, 0x22, 0x33, 0x44);

static AxlHandle g_henum[512];
static AxlGuid   g_penum[64];

/* Append the recognized net-protocol names installed on @p h to @p line
   (comma-separated), so the diagnostic shows where each stack layer lives. */
static void
append_net_names(AxlHandle h, char *line, size_t cap)
{
    size_t np = 0;
    if (axl_handle_protocols(h, g_penum, 64, &np) != AXL_OK) {
        return;
    }
    for (size_t i = 0; i < np && i < 64; i++) {
        char nm[24];
        if (axl_net_protocol_name(&g_penum[i], nm, sizeof(nm)) != AXL_OK) {
            continue;
        }
        size_t used = axl_strlen(line);
        if (used > 0 && used + 1 < cap) {
            line[used++] = ',';
            line[used]   = '\0';
        }
        axl_strlcpy(line + axl_strlen(line), nm, cap - axl_strlen(line));
    }
}

/* Diagnostic: where does the IP4 networking stack land — on the SNP controller
   handle itself, or on its child handles? Determines whether the per-NIC stack
   view must walk children. Prints P1C_STACK lines so the answer is readable
   from the serial log. Not an assertion (the exact set is platform-dependent). */
static void
probe_net_stack_location(void)
{
    size_t nsnp = 0;
    if (axl_handle_list(&GUID_SIMPLE_NETWORK, g_henum, 512, &nsnp) != AXL_OK
        || nsnp == 0) {
        axl_printf("P1C_STACK: no SNP handle\n");
        return;
    }
    AxlHandle snp = g_henum[0];

    /* Net protocols directly on the SNP/NIC controller handle. */
    char on_ctrl[256] = "";
    append_net_names(snp, on_ctrl, sizeof(on_ctrl));

    /* Net protocols across the SNP handle's children. */
    AxlHandle kids[64];
    size_t    nkids = 0;
    char      on_kids[256] = "";
    axl_handle_children(snp, kids, 64, &nkids);
    for (size_t k = 0; k < nkids && k < 64; k++) {
        append_net_names(kids[k], on_kids, sizeof(on_kids));
    }

    axl_printf("P1C_STACK: snp_handles=%zu nchildren=%zu\n", nsnp, nkids);
    axl_printf("P1C_STACK: on_controller=[%s]\n", on_ctrl);
    axl_printf("P1C_STACK: on_children=[%s]\n", on_kids);
}

static void
test_handle_enum(void)
{
    // --- axl_handle_list ----------------------------------------------------
    test_check(axl_handle_list(NULL, NULL, 0, NULL) == AXL_ERR,
               "handle_list: NULL count returns AXL_ERR");

    size_t all = 0;
    test_check(axl_handle_list(NULL, NULL, 0, &all) == AXL_OK,
               "handle_list: count-only (all handles) returns AXL_OK");
    test_check(all > 0, "handle_list: the handle database is non-empty");

    size_t n = 0;
    test_check(axl_handle_list(NULL, g_henum, 512, &n) == AXL_OK && n == all,
               "handle_list: populate count matches count-only total");
    bool all_nonnull = true;
    for (size_t i = 0; i < n && i < 512; i++) {
        if (g_henum[i] == NULL) {
            all_nonnull = false;
        }
    }
    test_check(all_nonnull, "handle_list: every enumerated handle is non-NULL");

    size_t n1 = 0;
    test_check(axl_handle_list(NULL, g_henum, 1, &n1) == AXL_OK && n1 == all,
               "handle_list: count is full total even when cap < total");

    size_t ndp = 0;
    test_check(axl_handle_list(&GUID_DEVICE_PATH, NULL, 0, &ndp) == AXL_OK,
               "handle_list: by-GUID (DevicePath) count returns AXL_OK");
    test_check(ndp > 0 && ndp <= all,
               "handle_list: DevicePath subset is non-empty and <= all handles");

    // --- axl_handle_protocols ----------------------------------------------
    test_check(axl_handle_protocols(NULL, NULL, 0, NULL) == AXL_ERR,
               "handle_protocols: NULL handle returns AXL_ERR");

    size_t ndp_h = 0;
    axl_handle_list(&GUID_DEVICE_PATH, g_henum, 512, &ndp_h);
    test_check(ndp_h > 0, "handle_protocols: have a DevicePath handle to inspect");
    if (ndp_h > 0) {
        size_t np = 0;
        test_check(axl_handle_protocols(g_henum[0], NULL, 0, &np) == AXL_OK
                       && np > 0,
                   "handle_protocols: count-only returns AXL_OK with >0 protocols");
        size_t np2 = 0;
        test_check(axl_handle_protocols(g_henum[0], g_penum, 64, &np2) == AXL_OK
                       && np2 == np,
                   "handle_protocols: populate count matches count-only total");
        bool has_dp = false;
        for (size_t i = 0; i < np2 && i < 64; i++) {
            if (axl_guid_equal(&g_penum[i], &GUID_DEVICE_PATH)) {
                has_dp = true;
            }
        }
        test_check(has_dp,
                   "handle_protocols: a DevicePath handle lists DevicePath (reverse-consistent)");
    }

    // --- axl_net_protocol_name (exact strings) -----------------------------
    char nm[32];
    test_check(axl_net_protocol_name(NULL, nm, sizeof(nm)) == AXL_ERR,
               "net_protocol_name: NULL guid returns AXL_ERR");
    test_check(axl_net_protocol_name(&GUID_SIMPLE_NETWORK, nm, sizeof(nm)) == AXL_OK
                   && axl_strcmp(nm, "SimpleNetwork") == 0,
               "net_protocol_name: SNP GUID -> \"SimpleNetwork\"");
    test_check(axl_net_protocol_name(&GUID_IP4CONFIG2, nm, sizeof(nm)) == AXL_OK
                   && axl_strcmp(nm, "Ip4Config2") == 0,
               "net_protocol_name: Ip4Config2 GUID -> \"Ip4Config2\"");
    test_check(axl_net_protocol_name(&GUID_DEVICE_PATH, nm, sizeof(nm)) == AXL_ERR,
               "net_protocol_name: non-net GUID (DevicePath) returns AXL_ERR");

    // --- axl_handle_drivers -------------------------------------------------
    test_check(axl_handle_drivers(NULL, NULL, 0, NULL) == AXL_ERR,
               "handle_drivers: NULL controller returns AXL_ERR");

    /* A bound PCI controller (the PCI bus driver manages bridges on both
       arches) has at least one managing driver. */
    AxlPciAddr *q          = NULL;
    AxlHandle   bound_ctrl = NULL;
    while ((q = axl_pci_next(q)) != NULL) {
        AxlHandle h = NULL;
        bool      b = false;
        if (axl_pci_to_handle(*q, &h) == AXL_OK
            && axl_pci_driver_bound(*q, &b, NULL, 0) == AXL_OK && b) {
            bound_ctrl = h;
            break;
        }
    }
    test_check(bound_ctrl != NULL, "handle_drivers: a bound PCI controller exists");
    if (bound_ctrl != NULL) {
        size_t nd = 0;
        test_check(axl_handle_drivers(bound_ctrl, NULL, 0, &nd) == AXL_OK && nd >= 1,
                   "handle_drivers: a bound controller has >= 1 managing driver");
        AxlHandle drv[8] = { NULL };
        size_t    nd2    = 0;
        test_check(axl_handle_drivers(bound_ctrl, drv, 8, &nd2) == AXL_OK && nd2 == nd,
                   "handle_drivers: populate count matches count-only total");
        test_check(nd2 == 0 || drv[0] != NULL,
                   "handle_drivers: managing driver handle is non-NULL");
    }

    // --- axl_handle_children ------------------------------------------------
    test_check(axl_handle_children(NULL, NULL, 0, NULL) == AXL_ERR,
               "handle_children: NULL controller returns AXL_ERR");

    /* Some controller has children (a PCI root bridge / bus always produces
       child controllers). Scan for the first one. */
    size_t    nall          = 0;
    AxlHandle parent         = NULL;
    size_t    parent_nkids   = 0;
    axl_handle_list(NULL, g_henum, 512, &nall);
    size_t scan = (nall < 512) ? nall : 512;
    for (size_t i = 0; i < scan; i++) {
        size_t nk = 0;
        if (axl_handle_children(g_henum[i], NULL, 0, &nk) == AXL_OK && nk > 0) {
            parent       = g_henum[i];
            parent_nkids = nk;
            break;
        }
    }
    test_check(parent != NULL && parent_nkids > 0,
               "handle_children: a controller with child controllers exists");
    if (parent != NULL) {
        AxlHandle kids[64] = { NULL };
        size_t    nk2      = 0;
        test_check(axl_handle_children(parent, kids, 64, &nk2) == AXL_OK,
                   "handle_children: populate returns AXL_OK");
        bool kids_nonnull = (nk2 > 0);
        for (size_t i = 0; i < nk2 && i < 64; i++) {
            if (kids[i] == NULL) {
                kids_nonnull = false;
            }
        }
        test_check(kids_nonnull, "handle_children: every child handle is non-NULL");
    }

    // --- axl_protocol_guid_name (canonical spec names; exact strings) ------
    test_check(axl_protocol_guid_name(NULL, nm, sizeof(nm)) == AXL_ERR,
               "protocol_guid_name: NULL guid returns AXL_ERR");
    /* A protocol GUID resolves to its TYPE name — the macro minus "_GUID". */
    test_check(axl_protocol_guid_name(&GUID_DEVICE_PATH, nm, sizeof(nm)) == AXL_OK
                   && axl_strcmp(nm, "EFI_DEVICE_PATH_PROTOCOL") == 0,
               "protocol_guid_name: DevicePath -> \"EFI_DEVICE_PATH_PROTOCOL\"");
    /* Net protocol GUIDs come from the SAME table — uniformly spec-named,
       not the short label axl_net_protocol_name returns. */
    test_check(axl_protocol_guid_name(&GUID_SIMPLE_NETWORK, nm, sizeof(nm)) == AXL_OK
                   && axl_strcmp(nm, "EFI_SIMPLE_NETWORK_PROTOCOL") == 0,
               "protocol_guid_name: net GUID -> \"EFI_SIMPLE_NETWORK_PROTOCOL\"");
    /* A non-protocol GUID identifier keeps its full "_GUID" name. This GUID is
       ALSO the one spec case where two names share identical bytes
       (EFI_ACPI_TABLE_GUID == EFI_ACPI_20_TABLE_GUID); the resolver returns the
       lexicographically-first, deterministically — pins both behaviours. */
    test_check(axl_protocol_guid_name(&GUID_ACPI_TABLE, nm, sizeof(nm)) == AXL_OK
                   && axl_strcmp(nm, "EFI_ACPI_20_TABLE_GUID") == 0,
               "protocol_guid_name: aliased table GUID -> first name, keeps _GUID");
    /* A cap too small to hold the name is AXL_ERR, never a truncated name. */
    test_check(axl_protocol_guid_name(&GUID_DEVICE_PATH, nm, 8) == AXL_ERR,
               "protocol_guid_name: undersized cap returns AXL_ERR (no truncation)");
    test_check(axl_protocol_guid_name(&GUID_BOGUS, nm, sizeof(nm)) == AXL_ERR,
               "protocol_guid_name: unknown GUID returns AXL_ERR");

    // --- axl_protocol_name_count / _at (dictionary iteration) --------------
    size_t ncount = axl_protocol_name_count();
    test_check(ncount > 100, "protocol_name_count: non-trivial table");
    /* Rows are name-sorted, in range, and each name round-trips through the
       guid->name resolver (the two agree on every row). */
    bool sorted = true, roundtrips = true;
    const char *prev = NULL;
    for (size_t i = 0; i < ncount; i++) {
        const AxlGuid *g = NULL;
        const char    *n = NULL;
        if (axl_protocol_name_at(i, &g, &n) != AXL_OK || g == NULL || n == NULL) {
            roundtrips = false;
            break;
        }
        if (prev != NULL && axl_strcmp(prev, n) > 0) {
            sorted = false;
        }
        prev = n;
        /* Resolve g back and confirm it names SOME row's name (aliases mean it
           may resolve to an earlier-sorted synonym, so compare by resolving). */
        char back[96];
        if (axl_protocol_guid_name(g, back, sizeof(back)) != AXL_OK) {
            roundtrips = false;
            break;
        }
    }
    test_check(sorted, "protocol_name_at: rows are name-sorted");
    test_check(roundtrips, "protocol_name_at: every row resolves via guid_name");
    test_check(axl_protocol_name_at(ncount, NULL, NULL) == AXL_ERR,
               "protocol_name_at: index == count returns AXL_ERR");

    // --- axl_handle_parents -------------------------------------------------
    test_check(axl_handle_parents(NULL, NULL, 0, NULL) == AXL_ERR,
               "handle_parents: NULL controller returns AXL_ERR");
    /* Round-trip vs axl_handle_children: a child of @p parent must report
       @p parent among its parents. Reuses the controller-with-children found
       above. */
    if (parent != NULL) {
        AxlHandle kids[64] = { NULL };
        size_t    nk       = 0;
        if (axl_handle_children(parent, kids, 64, &nk) == AXL_OK && nk > 0) {
            AxlHandle parents[16] = { NULL };
            size_t    npar        = 0;
            test_check(axl_handle_parents(kids[0], parents, 16, &npar) == AXL_OK
                           && npar >= 1,
                       "handle_parents: a child controller has >= 1 parent");
            bool found = false;
            for (size_t i = 0; i < npar && i < 16; i++) {
                if (parents[i] == parent) {
                    found = true;
                }
            }
            test_check(found,
                       "handle_parents: child's parents include the original parent (round-trip)");
        }
    }

    /* Diagnostic for the handoff question (not an assertion): where the IP4
       stack lands relative to the SNP handle. */
    probe_net_stack_location();
}

// ---------------------------------------------------------------------------
// A driver that cannot be read is reported through the return value.
//
// axl_driver_load on an unmapped volume walks the whole failure path: the
// DevicePath load is skipped (no volume handle to build one from), the buffer
// fallback's read fails, and AXL_ERR comes back. That read failure used to
// announce itself twice at warning -- once from axl-fs.c, once from
// axl-driver.c -- to a caller that was already checking the status.
//
// Ten lines above the axl-driver.c site, the LoadImage failure it falls back
// FROM was already axl_debug, so the file contradicted itself inside a single
// function.
// ---------------------------------------------------------------------------

static void
test_failed_driver_load_is_quiet_at_info(void)
{
    AxlDriverHandle h = NULL;

    /* "driver" rather than "fs": both domains speak on this path, and the
       axl-driver.c site is the one with no other coverage. */
    test_log_quiet_begin("driver");
    int rc = axl_driver_load("fs99:\\axl-quiet-nonexistent.efi", &h);
    test_log_quiet_end("quiet: driver load read failure logs nothing at INFO");
    test_check(rc == AXL_ERR,
               "quiet: driver load from an unmapped volume returns AXL_ERR");
    test_check(h == NULL, "quiet: driver load failure leaves the handle NULL");
    /* Premise-broke path: unload rather than leak the image. */
    if (h != NULL) {
        axl_driver_unload(h);
    }
}

// ---------------------------------------------------------------------------
// UEFI 2.11 Table 7.3: LoadImage() and StartImage() only below TPL_CALLBACK.
// Every public path that reaches either refuses at TPL_CALLBACK or above with
// AXL_UNSUPPORTED, before touching the firmware -- so the refused calls below
// are safe negatives even with arguments a real call could not survive. Each
// is paired with the SAME call at TPL_APPLICATION, which gets past the guard
// and fails (or succeeds) the way it always did: the refusal is the TPL's
// doing, not the arguments'.
// ---------------------------------------------------------------------------

/* Not published by anything: an ensure for it must load. */
static const AxlGuid TPL_UNREG_GUID = {
    0x7d1c0a55, 0x2b3e, 0x4f61, { 0x9a, 0x0c, 0x1d, 0x2e, 0x3f, 0x40, 0x51, 0x62 }
};

static int
tpl_svc_setup(AxlLoop *loop, void *user)
{
    (void)loop;
    (void)user;
    return AXL_OK;
}

static const AxlService TPL_SVC = {
    .name  = "axl-test-tpl-svc",
    .setup = tpl_svc_setup,
};

/* Every ERROR line in the "driver" domain, copied without allocating (a
   log handler must not allocate). More than TPL_ERR_MAX lines is counted
   but not kept. */
#define TPL_ERR_MAX 32
static char   g_tpl_err[TPL_ERR_MAX][256];
static size_t g_tpl_err_n;

static void
tpl_err_capture(int level, const char *domain, const char *message,
                const AxlRealtime *stamp, void *data)
{
    (void)stamp;
    (void)data;
    if (level != AXL_LOG_ERROR || domain == NULL
        || axl_strcmp(domain, "driver") != 0) {
        return;
    }
    if (g_tpl_err_n < TPL_ERR_MAX) {
        size_t n = axl_strlen(message);
        if (n >= sizeof(g_tpl_err[0])) {
            n = sizeof(g_tpl_err[0]) - 1;
        }
        axl_memcpy(g_tpl_err[g_tpl_err_n], message, n);
        g_tpl_err[g_tpl_err_n][n] = '\0';
    }
    g_tpl_err_n++;
}

#define TPL_N_CALLS 21

/* Make every guarded call once, at whatever TPL the caller holds. */
static void
tpl_make_calls(int rc[TPL_N_CALLS], const unsigned char *zeros, size_t zeros_len)
{
    AxlDriverHandle h    = NULL;
    AxlImage       *img  = NULL;
    void           *ifc  = NULL;
    size_t          n    = 0;
    int             code = 0;
    AxlServiceDeploy deploy = {
        .service     = &TPL_SVC,
        .driver_path = "fs99:\\axl-tpl-nonexistent.efi",
    };

    rc[0]  = axl_driver_load("fs99:\\axl-tpl-nonexistent.efi", &h);
    rc[1]  = axl_driver_load_sibling("definitely-not-here.efi", &h);
    rc[2]  = axl_driver_load_buffer(zeros, zeros_len, &h);
    rc[3]  = axl_driver_load_buffer_with_image_info(zeros, zeros_len, NULL, &h);
    rc[4]  = axl_driver_start(NULL);
    rc[5]  = axl_driver_load_dir("fs99:\\axl-tpl-nodir", NULL, &n);
    rc[6]  = axl_driver_ensure(&TPL_UNREG_GUID, "axl-tpl-nonexistent.efi");
    rc[7]  = axl_driver_ensure_with_embedded(&TPL_UNREG_GUID, "axl-tpl-nonexistent.efi",
                                             zeros, zeros_len, NULL, NULL, 0);
    rc[8]  = axl_driver_ensure_from_path(&TPL_UNREG_GUID,
                                         "fs99:\\axl-tpl-nonexistent.efi", NULL, 0);
    rc[9]  = axl_driver_ensure_embedded_only(&TPL_UNREG_GUID, zeros, zeros_len,
                                             "axl-tpl.efi", NULL, 0);
    rc[10] = axl_image_load("fs99:\\axl-tpl-nonexistent.efi", &img);
    rc[11] = axl_image_start(NULL, &code);
    rc[12] = axl_image_run("fs99:\\axl-tpl-nonexistent.efi", NULL, &code);
    rc[13] = axl_image_run_fv_file(&TPL_UNREG_GUID, NULL, &code);
    rc[14] = axl_service_reload(&TPL_SVC, "fs99:\\axl-tpl-nonexistent.efi");
    rc[15] = axl_service_reload_buffer(&TPL_SVC, zeros, zeros_len);
    rc[16] = axl_service_start_embedded(&deploy);
    rc[17] = axl_shared_driver_locate("axl-tpl-shared", "definitely-not-here.efi",
                                      NULL, 0, &ifc);
    rc[18] = axl_shared_driver_locate_sibling("axl-tpl-shared", "definitely-not-here.efi",
                                              &ifc);
    /* consult_db asks the firmware's LoadImage to judge the signature: a
       verify that cannot ask must not answer as if it had (fail closed). */
    AxlImageSignatureInfo sig;
    rc[19] = axl_image_verify_signature("fs99:\\axl-tpl-nonexistent.efi", true, &sig);
    axl_image_signature_info_free(&sig);
    rc[20] = axl_shared_driver_locate_with_image_info("axl-tpl-shared",
                                                      "definitely-not-here.efi",
                                                      NULL, 0, NULL, 0, NULL, &ifc);
    /* Premise-broke paths: nothing above may have loaded anything. */
    if (h != NULL) {
        axl_driver_unload(h);
    }
    if (img != NULL) {
        axl_image_unload(img);
    }
}

static void
test_image_calls_refused_at_raised_tpl(void)
{
    static const char *const names[TPL_N_CALLS] = {
        "axl_driver_load", "axl_driver_load_sibling", "axl_driver_load_buffer",
        "axl_driver_load_buffer_with_image_info", "axl_driver_start",
        "axl_driver_load_dir", "axl_driver_ensure", "axl_driver_ensure_with_embedded",
        "axl_driver_ensure_from_path", "axl_driver_ensure_embedded_only",
        "axl_image_load", "axl_image_start", "axl_image_run", "axl_image_run_fv_file",
        "axl_service_reload", "axl_service_reload_buffer", "axl_service_start_embedded",
        "axl_shared_driver_locate", "axl_shared_driver_locate_sibling",
        "axl_image_verify_signature", "axl_shared_driver_locate_with_image_info",
    };
    /* What each returns at TPL_APPLICATION with the same arguments: the
       guard is passed, and the call fails on what is (not) there -- or, for
       a directory that does not exist, succeeds having loaded nothing. A
       zero-filled buffer is no PE image: LoadImage refuses it cleanly. */
    static const int at_app[TPL_N_CALLS] = {
        AXL_ERR, AXL_NOT_FOUND, AXL_ERR, AXL_ERR, AXL_ERR,
        AXL_OK, AXL_NOT_FOUND, AXL_NOT_FOUND, AXL_NOT_FOUND, AXL_NOT_FOUND,
        AXL_ERR, AXL_ERR, AXL_ERR, AXL_ERR,
        AXL_INVALID, AXL_INVALID, AXL_NOT_FOUND, AXL_NOT_FOUND, AXL_NOT_FOUND,
        AXL_ERR, AXL_NOT_FOUND,
    };
    static unsigned char zeros[4096];
    int raised[TPL_N_CALLS];
    int app[TPL_N_CALLS];
    char msg[160];

    test_check(axl_log_add_handler(tpl_err_capture, NULL) == AXL_OK,
               "tpl: the error-capture handler installs");
    g_tpl_err_n = 0;

    unsigned prev = axl_tpl_raise(AXL_TPL_CALLBACK);
    tpl_make_calls(raised, zeros, sizeof(zeros));
    axl_tpl_restore(prev);

    for (size_t i = 0; i < TPL_N_CALLS; i++) {
        axl_snprintf(msg, sizeof(msg), "tpl: %s at TPL_CALLBACK returns AXL_UNSUPPORTED (%d)",
                     names[i], raised[i]);
        test_check(raised[i] == AXL_UNSUPPORTED, msg);
    }
    /* One error line per refused call -- exactly TPL_N_CALLS -- each naming
       the call that was made, then the rule. */
    size_t nerr = g_tpl_err_n;
    axl_snprintf(msg, sizeof(msg), "tpl: exactly %d refusal lines (%zu)", TPL_N_CALLS, nerr);
    test_check(nerr == TPL_N_CALLS, msg);
    for (size_t i = 0; i < TPL_N_CALLS && i < nerr && i < TPL_ERR_MAX; i++) {
        char want[256];
        axl_snprintf(want, sizeof(want),
                     "%s: refused at TPL 8 -- UEFI allows LoadImage and StartImage "
                     "only below TPL_CALLBACK (UEFI 2.11 Table 7.3); call it from an "
                     "application, such as a Shell command", names[i]);
        axl_snprintf(msg, sizeof(msg), "tpl: refusal line %zu names %s, exactly", i, names[i]);
        test_check(axl_strcmp(g_tpl_err[i], want) == 0, msg);
    }

    /* Without consult_db a verify loads nothing, so it is not refused: the
       missing file is the answer, as at TPL_APPLICATION. */
    {
        AxlImageSignatureInfo sig;
        prev = axl_tpl_raise(AXL_TPL_CALLBACK);
        int nodb = axl_image_verify_signature("fs99:\\axl-tpl-nonexistent.efi", false, &sig);
        axl_tpl_restore(prev);
        axl_image_signature_info_free(&sig);
        test_check(nodb == AXL_ERR,
                   "tpl: axl_image_verify_signature without consult_db at TPL_CALLBACK is "
                   "not refused (AXL_ERR for the missing file)");
    }
    /* An ensure whose protocol is already published loads nothing, so it is
       not refused: a callback may still ask "is it there?". */
    AxlHandle reg = NULL;
    test_check(axl_protocol_install(&TPL_UNREG_GUID, &g_iface, &reg) == AXL_OK,
               "tpl: install the protocol the ensure looks for");
    prev = axl_tpl_raise(AXL_TPL_CALLBACK);
    int warm = axl_driver_ensure(&TPL_UNREG_GUID, "axl-tpl-nonexistent.efi");
    axl_tpl_restore(prev);
    test_check(warm == AXL_OK,
               "tpl: axl_driver_ensure of a published protocol at TPL_CALLBACK is AXL_OK");
    test_check(axl_protocol_uninstall(reg, &TPL_UNREG_GUID, &g_iface) == AXL_OK,
               "tpl: uninstall the protocol again");

    /* Control: the same calls at TPL_APPLICATION pass the guard. */
    tpl_make_calls(app, zeros, sizeof(zeros));
    for (size_t i = 0; i < TPL_N_CALLS; i++) {
        axl_snprintf(msg, sizeof(msg), "tpl: %s at TPL_APPLICATION returns %d (%d)",
                     names[i], at_app[i], app[i]);
        test_check(app[i] == at_app[i], msg);
    }
    axl_log_remove_handler(tpl_err_capture);
}

int
test_driver_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    test_print_header("AxlDriver");

    test_install_fresh_handle();
    test_install_null_safety();
    test_buffer_identity_and_sibling();
    test_self_load_refused();
    test_self_identity_cached();
    test_driver_binding();
    test_disconnect_handle_contract();
    test_driver_info();
    test_driver_discovery();
    test_handle_enum();

    test_deps_parse();
    test_deps_walk_order();
    test_deps_walk_prune();
    test_deps_walk_cycle();
    test_deps_walk_diamond();
    test_deps_walk_missing_dep();
    test_deps_parse_errors();
    test_failed_driver_load_is_quiet_at_info();
    test_image_calls_refused_at_raised_tpl();

    return test_print_results();
}

AXL_APP(test_driver_main)
