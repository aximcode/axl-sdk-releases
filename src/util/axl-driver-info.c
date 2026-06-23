/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-driver-info.c
    Read-only driver / device discovery (the shell `drivers` / `devices` /
    bound-driver views). See axl-driver-info.h.

    Built over the firmware driver model: EFI_DRIVER_BINDING_PROTOCOL
    handles for the loaded-driver list, EFI_COMPONENT_NAME2_PROTOCOL for
    names, and OpenProtocolInformation for who-manages-whom.
**/

#include "../backend/axl-backend.h"
#include <axl/axl-driver-info.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-log.h>
#include <uefi/axl-uefi.h>

AXL_LOG_DOMAIN("driverinfo");

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/* Snapshot every handle in the system (LocateHandleBuffer AllHandles).
   Caller frees *handles via FreePool. Returns AXL_OK / AXL_ERR. */
static int
all_handles(EFI_HANDLE **handles, UINTN *count)
{
    *handles = NULL;
    *count   = 0;
    EFI_STATUS st = axl_bs()->LocateHandleBuffer(AllHandles, NULL, NULL,
                                                 count, handles);
    return (EFI_ERROR(st) || *handles == NULL) ? AXL_ERR : AXL_OK;
}

/* Read the English ComponentName2 driver name from a handle into a UTF-8
   buffer. Returns AXL_OK if a name was written, AXL_ERR otherwise. */
static int
read_component_name(EFI_HANDLE h, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return AXL_ERR;
    }
    out[0] = '\0';

    EFI_COMPONENT_NAME2_PROTOCOL *cn = NULL;
    EFI_GUID cn_guid = gEfiComponentName2ProtocolGuid;
    if (EFI_ERROR(axl_bs()->HandleProtocol(h, &cn_guid, (void **)&cn))
        || cn == NULL || cn->GetDriverName == NULL) {
        return AXL_ERR;
    }

    CHAR16 *name = NULL;
    /* "en" is the ISO 639-2 "en"/"eng" RFC 4646 language; try "en" then
       the driver's first supported language. */
    if (EFI_ERROR(cn->GetDriverName(cn, (CHAR8 *)"en", &name))
        || name == NULL) {
        char *langs = cn->SupportedLanguages;
        if (langs == NULL
            || EFI_ERROR(cn->GetDriverName(cn, (CHAR8 *)langs, &name))
            || name == NULL) {
            return AXL_ERR;
        }
    }
    axl_ucs2_to_utf8_buf((const unsigned short *)name, out, cap);
    return (out[0] != '\0') ? AXL_OK : AXL_ERR;
}

/* Does @p controller expose a network protocol (SNP / NII / MNP / IP4)?
   That makes the driver managing it a network driver. */
static bool
handle_is_network(EFI_HANDLE controller)
{
    static const EFI_GUID *net_guids[] = {
        &EFI_SIMPLE_NETWORK_PROTOCOL_GUID,
        &EFI_IP4_PROTOCOL_GUID,
        &EFI_IP4_CONFIG2_PROTOCOL_GUID,
    };
    for (size_t i = 0; i < sizeof(net_guids) / sizeof(net_guids[0]); i++) {
        void    *iface = NULL;
        EFI_GUID g     = *net_guids[i];
        if (!EFI_ERROR(axl_bs()->HandleProtocol(controller, &g, &iface))) {
            return true;
        }
    }
    /* NII (3.1 + legacy) live in the hand-written extra header. */
    void    *iface = NULL;
    EFI_GUID nii   = gEfiNetworkInterfaceIdentifierProtocolGuid_31;
    if (!EFI_ERROR(axl_bs()->HandleProtocol(controller, &nii, &iface))) {
        return true;
    }
    return false;
}

/* Is @p controller managed by driver-binding handle @p driver — i.e. does
   @p driver hold any protocol on @p controller BY_DRIVER? */
static bool
controller_managed_by(EFI_HANDLE controller, EFI_HANDLE driver)
{
    EFI_GUID **protos = NULL;
    UINTN      nproto = 0;
    if (EFI_ERROR(axl_bs()->ProtocolsPerHandle(controller, &protos, &nproto))
        || protos == NULL) {
        return false;
    }
    bool managed = false;
    for (UINTN p = 0; p < nproto && !managed; p++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *info = NULL;
        UINTN n = 0;
        if (EFI_ERROR(axl_bs()->OpenProtocolInformation(controller, protos[p],
                                                        &info, &n))
            || info == NULL) {
            continue;
        }
        for (UINTN e = 0; e < n; e++) {
            if (info[e].AgentHandle == driver
                && (info[e].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER)) {
                managed = true;
                break;
            }
        }
        axl_bs()->FreePool(info);
    }
    axl_bs()->FreePool(protos);
    return managed;
}

