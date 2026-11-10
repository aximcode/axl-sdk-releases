/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * axl-service-stage.c -- the staged upgrade (axl-service.h, "Staged
 * upgrade"): a service stages a new image beside the one it runs from, and
 * its launcher applies it at the next start -- a trial from P.new, then a
 * promotion that never renames the known-good image -- with an AxlAttempt
 * breadcrumb recording each phase before the change it describes.
 *
 * The decision table lives in the header's docstring for
 * axl_service_start_staged; stage_resolve() below follows it row by row, in
 * its order.
 */

#include "../backend/axl-backend.h"
#include "../pack/axl-pack-format.h"
#include "../util/axl-driver-internal.h"
#include "axl-service-internal.h"
#include <axl/axl-service.h>
#include <axl/axl-attempt.h>
#include <axl/axl-digest.h>
#include <axl/axl-driver.h>
#include <axl/axl-fs.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-nvstore.h>
#include <axl/axl-path.h>
#include <axl/axl-str.h>
#include <axl/axl-sys.h>
#include <axl/axl-wait.h>
#include <axl/axl-watchdog.h>

AXL_LOG_DOMAIN("service");

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

#define STAGE_TRIES_DEFAULT       2u
#define STAGE_WATCHDOG_DEFAULT    300u
#define STAGE_GRACE_DEFAULT       10u
#define STAGE_GRACE_WATCHDOG_PAD  60u      ///< the grace period always fits inside the watchdog
#define STAGE_PATH_MAX            512u
#define STAGE_NS_MAX              96u
#define STAGE_REC_MAX             AXL_ATTEMPT_NAME_MAX
#define STAGE_LAST_KEY            "Last"
#define STAGE_FLIGHT_KEY          "InFlight" ///< VOLATILE: set from a trial's start to its promotion's end
#define STAGE_NS_SLOTS            8u       ///< services one image stages for

/* PE layout (PE32+ only: a UEFI driver for x64 or aarch64). */
#define PE_DOS_LFANEW             0x3Cu
#define PE_COFF_MACHINE           4u       ///< offsets from the "PE\0\0" signature
#define PE_COFF_NSECTIONS         6u
#define PE_COFF_OPT_SIZE          20u
#define PE_OPT                    24u
#define PE_OPT_MAGIC_PE32PLUS     0x20Bu
#define PE_OPT_CHECKSUM           64u      ///< offsets from the optional header
#define PE_OPT_SUBSYSTEM          68u
#define PE_OPT_NUM_DIRS           108u
#define PE_OPT_DIRS               112u
#define PE_DIR_SECURITY           4u
#define PE_SECTION_SIZE           40u
#define PE_SECT_RAW_SIZE          16u
#define PE_SECT_RAW_PTR           20u
#define PE_SUBSYSTEM_BS_DRIVER    11u
#define PE_SUBSYSTEM_RT_DRIVER    12u

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/* What an image loads as: see "An image's KEY" in axl-service.h. */
typedef struct {
    uint32_t crc;
    uint32_t size;
} StageKey;

typedef struct {
    char p[STAGE_PATH_MAX];
    char nw[STAGE_PATH_MAX];
    char tmp[STAGE_PATH_MAX];
    char bak[STAGE_PATH_MAX];
    char failed[STAGE_PATH_MAX];
} StagePaths;

typedef struct {
    char           ns[STAGE_NS_MAX];
    const AxlGuid *vendor;   ///< a slot of m_ns_guid: the namespace keeps the pointer
    AxlAttempt     at;
} StageNv;

typedef enum {
    REC_NONE = 0,
    REC_TRIAL,
    REC_PROMOTE_BAK,
    REC_PROMOTE_SWAP,
} StageRecKind;

typedef struct {
    StageRecKind kind;
    uint32_t     n;      ///< trial number (REC_TRIAL)
    StageKey     key;
} StageRec;

typedef struct {
    uint32_t tries;
    uint32_t watchdog_sec;
    uint32_t grace_sec;      ///< 0 = commit at publish
} StageOpts;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

/* AxlNvstore keeps a namespace's GUID by pointer for the life of the image,
   so each service's GUID lives here, not on a caller's stack. */
static AxlGuid m_ns_guid[STAGE_NS_SLOTS];
static size_t  m_ns_used;

// ---------------------------------------------------------------------------
// Static prototypes
// ---------------------------------------------------------------------------

static int  stage_paths(const AxlServiceDeploy *deploy, StagePaths *sp);
static int  stage_nv_open(const AxlService *svc, StageNv *nv);
static StageOpts stage_opts(const AxlServiceDeploy *deploy);
static int  image_key(const uint8_t *buf, size_t len, StageKey *key);
static int  image_check(const uint8_t *buf, size_t len, StageKey *key);
static int  file_key(const char *path, StageKey *key);
static bool file_exists(const char *path);
static int  file_same(const char *path, const void *buf, size_t len, bool *same);
static void key_name(const StageKey *key, char *out, size_t cap);
static bool key_quarantined(StageNv *nv, const StageKey *key);
static bool rec_read(StageNv *nv, StageRec *rec);
static bool rec_write(StageNv *nv, const char *phase, uint32_t n, const StageKey *key);
static void stage_cut(const char *phase);
static void last_write(StageNv *nv, AxlServiceStagedResult r);
static int  copy_file(const char *from, const char *to);
static void sweep_asides(const StagePaths *sp);

// ---------------------------------------------------------------------------
// Paths, NV state, options
// ---------------------------------------------------------------------------

static int
stage_paths(const AxlServiceDeploy *deploy, StagePaths *sp)
{
    const char *p = deploy->driver_path;
    int n = axl_snprintf(sp->p, sizeof(sp->p), "%s", p);
    if (n <= 0 || (size_t)n + sizeof(".failed") > sizeof(sp->p)) {
        return AXL_INVALID;
    }
    axl_snprintf(sp->nw, sizeof(sp->nw), "%s.new", p);
    axl_snprintf(sp->tmp, sizeof(sp->tmp), "%s.new.tmp", p);
    axl_snprintf(sp->bak, sizeof(sp->bak), "%s.bak", p);
    axl_snprintf(sp->failed, sizeof(sp->failed), "%s.failed", p);
    return AXL_OK;
}

/* The service's own AxlNvstore namespace, bound to its GUID: the breadcrumb
   (Trying), the quarantine, and the last outcome. */
static int
stage_nv_open(const AxlService *svc, StageNv *nv)
{
    AxlGuid g;
    if (axl_service_guid(svc, &g) != AXL_OK) {
        return AXL_INVALID;
    }
    nv->vendor = NULL;
    for (size_t i = 0; i < m_ns_used && nv->vendor == NULL; i++) {
        if (axl_guid_equal(&m_ns_guid[i], &g)) {
            nv->vendor = &m_ns_guid[i];
        }
    }
    if (nv->vendor == NULL) {
        if (m_ns_used == STAGE_NS_SLOTS) {
            axl_error("staged upgrade: more than %u services in one image",
                      (unsigned)STAGE_NS_SLOTS);
            return AXL_ERR;
        }
        m_ns_guid[m_ns_used] = g;
        nv->vendor = &m_ns_guid[m_ns_used++];
    }
    /* Named from the GUID, so any service name fits the namespace's limit. */
    axl_snprintf(nv->ns, sizeof(nv->ns), "axlsvc-%08x%04x%04x", (unsigned)g.data1,
                 (unsigned)g.data2, (unsigned)g.data3);
    return axl_attempt_init(&nv->at, nv->ns, nv->vendor) == AXL_OK ? AXL_OK : AXL_ERR;
}

static StageOpts
stage_opts(const AxlServiceDeploy *deploy)
{
    const AxlServiceStartOpts *o = deploy->staged_opts;
    StageOpts r = {
        .tries        = (o != NULL && o->tries != 0) ? o->tries : STAGE_TRIES_DEFAULT,
        .watchdog_sec = (o != NULL && o->watchdog_sec != 0) ? o->watchdog_sec
                                                            : STAGE_WATCHDOG_DEFAULT,
        .grace_sec    = (o == NULL || o->commit_grace_sec == 0) ? STAGE_GRACE_DEFAULT
                        : (o->commit_grace_sec == AXL_SERVICE_COMMIT_AT_PUBLISH)
                          ? 0u : o->commit_grace_sec,
    };
    return r;
}

// ---------------------------------------------------------------------------
// Image identity and validation
// ---------------------------------------------------------------------------

static uint32_t
rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t
rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Validate a plain PE32+ driver for this machine; on success *pe and *opt
   are the offsets of the signature and the optional header. */
static int
pe_check(const uint8_t *buf, size_t len, size_t *pe_out, size_t *opt_out)
{
    if (len < 0x40 || buf[0] != 'M' || buf[1] != 'Z') {
        return AXL_INVALID;
    }
    size_t pe = rd32(buf + PE_DOS_LFANEW);
    if (pe > len || len - pe < PE_OPT + PE_OPT_DIRS
        || axl_memcmp(buf + pe, "PE\0\0", 4) != 0) {
        return AXL_INVALID;
    }
    size_t opt = pe + PE_OPT;
    size_t opt_size = rd16(buf + pe + PE_COFF_OPT_SIZE);
    if (rd16(buf + pe + PE_COFF_MACHINE) != AXL_PACK_MACHINE_NATIVE
        || rd16(buf + opt) != PE_OPT_MAGIC_PE32PLUS
        || opt_size < PE_OPT_DIRS || len - opt < opt_size) {
        return AXL_INVALID;
    }
    uint32_t sub = rd16(buf + opt + PE_OPT_SUBSYSTEM);
    if (sub != PE_SUBSYSTEM_BS_DRIVER && sub != PE_SUBSYSTEM_RT_DRIVER) {
        return AXL_INVALID;
    }
    /* The data directories must fit in the optional header the image
       declares (itself checked against the buffer above): a header that
       claims more would have the key read past it. */
    if (rd32(buf + opt + PE_OPT_NUM_DIRS) > (opt_size - PE_OPT_DIRS) / 8u) {
        return AXL_INVALID;
    }
    *pe_out  = pe;
    *opt_out = opt;
    return AXL_OK;
}

/* The key of a plain PE: a CRC over the headers and the sections up to the
   last section's end, leaving out OptionalHeader.CheckSum, the security
   data-directory entry and the certificate table -- what re-signing
   rewrites, as the Authenticode hash leaves them out -- and any overlay. */
static int
pe_key(const uint8_t *buf, size_t len, size_t pe, size_t opt, StageKey *key)
{
    size_t opt_size = rd16(buf + pe + PE_COFF_OPT_SIZE);
    size_t nsect    = rd16(buf + pe + PE_COFF_NSECTIONS);
    size_t sect     = opt + opt_size;
    if (sect > len || nsect > (len - sect) / PE_SECTION_SIZE) {
        return AXL_INVALID;
    }
    size_t end = sect + nsect * PE_SECTION_SIZE;
    for (size_t i = 0; i < nsect; i++) {
        const uint8_t *s   = buf + sect + i * PE_SECTION_SIZE;
        size_t         raw = rd32(s + PE_SECT_RAW_PTR);
        size_t         sz  = rd32(s + PE_SECT_RAW_SIZE);
        if (raw > len || sz > len - raw) {
            return AXL_INVALID;
        }
        if (sz != 0 && raw + sz > end) {
            end = raw + sz;
        }
    }

    /* Ranges left out, in file order: CheckSum, the security directory
       entry, and the certificate table (a file offset, unlike the others). */
    size_t skip[3][2] = {
        { opt + PE_OPT_CHECKSUM, 4 },
        { 0, 0 },
        { 0, 0 },
    };
    if (rd32(buf + opt + PE_OPT_NUM_DIRS) > PE_DIR_SECURITY) {
        size_t dir = opt + PE_OPT_DIRS + PE_DIR_SECURITY * 8u;
        skip[1][0] = dir;
        skip[1][1] = 8;
        size_t ct  = rd32(buf + dir);
        size_t cts = rd32(buf + dir + 4);
        if (ct != 0 && ct < end) {
            skip[2][0] = ct;
            skip[2][1] = (cts < end - ct) ? cts : end - ct;
        }
    }

    uint32_t crc = 0;
    size_t   at  = 0;
    for (size_t i = 0; i < 3; i++) {
        if (skip[i][1] == 0 || skip[i][0] < at) {
            continue;
        }
        crc = axl_crc32(crc, buf + at, skip[i][0] - at);
        at  = skip[i][0] + skip[i][1];
    }
    crc = axl_crc32(crc, buf + at, end - at);
    key->crc  = crc;
    key->size = (uint32_t)end;
    return AXL_OK;
}

static void *
stage_pack_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return axl_malloc(n);
}

