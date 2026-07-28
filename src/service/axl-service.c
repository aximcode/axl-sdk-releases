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
#include <axl/axl-service.h>
#include <axl/axl-args.h>
#include <axl/axl-driver.h>
#include <axl/axl-loop.h>
#include <axl/axl-runtime.h>  /* axl_loop_default */
#include <axl/axl-config.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>
#include <axl/axl-str.h>
#include <axl/axl-stream.h>

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

int
axl_service_guid(const AxlService *svc, AxlGuid *out)
{
    if (svc == NULL || svc->name == NULL || out == NULL) {
        return AXL_ERR;
    }
    return axl_guid_v5(&AXL_SERVICE_NAMESPACE, svc->name, out);
}

// ---------------------------------------------------------------------------
// LoadOptions buffer cap. axl_config_to_string serializes set-only
// values (defaults are re-applied on the parse side from the descriptor
// table), so a typical service's payload is well under 1 KiB. Cap at
// 4 KiB as headroom for MULTI-heavy configs (e.g. a dozen
// "header.X-Foo: bar" entries). Hitting the cap surfaces as an explicit
// AXL_ERR rather than truncated load options.
// ---------------------------------------------------------------------------

#define SERVICE_LOAD_OPTIONS_MAX  4096

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

// Reload variant of the LoadOptions payload (axl_service_reload). Same 8-byte
// framing, distinct magic. Layout after the magic: old-image-handle (ptr-sized)
// + handoff-event handle (ptr-sized) + the normal config C-string. The
// replacement driver reads the two handles, arms the event on its own loop, and
// unloads the old image from that event's callback (off the old image's stack).
#define SERVICE_LO_MAGIC_RELOAD  "AXLSVR1"   ///< 7 chars + trailing NUL = 8 bytes
#define SERVICE_RELOAD_HDR_LEN   (SERVICE_LO_MAGIC_LEN + 2u * sizeof(void *))

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
    if (td_rc != AXL_OK) {
        axl_warning("service '%s': teardown returned %d", nm, td_rc);
    }
    return td_rc;
}

// ---------------------------------------------------------------------------
// Driver-mode deployment
// ---------------------------------------------------------------------------

int
axl_service_attach_driver(
    AxlLoop          *loop,
    const AxlService *svc
    )
{
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
        axl_warning("service '%s': setup returned %d - not attaching", nm, rc);
        return rc;
    }

    if (axl_loop_attach_driver(loop, tick_ms) != AXL_OK) {
        axl_warning("service '%s': attach_driver failed - running teardown",
                    nm);
        axl_service_teardown(svc);
        return AXL_ERR;
    }

    return AXL_OK;
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

/* Self-reload state (axl_service_reload). On the OLD side, set true once reload
   has run teardown+detach itself, so the unload stub the replacement triggers
   via UnloadImage does NOT tear down a second time (it still frees the loop).
   On the NEW side, the handoff decoded from the reload LoadOptions: the old
   image to reclaim once it signals it is idle, and that signal event. */
static bool           m_drv_reload_torn_down;
static AxlHandle      m_drv_reload_old;
static AxlEventHandle m_drv_reload_evt;

/* NEW side: the old image signalled it has detached and is off-stack — reclaim
   it from our own loop tick. Runs once. */
static bool
service_reload_unload_old(void *data)
{
    (void)data;
    if (m_drv_reload_old != NULL) {
        EFI_STATUS st = axl_efi_call(axl_bs()->UnloadImage, 1,
                                     (EFI_HANDLE)m_drv_reload_old);
        axl_info("reload reclaimed old image rc=0x%llx",
                 (unsigned long long)st);
        m_drv_reload_old = NULL;
    }
    if (m_drv_reload_evt != NULL) {
        axl_backend_event_close(m_drv_reload_evt);
        m_drv_reload_evt = NULL;
    }
    return AXL_SOURCE_REMOVE;
}

/* AXL_SERVICE_DEFAULT_TICK_MS is published in <axl/axl-service.h>
 * — same value, single source of truth shared with consumers. */

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
     * Both detach and teardown rcs OR into _rc — a teardown failure
     * surfaces to gBS->UnloadImage as EFI_ABORTED rather than getting
     * absorbed into a "successful" unload that left consumer state
     * dangling. */
    int rc = AXL_OK;
    if (m_drv_loop != NULL && m_drv_svc != NULL) {
        /* A self-reload already ran detach + teardown itself (it had to release
           the ports before the replacement bound them). Don't do it twice —
           just free the loop it could not free from inside its own callback. */
        if (!m_drv_reload_torn_down) {
            int detach_rc   = axl_service_detach_driver(m_drv_loop, m_drv_svc);
            int teardown_rc = axl_service_teardown(m_drv_svc);
            rc = (detach_rc != AXL_OK) ? detach_rc : teardown_rc;
        }
        axl_loop_free(m_drv_loop);
        m_drv_loop = NULL;
    }
    m_drv_reload_torn_down = false;
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
    return (rc == 0) ? EFI_SUCCESS : EFI_ABORTED;
}

