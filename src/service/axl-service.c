/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-service.c
    Driver-shaped lifecycle wrapper over AxlLoop — see
    <axl/axl-service.h>.

    AxlService runs as a DXE driver. Two operational shapes:
      driver-tick (raw)    axl_service_attach_driver  — used by the
                           AXL_SERVICE_DRIVER macro
      embedded-driver      foreground app calls
                           axl_service_start_embedded with a baked-in
                           driver image; the driver's own
                           AXL_SERVICE_DRIVER macro decodes the
                           foreground's serialized options on entry.
                           axl_service_stop / _is_running cover the
                           rest of the foreground side.

    There is no in-process foreground run path. main() in a service
    consumer is a launcher / supervisor, not the body of the service.
**/

#include "../backend/axl-backend.h"
#include "../mem/axl-mem-internal.h"           /* _axl_mem_dump_leaks_at_exit */
#include "../runtime/axl-registry-internal.h"  /* _axl_registry_sweep */
#include "../runtime/axl-libc-heap-internal.h" /* _axl_libc_heap_release */
#include "../util/axl-driver-internal.h"         /* _axl_image_tpl_refused */
#include "axl-service-internal.h"                 /* options builder, staged start */
#include <axl/axl-service.h>
#include <axl/axl-args.h>
#include <axl/axl-atexit.h>
#include <axl/axl-driver.h>
#include <axl/axl-loop.h>
#include <axl/axl-runtime.h>  /* axl_loop_default */
#include <axl/axl-config.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>
#include <axl/axl-sys.h>   /* axl_reset */

#include <stddef.h>         /* offsetof */

AXL_LOG_DOMAIN("service");

// ---------------------------------------------------------------------------
// AXL_SERVICE namespace UUID — used by axl_guid_v5 to derive each
// service's identity GUID from its name. Bumping this would invalidate
// every existing consumer's published GUID, so it's effectively a
// frozen constant. Generated as a v4 UUID for AximCode's AxlService
// module — has no meaning beyond "stable namespace seed."
// ---------------------------------------------------------------------------

static const AxlGuid AXL_SERVICE_NAMESPACE = AXL_GUID(
    0x7d4c0e3a, 0x2f6b, 0x4f8e,
    0x9a, 0x1c, 0x3d, 0x5e, 0x7f, 0x9b, 0x1c, 0x2d);

/* The state tags' namespace (axl_service_state): "<name>/stopping" and
   "<name>/degraded" are derived HERE, not in AXL_SERVICE_NAMESPACE, so no
   service name -- not even one that contains "/stopping" -- can publish the
   GUID of another service's tag. Frozen like the service namespace: launchers
   and drivers of different SDK versions must agree on it. A v4 UUID. */
static const AxlGuid AXL_SERVICE_TAG_NAMESPACE = AXL_GUID(
    0xc2c92e60, 0x01dc, 0x44a4,
    0x9f, 0x8c, 0xe7, 0x3a, 0x8e, 0x49, 0x24, 0x9e);

int
axl_service_guid(const AxlService *svc, AxlGuid *out)
{
    if (svc == NULL || svc->name == NULL || out == NULL) {
        return AXL_ERR;
    }
    return axl_guid_v5(&AXL_SERVICE_NAMESPACE, svc->name, out);
}

#define SERVICE_LOAD_OPTIONS_MAX  AXL_SERVICE_LOAD_OPTIONS_MAX   /* see the internal header */

// ---------------------------------------------------------------------------
// LoadOptions wire-format magic header.
//
// LoadOptions has no spec-mandated encoding. UEFI shell launches pass
// a UCS-2 string; programmatic loaders (axl_driver_set_load_options)
// pass arbitrary bytes. Without a discriminator, an AXL_SERVICE_DRIVER
// image launched by a shell user (instead of axl_service_start_embedded)
// would feed UCS-2 bytes to axl_config_from_string and decode garbage.
//
// AXL prefixes its payload with this 8-byte magic (ASCII "AXLSVC" + a
// 1-byte version + a 1-byte trailing NUL so the prefix is itself a
// valid C-string). The driver-side macro checks the magic before
// invoking the parser; non-matching LoadOptions are skipped (defaults
// from the descriptor table apply instead, with an axl_warning).
//
// Bumping the version byte is how we introduce wire-format changes
// without breaking older driver images deployed against newer
// foreground apps (they just decline to decode and use defaults).
// ---------------------------------------------------------------------------

#define SERVICE_LO_MAGIC      "AXLSVC1"   ///< 7 chars + trailing NUL = 8 bytes
#define SERVICE_LO_MAGIC_LEN  8

// ---------------------------------------------------------------------------
// Public teardown helper — single source of truth for the
// teardown-call shape. Each public site that owns a "setup ran →
// teardown should run" relationship invokes this explicitly. Keeping
// the logging here means consumers see consistent enter/exit lines
// regardless of which deploy mode they use.
//
// P1 pulled this out of axl_service_detach_driver so the macro's
// unload stub can call it directly — previously the stub appeared
// (to anyone reading the macro source) to skip teardown because the
// call was hidden inside detach_driver.
// ---------------------------------------------------------------------------

int
axl_service_teardown(const AxlService *svc)
{
    if (svc == NULL || svc->teardown == NULL) {
        return AXL_OK;
    }
    const char *nm = svc->name != NULL ? svc->name : "(unnamed)";
    axl_debug("service '%s': teardown ENTER", nm);
    int td_rc = svc->teardown(svc->user);
    axl_debug("service '%s': teardown EXIT rc=%d", nm, td_rc);
    /* No warning here: the caller reports a failure once, saying what it
       leaves behind (service_report_teardown_failed). */
    return td_rc;
}

/* The one line a failed teardown produces in the service's own image. A
   failed teardown always leaves the image resident, published and with its
   loop detached -- after an unload or after a rolled-back attach alike. */
static void
service_report_teardown_failed(const AxlService *svc, int rc)
{
    /* log-level: the image stays resident and the caller's status does not
       say that it is the teardown, nor that the next unload retries it --
       the firmware sees only EFI_ABORTED, or a DriverEntry that succeeded */
    axl_warning("service '%s': teardown failed - returned %d; the image stays "
                "resident with its loop stopped, and the next unload retries "
                "the teardown",
                svc->name != NULL ? svc->name : "(unnamed)", rc);
}

// ---------------------------------------------------------------------------
// Driver-mode deployment
// ---------------------------------------------------------------------------

/* Production's answer at every test point: never. A fixture's driver image
   links a strong definition (see axl-service-internal.h). */
__attribute__((weak)) bool
_axl_service_test_point(const char *point)
{
    (void)point;
    return false;
}

/* axl_service_attach_driver, also saying whether a failure left setup's
   resources HELD: setup returned AXL_SERVICE_SETUP_HELD, or the tick could
   not be attached and the rollback teardown failed. Only the AXL_SERVICE_DRIVER
   entry acts on @p held; the public function keeps its answer to OK or ERR. */