static void
stage_pack_free(void *ctx, void *p)
{
    (void)ctx;
    axl_free(p);
}

/* Validate an image for staging and compute its key. A pack is unpacked --
   which checks its header and raw-image CRCs -- and the image inside is
   checked as a driver and keyed exactly as it would be bare: the key names
   the image that LOADS, so a quarantined image is refused packed or not. */
static int
image_check(const uint8_t *buf, size_t len, StageKey *key)
{
    size_t off = 0, plen = 0;
    if (axl_pack_find_payload(buf, len, &off, &plen, NULL) == AXL_PACK_OK) {
        AxlPackAlloc a    = { stage_pack_alloc, stage_pack_free, NULL };
        void        *raw  = NULL;
        size_t       rlen = 0;
        if (axl_pack_unpack(buf + off, plen, AXL_PACK_MACHINE_NATIVE, &a, &raw,
                            &rlen) != AXL_PACK_OK) {
            return AXL_INVALID;
        }
        size_t pe, opt;
        int rc = pe_check(raw, rlen, &pe, &opt);
        if (rc == AXL_OK) {
            rc = pe_key(raw, rlen, pe, opt, key);
        }
        axl_free(raw);
        return rc == AXL_OK ? AXL_OK : AXL_INVALID;
    }
    size_t pe, opt;
    if (pe_check(buf, len, &pe, &opt) != AXL_OK) {
        return AXL_INVALID;
    }
    return pe_key(buf, len, pe, opt, key);
}