/* Count the controllers @p driver manages, and set *any_net if any managed
   controller is a network controller. */
static uint32_t
count_managed(EFI_HANDLE driver, EFI_HANDLE *handles, UINTN nhandles,
              bool *any_net)
{
    uint32_t count = 0;
    *any_net = false;
    for (UINTN i = 0; i < nhandles; i++) {
        if (controller_managed_by(handles[i], driver)) {
            count++;
            if (handle_is_network(handles[i])) {
                *any_net = true;
            }
        }
    }
    return count;
}

/* Safe, side-effect-free network-name heuristic for idle drivers. */
static bool
name_looks_network(const char *name)
{
    static const char *kw[] = {
        "Network", "network", "Ethernet", "UNDI", "SNP", "NIC", "LAN",
        "VLAN", "TCP", "DHCP", "iSCSI", "PXE", "MNP", "ARP", "DNS",
        "E1000", "e1000", "Virtio Net", "VirtioNet", "Wi-Fi", "WiFi",
    };
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        if (axl_strstr(name, kw[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/* The driver-binding handle managing @p controller (any BY_DRIVER agent),
   or NULL if unmanaged. (Defined here; also used by the P2 query below.) */
static EFI_HANDLE controller_driver(EFI_HANDLE controller);

/* Collect controller handles that are network-CLASS PCI functions (base class
   0x02) and currently UNBOUND — the genuine "idle NIC" case. A truly unbound
   NIC exposes only PciIo (SNP/IP4 are produced by a driver, so a protocol-based
   test would miss it); detect by PCI class via GetLocation + the class code.
   Writes up to @p cap handles, returns the count. */
static UINTN
collect_unbound_nics(EFI_HANDLE *out, UINTN cap)
{
    EFI_GUID    pio_guid = gEfiPciIoProtocolGuid;
    UINTN       nh       = 0;
    EFI_HANDLE *hs       = NULL;
    if (EFI_ERROR(axl_bs()->LocateHandleBuffer(ByProtocol, &pio_guid, NULL,
                                               &nh, &hs))
        || hs == NULL) {
        return 0;
    }
    UINTN found = 0;
    for (UINTN i = 0; i < nh && found < cap; i++) {
        EFI_PCI_IO_PROTOCOL *pio = NULL;
        if (EFI_ERROR(axl_bs()->HandleProtocol(hs[i], &pio_guid, (void **)&pio))
            || pio == NULL || pio->GetLocation == NULL) {
            continue;
        }
        UINTN seg = 0, bus = 0, dev = 0, fn = 0;
        if (EFI_ERROR(pio->GetLocation(pio, &seg, &bus, &dev, &fn))) {
            continue;
        }
        AxlPciAddr addr = { (uint16_t)seg, (uint8_t)bus, (uint8_t)dev,
                            (uint8_t)fn };
        uint32_t cls = 0;
        if (axl_pci_get_class_code(addr, &cls) != AXL_OK
            || (cls >> 16) != AXL_PCI_CLASS_NETWORK) {
            continue;
        }
        if (controller_driver(hs[i]) == NULL) {   /* unbound */
            out[found++] = hs[i];
        }
    }
    axl_bs()->FreePool(hs);
    return found;
}

/* For an idle (managing-nothing) driver: does it claim any unbound NIC? Probe
   its Supported() against the precomputed unbound-NIC list. This only does work
   when an idle driver AND an unbound NIC coexist; the auto-connecting OVMF/AAVMF
   harness binds NICs, so tests never reach the probe. */
static bool
idle_driver_supports_unbound_net(EFI_HANDLE driver,
                                 EFI_HANDLE *nics, UINTN nnics)
{
    if (nnics == 0) {
        return false;
    }
    EFI_DRIVER_BINDING_PROTOCOL *db = NULL;
    EFI_GUID db_guid = gEfiDriverBindingProtocolGuid;
    if (EFI_ERROR(axl_bs()->HandleProtocol(driver, &db_guid, (void **)&db))
        || db == NULL || db->Supported == NULL) {
        return false;
    }
    for (UINTN i = 0; i < nnics; i++) {
        if (!EFI_ERROR(db->Supported(db, nics[i], NULL))) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public — P1: the drivers view
// ---------------------------------------------------------------------------

int
axl_driver_list_loaded(AxlDriverInfo *out, size_t cap, size_t *count)
{
    if (count == NULL) {
        return AXL_ERR;
    }
    *count = 0;

    EFI_GUID    db_guid = gEfiDriverBindingProtocolGuid;
    UINTN       ndrv    = 0;
    EFI_HANDLE *drivers = NULL;
    EFI_STATUS  st = axl_bs()->LocateHandleBuffer(ByProtocol, &db_guid, NULL,
                                                  &ndrv, &drivers);
    if (EFI_ERROR(st) || drivers == NULL) {
        return (st == EFI_NOT_FOUND) ? AXL_OK : AXL_ERR;
    }

    EFI_HANDLE *handles  = NULL;
    UINTN       nhandles = 0;
    all_handles(&handles, &nhandles);   /* best-effort; NULL → 0 managed */

    /* Unbound network-class controllers, computed once for the idle-driver
       network probe (layer 3 of is_network). */
    EFI_HANDLE unbound_nics[32];
    UINTN      n_unbound = collect_unbound_nics(unbound_nics, 32);

    for (UINTN i = 0; i < ndrv; i++) {
        EFI_DRIVER_BINDING_PROTOCOL *db = NULL;
        if (EFI_ERROR(axl_bs()->HandleProtocol(drivers[i], &db_guid,
                                               (void **)&db))
            || db == NULL) {
            continue;
        }

        size_t idx = *count;
        (*count)++;
        if (out == NULL || idx >= cap) {
            continue;  /* count-only / past-capacity: keep counting */
        }

        AxlDriverInfo *e = &out[idx];
        axl_memset(e, 0, sizeof(*e));
        e->handle  = (AxlHandle)drivers[i];
        e->version = db->Version;
        read_component_name(drivers[i], e->name, sizeof(e->name));

        bool any_net = false;
        e->num_devices = (handles != NULL)
            ? count_managed(drivers[i], handles, nhandles, &any_net) : 0;

        /* Layered network classification: (1) authoritative — manages a
           network controller; (2) safe name heuristic; (3) idle driver that
           Supported()s an unbound NIC. */
        e->is_network = any_net
            || name_looks_network(e->name)
            || (e->num_devices == 0
                && idle_driver_supports_unbound_net(drivers[i], unbound_nics,
                                                    n_unbound));
    }

    if (handles != NULL) {
        axl_bs()->FreePool(handles);
    }
    axl_bs()->FreePool(drivers);
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Public — P1a: name a handle
// ---------------------------------------------------------------------------

int
axl_handle_name(AxlHandle h, char *out, size_t cap)
{
    if (h == NULL || out == NULL || cap == 0) {
        return AXL_ERR;
    }
    out[0] = '\0';

    /* A driver handle: its ComponentName2 name. */
    if (read_component_name((EFI_HANDLE)h, out, cap) == AXL_OK) {
        return AXL_OK;
    }

    /* Otherwise the device-path text. */
    EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
    EFI_GUID dpg = EFI_DEVICE_PATH_PROTOCOL_GUID;
    if (!EFI_ERROR(axl_bs()->HandleProtocol((EFI_HANDLE)h, &dpg, (void **)&dp))
        && dp != NULL) {
        char *text = axl_device_path_to_text(dp);
        if (text != NULL) {
            axl_snprintf(out, cap, "%s", text);
            axl_free(text);
            return (out[0] != '\0') ? AXL_OK : AXL_ERR;
        }
    }
    return AXL_ERR;
}

// ---------------------------------------------------------------------------
// Public — P2 / P5: PCI address <-> controller handle, bound-driver query
// ---------------------------------------------------------------------------

/* The driver-binding handle managing @p controller (any BY_DRIVER agent),
   or NULL if unmanaged. */
static EFI_HANDLE
controller_driver(EFI_HANDLE controller)
{
    EFI_GUID **protos = NULL;
    UINTN      nproto = 0;
    if (EFI_ERROR(axl_bs()->ProtocolsPerHandle(controller, &protos, &nproto))
        || protos == NULL) {
        return NULL;
    }
    EFI_HANDLE drv = NULL;
    for (UINTN p = 0; p < nproto && drv == NULL; p++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *info = NULL;
        UINTN n = 0;
        if (EFI_ERROR(axl_bs()->OpenProtocolInformation(controller, protos[p],
                                                        &info, &n))
            || info == NULL) {
            continue;
        }
        for (UINTN e = 0; e < n; e++) {
            if (info[e].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) {
                drv = info[e].AgentHandle;
                break;
            }
        }
        axl_bs()->FreePool(info);
    }
    axl_bs()->FreePool(protos);
    return drv;
}

int
axl_pci_to_handle(AxlPciAddr addr, AxlHandle *out)
{
    if (out == NULL) {
        return AXL_ERR;
    }
    *out = NULL;

    EFI_GUID    pio_guid = gEfiPciIoProtocolGuid;
    UINTN       nh       = 0;
    EFI_HANDLE *hs       = NULL;
    if (EFI_ERROR(axl_bs()->LocateHandleBuffer(ByProtocol, &pio_guid, NULL,
                                               &nh, &hs))
        || hs == NULL) {
        return AXL_ERR;
    }

    int rc = AXL_ERR;
    for (UINTN i = 0; i < nh; i++) {
        EFI_PCI_IO_PROTOCOL *pio = NULL;
        if (EFI_ERROR(axl_bs()->HandleProtocol(hs[i], &pio_guid, (void **)&pio))
            || pio == NULL || pio->GetLocation == NULL) {
            continue;
        }
        UINTN seg = 0, bus = 0, dev = 0, fn = 0;
        if (EFI_ERROR(pio->GetLocation(pio, &seg, &bus, &dev, &fn))) {
            continue;
        }
        if (seg == addr.seg && bus == addr.bus
            && dev == addr.dev && fn == addr.func) {
            *out = (AxlHandle)hs[i];
            rc = AXL_OK;
            break;
        }
    }
    axl_bs()->FreePool(hs);
    return rc;
}

int
axl_pci_driver_bound(AxlPciAddr addr, bool *bound, char *drv_name, size_t drv_cap)
{
    if (bound == NULL) {
        return AXL_ERR;
    }
    *bound = false;
    if (drv_name != NULL && drv_cap > 0) {
        drv_name[0] = '\0';
    }

    AxlHandle ctrl = NULL;
    if (axl_pci_to_handle(addr, &ctrl) != AXL_OK || ctrl == NULL) {
        return AXL_ERR;   /* no PCI_IO controller handle for this address */
    }

    EFI_HANDLE drv = controller_driver((EFI_HANDLE)ctrl);
    if (drv != NULL) {
        *bound = true;
        if (drv_name != NULL && drv_cap > 0) {
            read_component_name(drv, drv_name, drv_cap);
        }
    }
    return AXL_OK;
}

// ---------------------------------------------------------------------------
// Public — P3: bind a specific driver to a controller
// ---------------------------------------------------------------------------

int
axl_driver_bind(AxlHandle controller, AxlHandle driver)
{
    if (controller == NULL) {
        return AXL_ERR;
    }
    /* NULL-terminated single-driver list for the targeted case; NULL =
       "any applicable driver" (== axl_driver_connect_handle). Recursive=FALSE:
       bind this driver to this controller, not the whole subtree (follow with
       axl_net_connect_stack to bring an upper network stack up). */
    EFI_HANDLE list[2] = { (EFI_HANDLE)driver, NULL };
    EFI_STATUS st = axl_bs()->ConnectController(
        (EFI_HANDLE)controller,
        (driver != NULL) ? list : NULL,
        NULL,
        FALSE);

    /* ConnectController returns EFI_NOT_FOUND both when nothing NEW connected
       (the controller was already bound) and when no driver would bind — the
       return code alone can't tell "done" from "failed." Verify the outcome:
       success means the controller ends up managed (by the requested driver,
       if one was named). */
    if (driver != NULL) {
        return controller_managed_by((EFI_HANDLE)controller,
                                     (EFI_HANDLE)driver) ? AXL_OK : AXL_ERR;
    }
    if (!EFI_ERROR(st)) {
        return AXL_OK;
    }
    return (controller_driver((EFI_HANDLE)controller) != NULL)
               ? AXL_OK : AXL_ERR;
}

// ---------------------------------------------------------------------------
// Public — P1b/P1c: generic handle + protocol enumeration
// ---------------------------------------------------------------------------

int
axl_handle_list(const AxlGuid *protocol, AxlHandle *out, size_t cap,
                size_t *count)
{
    if (count == NULL) {
        return AXL_ERR;
    }
    *count = 0;

    EFI_HANDLE *hs = NULL;
    UINTN       nh = 0;
    EFI_STATUS  st;
    if (protocol == NULL) {
        st = axl_bs()->LocateHandleBuffer(AllHandles, NULL, NULL, &nh, &hs);
    } else {
        /* AxlGuid is layout-compatible with EFI_GUID; copy into a typed
           local so the firmware sees its own type. */
        EFI_GUID g;
        axl_memcpy(&g, protocol, sizeof(EFI_GUID));
        st = axl_bs()->LocateHandleBuffer(ByProtocol, &g, NULL, &nh, &hs);
    }
    if (EFI_ERROR(st)) {
        /* No handle matches the GUID (or an empty database) is success with
           count 0 — only a real firmware failure is AXL_ERR. */
        return (st == EFI_NOT_FOUND) ? AXL_OK : AXL_ERR;
    }
    if (hs == NULL) {
        return AXL_OK;
    }

    for (UINTN i = 0; i < nh && out != NULL && i < cap; i++) {
        out[i] = (AxlHandle)hs[i];
    }
    *count = (size_t)nh;
    axl_bs()->FreePool(hs);
    return AXL_OK;
}

int
axl_handle_protocols(AxlHandle h, AxlGuid *out, size_t cap, size_t *count)
{
    if (h == NULL || count == NULL) {
        return AXL_ERR;
    }
    *count = 0;

    EFI_GUID **protos = NULL;
    UINTN      nproto = 0;
    if (EFI_ERROR(axl_bs()->ProtocolsPerHandle((EFI_HANDLE)h, &protos, &nproto))
        || protos == NULL) {
        return AXL_ERR;
    }

    /* ProtocolsPerHandle never returns NULL GUID entries in its buffer, so
       every counted slot is written — no guard needed (and counting an entry
       we didn't write would break the truncation contract). */
    for (UINTN i = 0; i < nproto && out != NULL && i < cap; i++) {
        axl_memcpy(&out[i], protos[i], sizeof(AxlGuid));
    }
    *count = (size_t)nproto;
    axl_bs()->FreePool(protos);
    return AXL_OK;
}

int
axl_net_protocol_name(const AxlGuid *guid, char *out, size_t cap)
{
    if (guid == NULL || out == NULL || cap == 0) {
        return AXL_ERR;
    }
    out[0] = '\0';

    /* The networking stack, bottom to top, plus the IPv6 peers. The GUID
       symbols are file-scope statics from the generated headers, so their
       addresses are valid for this table. */
    static const struct {
        const EFI_GUID *guid;
        const char     *name;
    } net_names[] = {
        { &gEfiSimpleNetworkProtocolGuid,  "SimpleNetwork"  },
        { &gEfiManagedNetworkProtocolGuid, "ManagedNetwork" },
        { &gEfiArpProtocolGuid,            "Arp"            },
        { &gEfiIp4ProtocolGuid,            "Ip4"            },
        { &gEfiIp4Config2ProtocolGuid,     "Ip4Config2"     },
        { &gEfiDhcp4ProtocolGuid,          "Dhcp4"          },
        { &gEfiTcp4ProtocolGuid,           "Tcp4"           },
        { &gEfiUdp4ProtocolGuid,           "Udp4"           },
        { &gEfiDns4ProtocolGuid,           "Dns4"           },
        { &gEfiMtftp4ProtocolGuid,         "Mtftp4"         },
        { &gEfiIp6ProtocolGuid,            "Ip6"            },
        { &gEfiDhcp6ProtocolGuid,          "Dhcp6"          },
        { &gEfiTcp6ProtocolGuid,           "Tcp6"           },
        { &gEfiUdp6ProtocolGuid,           "Udp6"           },
        { &gEfiDns6ProtocolGuid,           "Dns6"           },
    };

    for (size_t i = 0; i < sizeof(net_names) / sizeof(net_names[0]); i++) {
        if (axl_guid_equal((const AxlGuid *)net_names[i].guid, guid)) {
            axl_strlcpy(out, net_names[i].name, cap);
            return AXL_OK;
        }
    }
    return AXL_ERR;
}

/* Collect the DISTINCT handles referenced by OpenProtocolInformation entries
   on @p controller whose Attributes match @p attr_mask — the AgentHandle when
   @p take_controller is false (managing drivers), the ControllerHandle when
   true (child controllers). Fills @p out up to @p cap and sets *count to the
   distinct total (the same truncation contract as axl_driver_list_loaded).
   Shared by axl_handle_drivers and axl_handle_children. */
static int
collect_open_info(EFI_HANDLE controller, uint32_t attr_mask,
                  bool take_controller, AxlHandle *out, size_t cap,
                  size_t *count)
{
    *count = 0;

    EFI_GUID **protos = NULL;
    UINTN      nproto = 0;
    /* ProtocolsPerHandle has no empty case — a valid handle always yields at
       least one protocol — so failure is a genuine error (bad handle / OOM),
       not "0 drivers/children". The count-0 result comes from the loop below
       finding no matching OpenProtocolInformation entries. Match the AXL_ERR
       contract (and axl_handle_protocols, which wraps the same primitive). */
    if (EFI_ERROR(axl_bs()->ProtocolsPerHandle(controller, &protos, &nproto))
        || protos == NULL) {
        return AXL_ERR;
    }

    /* Dedup set. A controller realistically has only a handful of distinct
       managing drivers / child controllers; 256 is far above any real
       topology. If it were ever exceeded we stop tracking new distinct
       handles for dedup (count could then drift) — never hit in practice. */
    AxlHandle seen[256];
    size_t    nseen    = 0;
    size_t    distinct = 0;

    for (UINTN p = 0; p < nproto; p++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *info = NULL;
        UINTN n = 0;
        if (EFI_ERROR(axl_bs()->OpenProtocolInformation(controller, protos[p],
                                                        &info, &n))
            || info == NULL) {
            continue;
        }
        for (UINTN e = 0; e < n; e++) {
            if (!(info[e].Attributes & attr_mask)) {
                continue;
            }
            EFI_HANDLE cand = take_controller ? info[e].ControllerHandle
                                              : info[e].AgentHandle;
            if (cand == NULL) {
                continue;
            }
            bool dup = false;
            for (size_t s = 0; s < nseen; s++) {
                if (seen[s] == (AxlHandle)cand) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            if (nseen < sizeof(seen) / sizeof(seen[0])) {
                seen[nseen++] = (AxlHandle)cand;
            }
            if (out != NULL && distinct < cap) {
                out[distinct] = (AxlHandle)cand;
            }
            distinct++;
        }
        axl_bs()->FreePool(info);
    }
    axl_bs()->FreePool(protos);
    *count = distinct;
    return AXL_OK;
}

int
axl_handle_drivers(AxlHandle controller, AxlHandle *drivers, size_t cap,
                   size_t *count)
{
    if (controller == NULL || count == NULL) {
        return AXL_ERR;
    }
    return collect_open_info((EFI_HANDLE)controller, EFI_OPEN_PROTOCOL_BY_DRIVER,
                             false, drivers, cap, count);
}

int
axl_handle_children(AxlHandle controller, AxlHandle *out, size_t cap,
                    size_t *count)
{
    if (controller == NULL || count == NULL) {
        return AXL_ERR;
    }
    return collect_open_info((EFI_HANDLE)controller,
                             EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER,
                             true, out, cap, count);
}

int
axl_protocol_guid_name(const AxlGuid *guid, char *out, size_t cap)
{
    if (guid == NULL || out == NULL || cap == 0) {
        return AXL_ERR;
    }

    /* The networking stack lives in axl_net_protocol_name; consult it first so
       there is a single source of truth for the net names. */
    if (axl_net_protocol_name(guid, out, cap) == AXL_OK) {
        return AXL_OK;
    }
    out[0] = '\0';

    /* The common device / driver / bus / console protocols a Devices view
       shows. Same file-scope-static GUID symbols as elsewhere. */
    static const struct {
        const EFI_GUID *guid;
        const char     *name;
    } proto_names[] = {
        { &gEfiDevicePathProtocolGuid,              "DevicePath"            },
        { &gEfiDevicePathToTextProtocolGuid,        "DevicePathToText"      },
        { &gEfiLoadedImageProtocolGuid,             "LoadedImage"           },
        { &gEfiLoadedImageDevicePathProtocolGuid,   "LoadedImageDevicePath" },
        { &gEfiDriverBindingProtocolGuid,           "DriverBinding"         },
        { &gEfiComponentName2ProtocolGuid,          "ComponentName2"        },
        { &gEfiSimpleFileSystemProtocolGuid,        "SimpleFileSystem"      },
        { &gEfiBlockIoProtocolGuid,                 "BlockIo"               },
        { &gEfiBlockIo2ProtocolGuid,                "BlockIo2"              },
        { &gEfiDiskIoProtocolGuid,                  "DiskIo"                },
        { &gEfiPciIoProtocolGuid,                   "PciIo"                 },
        { &gEfiPciRootBridgeIoProtocolGuid,         "PciRootBridgeIo"       },
        { &gEfiGraphicsOutputProtocolGuid,          "GraphicsOutput"        },
        { &gEfiSimpleTextInputExProtocolGuid,       "SimpleTextInputEx"     },
        { &gEfiSerialIoProtocolGuid,                "SerialIo"              },
        { &gEfiSimplePointerProtocolGuid,           "SimplePointer"         },
        { &gEfiAbsolutePointerProtocolGuid,         "AbsolutePointer"       },
        { &gEfiUsbIoProtocolGuid,                   "UsbIo"                 },
        { &gEfiUsb2HcProtocolGuid,                  "Usb2Hc"                },
        { &gEfiNvmExpressPassThruProtocolGuid,      "NvmExpressPassThru"    },
        { &gEfiAtaPassThruProtocolGuid,             "AtaPassThru"           },
        { &gEfiNetworkInterfaceIdentifierProtocolGuid_31, "Nii"            },
    };

    for (size_t i = 0; i < sizeof(proto_names) / sizeof(proto_names[0]); i++) {
        if (axl_guid_equal((const AxlGuid *)proto_names[i].guid, guid)) {
            axl_strlcpy(out, proto_names[i].name, cap);
            return AXL_OK;
        }
    }
    return AXL_ERR;
}

/* Does @p parent produce @p child — i.e. did @p child open one of @p parent's
   protocols BY_CHILD_CONTROLLER? The parent-side view of the child relation. */
static bool
handle_produces_child(EFI_HANDLE parent, EFI_HANDLE child)
{
    EFI_GUID **protos = NULL;
    UINTN      nproto = 0;
    if (EFI_ERROR(axl_bs()->ProtocolsPerHandle(parent, &protos, &nproto))
        || protos == NULL) {
        return false;
    }
    bool found = false;
    for (UINTN p = 0; p < nproto && !found; p++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *info = NULL;
        UINTN n = 0;
        if (EFI_ERROR(axl_bs()->OpenProtocolInformation(parent, protos[p],
                                                        &info, &n))
            || info == NULL) {
            continue;
        }
        for (UINTN e = 0; e < n; e++) {
            if ((info[e].Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER)
                && info[e].ControllerHandle == child) {
                found = true;
                break;
            }
        }
        axl_bs()->FreePool(info);
    }
    axl_bs()->FreePool(protos);
    return found;
}

int
axl_handle_parents(AxlHandle controller, AxlHandle *out, size_t cap,
                   size_t *count)
{
    if (controller == NULL || count == NULL) {
        return AXL_ERR;
    }
    *count = 0;

    EFI_HANDLE *hs = NULL;
    UINTN       nh = 0;
    if (EFI_ERROR(axl_bs()->LocateHandleBuffer(AllHandles, NULL, NULL, &nh, &hs))
        || hs == NULL) {
        return AXL_ERR;
    }

    /* Scan every handle; a candidate is a parent if it produced @p controller
       as a BY_CHILD_CONTROLLER child. Handles in the buffer are distinct, so
       no dedup is needed. */
    size_t distinct = 0;
    for (UINTN i = 0; i < nh; i++) {
        if ((AxlHandle)hs[i] == controller) {
            continue;
        }
        if (!handle_produces_child(hs[i], (EFI_HANDLE)controller)) {
            continue;
        }
        if (out != NULL && distinct < cap) {
            out[distinct] = (AxlHandle)hs[i];
        }
        distinct++;
    }
    axl_bs()->FreePool(hs);
    *count = distinct;
    return AXL_OK;
}