static int
service_attach(
    AxlLoop          *loop,
    const AxlService *svc,
    bool             *held
    )
{
    *held = false;
    if (loop == NULL || svc == NULL || svc->setup == NULL) {
        return AXL_ERR;
    }
    /* Single source of truth for tick period: svc->driver_tick_ms with
       0 → AXL_SERVICE_DEFAULT_TICK_MS. Same fallback the
       AXL_SERVICE_DRIVER macro relies on, so consumers see one rule. */
    uint64_t tick_ms = (svc->driver_tick_ms != 0)
                     ? svc->driver_tick_ms
                     : AXL_SERVICE_DEFAULT_TICK_MS;

    const char *nm = svc->name != NULL ? svc->name : "(unnamed)";
    axl_debug("service '%s': attach_driver tick=%llu ms - setup ENTER",
              nm, (unsigned long long)tick_ms);
    int rc = svc->setup(loop, svc->user);
    axl_debug("service '%s': setup EXIT rc=%d", nm, rc);
    if (rc != AXL_OK) {
        /* log-level: the framework DECLINES to attach and carries on, so the
           run continues with the service simply absent. Nothing downstream
           returns "a service was supposed to be here", which makes this line
           the only report that one failed to start rather than being silently
           swallowed. (test-service-startfail and test-service-reload-fail
           assert it, but that is a consequence of it mattering, not the
           reason it is a warning.) */
        axl_warning("service '%s': setup returned %d - not attaching", nm, rc);
        /* Every setup failure is AXL_ERR here, whatever setup returned: an
           AXL_BUSY passed up from, say, axl_serial_open means only "setup
           failed", and must never read as one of this function's own
           outcomes. */
        *held = (rc == AXL_SERVICE_SETUP_HELD);
        if (*held && svc->teardown == NULL) {
            /* log-level: the framework overrides what setup asked for, and the
               caller's status (an entry failure) cannot say so */
            axl_warning("service '%s': setup returned AXL_SERVICE_SETUP_HELD "
                        "with no teardown to finish it - treating it as a "
                        "failure", nm);
            *held = false;
        }
        return AXL_ERR;
    }

    if (_axl_service_test_point("attach")
        || axl_loop_attach_driver(loop, tick_ms) != AXL_OK) {
        axl_debug("service '%s': attach_driver failed - running teardown",
                  nm);
        int td_rc = axl_service_teardown(svc);
        if (td_rc != AXL_OK) {
            /* Setup's resources are still held -- a device that calls into
               this image, say -- so the image must stay resident. */
            service_report_teardown_failed(svc, td_rc);
            *held = true;
        }
        return AXL_ERR;
    }

    return AXL_OK;
}

int
axl_service_attach_driver(
    AxlLoop          *loop,
    const AxlService *svc
    )
{
    bool held;
    return service_attach(loop, svc, &held);
}

int
axl_service_detach_driver(AxlLoop *loop, const AxlService *svc)
{
    /* P1 contract change (vs. earlier AXL_SERVICE releases): this
       function detaches the firmware-tick timer ONLY. It does NOT
       call svc->teardown — the caller does. AXL_SERVICE_DRIVER's
       unload stub and axl_service_attach_driver's failure path each
       invoke axl_service_teardown explicitly so the teardown
       contract is visible at every call site rather than hidden
       inside detach_driver's success path.

       Why: the previous behavior silently skipped teardown if
       axl_loop_detach_driver returned ERR (e.g., the loop was
       never attached). New failure modes added later to
       axl_loop_detach_driver would inherit that silent skip. The
       split keeps each function's responsibility narrow.

       External-consumer impact: any consumer that called
       detach_driver expecting it to also run teardown loses that —
       call axl_service_teardown (or the public path of choice)
       yourself. In-tree consumers (the macro) updated. */
    (void)svc;  /* unused now; kept for ABI/source compat */
    if (loop == NULL || svc == NULL) {
        return AXL_ERR;
    }
    return axl_loop_detach_driver(loop);
}

// ---------------------------------------------------------------------------
// AXL_SERVICE_DRIVER macro support — library-side DriverEntry / Unload.
// One driver image hosts exactly one DriverEntry symbol, so a single
// per-image static state block is enough; the macro emits a one-line
// shim that hands &svc to _axl_service_driver_init, and the unload
// path is wired to _axl_service_driver_unload through axl_driver_set_unload.
// ---------------------------------------------------------------------------

static AxlLoop          *m_drv_loop;
static AxlConfig        *m_drv_cfg;     /* keeps STRING-typed option values
                                         * alive across the service lifetime —
                                         * cfg owns the strdup'd bytes; freeing
                                         * it earlier would dangle the user
                                         * struct's `const char *` fields. */
static AxlHandle         m_drv_handle;  /* this driver image's handle —
                                         * published with the GUID derived
                                         * from svc->name so
                                         * axl_service_stop's
                                         * LocateHandleBuffer can recover it
                                         * from the GUID and unload. Seeded
                                         * to gImageHandle in init before the
                                         * install call so the install
                                         * adds to the existing handle rather
                                         * than creating a sentinel. */
static const AxlService *m_drv_svc;     /* descriptor pointer — the unload
                                         * stub needs it to know which svc to
                                         * detach/teardown/unregister. */
static bool              m_drv_ticking; /* the loop's driver tick is attached.
                                         * A failed teardown leaves the loop
                                         * detached, and the next unload must
                                         * not detach it again (that returns
                                         * AXL_ERR) before retrying teardown. */
static AxlGuid           m_drv_guid;    /* the service GUID published on
                                         * m_drv_handle, for the unwind. */


/* The degraded tag's interface: the reason, copied, readable by any image for
   as long as the tag is published (it lives in this image, which cannot
   unload without removing the tag).

   FROZEN LAYOUT -- a launcher and a driver may come from different SDK
   versions: { uint32_t size; char why[]; }, `size` being the whole struct's.
   Grow it only at the end, never reorder or shrink. A reader follows the UEFI
   size rule (axl_service_state_reason): any `size` holding at least one byte
   of `why` is accepted, and it reads no more of `why` than both layouts
   have, terminating it itself. */
typedef struct {
    uint32_t size;                              /* sizeof(ServiceDegradedTag) */
    char     why[AXL_SERVICE_REASON_MAX + 1];
} ServiceDegradedTag;

static AxlGuid            m_stop_guid;   /* "<name>/stopping" */
static AxlGuid            m_deg_guid;    /* "<name>/degraded" */
static int                m_stop_iface;  /* the stopping tag's interface: a marker */
static ServiceDegradedTag m_deg_iface;
static bool               m_stop_on;
static bool               m_deg_on;

/* AXL_SERVICE_DEFAULT_TICK_MS is published in <axl/axl-service.h>
 * — same value, single source of truth shared with consumers. */

/* The GUID of @p svc's "<name>/<suffix>" state tag. */
int
_axl_service_tag_guid(const AxlService *svc, const char *suffix, AxlGuid *out)
{
    char name[160];
    if (svc == NULL || svc->name == NULL
        || (size_t)axl_snprintf(name, sizeof(name), "%s/%s", svc->name, suffix)
               >= sizeof(name)) {
        return AXL_ERR;
    }
    return axl_guid_v5(&AXL_SERVICE_TAG_NAMESPACE, name, out);
}