/* The key of bytes already on disk (validated, but a bad one has no key). */
static int
image_key(const uint8_t *buf, size_t len, StageKey *key)
{
    return image_check(buf, len, key);
}

static int
file_key(const char *path, StageKey *key)
{
    void  *buf = NULL;
    size_t len = 0;
    if (axl_file_get_contents(path, &buf, &len) != AXL_OK || buf == NULL) {
        return AXL_NOT_FOUND;
    }
    int rc = image_key(buf, len, key);
    axl_free(buf);
    return rc;
}

static bool
file_exists(const char *path)
{
    AxlFsEntry e;
    return axl_file_info(path, &e) == AXL_OK && !axl_fs_entry_is_dir(&e);
}

/* *same: @p path holds exactly @p len bytes equal to @p buf. AXL_OK, or
   AXL_NOT_FOUND when the file is absent, AXL_ERR when it cannot be read. */
static int
file_same(const char *path, const void *buf, size_t len, bool *same)
{
    *same = false;
    if (!file_exists(path)) {
        return AXL_NOT_FOUND;
    }
    void  *fb = NULL;
    size_t fl = 0;
    if (axl_file_get_contents(path, &fb, &fl) != AXL_OK) {
        return AXL_ERR;
    }
    *same = (fl == len) && (len == 0 || axl_memcmp(fb, buf, len) == 0);
    axl_free(fb);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// The record, the quarantine, the outcome
// ---------------------------------------------------------------------------

static void
key_name(const StageKey *key, char *out, size_t cap)
{
    axl_snprintf(out, cap, "%08x:%u", (unsigned)key->crc, (unsigned)key->size);
}

static bool
key_eq(const StageKey *a, const StageKey *b)
{
    return a->crc == b->crc && a->size == b->size;
}

static bool
key_quarantined(StageNv *nv, const StageKey *key)
{
    char name[STAGE_REC_MAX];
    key_name(key, name, sizeof(name));
    return axl_attempt_is_quarantined(&nv->at, name);
}

/* "<crc hex>:<size decimal>", exactly. */
static bool
key_parse(const char *s, StageKey *key)
{
    const char *end = NULL;
    if (axl_str_to_u32(s, 16, &key->crc, &end) != AXL_OK || end == s || *end != ':') {
        return false;
    }
    const char *z = end + 1;
    return axl_str_to_u32(z, 10, &key->size, &end) == AXL_OK && end != z && *end == '\0';
}

static bool
rec_read(StageNv *nv, StageRec *rec)
{
    char buf[STAGE_REC_MAX];
    rec->kind = REC_NONE;
    if (!axl_attempt_pending(&nv->at, buf, sizeof(buf))) {
        return false;
    }
    if (axl_strncmp(buf, "promote:bak:", 12) == 0 && key_parse(buf + 12, &rec->key)) {
        rec->kind = REC_PROMOTE_BAK;
    } else if (axl_strncmp(buf, "promote:swap:", 13) == 0
               && key_parse(buf + 13, &rec->key)) {
        rec->kind = REC_PROMOTE_SWAP;
    } else if (axl_strncmp(buf, "trial:", 6) == 0) {
        const char *end = NULL;
        uint32_t    n   = 0;
        if (axl_str_to_u32(buf + 6, 10, &n, &end) == AXL_OK && end != buf + 6
            && *end == ':' && n > 0 && key_parse(end + 1, &rec->key)) {
            rec->kind = REC_TRIAL;
            rec->n    = n;
        }
    }
    if (rec->kind == REC_NONE) {
        /* Not ours to interpret: say so and treat it as nothing recorded. */
        axl_warning("staged upgrade: unrecognised record '%s' ignored", buf);
        axl_attempt_end(&nv->at);
    }
    return rec->kind != REC_NONE;
}

/* Record @p phase BEFORE the change it describes. False: nothing new is
   recorded, and the change must not be made. */
static bool
rec_write(StageNv *nv, const char *phase, uint32_t n, const StageKey *key)
{
    char kn[STAGE_REC_MAX];
    char rec[STAGE_REC_MAX];
    axl_snprintf(rec, sizeof(rec), "nv:%s", phase);
    if (_axl_service_stage_test_point(rec)) {
        return false;
    }
    key_name(key, kn, sizeof(kn));
    if (n != 0) {
        axl_snprintf(rec, sizeof(rec), "%s:%u:%s", phase, (unsigned)n, kn);
    } else {
        axl_snprintf(rec, sizeof(rec), "%s:%s", phase, kn);
    }
    return axl_attempt_begin(&nv->at, rec);
}

/* Production's answer at every test point: never. The stage suites' launcher
   links a strong definition (see axl-service-internal.h). */
__attribute__((weak)) bool
_axl_service_stage_test_point(const char *point)
{
    (void)point;
    return false;
}

/* The power-loss test cut: reset right here when a test asks for @p phase. */
static void
stage_cut(const char *phase)
{
    if (_axl_service_stage_test_point(phase)) {
        axl_warning("staged upgrade: test cut at %s -- resetting", phase);
        axl_reset(AXL_RESET_COLD);
    }
}

static void
last_write(StageNv *nv, AxlServiceStagedResult r)
{
    uint32_t v = (uint32_t)r;
    if (axl_nvstore_set(nv->ns, STAGE_LAST_KEY, &v, sizeof(v), nv->at.flags) != AXL_OK) {
        axl_warning("staged upgrade: the outcome could not be recorded");
    }
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

static int
copy_file(const char *from, const char *to)
{
    void  *buf = NULL;
    size_t len = 0;
    if (axl_file_get_contents(from, &buf, &len) != AXL_OK) {
        return AXL_ERR;
    }
    int rc = axl_file_write_atomic(to, buf, len);
    axl_free(buf);
    return rc == AXL_OK ? AXL_OK : AXL_ERR;
}

typedef struct {
    const StagePaths *sp;
} SweepCtx;

static int
sweep_cb(const char *full_path, const AxlFsEntry *entry, void *user)
{
    SweepCtx *c = (SweepCtx *)user;
    if (axl_fs_entry_is_dir(entry) || !axl_fnmatch("~AXL*.TMP", entry->name)) {
        return AXL_OK;
    }
    void  *buf = NULL;
    size_t len = 0;
    if (axl_file_get_contents(full_path, &buf, &len) != AXL_OK) {
        return AXL_OK;
    }
    const char *const known[] = { c->sp->p, c->sp->bak, c->sp->nw, c->sp->failed };
    bool matched = false;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]) && !matched; i++) {
        bool same = false;
        matched = file_same(known[i], buf, len, &same) == AXL_OK && same;
    }
    axl_free(buf);
    if (matched) {
        if (axl_file_delete(full_path) == AXL_OK) {
            axl_debug("staged upgrade: removed %s, a copy a lost rename left", full_path);
        }
    } else {
        axl_warning("staged upgrade: %s matches no image here; left in place",
                    full_path);
    }
    return AXL_OK;
}