AxlEfiStatus
_axl_service_driver_init(
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
    if (axl_service_guid(svc, &svc_guid) != AXL_OK) {
        m_drv_svc = NULL;
        return AXL_EFI_INVALID_PARAMETER;
    }

    /* Decode LoadOptions if present AND an AXL magic header matches.
     * SERVICE_LO_MAGIC / SERVICE_LO_MAGIC_RELOAD distinguish payloads that
     * axl_service_start_embedded / axl_service_reload produced from UCS-2
     * LoadOptions a shell user might pass — the latter would decode as garbage.
     * On a mismatch (or no LoadOptions at all), defaults from the descriptor
     * table apply. The self-reload handoff is extracted here regardless of
     * whether the service has options. */
    const void *lo_buf   = NULL;
    size_t      lo_size  = 0;
    const char *cfg_body = NULL;
    if (axl_driver_get_load_options_raw(&lo_buf, &lo_size) == AXL_OK
        && lo_buf != NULL && lo_size >= SERVICE_LO_MAGIC_LEN) {
        if (lo_size >= SERVICE_RELOAD_HDR_LEN
            && axl_memcmp(lo_buf, SERVICE_LO_MAGIC_RELOAD,
                          SERVICE_LO_MAGIC_LEN) == 0) {
            /* Reload handoff: [magic][old-image-handle][event][config C-string].
               Copy the two ptr-sized handles out (aligned via memcpy). The
               config body is only present when there are bytes past the fixed
               header — a strict '>' (like the normal-magic branch below) so a
               buffer of exactly SERVICE_RELOAD_HDR_LEN can't hand
               axl_config_from_string a pointer one past the end with no NUL. */
            const uint8_t *p = (const uint8_t *)lo_buf + SERVICE_LO_MAGIC_LEN;
            axl_memcpy(&m_drv_reload_old, p, sizeof(void *));
            axl_memcpy(&m_drv_reload_evt, p + sizeof(void *), sizeof(void *));
            if (lo_size > SERVICE_RELOAD_HDR_LEN) {
                cfg_body = (const char *)lo_buf + SERVICE_RELOAD_HDR_LEN;
            }
        } else if (lo_size > SERVICE_LO_MAGIC_LEN
                   && axl_memcmp(lo_buf, SERVICE_LO_MAGIC,
                                 SERVICE_LO_MAGIC_LEN) == 0) {
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
    if (axl_protocol_install(&svc_guid,
                             (void *)svc,
                             &m_drv_handle) != AXL_OK) {
        axl_warning("AXL_SERVICE_DRIVER: protocol_install failed");
        if (m_drv_cfg != NULL) {
            axl_config_free(m_drv_cfg); m_drv_cfg = NULL;
        }
        m_drv_svc = NULL;
        return AXL_EFI_ABORTED;
    }

    m_drv_loop = axl_loop_new();
    if (m_drv_loop == NULL) {
        axl_protocol_uninstall(m_drv_handle, &svc_guid,
                                           (void *)svc);
        m_drv_handle = NULL;
        if (m_drv_cfg != NULL) {
            axl_config_free(m_drv_cfg); m_drv_cfg = NULL;
        }
        m_drv_svc = NULL;
        return AXL_EFI_OUT_OF_RESOURCES;
    }

    if (axl_service_attach_driver(m_drv_loop, svc) != AXL_OK) {
        axl_loop_free(m_drv_loop);
        m_drv_loop = NULL;
        axl_protocol_uninstall(m_drv_handle, &svc_guid,
                                           (void *)svc);
        m_drv_handle = NULL;
        if (m_drv_cfg != NULL) {
            axl_config_free(m_drv_cfg); m_drv_cfg = NULL;
        }
        m_drv_svc = NULL;
        return AXL_EFI_ABORTED;
    }

    /* Self-reload handoff (replacement side): now that we are up and serving on
       the reused ports, arm the old image's signal event on OUR loop. When it
       fires (the old image has detached and gone off-stack) we reclaim it. */
    if (m_drv_reload_old != NULL && m_drv_reload_evt != NULL) {
        if (axl_loop_add_event(m_drv_loop, m_drv_reload_evt,
                               service_reload_unload_old, NULL) == 0) {
            /* Couldn't watch the handoff event (loop source table exhausted).
               We must NOT reclaim the old image from here: this whole
               DriverEntry runs synchronously inside the old image's
               axl_driver_start() call, so the old image is still live on the
               stack (it detaches + signals only AFTER start returns).
               UnloadImage(old) now would free an image mid-execution (#GP on
               return) and double-free the loop it is still using. Leave the
               old image resident instead — it detaches and goes idle once it
               signals, so a leaked-but-quiescent image is strictly safer than
               a use-after-free. */
            axl_warning("AXL_SERVICE_DRIVER: reload handoff add_event failed; "
                        "old image left resident (leaked), not reclaimed");
        }
    }
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
        axl_warning("service '%s': embedded_only and driver_path are mutually "
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
        axl_warning("service: start_embedded - descriptor missing name");
        return AXL_ERR;
    }

    /* Build the LoadOptions payload directly from the foreground app's
       option struct (no intermediate AxlConfig — the consumer may have
       populated user via axl_args_get_*, never touching AxlConfig).
       Layout: SERVICE_LO_MAGIC (8 bytes including trailing NUL) +
       URL-encoded query string (NUL-terminated). The magic gives the
       driver-side macro a way to distinguish "AXL serialized this"
       from "shell launched me with a UCS-2 string." */
    char load_opts[SERVICE_LOAD_OPTIONS_MAX];
    size_t load_opts_len = 0;

    /* Always emit the magic header even when there are no options to
       serialize — the driver image's discriminator check still needs
       it. An empty payload after the magic is a legal "use all
       defaults" signal. */
    axl_memcpy(load_opts, SERVICE_LO_MAGIC, SERVICE_LO_MAGIC_LEN);
    char  *body_buf  = load_opts + SERVICE_LO_MAGIC_LEN;
    size_t body_size = sizeof(load_opts) - SERVICE_LO_MAGIC_LEN;
    body_buf[0] = '\0';

    if (svc->opts_descs != NULL && svc->user != NULL) {
        if (axl_config_target_to_string(svc->opts_descs, svc->user,
                                        body_buf, body_size) != AXL_OK) {
            axl_warning("service '%s': options too large for "
                        "%d-byte LoadOptions buffer",
                        svc->name != NULL ? svc->name : "(unnamed)",
                        SERVICE_LOAD_OPTIONS_MAX);
            return AXL_ERR;
        }
    }
    /* Magic header + body + body's trailing NUL. */
    load_opts_len = SERVICE_LO_MAGIC_LEN + axl_strlen(body_buf) + 1;

    /* Pinned: exactly this file, no 4-path search and no embedded blob, so
       a stale copy of driver_name elsewhere on the box cannot shadow the
       image the launcher staged. */
    if (deploy->driver_path != NULL) {
        return axl_driver_ensure_from_path(
            &svc_guid,
            deploy->driver_path,
            load_opts_len > 0 ? load_opts : NULL,
            load_opts_len);
    }

    /* Embedded-only: load the baked-in blob directly, skipping the disk search
       so a stale loose <driver_name> beside the launcher cannot shadow it. */
    if (deploy->embedded_only) {
        return axl_driver_ensure_embedded_only(
            &svc_guid,
            deploy->driver_blob,
            deploy->driver_blob_len,
            deploy->driver_name,
            load_opts_len > 0 ? load_opts : NULL,
            load_opts_len);
    }

    return axl_driver_ensure_with_embedded(
        &svc_guid,
        deploy->driver_name,
        deploy->driver_blob,
        deploy->driver_blob_len,
        NULL,                     /* no override_name */
        load_opts_len > 0 ? load_opts : NULL,
        load_opts_len);
}

int
axl_service_stop(const AxlServiceDeploy *deploy)
{
    if (deploy == NULL || deploy->service == NULL) {
        return AXL_ERR;
    }
    AxlGuid svc_guid;
    if (axl_service_guid(deploy->service, &svc_guid) != AXL_OK) {
        axl_warning("service: stop - descriptor missing name");
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
        axl_warning("service '%s': enumerate_guid failed", nm);
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
            /* axl_driver_unload already logged the raw EFI_STATUS plus
               the held-protocol hint where applicable; here we just
               add the service name as context. Don't restate the cause
               — duplicate framing makes both lines harder to scan. */
            axl_warning("service '%s': stop failed for image handle %p "
                        "(see preceding axl_driver_unload warning for "
                        "the EFI_STATUS)",
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

static int
service_main_start(AxlArgs *a)
{
    const AxlServiceDeploy *deploy = m_main_deploy;
    service_main_apply_args(deploy->service, a);

    if (axl_service_is_running(deploy)) {
        axl_printf("%s: already running\n", deploy->service->name);
        return 0;
    }
    int rc = axl_service_start_embedded(deploy);
    if (rc != AXL_OK) {
        axl_printf("%s: start failed (rc=%d)\n", deploy->service->name, rc);
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
    (void)a;
    const AxlServiceDeploy *deploy = m_main_deploy;
    if (!axl_service_is_running(deploy)) {
        axl_printf("%s: not running\n", deploy->service->name);
        return 0;
    }
    int rc = axl_service_stop(deploy);
    if (rc != AXL_OK) {
        axl_printf("%s: stop failed (rc=%d)\n", deploy->service->name, rc);
        return 1;
    }
    axl_printf("%s: stopped\n", deploy->service->name);
    return 0;
}

static int
service_main_status(AxlArgs *a)
{
    (void)a;
    const AxlServiceDeploy *deploy = m_main_deploy;
    bool running = axl_service_is_running(deploy);
    axl_printf("%s: %s\n", deploy->service->name,
               running ? "running" : "stopped");
    return running ? 0 : 1;
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
    AxlArgDesc *flags = axl_malloc((n_opts + 2) * sizeof(AxlArgDesc));
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
    flags[n_opts + 1] = (AxlArgDesc){0};

    const AxlArgsNode verbs[] = {
        { .name = "start",  .help = "Start the service driver",
          .flags = flags, .handler = service_main_start },
        { .name = "stop",   .help = "Stop a running service driver",
          .handler = service_main_stop },
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
// Self-reload (in-place upgrade). Called from inside a running
// AXL_SERVICE_DRIVER service to hot-swap to a new image with a port hand-off,
// then be reclaimed by the replacement. See axl-service.h.
// ---------------------------------------------------------------------------

/* How many handles currently publish @p guid? Returns false (and leaves
   @p out_count untouched) when the enumeration itself failed — the caller
   must NOT read a failed enumeration as "nobody publishes it". */
static bool
service_count_publishers(const AxlGuid *guid, size_t *out_count)
{
    void   **handles = NULL;
    size_t   count   = 0;
    if (axl_protocol_enumerate_guid(guid, &handles, &count) != AXL_OK) {
        return false;
    }
    axl_free(handles);
    *out_count = count;
    return true;
}

/* Common tail for "the replacement did not come up." Our teardown has already
   run (the ports had to be free before the replacement could bind them), so
   this service is down either way: stop our timer so we do not keep
   dispatching over torn-down state, flag torn_down so a later unload
   (watchdog / axl_service_stop) frees the loop without running teardown a
   second time, and drop the handoff event nobody will ever signal. */
static AxlStatus
service_reload_start_failed(const AxlService *svc, AxlEventHandle evt)
{
    axl_service_detach_driver(m_drv_loop, svc);
    m_drv_reload_torn_down = true;
    axl_backend_event_close(evt);
    return AXL_ERR;
}

static AxlStatus
service_reload_impl(const AxlService *svc, const char *path,
                    const void *buf, size_t buf_len)
{
    /* Must be the running AXL_SERVICE_DRIVER-hosted service. Caller misuse —
       nothing has been touched, so this is NOT the AXL_ERR "service is down"
       code. */
    if (svc == NULL || svc != m_drv_svc || m_drv_loop == NULL) {
        return AXL_INVALID;
    }

    /* Serialize the current config (empty if the service has no options). */
    char cfg_str[SERVICE_LOAD_OPTIONS_MAX];
    cfg_str[0] = '\0';
    if (m_drv_cfg != NULL
        && axl_config_to_string(m_drv_cfg, cfg_str, sizeof(cfg_str)) != AXL_OK) {
        return AXL_NO_RESOURCES;
    }

    /* Handoff event OLD signals once it is idle. */
    AxlEventHandle evt = NULL;
    if (axl_backend_event_create(&evt) != AXL_OK) {
        return AXL_NO_RESOURCES;
    }

    /* Build reload LoadOptions: [magic][old-handle][event][config C-string]. */
    uint8_t lo[SERVICE_LOAD_OPTIONS_MAX];
    size_t  cfg_len = axl_strlen(cfg_str) + 1;          /* include the NUL */
    size_t  lo_len  = SERVICE_RELOAD_HDR_LEN + cfg_len;
    if (lo_len > sizeof(lo)) {
        axl_backend_event_close(evt);
        return AXL_NO_RESOURCES;
    }
    void *old_h = (void *)m_drv_handle;
    axl_memcpy(lo, SERVICE_LO_MAGIC_RELOAD, SERVICE_LO_MAGIC_LEN);
    axl_memcpy(lo + SERVICE_LO_MAGIC_LEN, &old_h, sizeof(void *));
    axl_memcpy(lo + SERVICE_LO_MAGIC_LEN + sizeof(void *), &evt, sizeof(void *));
    axl_memcpy(lo + SERVICE_RELOAD_HDR_LEN, cfg_str, cfg_len);

    /* Load the replacement first (validate before releasing our ports). A load
       failure is recoverable — nothing torn down, this service keeps serving —
       so it reports AXL_NOT_FOUND, never the AXL_ERR that means "down". */
    AxlDriverHandle h = NULL;
    int lrc = (buf != NULL)
              ? axl_driver_load_buffer((const unsigned char *)buf, buf_len, &h)
              : axl_driver_load(path, &h);
    if (lrc != AXL_OK) {
        axl_backend_event_close(evt);
        return AXL_NOT_FOUND;
    }
    if (axl_driver_set_load_options(h, lo, lo_len) != AXL_OK) {
        axl_driver_unload(h);
        axl_backend_event_close(evt);
        return AXL_NO_RESOURCES;
    }

    /* Release our ports NOW so the replacement can bind them. The unload stub
       the replacement triggers won't run teardown again (m_drv_reload_torn_down
       below), so teardown runs exactly once here. */
    axl_service_teardown(svc);

    /* Belt for the start check below. axl_driver_ensure_with_embedded verifies
       a freshly-started driver with a plain LocateProtocol — that would be
       useless here, because THIS image still publishes the service GUID until
       its unload stub runs. Count the publishers instead: only an increase
       proves the replacement published its own identity. A failed enumeration
       disarms the check rather than failing the reload — a false "start
       failed" would leave the caller resetting a box whose replacement is
       actually up.

       Snapshot AFTER teardown, immediately before the start, so the count
       reflects the world the replacement is about to join. The framework
       never uninstalls the service GUID during teardown, but a consumer
       teardown that did would otherwise leave this stale-high and turn a
       genuine success into a false start failure — the exact outcome the
       disarm bias above exists to avoid. */
    AxlGuid svc_guid;
    size_t  pub_before  = 0;
    bool    pub_counted = axl_service_guid(svc, &svc_guid) == AXL_OK
                          && service_count_publishers(&svc_guid, &pub_before);

    /* Start the replacement: its DriverEntry rebinds the ports, comes up
       resident, and arms our handoff event on its own loop. A start failure
       here leaves us down (ports already released via teardown above) — the
       caller must treat it as fatal. */
    if (axl_driver_start(h) != AXL_OK) {
        return service_reload_start_failed(svc, evt);
    }

    /* Started, but did it actually attach? A DriverEntry that returns success
       without publishing the service protocol (a foreign image, or an AXL
       image whose entry status was mangled on the way out) would otherwise be
       reported as a healthy hot-swap of a service that is not there. */
    size_t pub_after = 0;
    if (pub_counted
        && service_count_publishers(&svc_guid, &pub_after)
        && pub_after <= pub_before)
    {
        axl_warning("service '%s': replacement started but did not publish the "
                    "service protocol - treating as a start failure",
                    svc->name != NULL ? svc->name : "(unnamed)");
        axl_driver_unload(h);
        return service_reload_start_failed(svc, evt);
    }

    /* Quiesce: stop our timer, flag "already torn down" so the replacement's
       UnloadImage-triggered unload stub only frees the loop, then signal. The
       replacement reclaims us from its tick once this callback has returned and
       we are off-stack. */
    axl_service_detach_driver(m_drv_loop, svc);
    m_drv_reload_torn_down = true;
    axl_backend_event_signal(evt);
    return AXL_OK;
}

AxlStatus
axl_service_reload(const AxlService *svc, const char *new_path)
{
    if (new_path == NULL) {
        return AXL_INVALID;
    }
    return service_reload_impl(svc, new_path, NULL, 0);
}

AxlStatus
axl_service_reload_buffer(const AxlService *svc, const void *image, size_t image_len)
{
    if (image == NULL || image_len == 0) {
        return AXL_INVALID;
    }
    return service_reload_impl(svc, NULL, image, image_len);
}