/* Publish (or withdraw) that this image is half-stopped. */
static void
service_stopping_set(bool on)
{
    if (on == m_stop_on || m_drv_handle == NULL) {
        return;
    }
    if (on) {
        AxlHandle h = m_drv_handle;
        if (axl_protocol_install(&m_stop_guid, &m_stop_iface, &h) != AXL_OK) {
            /* log-level: nothing else reports that status and start cannot
               tell this service is half-stopped */
            axl_error("service '%s': could not publish that it is stopping; "
                      "status will report it running", m_drv_svc->name);
            return;
        }
    } else {
        axl_protocol_uninstall(m_drv_handle, &m_stop_guid, &m_stop_iface);
    }
    m_stop_on = on;
}

/* Withdraw both state tags: the image is about to unload. */
static void
service_tags_clear(void)
{
    service_stopping_set(false);
    if (m_deg_on && m_drv_handle != NULL) {
        axl_protocol_uninstall(m_drv_handle, &m_deg_guid, &m_deg_iface);
        m_deg_on = false;
    }
}

int
axl_service_set_degraded(bool degraded, const char *why)
{
    if (degraded && why == NULL) {
        return AXL_INVALID;
    }
    if (m_drv_svc == NULL || m_drv_handle == NULL) {
        return AXL_ERR;
    }
    if (!degraded) {
        if (m_deg_on) {
            axl_protocol_uninstall(m_drv_handle, &m_deg_guid, &m_deg_iface);
            m_deg_on = false;
        }
        return AXL_OK;
    }
    /* Replacing the reason of a published tag in place: a reader at a lower
       TPL can see a mix of two reasons for one call, never an unterminated
       one, since the terminator at the end of the array is never written. */
    m_deg_iface.size = (uint32_t)sizeof(m_deg_iface);
    axl_strlcpy(m_deg_iface.why, why, sizeof(m_deg_iface.why));
    if (m_deg_on) {
        return AXL_OK;
    }
    AxlHandle h = m_drv_handle;
    if (axl_protocol_install(&m_deg_guid, &m_deg_iface, &h) != AXL_OK) {
        /* log-level: the caller gets AXL_ERR, but it is the operator who
           would have read the reason in status */
        axl_error("service '%s': could not publish that it is degraded (%s)",
                  m_drv_svc->name, why);
        return AXL_ERR;
    }
    m_deg_on = true;
    return AXL_OK;
}

/* Firmware calls the unload stub with EFIAPI calling convention
 * (ms_abi on x64) — declare it accordingly. The matching pointer is
 * registered with axl_driver_set_unload from _axl_service_driver_init. */
static EFI_STATUS EFIAPI
_axl_service_driver_unload_stub(EFI_HANDLE image_handle)
{
    (void)image_handle;
    /* Order: detach timer FIRST so no notify fires mid-teardown,
     * then run the consumer's teardown to release everything setup
     * opened (TCP4 children, AxlHttpServer, custom OpenProtocol —
     * see the held-protocol hazard note in axl-service.h), then free
     * the loop, then unregister the marker protocol, then drop the
     * cfg (which holds the strings teardown may have read).
     *
     * A FAILED TEARDOWN STOPS HERE and changes nothing else. The unload
     * returns EFI_ABORTED, so the image stays resident -- and so must
     * everything that finds and finishes it: the marker protocol (status,
     * `stop` and reload locate the image by it), the loop and the cfg
     * (teardown's next attempt may read both), m_drv_svc (the next unload
     * needs it to run teardown at all). Freeing them anyway made the next
     * unload find no service, skip teardown and return EFI_SUCCESS, and the
     * firmware freed the image under whatever the teardown had failed to
     * release (a console device still installed, for one).
     *
     * The loop is NOT re-attached. Teardown is not transactional: a failed
     * one may already have freed state its loop sources point at, relying
     * on axl_loop_free to drop those sources, so ticking them again could
     * run a callback into freed memory. The half-stopped service stays
     * resident, published and quiet until an unload's teardown succeeds. */
    if (m_drv_loop != NULL && m_drv_svc != NULL) {
        if (m_drv_ticking) {
            if (axl_service_detach_driver(m_drv_loop, m_drv_svc) != AXL_OK) {
                axl_warning("service '%s': detaching its loop failed",
                            m_drv_svc->name);
            }
            m_drv_ticking = false;
        }
        int td_rc = axl_service_teardown(m_drv_svc);
        if (td_rc != AXL_OK) {
            service_report_teardown_failed(m_drv_svc, td_rc);
            service_stopping_set(true);
            return EFI_ABORTED;
        }
        axl_loop_free(m_drv_loop);
        m_drv_loop = NULL;
    }
    service_tags_clear();
    if (m_drv_handle != NULL && m_drv_svc != NULL) {
        AxlGuid g;
        if (axl_service_guid(m_drv_svc, &g) == AXL_OK) {
            axl_protocol_uninstall(m_drv_handle, &g,
                                               (void *)m_drv_svc);
        }
        m_drv_handle = NULL;
    }
    if (m_drv_cfg != NULL) {
        axl_config_free(m_drv_cfg);
        m_drv_cfg = NULL;
    }
    m_drv_svc = NULL;

    /* C++ global destructors + axl_atexit callbacks, LAST and only on
       success — the same contract AXL_DRIVER's stub applies, for the same
       two reasons: the consumer's teardown above may read globals, and a
       failing unload (returned above) leaves the image RESIDENT, where
       destructed globals would be worse than undestructed ones.
       axl_driver_init (which _axl_service_driver_init calls) is what ran
       the constructors. */
    _axl_driver_cleanup_keep_heap();
    /* Then the same teardown verdict an application prints at exit, so the
       leak gate judges the DRIVER image and not just its launcher: sweep the
       tier-1 registry (it frees its own table, which would otherwise read as
       a leak), then report. */
    _axl_registry_sweep();
    _axl_mem_dump_leaks_at_exit();
    /* And newlib's arena, LAST -- after the sweep, whose destroy callbacks may
       still free into it. That is why this path keeps the heap through
       _axl_driver_cleanup_keep_heap above rather than calling
       axl_driver_cleanup. See axl-libc-heap-internal.h. */
    _axl_libc_heap_release();
    return EFI_SUCCESS;
}

static AxlEfiStatus
service_driver_init(
    void             *image_handle,
    void             *system_table,
    const AxlService *svc
    );

/* Undo what the entry published before setup ran: the loop, the state tags,
   the service protocol, the config. Idempotent -- each piece is cleared as it
   goes -- because it has two callers: the entry's own setup-failure return,
   and the atexit drain of an axl_exit with an EFI error from INSIDE setup.
   That exit never returns here, and EDK2 then unloads the image through
   CoreUnloadAndCloseImage, which removes the LoadedImage protocols but not
   this one: the service GUID stayed installed with an interface pointer into
   the freed image (test-heap-release-qemu.sh, HEAPSVC).

   Registered once and never removed: on every other path it finds nothing
   left to do. A failed entry has unwound itself before its
   axl_driver_cleanup drains atexit, and a resident service's unload stub
   clears all four before ITS drain -- so leaving it armed costs one atexit
   slot, where a disarm would be a branch no test can tell from its
   absence. */