/* Remove the asides a lost rename left, when byte-identical to a file we
   keep; log the rest. Never deletes a file without a full match. */
static void
sweep_asides(const StagePaths *sp)
{
    SweepCtx c = { .sp = sp };
    AXL_AUTO_FREE char *dir = axl_path_get_dirname(sp->p);
    if (dir == NULL) {
        return;
    }
    axl_dir_walk(dir, sweep_cb, &c, 1);
}

// ---------------------------------------------------------------------------
// Stage
// ---------------------------------------------------------------------------

AxlStatus
axl_service_stage(const AxlServiceDeploy *deploy, const AxlServiceStageOpts *opts,
                  const void *image, size_t image_len)
{
    if (deploy == NULL || deploy->service == NULL || image == NULL || image_len == 0) {
        return AXL_INVALID;
    }
    if (deploy->driver_path == NULL) {
        return AXL_UNSUPPORTED;
    }
    if (axl_service_upgrade_in_flight(deploy->service)) {
        axl_debug("staged upgrade: a candidate is in its trial; nothing staged");
        return AXL_BUSY;
    }
    StagePaths sp;
    StageNv    nv;
    StageKey   key;
    if (stage_paths(deploy, &sp) != AXL_OK || stage_nv_open(deploy->service, &nv) != AXL_OK) {
        return AXL_INVALID;
    }
    if (image_check(image, image_len, &key) != AXL_OK) {
        axl_debug("staged upgrade: not a driver image for this machine");
        return AXL_INVALID;
    }
    if (key_quarantined(&nv, &key)) {
        char kn[STAGE_REC_MAX];
        key_name(&key, kn, sizeof(kn));
        axl_debug("staged upgrade: image %s failed its trial before", kn);
        return AXL_DENIED;
    }
    if (opts != NULL && opts->verify != NULL
        && opts->verify(image, image_len, opts->verify_ud) != AXL_OK) {
        return AXL_DENIED;
    }

    bool same = false;
    int  src  = file_same(sp.p, image, image_len, &same);
    if (src == AXL_ERR) {
        /* log-level: AXL_ERR covers this and a failed write; which one is
           said only here */
        axl_error("staged upgrade: %s could not be read to compare", sp.p);
        return AXL_ERR;
    }
    if (same) {
        axl_debug("staged upgrade: the image is the one running; nothing staged");
        return axl_service_unstage(deploy);
    }
    int wrc = axl_file_write_atomic(sp.nw, image, image_len);
    if (wrc != AXL_OK) {
        /* log-level: the caller gets AXL_ERR; the volume's own reason is here */
        axl_error("staged upgrade: %s could not be written (%d)", sp.nw, wrc);
        return AXL_ERR;
    }
    axl_debug("staged upgrade: %s staged (%zu bytes)", sp.nw, image_len);
    return AXL_OK;
}

AxlStatus
axl_service_unstage(const AxlServiceDeploy *deploy)
{
    if (deploy == NULL || deploy->service == NULL) {
        return AXL_INVALID;
    }
    if (deploy->driver_path == NULL) {
        return AXL_UNSUPPORTED;
    }
    StagePaths sp;
    if (stage_paths(deploy, &sp) != AXL_OK) {
        return AXL_INVALID;
    }
    if (axl_service_upgrade_in_flight(deploy->service)) {
        axl_debug("staged upgrade: a candidate is in its trial; nothing unstaged");
        return AXL_BUSY;
    }
    int rc = AXL_OK;
    if (file_exists(sp.tmp) && axl_file_delete(sp.tmp) != AXL_OK) {
        rc = AXL_ERR;
    }
    if (file_exists(sp.nw) && axl_file_delete(sp.nw) != AXL_OK) {
        rc = AXL_ERR;
    }
    return rc;
}

bool
axl_service_is_staged(const AxlServiceDeploy *deploy)
{
    StagePaths sp;
    return deploy != NULL && deploy->service != NULL && deploy->driver_path != NULL
           && stage_paths(deploy, &sp) == AXL_OK && file_exists(sp.nw);
}

AxlStatus
axl_service_last_upgrade(const AxlService *svc, AxlServiceStagedResult *out)
{
    if (svc == NULL || out == NULL) {
        return AXL_INVALID;
    }
    StageNv nv;
    if (stage_nv_open(svc, &nv) != AXL_OK) {
        return AXL_INVALID;
    }
    if (axl_service_upgrade_in_flight(svc)) {
        return AXL_BUSY;   /* what is stored is the previous attempt's */
    }
    uint32_t v  = 0;
    size_t   sz = sizeof(v);
    if (axl_nvstore_get(nv.ns, STAGE_LAST_KEY, &v, &sz) != AXL_OK || sz != sizeof(v)
        || v > (uint32_t)AXL_SERVICE_STAGED_ROLLED_BACK) {
        return AXL_NOT_FOUND;
    }
    *out = (AxlServiceStagedResult)v;
    return AXL_OK;
}

bool
axl_service_upgrade_in_flight(const AxlService *svc)
{
    StageNv  nv;
    uint8_t  v  = 0;
    size_t   sz = sizeof(v);
    /* The launcher's own volatile marker, not the NV record: a record
       outlives the boot that set it (a dead trial, a refused promotion
       record, `start --no-staged`), and the resolve table already handles a
       stage over one. Only the trial and promotion running NOW are refused,
       and a reset ends them and the marker together. */
    return svc != NULL && stage_nv_open(svc, &nv) == AXL_OK
           && axl_nvstore_get(nv.ns, STAGE_FLIGHT_KEY, &v, &sz) == AXL_OK;
}

AxlStatus
axl_service_clear_quarantine(const AxlService *svc)
{
    if (svc == NULL) {
        return AXL_INVALID;
    }
    StageNv nv;
    if (stage_nv_open(svc, &nv) != AXL_OK) {
        return AXL_INVALID;
    }
    char *q = NULL;
    if (axl_nvstore_get_str(nv.ns, nv.at.quarantine_key, &q) != AXL_OK) {
        return AXL_OK;   /* nothing quarantined */
    }
    axl_free(q);
    return axl_nvstore_delete(nv.ns, nv.at.quarantine_key) == AXL_OK ? AXL_OK : AXL_ERR;
}

// ---------------------------------------------------------------------------
// Start: resolve, trial, promote
// ---------------------------------------------------------------------------

typedef struct {
    const AxlServiceDeploy *deploy;
    StagePaths              sp;
    StageNv                 nv;
    StageOpts               o;
    AxlGuid                 guid;
    bool                    flight;   ///< the in-flight marker is set
} StageRun;

/* The in-flight marker (axl_service_upgrade_in_flight): set from before the
   candidate can run until its promotion is over -- and cleared BEFORE P is
   started instead, or P's setup would read AXL_BUSY for the outcome just
   written. VOLATILE, so a reset clears it with the trial; one that cannot be
   written leaves the API unguarded, and promote()'s re-key of P.new is then
   what holds. */
static void
flight_set(StageRun *r)
{
    uint8_t one = 1;
    r->flight = axl_nvstore_set(r->nv.ns, STAGE_FLIGHT_KEY, &one, sizeof(one),
                                AXL_NV_BOOT) == AXL_OK;
    if (!r->flight) {
        axl_warning("staged upgrade: the in-flight marker could not be set");
    }
}

static void
flight_clear(StageRun *r)
{
    if (r->flight && axl_nvstore_delete(r->nv.ns, STAGE_FLIGHT_KEY) != AXL_OK) {
        /* log-level: stage/unstage then answer AXL_BUSY until a reset */
        axl_error("staged upgrade: the in-flight marker could not be cleared");
    }
    r->flight = false;
}

/* P missing and P.bak present (FAT lost P, or a promotion's files were
   damaged): put the fallback back, by copy, so P.bak stays. */
static void
restore_p(StageRun *r)
{
    if (file_exists(r->sp.p) || !file_exists(r->sp.bak)) {
        return;
    }
    if (copy_file(r->sp.bak, r->sp.p) == AXL_OK) {
        /* log-level: a missing image restored; nothing else reports it */
        axl_error("staged upgrade: %s was missing; restored from %s", r->sp.p, r->sp.bak);
    }
}

/* Start P (the pinned image) as axl_service_start_embedded would -- from
   P.bak if P itself is gone, on every path that starts it. */
static AxlStatus
start_p(StageRun *r)
{
    restore_p(r);
    AxlStatus rc = axl_service_start_embedded(r->deploy);
    /* AXL_BUSY: P came up half-stopped (or one already is) -- not "not found". */
    return rc == AXL_OK || rc == AXL_BUSY ? rc : AXL_NOT_FOUND;
}

/* Quarantine @p kn, and read it back: axl_attempt_quarantine is
   best-effort and says nothing of a failed write. The test seam drops the
   write the same silent way, so it is the read-back that must notice. */
static bool
quarantine(StageRun *r, const char *kn)
{
    if (!_axl_service_stage_test_point("nv:quarantine")) {
        axl_attempt_quarantine(&r->nv.at, kn);
    }
    return axl_attempt_is_quarantined(&r->nv.at, kn);
}