static void
service_entry_unwind(
    void
    )
{
    if (m_drv_loop != NULL) {
        axl_loop_free(m_drv_loop);
        m_drv_loop = NULL;
    }
    service_tags_clear();       /* a degraded tag setup set */
    if (m_drv_handle != NULL) {
        axl_protocol_uninstall(m_drv_handle, &m_drv_guid, (void *)m_drv_svc);
        m_drv_handle = NULL;
    }
    if (m_drv_cfg != NULL) {
        axl_config_free(m_drv_cfg);
        m_drv_cfg = NULL;
    }
    m_drv_svc = NULL;
}

static void
service_entry_unwind_cb(
    void *unused
    ) AXL_CB_NOEXCEPT
{
    (void)unused;
    service_entry_unwind();
}

AxlEfiStatus
_axl_service_driver_init(
    void             *image_handle,
    void             *system_table,
    const AxlService *svc
    )
{
    /* The service's DriverEntry, bracketed like AXL_DRIVER's: an axl_exit
       with an EFI error from inside setup is known to unload the image, so it
       runs the image's destructors and releases its C heap (see
       _axl_driver_entry_begin). It does NOT uninstall the service's
       protocol, which such an exit has always left behind. */
    _axl_driver_entry_begin();
    AxlEfiStatus st = service_driver_init(image_handle, system_table, svc);
    _axl_driver_entry_end();
    return st;
}