/* Step 4 from the quarantine on: quarantine the key, rename P.new to
   P.failed, clear the record. The candidate is not running. A quarantine
   that fails is reported and not retried: the candidate is out of the way
   either way, and only a re-stage of the same image would try it again. */
static int
fail_candidate(StageRun *r, const StageKey *key)
{
    char kn[STAGE_REC_MAX];
    key_name(key, kn, sizeof(kn));
    bool quarantined = quarantine(r, kn);
    if (!quarantined) {
        /* log-level: the start goes on with P; this is the only report */
        axl_error("staged upgrade: %s could not be quarantined; it is not retried", kn);
    }
    stage_cut("quarantined");
    int rc = AXL_OK;
    if (file_exists(r->sp.nw)
        && axl_file_replace(r->sp.nw, r->sp.failed, 0, NULL) != AXL_OK) {
        /* log-level: the start goes on with P; this is the only report */
        axl_error("staged upgrade: %s could not be renamed to %s", r->sp.nw, r->sp.failed);
        rc = AXL_ERR;
    }
    axl_attempt_end(&r->nv.at);
    axl_warning("staged upgrade: candidate %s failed its trial; %skept as %s",
                kn, quarantined ? "quarantined, " : "", r->sp.failed);
    return rc;
}

/* What promote() did. */
typedef enum {
    PROMOTE_DONE = 0,
    PROMOTE_FAILED,        ///< a file step failed; the record stays for the next start
    PROMOTE_NOT_RECORDED,  ///< a phase could not be recorded; nothing more was done
    PROMOTE_SUPERSEDED,    ///< P.new is no longer the candidate; the record is cleared
} PromoteRc;

/* A phase of the promotion could not be recorded: stop there. */
static PromoteRc
promote_unrecorded(const char *phase, const StageKey *key)
{
    char kn[STAGE_REC_MAX];
    key_name(key, kn, sizeof(kn));
    /* log-level: the start reports success (a service is running); this is
       the only report */
    axl_error("staged upgrade: promotion of %s: %s could not be recorded; "
              "the promotion is left for the next start", kn, phase);
    return PROMOTE_NOT_RECORDED;
}

/* P.new is still the candidate @p key was taken from. axl_service_stage
   refuses while the candidate is in flight, but any writer of the file can
   still replace or remove it while the candidate serves. */
static bool
still_staged(const StageRun *r, const StageKey *key)
{
    StageKey now;
    return file_key(r->sp.nw, &now) == AXL_OK && key_eq(&now, key);
}

/* P.new changed under the promotion: promote nothing, and leave whatever it
   holds now for the next start to trial. The candidate keeps serving. */
static PromoteRc
promote_superseded(StageRun *r, const StageKey *key)
{
    char kn[STAGE_REC_MAX];
    key_name(key, kn, sizeof(kn));
    axl_warning("staged upgrade: %s changed during the trial of %s (replaced or "
                "removed); not promoted -- the next start trials what it holds",
                r->sp.nw, kn);
    axl_attempt_end(&r->nv.at);
    return PROMOTE_SUPERSEDED;
}

/* Step 5: P.bak = a copy of P, then P = P.new. With no P (a first upgrade),
   a plain rename and a warning. @p done_as is the outcome recorded once the
   files are moved (or a file step failed) -- BEFORE the record is cleared,
   so a service reading axl_service_last_upgrade never sees the previous
   verdict once nothing is in flight. */
static PromoteRc
promote(StageRun *r, const StageKey *key, bool from_bak_record,
        AxlServiceStagedResult done_as)
{
    if (!still_staged(r, key)) {
        return promote_superseded(r, key);
    }
    if (file_exists(r->sp.p)) {
        if (!from_bak_record) {
            if (!rec_write(&r->nv, "promote:bak", 0, key)) {
                return promote_unrecorded("promote:bak", key);
            }
            stage_cut("promote:bak");
        }
        if (copy_file(r->sp.p, r->sp.bak) != AXL_OK) {
            /* log-level: the caller gets AXL_ERR; which copy failed is here */
            axl_error("staged upgrade: %s could not be copied to %s", r->sp.p, r->sp.bak);
            last_write(&r->nv, done_as);
            return PROMOTE_FAILED;
        }
    } else {
        axl_warning("staged upgrade: %s is missing; promoting with no fallback image",
                    r->sp.p);
    }
    if (!rec_write(&r->nv, "promote:swap", 0, key)) {
        return promote_unrecorded("promote:swap", key);
    }
    stage_cut("promote:swap");
    if (!still_staged(r, key)) {
        return promote_superseded(r, key);
    }
    if (axl_file_replace(r->sp.nw, r->sp.p, 0, NULL) != AXL_OK) {
        /* log-level: the caller gets AXL_ERR; the record stays, so the next
           start finishes the promotion */
        axl_error("staged upgrade: %s could not be put in place as %s", r->sp.nw, r->sp.p);
        last_write(&r->nv, done_as);
        return PROMOTE_FAILED;
    }
    stage_cut("swapped");
    last_write(&r->nv, done_as);
    axl_attempt_end(&r->nv.at);
    return PROMOTE_DONE;
}

/* Step 3: trial-run P.new as trial @p n. On success it is running and
   promoted; on failure step 4 and P are done. */
static AxlStatus
trial(StageRun *r, const StageKey *key, uint32_t n, bool retried,
      AxlServiceStagedResult *result)
{
    if (!rec_write(&r->nv, "trial", n, key)) {
        /* Never trial without the record: a boot that died in it would not be
           counted, and a candidate that wedges would be tried forever. */
        char kn[STAGE_REC_MAX];
        key_name(key, kn, sizeof(kn));
        axl_warning("staged upgrade: the trial of %s could not be recorded; "
                    "starting %s, keeping %s", kn, r->sp.p, r->sp.nw);
        return start_p(r);
    }
    stage_cut("trial");
    flight_set(r);
    axl_watchdog_set(r->o.watchdog_sec);

    void  *buf = NULL;
    size_t len = 0;
    AxlDriverHandle h = NULL;
    char   lo[AXL_SERVICE_LOAD_OPTIONS_MAX];
    size_t lo_len = 0;
    bool   up = false;
    if (axl_file_get_contents(r->sp.nw, &buf, &len) == AXL_OK
        && _axl_driver_load_buffer_at(r->sp.p, buf, len, &h) == AXL_OK) {
        void *iface = NULL;
        up = _axl_service_build_load_options(r->deploy->service, lo, &lo_len) == AXL_OK
             && axl_driver_set_load_options(h, lo, lo_len) == AXL_OK
             && axl_driver_start(h) == AXL_OK
             && axl_protocol_find_guid(&r->guid, &iface) == AXL_OK
             /* a candidate that came up half-stopped is not up */
             && axl_service_state(r->deploy->service) != AXL_SERVICE_STOPPING;
    }
    axl_free(buf);

    if (!up) {
        axl_watchdog_disarm();
        if (h != NULL) {
            axl_driver_unload(h);
        }
        int frc = fail_candidate(r, key);
        *result = AXL_SERVICE_STAGED_ROLLED_BACK;
        last_write(&r->nv, *result);
        flight_clear(r);
        AxlStatus prc = start_p(r);
        return prc != AXL_OK ? prc : (frc != AXL_OK ? AXL_ERR : AXL_OK);
    }

    /* Published. The grace period must fit inside the watchdog whatever the
       setup took: re-arm it for grace + a pad, and wait with the record set. */
    if (r->o.grace_sec != 0) {
        axl_watchdog_set((uint64_t)r->o.grace_sec + STAGE_GRACE_WATCHDOG_PAD);
        axl_msleep((uint64_t)r->o.grace_sec * 1000u);
    }
    axl_watchdog_disarm();
    AxlServiceStagedResult done_as =
        retried ? AXL_SERVICE_STAGED_RETRIED : AXL_SERVICE_STAGED_APPLIED;
    PromoteRc prc = promote(r, key, false, done_as);
    flight_clear(r);
    if (prc == PROMOTE_NOT_RECORDED || prc == PROMOTE_SUPERSEDED) {
        /* The candidate serves, unpromoted (NONE). Not recorded: with its
           trial record still set, the next start counts this boot as one
           that died and tries it again. Superseded: the next start trials
           what P.new holds now. */
        return AXL_OK;
    }
    *result = done_as;
    return prc == PROMOTE_DONE ? AXL_OK : AXL_ERR;
}