static AxlEfiStatus
service_driver_init(
    void             *image_handle,
    void             *system_table,
    const AxlService *svc
    )
{
    if (svc == NULL || svc->setup == NULL || svc->name == NULL) {
        return AXL_EFI_INVALID_PARAMETER;
    }
    m_drv_svc = svc;
    /* axl_driver_init MUST run before any axl_malloc-using helper —
       it's the call that wires gBS/gST so the heap works. The GUID
       derivation below uses axl_checksum_*, which mallocs internally;
       deriving it pre-init returned NULL and silently aborted the
       driver load (no setup line in the serial log). */
    axl_driver_init((AxlHandle)image_handle, (AxlSystemTable *)system_table);
    axl_driver_set_unload((void *)_axl_service_driver_unload_stub);

    AxlGuid svc_guid;
    if (axl_service_guid(svc, &svc_guid) != AXL_OK
        || _axl_service_tag_guid(svc, "stopping", &m_stop_guid) != AXL_OK
        || _axl_service_tag_guid(svc, "degraded", &m_deg_guid) != AXL_OK) {
        m_drv_svc = NULL;
        axl_driver_cleanup();   /* see the entry-failure note below */
        return AXL_EFI_INVALID_PARAMETER;
    }

    /* Decode LoadOptions if present AND the AXL magic header matches.
     * SERVICE_LO_MAGIC distinguishes a payload axl_service_start_embedded
     * or axl_service_reload produced from UCS-2 LoadOptions a shell user
     * might pass -- the latter would decode as garbage. On a mismatch (or
     * no LoadOptions at all), defaults from the descriptor table apply. */
    const void *lo_buf   = NULL;
    size_t      lo_size  = 0;
    const char *cfg_body = NULL;
    if (axl_driver_get_load_options_raw(&lo_buf, &lo_size) == AXL_OK
        && lo_buf != NULL && lo_size >= SERVICE_LO_MAGIC_LEN) {
        if (lo_size > SERVICE_LO_MAGIC_LEN
            && axl_memcmp(lo_buf, SERVICE_LO_MAGIC, SERVICE_LO_MAGIC_LEN) == 0) {
            cfg_body = (const char *)lo_buf + SERVICE_LO_MAGIC_LEN;
        } else {
            axl_warning("AXL_SERVICE_DRIVER: LoadOptions present but lacks "
                        "AXL magic header (likely shell-launched UCS-2); "
                        "ignoring and using descriptor defaults");
        }
    }
    if (svc->opts_descs != NULL && svc->user != NULL) {
        m_drv_cfg = axl_config_new(svc->opts_descs, NULL, svc->user);
        if (m_drv_cfg != NULL && cfg_body != NULL
            && axl_config_from_string(m_drv_cfg, cfg_body) != AXL_OK) {
            axl_warning("AXL_SERVICE_DRIVER: LoadOptions decode failed; "
                        "continuing with descriptor defaults");
        }
    }

    /* Publish the name-derived service GUID on the driver image's own handle so
     * axl_driver_ensure_with_embedded's "did the driver register the
     * protocol?" verify step sees us, axl_service_is_running on the
     * launcher side detects a live deploy via LocateProtocol, AND
     * axl_service_stop can recover the image handle from the GUID
     * via LocateHandleBuffer. Seeding m_drv_handle with the firmware's
     * EFI_HANDLE before the install makes InstallProtocolInterface ADD
     * to the existing handle rather than create a fresh sentinel.
     * Failure here aborts the load — without the protocol registration
     * the launcher's verify step would unload us anyway. */
    m_drv_handle = (AxlHandle)image_handle;
    m_drv_guid   = svc_guid;
    if (axl_protocol_install(&svc_guid,
                             (void *)svc,
                             &m_drv_handle) != AXL_OK) {
        axl_debug("AXL_SERVICE_DRIVER: protocol_install failed");
        if (m_drv_cfg != NULL) {
            axl_config_free(m_drv_cfg); m_drv_cfg = NULL;
        }
        m_drv_svc = NULL;
        axl_driver_cleanup();   /* see the entry-failure note below */
        return AXL_EFI_ABORTED;
    }

    /* From here an exit with an EFI error inside setup must undo the
       publication. A registration failure leaves only that case uncovered;
       the load itself does not depend on it, so it is reported, not fatal. */
    if (axl_atexit(service_entry_unwind_cb, NULL) == 0) {
        axl_warning("AXL_SERVICE_DRIVER: entry unwind not registered; an "
                    "axl_exit from setup would leave the service published");
    }

    m_drv_loop = axl_loop_new();
    if (m_drv_loop == NULL) {
        service_entry_unwind();
        axl_driver_cleanup();   /* see the entry-failure note below */
        return AXL_EFI_OUT_OF_RESOURCES;
    }

    bool held      = false;
    int  attach_rc = service_attach(m_drv_loop, svc, &held);
    if (attach_rc != AXL_OK && held) {
        /* Setup's resources are still held -- its setup said so
           (AXL_SERVICE_SETUP_HELD), or the tick could not be attached and the
           rollback teardown failed -- and they may call into this image.
           Returning an error here would have the firmware free the image
           WITHOUT calling Unload (see the entry-failure note below), so come
           up half-stopped instead -- exactly the state a failed unload
           leaves: resident, published, tagged stopping, the loop kept but not
           ticking -- and let the next unload run the teardown. */
        m_drv_ticking = false;
        service_stopping_set(true);
        return AXL_EFI_SUCCESS;
    }
    if (attach_rc != AXL_OK) {
        service_entry_unwind();
        /* ENTRY FAILURE IS THE OTHER TEARDOWN PATH. EDK2's CoreStartImage
           unloads an image whose DriverEntry returned an error through
           CoreUnloadAndCloseImage, which -- unlike CoreUnloadImage -- never
           invokes Image->Info.Unload. So _axl_service_driver_unload_stub does
           NOT run on any of these four returns, and this is the only place the
           constructors axl_driver_init ran can be undone. Without it a failed
           service load leaks every global's destructor, the atexit table
           itself, and on an exceptions build the registered .eh_frame table.
           Idempotent, so the success path's later call is unaffected. */
        axl_driver_cleanup();
        return AXL_EFI_ABORTED;
    }
    m_drv_ticking = true;

    return AXL_EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Embedded-driver deployment
// ---------------------------------------------------------------------------

bool
axl_service_is_running(const AxlServiceDeploy *deploy)
{
    if (deploy == NULL || deploy->service == NULL) {
        return false;
    }
    AxlGuid g;
    if (axl_service_guid(deploy->service, &g) != AXL_OK) {
        return false;
    }
    void *iface = NULL;
    return axl_protocol_find_guid(&g, &iface) == AXL_OK;
}

/* Whether any handle publishes @p guid. */
static bool
service_guid_published(const AxlGuid *guid)
{
    void *iface = NULL;
    return axl_protocol_find_guid(guid, &iface) == AXL_OK;
}

#define SERVICE_STOPPING_REASON  "teardown failed; 'stop' retries; a reset clears it"

AxlServiceState
axl_service_state(const AxlService *svc)
{
    AxlGuid g;
    /* The tags are checked first: the worst state wins, and a tag exists
       only on a handle that also publishes the service. */
    if (_axl_service_tag_guid(svc, "stopping", &g) == AXL_OK && service_guid_published(&g)) {
        return AXL_SERVICE_STOPPING;
    }
    if (_axl_service_tag_guid(svc, "degraded", &g) == AXL_OK && service_guid_published(&g)) {
        return AXL_SERVICE_DEGRADED;
    }
    if (axl_service_guid(svc, &g) == AXL_OK && service_guid_published(&g)) {
        return AXL_SERVICE_RUNNING;
    }
    return AXL_SERVICE_STOPPED;
}

int
axl_service_state_reason(const AxlService *svc, char *buf, size_t size)
{
    if (svc == NULL || buf == NULL || size == 0) {
        return AXL_INVALID;
    }
    buf[0] = '\0';
    AxlServiceState st = axl_service_state(svc);
    if (st == AXL_SERVICE_STOPPING) {
        axl_strlcpy(buf, SERVICE_STOPPING_REASON, size);
        return AXL_OK;
    }
    AxlGuid g;
    void   *iface = NULL;
    if (st != AXL_SERVICE_DEGRADED || _axl_service_tag_guid(svc, "degraded", &g) != AXL_OK
        || axl_protocol_find_guid(&g, &iface) != AXL_OK || iface == NULL) {
        return AXL_NOT_FOUND;
    }
    /* The frozen layout's size rule (see ServiceDegradedTag). */
    const ServiceDegradedTag *tag  = (const ServiceDegradedTag *)iface;
    const size_t              head = offsetof(ServiceDegradedTag, why);
    if (tag->size < head + 1) {
        axl_strlcpy(buf, "(degraded; reason unreadable)", size);
        return AXL_OK;
    }
    size_t n = tag->size - head;                 /* the writer's why[] */
    if (n > sizeof(tag->why)) {
        n = sizeof(tag->why);                    /* no more than ours has */
    }
    if (n > size - 1) {
        n = size - 1;
    }
    size_t len = 0;
    while (len < n && tag->why[len] != '\0') {
        len++;
    }
    axl_memcpy(buf, tag->why, len);
    buf[len] = '\0';
    return AXL_OK;
}

/* Build the LoadOptions payload from @p svc's option struct (no intermediate
   AxlConfig -- the consumer may have populated user via axl_args_get_*, never
   touching AxlConfig). Layout: SERVICE_LO_MAGIC (8 bytes including trailing
   NUL) + URL-encoded query string (NUL-terminated). The magic gives the
   driver-side macro a way to distinguish "AXL serialized this" from "shell
   launched me with a UCS-2 string." Always emits the magic, even with no
   options: an empty payload after it is a legal "use all defaults" signal.
   @p buf holds SERVICE_LOAD_OPTIONS_MAX bytes. */
int
_axl_service_build_load_options(const AxlService *svc, char *buf, size_t *out_len)
{
    axl_memcpy(buf, SERVICE_LO_MAGIC, SERVICE_LO_MAGIC_LEN);
    char  *body_buf  = buf + SERVICE_LO_MAGIC_LEN;
    size_t body_size = SERVICE_LOAD_OPTIONS_MAX - SERVICE_LO_MAGIC_LEN;
    body_buf[0] = '\0';

    if (svc->opts_descs != NULL && svc->user != NULL) {
        if (axl_config_target_to_string(svc->opts_descs, svc->user,
                                        body_buf, body_size) != AXL_OK) {
            /* log-level: the start fails on a limit the user can do something
               about (a long path or string option), and the caller's own
               message ("could not start") cannot name it */
            axl_error("service '%s': its options (URL-encoded) do not fit the "
                      "%d-byte LoadOptions -- shorten a long path or string option",
                      svc->name != NULL ? svc->name : "(unnamed)",
                      SERVICE_LOAD_OPTIONS_MAX);
            return AXL_ERR;
        }
    }
    /* Magic header + body + body's trailing NUL. */
    *out_len = SERVICE_LO_MAGIC_LEN + axl_strlen(body_buf) + 1;
    return AXL_OK;
}

AxlStatus
axl_service_start_embedded(const AxlServiceDeploy *deploy)
{
    if (deploy == NULL || deploy->service == NULL) {
        return AXL_ERR;
    }
    /* embedded_only ("skip disk, use the blob") and driver_path ("use exactly
       this disk file") are contradictory — fail fast on the misconfiguration
       rather than silently honoring one. */
    if (deploy->embedded_only && deploy->driver_path != NULL) {
        axl_debug("service '%s': embedded_only and driver_path are mutually "
                  "exclusive",
                  deploy->service->name != NULL ? deploy->service->name
                                                : "(unnamed)");
        return AXL_ERR;
    }
    /* Required fields depend on the route. driver_name is the filename the
       4-path search looks for and the blob is that search's fallback — so:
         - pinned driver_path reads neither (both unrequired);
         - embedded_only reads the blob directly (driver_name only names the
           loaded image), so the blob is required but driver_name is not;
         - the default search+embedded resolution needs both driver_name and
           the blob. */
    if (deploy->driver_path == NULL) {
        bool need_name = !deploy->embedded_only;
        if ((need_name && deploy->driver_name == NULL)
            || deploy->driver_blob == NULL
            || deploy->driver_blob_len == 0)
        {
            return AXL_ERR;
        }
    }

    const AxlService *svc = deploy->service;
    AxlGuid           svc_guid;
    if (axl_service_guid(svc, &svc_guid) != AXL_OK) {
        axl_debug("service: start_embedded - descriptor missing name");
        return AXL_ERR;
    }
    /* Half-stopped is not running: nothing serves, and nothing may be started
       over the image that still holds what its teardown did not release. */
    if (axl_service_state(svc) == AXL_SERVICE_STOPPING) {
        return AXL_BUSY;
    }
    /* Running already is an answer at any TPL; starting it is a load. */
    void *running = NULL;
    if (axl_protocol_find_guid(&svc_guid, &running) != AXL_OK
        && _axl_image_tpl_refused("axl_service_start_embedded")) {
        return AXL_UNSUPPORTED;
    }

    char   load_opts[SERVICE_LOAD_OPTIONS_MAX];
    size_t load_opts_len = 0;
    if (_axl_service_build_load_options(svc, load_opts, &load_opts_len) != AXL_OK) {
        return AXL_ERR;
    }

    AxlStatus rc;
    if (deploy->driver_path != NULL) {
        /* Pinned: exactly this file, no 4-path search and no embedded blob,
           so a stale copy of driver_name elsewhere on the box cannot shadow
           the image the launcher staged. */
        rc = axl_driver_ensure_from_path(
            &svc_guid,
            deploy->driver_path,
            load_opts_len > 0 ? load_opts : NULL,
            load_opts_len);
    } else if (deploy->embedded_only) {
        /* Embedded-only: load the baked-in blob directly, skipping the disk
           search so a stale loose <driver_name> beside the launcher cannot
           shadow it. */
        rc = axl_driver_ensure_embedded_only(
            &svc_guid,
            deploy->driver_blob,
            deploy->driver_blob_len,
            deploy->driver_name,
            load_opts_len > 0 ? load_opts : NULL,
            load_opts_len);
    } else {
        rc = axl_driver_ensure_with_embedded(
            &svc_guid,
            deploy->driver_name,
            deploy->driver_blob,
            deploy->driver_blob_len,
            NULL,                     /* no override_name */
            load_opts_len > 0 ? load_opts : NULL,
            load_opts_len);
    }
    /* Published is not proof it is serving: an entry whose setup held its
       resources, or whose rollback teardown failed, comes up half-stopped. */
    if (rc == AXL_OK && axl_service_state(svc) == AXL_SERVICE_STOPPING) {
        return AXL_BUSY;
    }
    return rc;
}

int
axl_service_stop(const AxlServiceDeploy *deploy)
{
    if (deploy == NULL || deploy->service == NULL) {
        return AXL_ERR;
    }
    AxlGuid svc_guid;
    if (axl_service_guid(deploy->service, &svc_guid) != AXL_OK) {
        axl_debug("service: stop - descriptor missing name");
        return AXL_ERR;
    }
    const char *nm = deploy->service->name;

    /* Resolve every handle publishing the service GUID. AXL_SERVICE_DRIVER
       installs the GUID on its own image handle, so each match is the
       image we need to UnloadImage on. axl_protocol_enumerate_guid hands
       back an axl_malloc'd array (already copied out of the firmware's
       LocateHandleBuffer scratch), so caller frees with axl_free. */
    void   **handles = NULL;
    size_t   count   = 0;
    if (axl_protocol_enumerate_guid(&svc_guid, &handles, &count) != AXL_OK) {
        axl_debug("service '%s': enumerate_guid failed", nm);
        return AXL_ERR;
    }
    if (count == 0) {
        /* Already stopped (or never launched). Idempotent success —
           callers that care can axl_service_is_running first. */
        axl_debug("service '%s': stop - already stopped", nm);
        axl_free(handles);
        return AXL_OK;
    }

    int rc = AXL_OK;
    for (size_t i = 0; i < count; i++) {
        axl_debug("service '%s': stop - unloading image handle %p",
                  nm, handles[i]);
        if (axl_driver_unload((AxlDriverHandle)handles[i]) != AXL_OK) {
            /* Debug only: axl_driver_unload already warned with the raw
               EFI_STATUS (and the held-protocol hint where it applies), an
               AXL_SERVICE_DRIVER image's own unload stub has said why it
               refused, and the caller gets AXL_ERR -- a fourth line for the
               same failure only buries the one that explains it. */
            axl_debug("service '%s': stop failed for image handle %p",
                      nm, handles[i]);
            rc = AXL_ERR;
        }
    }
    axl_free(handles);
    return rc;
}

// ---------------------------------------------------------------------------
// axl_service_supervise — standard "block on default loop, stop on quit"
// body. Public so consumers writing their own main() can compose it
// without re-deriving the loop_run / stop / rc-translate sequence.
// ---------------------------------------------------------------------------

int
axl_service_supervise(const AxlServiceDeploy *deploy)
{
    if (deploy == NULL || deploy->service == NULL) {
        return 1;
    }
    AxlLoop *loop   = axl_loop_default();
    int      run_rc = (loop != NULL) ? axl_loop_run(loop) : 0;
    int      stop_rc = axl_service_stop(deploy);
    /* axl_loop_run returns 0 on clean quit and -1 on Ctrl-C / break;
       both mean "supervisor saw a quit signal," not failure. Anything
       else is a real loop error. */
    return (run_rc == 0 || run_rc == -1)
           ? (stop_rc == AXL_OK ? 0 : 1)
           : 1;
}

// ---------------------------------------------------------------------------
// axl_service_main — default supervisor body (start/stop/status verbs,
// systemctl-flavored)
// ---------------------------------------------------------------------------

/* Map an AxlConfigDesc entry to an AxlArgs type. UINT/INT field-size
 * dispatch matches axl_config_target_to_string's pattern. STRING with
 * a non-NULL choices[] elevates to AXL_ARG_CHOICE so CLI validation
 * fires and --help lists the values. */
static int
cfg_type_to_arg_type(const AxlConfigDesc *d)
{
    switch (d->type) {
    case AXL_CFG_BOOL:
        return AXL_ARG_BOOL;
    case AXL_CFG_UINT:
        if (d->field_size == sizeof(uint8_t))  return AXL_ARG_U8;
        if (d->field_size == sizeof(uint16_t)) return AXL_ARG_U16;
        if (d->field_size == sizeof(uint32_t)) return AXL_ARG_U32;
        if (d->field_size == sizeof(uint64_t)) return AXL_ARG_U64;
        return -1;
    case AXL_CFG_INT:
        return AXL_ARG_S64;  /* AxlArgs only supports S64 for signed */
    case AXL_CFG_STRING:
        return (d->choices != NULL) ? AXL_ARG_CHOICE : AXL_ARG_STRING;
    default:
        return -1;
    }
}

/* Apply parsed AxlArgs values into svc->user via the descriptor table.
 * Mirrors what the deleted service_apply_args helper did. */
static int
service_main_apply_args(const AxlService *svc, AxlArgs *args)
{
    if (svc->opts_descs == NULL || svc->user == NULL) {
        return AXL_OK;
    }
    for (const AxlConfigDesc *d = svc->opts_descs; d->key != NULL; d++) {
        if (d->field_size == 0) continue;
        uint8_t *field = (uint8_t *)svc->user + d->offset;
        switch (d->type) {
        case AXL_CFG_BOOL:
            if (d->field_size == sizeof(bool)) {
                *(bool *)field = axl_args_get_bool(args, d->key);
            }
            break;
        case AXL_CFG_UINT: {
            uint64_t v = axl_args_get_uint(args, d->key);
            if (d->field_size == sizeof(uint8_t))  *(uint8_t  *)field = (uint8_t)v;
            else if (d->field_size == sizeof(uint16_t)) *(uint16_t *)field = (uint16_t)v;
            else if (d->field_size == sizeof(uint32_t)) *(uint32_t *)field = (uint32_t)v;
            else if (d->field_size == sizeof(uint64_t)) *(uint64_t *)field = v;
            break;
        }
        case AXL_CFG_INT: {
            int64_t v = axl_args_get_int(args, d->key);
            if (d->field_size == sizeof(int32_t))      *(int32_t *)field = (int32_t)v;
            else if (d->field_size == sizeof(int64_t)) *(int64_t *)field = v;
            break;
        }
        case AXL_CFG_STRING:
            if (d->field_size == sizeof(char *)) {
                *(const char **)field = axl_args_get_string(args, d->key);
            }
            break;
        default:
            break;
        }
    }
    return AXL_OK;
}

/* Verb handlers stash deploy via this thread-unsafe (single-threaded
 * UEFI) global. axl_service_main sets/clears it around axl_args_run. */
static const AxlServiceDeploy *m_main_deploy;

/* Print "<name>: stopping (...)" -- the one line every verb gives for a
   half-stopped service. */
static void
service_main_say_stopping(const AxlService *svc)
{
    axl_printf("%s: stopping (%s)\n", svc->name, SERVICE_STOPPING_REASON);
}

static int
service_main_start(AxlArgs *a)
{
    const AxlServiceDeploy *deploy = m_main_deploy;
    const AxlService       *svc    = deploy->service;
    service_main_apply_args(svc, a);

    char why[AXL_SERVICE_REASON_MAX + 1];
    switch (axl_service_state(svc)) {
    case AXL_SERVICE_STOPPING:
        /* Before the staged upgrade: nothing may be applied over an image
           that still holds what its teardown did not release. */
        service_main_say_stopping(svc);
        return 1;
    case AXL_SERVICE_DEGRADED:
        axl_service_state_reason(svc, why, sizeof(why));
        axl_printf("%s: already running (degraded: %s)\n", svc->name, why);
        return 0;
    case AXL_SERVICE_RUNNING:
        axl_printf("%s: already running\n", svc->name);
        return 0;
    case AXL_SERVICE_STOPPED:
    default:
        break;
    }
    /* A pinned image may have an upgrade staged beside it: apply it (or, with
       --no-staged, only resolve what the last boot left) as it starts. */
    AxlServiceStagedResult staged = AXL_SERVICE_STAGED_NONE;
    int rc = (deploy->driver_path != NULL)
             ? _axl_service_start_staged_opt(deploy, !axl_args_get_bool(a, "no-staged"),
                                             &staged)
             : axl_service_start_embedded(deploy);
    static const char *const staged_text[] = {
        [AXL_SERVICE_STAGED_NONE]        = NULL,
        [AXL_SERVICE_STAGED_APPLIED]     = "staged upgrade applied",
        [AXL_SERVICE_STAGED_RETRIED]     = "staged upgrade applied on a retry",
        [AXL_SERVICE_STAGED_ROLLED_BACK] = "staged upgrade rolled back",
    };
    if ((unsigned)staged < sizeof(staged_text) / sizeof(staged_text[0])
        && staged_text[staged] != NULL) {
        axl_printf("%s: %s\n", deploy->service->name, staged_text[staged]);
    }
    if (rc == AXL_BUSY && axl_service_state(svc) == AXL_SERVICE_STOPPING) {
        service_main_say_stopping(svc);   /* the image it started came up so */
        return 1;
    }
    if (rc != AXL_OK) {
        axl_printf("%s: start failed (rc=%d)\n", svc->name, rc);
        return 1;
    }

    if (axl_args_get_bool(a, "detach")) {
        return 0;
    }

    return axl_service_supervise(deploy);
}

static int
service_main_stop(AxlArgs *a)
{
    const AxlServiceDeploy *deploy = m_main_deploy;
    if (!axl_service_is_running(deploy)) {
        axl_printf("%s: not running\n", deploy->service->name);
        return 0;
    }
    int rc = axl_service_stop(deploy);
    bool reset = axl_args_get_bool(a, "reset");
    if (rc != AXL_OK && reset
        && axl_service_state(deploy->service) == AXL_SERVICE_STOPPING) {
        /* The last resort, for the one failure it is meant for: an
           AXL_SERVICE_DRIVER whose teardown still fails (only that sets
           STOPPING). There is no forced unload: freeing an image that
           something still calls into is a use-after-free. STOPPING after a
           failed stop means the stop reached the image and its teardown
           failed again. */
        axl_printf("%s: the teardown still fails - resetting the machine\n",
                   deploy->service->name);
        /* log-level: the machine is about to reset; this is the last line the
           log can carry about why */
        axl_warning("service '%s': stop --reset: the teardown still fails; "
                    "resetting", deploy->service->name);
        /* Before anything that might not come back (axl-log.h): an attached
           log file keeps its tail. */
        axl_log_flush();
        axl_reset(AXL_RESET_COLD);
    }
    if (rc != AXL_OK) {
        axl_printf("%s: stop failed (rc=%d)\n", deploy->service->name, rc);
        if (reset) {
            /* Not known to be a teardown failure: a foreign image with no
               Unload handler, a protocol another agent holds, an allocation
               failure -- or, rarely, a driver whose stopping tag could not be
               published. Say what was observed, not a guess at why. A reset
               would not reliably help -- a foreign image comes back at boot. */
            static const char *const state_name[] = {
                [AXL_SERVICE_STOPPED]  = "stopped",
                [AXL_SERVICE_RUNNING]  = "running",
                [AXL_SERVICE_DEGRADED] = "degraded",
                [AXL_SERVICE_STOPPING] = "stopping",
            };
            AxlServiceState st = axl_service_state(deploy->service);
            axl_printf("%s: not resetting - its state is %s, not stopping\n",
                       deploy->service->name,
                       (unsigned)st < sizeof(state_name) / sizeof(state_name[0])
                       ? state_name[st] : "unknown");
        }
        return 1;
    }
    axl_printf("%s: stopped\n", deploy->service->name);
    return 0;
}

static int
service_main_status(AxlArgs *a)
{
    (void)a;
    const AxlService *svc = m_main_deploy->service;
    char              why[AXL_SERVICE_REASON_MAX + 1];

    switch (axl_service_state(svc)) {
    case AXL_SERVICE_STOPPING:
        service_main_say_stopping(svc);
        return AXL_SERVICE_EXIT_STOPPING;
    case AXL_SERVICE_DEGRADED:
        /* Up, as systemd counts "degraded" active: exit 0. */
        axl_service_state_reason(svc, why, sizeof(why));
        axl_printf("%s: degraded (%s)\n", svc->name, why);
        return 0;
    case AXL_SERVICE_RUNNING:
        axl_printf("%s: running\n", svc->name);
        return 0;
    case AXL_SERVICE_STOPPED:
    default:
        axl_printf("%s: stopped\n", svc->name);
        return 1;
    }
}

int
axl_service_main(const AxlServiceDeploy *deploy, int argc, char **argv)
{
    if (deploy == NULL || deploy->service == NULL) {
        return 1;
    }

    /* Synthesize AxlArgDesc[] from svc->opts_descs (if any) plus the
     * standard --detach flag. The launch verb consumes both. */
    const AxlConfigDesc *descs = deploy->service->opts_descs;
    size_t n_opts = 0;
    if (descs != NULL) {
        for (const AxlConfigDesc *d = descs; d->key != NULL; d++) {
            n_opts++;
        }
    }
    AxlArgDesc *flags = axl_malloc((n_opts + 3) * sizeof(AxlArgDesc));
    if (flags == NULL) {
        return 1;
    }
    for (size_t i = 0; i < n_opts; i++) {
        int t = cfg_type_to_arg_type(&descs[i]);
        flags[i] = (AxlArgDesc){
            .name          = descs[i].key,
            .short_name    = descs[i].short_name,
            .type          = (t >= 0) ? t : AXL_ARG_STRING,
            .default_value = descs[i].default_value,
            .help          = descs[i].description,
            .choices       = descs[i].choices,
            /* Propagate the option's numeric range so the synthesized CLI
             * validates it (AxlArgDesc min/max are uint64_t, 0 = none — same
             * convention as the config descriptor; signed bounds ride the cast,
             * matching AxlArgDesc's documented signed-via-cast pattern). */
            .min           = (uint64_t)descs[i].min,
            .max           = (uint64_t)descs[i].max,
        };
    }
    flags[n_opts] = (AxlArgDesc){
        .name = "detach", .short_name = 'd', .type = AXL_ARG_BOOL,
        .help = "Launch driver and exit instead of supervising",
    };
    flags[n_opts + 1] = (AxlArgDesc){
        .name = "no-staged", .type = AXL_ARG_BOOL,
        .help = "Do not apply a staged upgrade (one is applied by default)",
    };
    flags[n_opts + 2] = (AxlArgDesc){0};

    static const AxlArgDesc stop_flags[] = {
        { .name = "reset", .type = AXL_ARG_BOOL,
          .help = "If the teardown still fails, reset the machine" },
        { 0 }
    };

    const AxlArgsNode verbs[] = {
        { .name = "start",  .help = "Start the service driver, applying a staged upgrade",
          .flags = flags, .handler = service_main_start },
        { .name = "stop",   .help = "Stop a running service driver",
          .flags = stop_flags, .handler = service_main_stop },
        { .name = "status", .help = "Show service state",
          .handler = service_main_status },
        { 0 }
    };
    AxlArgsNode root = {
        .name  = deploy->service->name,
        .verbs = verbs,
    };

    m_main_deploy = deploy;
    int rc = axl_args_run(argc, argv, &root);
    m_main_deploy = NULL;
    axl_free(flags);
    return rc;
}

// ---------------------------------------------------------------------------
// Reload (in-place upgrade), asked for by an APPLICATION: load the
// replacement, stop the running service, start the replacement. See
// axl-service.h.
// ---------------------------------------------------------------------------

static AxlStatus
service_reload_impl(const char *api, const AxlService *svc, const char *path,
                    const void *buf, size_t buf_len)
{
    /* LoadImage and StartImage below TPL_CALLBACK only: a service's own
       callbacks never qualify, which is why reload is an application's call. */
    if (_axl_image_tpl_refused(api)) {
        return AXL_UNSUPPORTED;
    }
    if (svc == NULL || (path == NULL && (buf == NULL || buf_len == 0))) {
        return AXL_INVALID;
    }
    AxlGuid svc_guid;
    if (axl_service_guid(svc, &svc_guid) != AXL_OK) {
        return AXL_INVALID;
    }
    const char *nm = svc->name;

    /* Running? And not from inside its own image: at TPL_APPLICATION the
       service's code runs only under an application's call into it, and an
       image cannot unload itself with that call still on its stack. */
    void   **handles = NULL;
    size_t   count   = 0;
    if (axl_protocol_enumerate_guid(&svc_guid, &handles, &count) != AXL_OK
        || count == 0) {
        axl_free(handles);
        return AXL_INVALID;
    }
    bool self = false;
    for (size_t i = 0; i < count; i++) {
        self = self || handles[i] == (void *)gImageHandle;
    }
    axl_free(handles);
    if (self) {
        axl_error("%s: service '%s' cannot replace its own image; call it from "
                  "an application, such as a Shell command", api, nm);
        return AXL_UNSUPPORTED;
    }

    /* The replacement starts with the options the caller's descriptor holds,
       as axl_service_start_embedded starts a service. */
    char   lo[SERVICE_LOAD_OPTIONS_MAX];
    size_t lo_len = 0;
    if (_axl_service_build_load_options(svc, lo, &lo_len) != AXL_OK) {
        return AXL_NO_RESOURCES;
    }

    /* Load the replacement first: a load failure is recoverable -- nothing
       is stopped, the service keeps serving -- so it reports AXL_NOT_FOUND,
       never the AXL_ERR that means "down". */
    AxlDriverHandle h = NULL;
    int lrc = (buf != NULL)
              ? axl_driver_load_buffer((const unsigned char *)buf, buf_len, &h)
              : axl_driver_load(path, &h);
    if (lrc != AXL_OK || h == NULL) {
        return AXL_NOT_FOUND;
    }
    if (axl_driver_set_load_options(h, lo, lo_len) != AXL_OK) {
        axl_driver_unload(h);
        return AXL_NO_RESOURCES;
    }

    /* Stop the running service -- UnloadImage, synchronously, from this
       application: its unload stub detaches the loop and runs its teardown
       (releasing the ports), and its image is gone, off every stack, before
       this returns. */
    AxlServiceDeploy running = { .service = svc };
    int stop_rc = axl_service_stop(&running);

    /* Still published: an image with no Unload handler (firmware-shipped,
       foreign) or one whose unload refused before it tore anything down is
       still serving. That is not "down": say so with a non-fatal code, and
       start nothing over it. axl_service_stop has already warned which
       handle refused. */
    void *still = NULL;
    if (axl_protocol_find_guid(&svc_guid, &still) == AXL_OK) {
        axl_debug("service '%s': still published after the stop (rc=%d); the "
                  "replacement was not started", nm, stop_rc);
        axl_driver_unload(h);
        return AXL_DENIED;
    }

    /* From here a failure leaves the service down. */
    if (stop_rc != AXL_OK) {
        /* log-level: AXL_ERR says only "down"; which of its three causes --
           this one, a failed start, or nothing published -- is said here */
        axl_warning("service '%s': the running image would not stop; the "
                    "replacement was not started", nm);
        axl_driver_unload(h);
        return AXL_ERR;
    }

    /* Start the replacement: its DriverEntry rebinds the ports and comes up
       resident. StartImage succeeding is not proof it attached -- a foreign
       image, or one whose setup failed, publishes nothing -- so look. */
    if (axl_driver_start(h) != AXL_OK) {
        axl_driver_unload(h);
        return AXL_ERR;
    }
    void *iface = NULL;
    if (axl_protocol_find_guid(&svc_guid, &iface) != AXL_OK) {
        /* log-level: AXL_ERR says only "down"; that StartImage succeeded and
           the replacement still published nothing is said only here */
        axl_warning("service '%s': replacement started but did not publish the "
                    "service protocol - treating as a start failure", nm);
        axl_driver_unload(h);
        return AXL_ERR;
    }
    return AXL_OK;
}

AxlStatus
axl_service_reload(const AxlService *svc, const char *new_path)
{
    return service_reload_impl("axl_service_reload", svc, new_path, NULL, 0);
}

AxlStatus
axl_service_reload_buffer(const AxlService *svc, const void *image, size_t image_len)
{
    return service_reload_impl("axl_service_reload_buffer", svc, NULL, image,
                               image_len);
}