AxlStatus
_axl_service_start_staged_opt(const AxlServiceDeploy *deploy, bool apply,
                              AxlServiceStagedResult *result)
{
    AxlServiceStagedResult dummy;
    if (result == NULL) {
        result = &dummy;
    }
    *result = AXL_SERVICE_STAGED_NONE;
    if (_axl_image_tpl_refused("axl_service_start_staged")) {
        return AXL_UNSUPPORTED;
    }
    if (deploy == NULL || deploy->service == NULL) {
        return AXL_INVALID;
    }
    if (deploy->driver_path == NULL) {
        return axl_service_start_embedded(deploy);
    }

    StageRun r = { .deploy = deploy, .o = stage_opts(deploy) };
    if (stage_paths(deploy, &r.sp) != AXL_OK || stage_nv_open(deploy->service, &r.nv) != AXL_OK
        || axl_service_guid(deploy->service, &r.guid) != AXL_OK) {
        return AXL_INVALID;
    }
    /* Half-stopped: nothing serves, and no staged image is touched. */
    if (axl_service_state(deploy->service) == AXL_SERVICE_STOPPING) {
        return AXL_BUSY;
    }
    void *running = NULL;
    if (axl_protocol_find_guid(&r.guid, &running) == AXL_OK) {
        return AXL_OK;   /* running: a staged image waits for the next start */
    }

    /* 1. Resolve what the last boot left -- the table's rows, in order. */
    StageRec rec;
    bool     has_new = file_exists(r.sp.nw);
    bool     has_p   = file_exists(r.sp.p);
    StageKey nkey    = { 0, 0 };
    bool     nkey_ok = has_new && file_key(r.sp.nw, &nkey) == AXL_OK;
    uint32_t next_n  = 1;
    bool     retried = false;

    bool has_rec = rec_read(&r.nv, &rec);
    if (has_rec && rec.kind != REC_TRIAL && has_new && !(nkey_ok && key_eq(&nkey, &rec.key))) {
        /* A promotion was cut, and P.new is no longer what it promoted (a
           restage since): that candidate was never trialled, so the record
           is dropped and it starts from trial 1. */
        char kn[STAGE_REC_MAX];
        key_name(&rec.key, kn, sizeof(kn));
        axl_warning("staged upgrade: %s was replaced during the promotion of %s; "
                    "trying the new one from the start", r.sp.nw, kn);
        axl_attempt_end(&r.nv.at);
        has_rec = false;
    }
    if (has_rec) {
        char kn[STAGE_REC_MAX];
        key_name(&rec.key, kn, sizeof(kn));
        if (rec.kind == REC_PROMOTE_BAK) {
            if (has_new) {
                axl_debug("staged upgrade: finishing the promotion of %s", kn);
                PromoteRc prc = promote(&r, &rec.key, true, AXL_SERVICE_STAGED_APPLIED);
                if (prc == PROMOTE_NOT_RECORDED || prc == PROMOTE_SUPERSEDED) {
                    /* promote:bak stays (the next start resumes), or was
                       cleared for a P.new read differently just now */
                    return start_p(&r);
                }
                *result = AXL_SERVICE_STAGED_APPLIED;
                AxlStatus src = start_p(&r);
                return src != AXL_OK ? src : (prc != PROMOTE_DONE ? AXL_ERR : AXL_OK);
            }
            /* log-level: FAT damage only; nothing else reports it */
            axl_error("staged upgrade: %s vanished during its promotion; starting %s",
                      r.sp.nw, r.sp.p);
            axl_attempt_end(&r.nv.at);
            *result = AXL_SERVICE_STAGED_ROLLED_BACK;
            last_write(&r.nv, *result);
            return start_p(&r);
        }
        if (rec.kind == REC_PROMOTE_SWAP) {
            StageKey pkey;
            if (has_new) {
                axl_debug("staged upgrade: finishing the promotion of %s", kn);
                int prc = AXL_OK;
                if (axl_file_replace(r.sp.nw, r.sp.p, 0, NULL) != AXL_OK) {
                    /* log-level: the record stays; the caller gets AXL_ERR */
                    axl_error("staged upgrade: %s could not be put in place as %s",
                              r.sp.nw, r.sp.p);
                    prc = AXL_ERR;
                } else {
                    axl_attempt_end(&r.nv.at);
                }
                *result = AXL_SERVICE_STAGED_APPLIED;
                last_write(&r.nv, *result);
                AxlStatus src = start_p(&r);
                return src != AXL_OK ? src : prc;
            }
            if (has_p) {
                axl_attempt_end(&r.nv.at);
                if (file_key(r.sp.p, &pkey) == AXL_OK && key_eq(&pkey, &rec.key)) {
                    *result = AXL_SERVICE_STAGED_APPLIED;
                } else {
                    /* log-level: a lost rename; nothing else reports it */
                    axl_error("staged upgrade: the promotion of %s lost the candidate; "
                              "starting %s", kn, r.sp.p);
                    *result = AXL_SERVICE_STAGED_ROLLED_BACK;
                }
                last_write(&r.nv, *result);
                return start_p(&r);
            }
            /* P and P.new both gone. */
            if (file_exists(r.sp.bak) && copy_file(r.sp.bak, r.sp.p) == AXL_OK) {
                /* log-level: FAT lost the candidate; nothing else reports it */
                axl_error("staged upgrade: the promotion of %s lost both images; "
                          "%s restored from %s", kn, r.sp.p, r.sp.bak);
                axl_attempt_end(&r.nv.at);
                *result = AXL_SERVICE_STAGED_ROLLED_BACK;
                last_write(&r.nv, *result);
                return start_p(&r);
            }
            /* log-level: the caller gets AXL_NOT_FOUND; the cause is here */
            axl_error("staged upgrade: the promotion of %s lost the image and there is "
                      "no fallback (%s)", kn, r.sp.bak);
            axl_attempt_end(&r.nv.at);
            *result = AXL_SERVICE_STAGED_ROLLED_BACK;
            last_write(&r.nv, *result);
            return AXL_NOT_FOUND;
        }
        /* REC_TRIAL */
        if (!has_new) {
            axl_attempt_end(&r.nv.at);
            if (key_quarantined(&r.nv, &rec.key)) {
                *result = AXL_SERVICE_STAGED_ROLLED_BACK;
                last_write(&r.nv, *result);
            }
            return start_p(&r);
        }
        if (nkey_ok && key_quarantined(&r.nv, &nkey)) {
            int frc = fail_candidate(&r, &nkey);
            *result = AXL_SERVICE_STAGED_ROLLED_BACK;
            last_write(&r.nv, *result);
            AxlStatus src = start_p(&r);
            return src != AXL_OK ? src : (frc != AXL_OK ? AXL_ERR : AXL_OK);
        }
        if (!nkey_ok || !key_eq(&nkey, &rec.key)) {
            /* A different candidate: its trial starts again. One that is not
               a valid image at all is set aside below -- never charged the
               recorded candidate's dead boots, nor quarantined under its
               key. */
            if (nkey_ok) {
                axl_warning("staged upgrade: %s is not the candidate trial %u recorded "
                            "(%s); trying it from the start", r.sp.nw, (unsigned)rec.n, kn);
            }
            next_n = 1;
        } else if (rec.n >= r.o.tries) {
            axl_warning("staged upgrade: %u boot(s) died trying %s; giving up",
                        (unsigned)rec.n, kn);
            int frc = fail_candidate(&r, &rec.key);
            *result = AXL_SERVICE_STAGED_ROLLED_BACK;
            last_write(&r.nv, *result);
            AxlStatus src = start_p(&r);
            return src != AXL_OK ? src : (frc != AXL_OK ? AXL_ERR : AXL_OK);
        } else {
            next_n  = rec.n + 1;
            retried = true;
            axl_warning("staged upgrade: a boot died trying %s; trying again (%u of %u)",
                     kn, (unsigned)next_n, (unsigned)r.o.tries);
        }
    } else {
        /* Before a trial too: its promotion copies P to P.bak. */
        restore_p(&r);
        if (apply && nkey_ok && key_quarantined(&r.nv, &nkey)) {
            /* Put there by hand -- a stage refuses it: never start it. */
            int frc = fail_candidate(&r, &nkey);
            *result = AXL_SERVICE_STAGED_ROLLED_BACK;
            last_write(&r.nv, *result);
            AxlStatus src = start_p(&r);
            return src != AXL_OK ? src : (frc != AXL_OK ? AXL_ERR : AXL_OK);
        }
    }

    sweep_asides(&r.sp);

    /* 2. Nothing staged (or --no-staged): P as usual. */
    if (!apply || !has_new) {
        return start_p(&r);
    }
    if (!nkey_ok) {
        /* An unreadable or invalid P.new cannot be trialled: treat it as a
           failed candidate. Its key is unknown, so it cannot be quarantined;
           it is renamed out of the way. */
        axl_warning("staged upgrade: %s is not a driver image for this machine", r.sp.nw);
        axl_attempt_end(&r.nv.at);
        if (axl_file_replace(r.sp.nw, r.sp.failed, 0, NULL) != AXL_OK) {
            axl_error("staged upgrade: %s could not be renamed to %s", r.sp.nw,
                      r.sp.failed);
        }
        *result = AXL_SERVICE_STAGED_ROLLED_BACK;
        last_write(&r.nv, *result);
        return start_p(&r);
    }
    /* 3-5. */
    return trial(&r, &nkey, next_n, retried, result);
}

AxlStatus
axl_service_start_staged(const AxlServiceDeploy *deploy, AxlServiceStagedResult *result)
{
    return _axl_service_start_staged_opt(deploy, true, result);
}
