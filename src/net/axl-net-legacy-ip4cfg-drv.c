/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-legacy-ip4cfg-drv.c
    The resident driver behind axl_net_legacy_ip4cfg_install(): it puts the
    pre-UEFI-2.4 IPv4 configuration protocols on every NIC that has
    EFI_IP4_CONFIG2_PROTOCOL, answering from that protocol on every call.

    WHY A SEPARATE IMAGE. The tools that bring a NIC up (netload) are
    applications: they exit, and their image is freed. A protocol installed
    from one would point into freed memory the moment an old tool read it.
    This driver is its own image, buffer-loaded from the copy libaxl.a embeds,
    so the code and the per-NIC instances outlive whoever loaded it. It links
    NO libaxl -- that would embed libaxl in libaxl -- so it uses only the UEFI
    headers and calls boot services directly, as the pack stub does.

    WHAT IT PRODUCES, per NIC handle that carries EFI_IP4_CONFIG2_PROTOCOL and
    no EFI_IP4_CONFIG_PROTOCOL (a real one is never shadowed):

      EFI_IP4_CONFIG_PROTOCOL      UEFI 2.3.1. GetData returns the station
                                   address, mask and route table. The NIC is
                                   "started" from the moment the protocol is
                                   installed (IP4Config2 already runs its
                                   policy), so Start reports ALREADY_STARTED;
                                   Stop and a later Start change only this
                                   protocol's own state, never the NIC.
      EFI_NIC_IP4_CONFIG_PROTOCOL  the EDK-private companion, which is what
                                   the old EFI 1.x shell's ifconfig reads:
                                   GetName ("ethN" + link address), GetInfo
                                   (policy + addresses; EFI_NOT_FOUND = "Not
                                   configured."), SetInfo (EFI_UNSUPPORTED).

    READ-ONLY, BY DESIGN. SetInfo is what the old `ifconfig -s` and `-c` call.
    Mapping it onto IP4Config2 would look richer, but the old protocol's
    "Perment" flag cannot be honoured -- IP4Config2 always persists a policy
    to NVRAM -- so a "temporary" static address set from an old shell would
    silently survive a reset. Refusing is the honest answer; netload and the
    modern Shell's ifconfig configure a NIC properly.

    LIFETIME. It is a UEFI driver-model driver: Start opens IP4Config2
    BY_DRIVER, so when the network stack takes a NIC's IP4Config2 away the
    firmware calls Stop first, and the old protocols leave with it. A NIC that
    appears later is picked up by a protocol notify on IP4Config2. Unload
    stops every NIC, then removes the driver.
**/

#include <uefi/axl-uefi.h>

#include "axl-net-legacy-ip4cfg.h"

#define LIC_SIGNATURE      0x4c434950u   /* "PICL" */
#define LIC_POLICY_DHCP    1u            /* EFI_IP4_CONFIG2_POLICY */
#define LIC_BINDING_VER    0x10u
#define LIC_CONTAINER(p, member) \
    ((LicNic *)((UINT8 *)(p) - __builtin_offsetof(LicNic, member)))

/* One NIC's instance: both protocol interfaces, from boot-services pool. */
typedef struct {
    UINT32                       signature;
    EFI_IP4_CONFIG_PROTOCOL      ip4;
    EFI_NIC_IP4_CONFIG_PROTOCOL  nic;
    EFI_HANDLE                   controller;
    BOOLEAN                      started;      /* EFI_IP4_CONFIG_PROTOCOL state */
    EFI_EVENT                    reconfig;     /* caller's, registered with IP4Config2 */
    EFI_EVENT                    done;         /* ours, registered with IP4Config2, or NULL */
    EFI_EVENT                    done_user;    /* the caller's DoneEvent it signals once */
} LicNic;

/* -fstack-protector-strong is on for every object in this tree, and its two
   hooks live in libaxl, which this image does not link (see the pack stub,
   which supplies them the same way for the same reason). */
unsigned long __stack_chk_guard = 0x00000aff0d0a0000UL;

static EFI_BOOT_SERVICES           *lic_bs;
static EFI_SYSTEM_TABLE            *lic_st;
static EFI_HANDLE                   lic_image;
static EFI_DRIVER_BINDING_PROTOCOL  lic_binding;
static EFI_EVENT                    lic_notify;
static VOID                        *lic_notify_reg;
static AxlNetLegacyIp4CfgMarker     lic_marker = { AXL_NET_LEGACY_IP4CFG_REVISION };
static EFI_GUID                     lic_marker_guid = AXL_NET_LEGACY_IP4CFG_MARKER_GUID;

void __stack_chk_fail(void);
EFI_STATUS EFIAPI axl_legacy_ip4cfg_entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);

void
__stack_chk_fail(void)
{
    /* No safe way back into a smashed frame, and no runtime to report it. */
    for (;;) {
    }
}

// ---------------------------------------------------------------------------
// IP4Config2, read fresh on every call
// ---------------------------------------------------------------------------

/* The NIC's live interface info (FreePool it) and, when @p policy is not
   NULL, its policy. EFI_NOT_FOUND when the NIC has no IP4Config2 any more,
   its data cannot be read, or -- with @p need_address -- it has no address
   yet: the one meaning every caller needs ("not configured"). Only a pool
   failure is reported apart, as EFI_OUT_OF_RESOURCES. */
static EFI_STATUS
lic_iface_info(
    EFI_HANDLE                       controller,
    BOOLEAN                          need_address,
    EFI_IP4_CONFIG2_INTERFACE_INFO **out,
    UINT32                          *policy
    )
{
    EFI_IP4_CONFIG2_PROTOCOL *cfg = NULL;
    EFI_IP4_CONFIG2_INTERFACE_INFO *info = NULL;
    UINTN size = 0;
    *out = NULL;

    if (EFI_ERROR(lic_bs->HandleProtocol(controller, &EFI_IP4_CONFIG2_PROTOCOL_GUID,
                                         (VOID **)&cfg)) || cfg == NULL) {
        return EFI_NOT_FOUND;
    }
    if (cfg->GetData(cfg, Ip4Config2DataTypeInterfaceInfo, &size, NULL)
            != EFI_BUFFER_TOO_SMALL || size < sizeof(*info)) {
        return EFI_NOT_FOUND;
    }
    if (EFI_ERROR(lic_bs->AllocatePool(EfiBootServicesData, size, (VOID **)&info))) {
        return EFI_OUT_OF_RESOURCES;
    }
    if (EFI_ERROR(cfg->GetData(cfg, Ip4Config2DataTypeInterfaceInfo, &size, info))
        || (info->RouteTableSize > 0 && info->RouteTable == NULL)) {
        lic_bs->FreePool(info);
        return EFI_NOT_FOUND;
    }
    if (policy != NULL) {
        UINTN psize = sizeof(*policy);
        if (EFI_ERROR(cfg->GetData(cfg, Ip4Config2DataTypePolicy, &psize, policy))) {
            lic_bs->FreePool(info);
            return EFI_NOT_FOUND;
        }
    }
    if (need_address
        && (info->StationAddress.Addr[0] | info->StationAddress.Addr[1]
            | info->StationAddress.Addr[2] | info->StationAddress.Addr[3]) == 0) {
        lic_bs->FreePool(info);
        return EFI_NOT_FOUND;
    }
    *out = info;
    return EFI_SUCCESS;
}

/* Fill the old EFI_IP4_IPCONFIG_DATA at @p dst, its route table right after
   it -- the layout the old Ip4Config driver returned, pointer included. */
static void
lic_fill_ipconfig(
    EFI_IP4_IPCONFIG_DATA                *dst,
    const EFI_IP4_CONFIG2_INTERFACE_INFO *info
    )
{
    UINTN routes = info->RouteTableSize;
    lic_bs->CopyMem(&dst->StationAddress, (VOID *)&info->StationAddress,
                    sizeof(dst->StationAddress));
    lic_bs->CopyMem(&dst->SubnetMask, (VOID *)&info->SubnetMask, sizeof(dst->SubnetMask));
    dst->RouteTableSize = (UINT32)routes;
    dst->RouteTable = NULL;
    if (routes > 0) {
        dst->RouteTable = (EFI_IP4_ROUTE_TABLE *)(dst + 1);
        lic_bs->CopyMem(dst->RouteTable, info->RouteTable,
                        routes * sizeof(EFI_IP4_ROUTE_TABLE));
    }
}

/* The NIC's link address, from its interface info. */
static void
lic_fill_nic_addr(
    NIC_ADDR                             *dst,
    const EFI_IP4_CONFIG2_INTERFACE_INFO *info
    )
{
    UINT32 len = info->HwAddressSize;
    if (len > sizeof(dst->MacAddr)) {
        len = sizeof(dst->MacAddr);
    }
    lic_bs->SetMem(dst, sizeof(*dst), 0);
    dst->Type = info->IfType;
    dst->Len  = (UINT8)len;
    lic_bs->CopyMem(&dst->MacAddr, (VOID *)&info->HwAddress, len);
}

// ---------------------------------------------------------------------------
// EFI_IP4_CONFIG_PROTOCOL
// ---------------------------------------------------------------------------

static void
lic_unregister(
    LicNic   *n,
    EFI_EVENT *ev
    )
{
    EFI_IP4_CONFIG2_PROTOCOL *cfg = NULL;
    if (*ev == NULL) {
        return;
    }
    if (!EFI_ERROR(lic_bs->HandleProtocol(n->controller, &EFI_IP4_CONFIG2_PROTOCOL_GUID,
                                          (VOID **)&cfg)) && cfg != NULL) {
        cfg->UnregisterDataNotify(cfg, Ip4Config2DataTypeInterfaceInfo, *ev);
    }
    *ev = NULL;
}

/* Watch the NIC's interface info with the caller's event, best effort: a
   firmware that refuses leaves the event unregistered and nothing else. */
static EFI_EVENT
lic_register(
    LicNic   *n,
    EFI_EVENT ev
    )
{
    EFI_IP4_CONFIG2_PROTOCOL *cfg = NULL;
    if (EFI_ERROR(lic_bs->HandleProtocol(n->controller, &EFI_IP4_CONFIG2_PROTOCOL_GUID,
                                         (VOID **)&cfg)) || cfg == NULL
        || EFI_ERROR(cfg->RegisterDataNotify(cfg, Ip4Config2DataTypeInterfaceInfo, ev))) {
        return NULL;
    }
    return ev;
}

/* Stop waiting for the address: our DoneEvent watcher, unregistered and
   closed. */
static void
lic_done_cancel(
    LicNic *n
    )
{
    EFI_EVENT ev = n->done;
    if (ev != NULL) {
        lic_unregister(n, &n->done);
        lic_bs->CloseEvent(ev);
    }
    n->done_user = NULL;
}

/* IP4Config2's data changed while a Start waits for an address: once one is
   there, signal the caller's DoneEvent -- once, as UEFI 2.3.1 has it -- and
   stop watching. */
static VOID EFIAPI
lic_on_done(
    EFI_EVENT  event,
    VOID      *ctx
    )
{
    LicNic *n = ctx;
    EFI_IP4_CONFIG2_INTERFACE_INFO *info = NULL;
    (void)event;
    if (n->done == NULL || EFI_ERROR(lic_iface_info(n->controller, TRUE, &info, NULL))) {
        return;
    }
    lic_bs->FreePool(info);
    lic_bs->SignalEvent(n->done_user);
    lic_done_cancel(n);
}

/* Never reconfigures: IP4Config2 already runs the NIC's policy. Starting
   again after a Stop signals DoneEvent at once when the NIC has an address,
   or once when one arrives; ReconfigEvent is handed to IP4Config2 to signal
   on every later change. */
static EFI_STATUS EFIAPI
lic_ip4_start(
    EFI_IP4_CONFIG_PROTOCOL *this,
    EFI_EVENT                done_event,
    EFI_EVENT                reconfig_event
    )
{
    if (this == NULL || done_event == NULL || reconfig_event == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    LicNic *n = LIC_CONTAINER(this, ip4);
    if (n->started) {
        return EFI_ALREADY_STARTED;
    }
    n->started = TRUE;
    n->reconfig = lic_register(n, reconfig_event);

    EFI_IP4_CONFIG2_INTERFACE_INFO *info = NULL;
    if (!EFI_ERROR(lic_iface_info(n->controller, TRUE, &info, NULL))) {
        lic_bs->FreePool(info);
        lic_bs->SignalEvent(done_event);
    } else {
        EFI_EVENT ev = NULL;
        if (!EFI_ERROR(lic_bs->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, lic_on_done, n,
                                           &ev))) {
            n->done_user = done_event;
            n->done = lic_register(n, ev);
            if (n->done == NULL) {
                lic_bs->CloseEvent(ev);
                n->done_user = NULL;
            } else {
                /* An address that arrived after the check above but before
                   the watch was registered changes nothing IP4Config2 will
                   report again: look once more, now that nothing is missed. */
                lic_on_done(ev, n);
            }
        }
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
lic_ip4_stop(
    EFI_IP4_CONFIG_PROTOCOL *this
    )
{
    if (this == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    LicNic *n = LIC_CONTAINER(this, ip4);
    if (!n->started) {
        return EFI_NOT_STARTED;
    }
    lic_unregister(n, &n->reconfig);
    lic_done_cancel(n);
    n->started = FALSE;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
lic_ip4_get_data(
    EFI_IP4_CONFIG_PROTOCOL *this,
    UINTN                   *size,
    EFI_IP4_IPCONFIG_DATA   *data
    )
{
    if (this == NULL || size == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    LicNic *n = LIC_CONTAINER(this, ip4);
    if (!n->started) {
        return EFI_NOT_STARTED;
    }
    EFI_IP4_CONFIG2_INTERFACE_INFO *info = NULL;
    EFI_STATUS st = lic_iface_info(n->controller, TRUE, &info, NULL);
    if (EFI_ERROR(st)) {
        /* Started, no address yet: the spec's "not ready". */
        return st == EFI_NOT_FOUND ? EFI_NOT_READY : st;
    }
    UINTN need = sizeof(*data) + info->RouteTableSize * sizeof(EFI_IP4_ROUTE_TABLE);
    if (data == NULL || *size < need) {
        *size = need;
        lic_bs->FreePool(info);
        return EFI_BUFFER_TOO_SMALL;
    }
    lic_fill_ipconfig(data, info);
    *size = need;
    lic_bs->FreePool(info);
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// EFI_NIC_IP4_CONFIG_PROTOCOL
// ---------------------------------------------------------------------------

/* Where @p controller falls among the handles carrying @p guid, or -1. */
static INTN
lic_index_of(
    EFI_HANDLE  controller,
    EFI_GUID   *guid
    )
{
    UINTN       count = 0;
    EFI_HANDLE *handles = NULL;
    INTN        at = -1;
    if (EFI_ERROR(lic_bs->LocateHandleBuffer(ByProtocol, guid, NULL, &count, &handles))) {
        return -1;
    }
    for (UINTN i = 0; i < count; i++) {
        if (handles[i] == controller) {
            at = (INTN)i;
            break;
        }
    }
    lic_bs->FreePool(handles);
    return at;
}

/* "ethN": N is the NIC's position among the MNP service-binding handles --
   the same numbering the old shell gives a NIC itself when no protocol names
   it, so a name does not change when this driver arrives. */
static EFI_STATUS EFIAPI
lic_nic_get_name(
    EFI_NIC_IP4_CONFIG_PROTOCOL *this,
    CHAR16                      *name,
    NIC_ADDR                    *nic_addr
    )
{
    if (this == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    LicNic *n = LIC_CONTAINER(this, nic);

    /* Never an error: the old shell stops listing NICs at the first GetName
       that fails, so a NIC whose link address cannot be read is still named,
       with the address zeroed. */
    if (nic_addr != NULL) {
        EFI_IP4_CONFIG2_INTERFACE_INFO *info = NULL;
        lic_bs->SetMem(nic_addr, sizeof(*nic_addr), 0);
        if (!EFI_ERROR(lic_iface_info(n->controller, FALSE, &info, NULL))) {
            lic_fill_nic_addr(nic_addr, info);
            lic_bs->FreePool(info);
        }
    }

    if (name != NULL) {
        INTN idx = lic_index_of(n->controller, &EFI_MANAGED_NETWORK_SERVICE_BINDING_PROTOCOL_GUID);
        if (idx < 0) {
            idx = lic_index_of(n->controller, &EFI_IP4_CONFIG2_PROTOCOL_GUID);
        }
        if (idx < 0) {
            idx = 0;
        }
        CHAR16 digits[24];
        UINTN  nd = 0;
        UINTN  v = (UINTN)idx;
        do {
            digits[nd++] = (CHAR16)(L'0' + v % 10);
            v /= 10;
        } while (v != 0);
        UINTN at = 0;
        name[at++] = L'e';
        name[at++] = L't';
        name[at++] = L'h';
        while (nd > 0) {
            name[at++] = digits[--nd];
        }
        name[at] = 0;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
lic_nic_get_info(
    EFI_NIC_IP4_CONFIG_PROTOCOL *this,
    UINTN                       *len,
    NIC_IP4_CONFIG_INFO         *config
    )
{
    if (this == NULL || len == NULL) {
        return EFI_INVALID_PARAMETER;
    }
    LicNic *n = LIC_CONTAINER(this, nic);
    EFI_IP4_CONFIG2_INTERFACE_INFO *info = NULL;
    UINT32 policy = 0;
    EFI_STATUS st = lic_iface_info(n->controller, TRUE, &info, &policy);
    if (EFI_ERROR(st)) {
        /* Whatever went wrong, the old shell must read "Not configured.":
           any other error ends its listing at this NIC. */
        return EFI_NOT_FOUND;
    }
    UINTN need = sizeof(*config) + info->RouteTableSize * sizeof(EFI_IP4_ROUTE_TABLE);
    if (config == NULL || *len < need) {
        *len = need;
        lic_bs->FreePool(info);
        return EFI_BUFFER_TOO_SMALL;
    }
    lic_bs->SetMem(config, sizeof(*config), 0);
    lic_fill_nic_addr(&config->NicAddr, info);
    config->Source  = policy == LIC_POLICY_DHCP ? IP4_CONFIG_SOURCE_DHCP
                                                : IP4_CONFIG_SOURCE_STATIC;
    config->Perment = TRUE;   /* IP4Config2 keeps its policy in NVRAM */
    lic_fill_ipconfig(&config->Ip4Info, info);
    *len = need;
    lic_bs->FreePool(info);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
lic_nic_set_info(
    EFI_NIC_IP4_CONFIG_PROTOCOL *this,
    NIC_IP4_CONFIG_INFO         *config,
    BOOLEAN                      reconfig
    )
{
    (void)config;
    (void)reconfig;
    return this == NULL ? EFI_INVALID_PARAMETER : EFI_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// Driver binding
// ---------------------------------------------------------------------------

static EFI_STATUS EFIAPI
lic_supported(
    EFI_DRIVER_BINDING_PROTOCOL *this,
    EFI_HANDLE                   controller,
    EFI_DEVICE_PATH_PROTOCOL    *remaining
    )
{
    VOID *iface = NULL;
    (void)remaining;
    /* Never shadow a real one (nor stack a second copy of ours). */
    if (!EFI_ERROR(lic_bs->HandleProtocol(controller, &gEfiIp4ConfigProtocolGuid, &iface))
        || !EFI_ERROR(lic_bs->HandleProtocol(controller, &gEfiNicIp4ConfigProtocolGuid,
                                             &iface))) {
        return EFI_UNSUPPORTED;
    }
    EFI_STATUS st = lic_bs->OpenProtocol(controller, &EFI_IP4_CONFIG2_PROTOCOL_GUID, &iface,
                                         this->DriverBindingHandle, controller,
                                         EFI_OPEN_PROTOCOL_BY_DRIVER);
    if (EFI_ERROR(st)) {
        return st;
    }
    lic_bs->CloseProtocol(controller, &EFI_IP4_CONFIG2_PROTOCOL_GUID,
                          this->DriverBindingHandle, controller);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
lic_start(
    EFI_DRIVER_BINDING_PROTOCOL *this,
    EFI_HANDLE                   controller,
    EFI_DEVICE_PATH_PROTOCOL    *remaining
    )
{
    VOID   *iface = NULL;
    LicNic *n = NULL;
    (void)remaining;
    EFI_STATUS st = lic_bs->OpenProtocol(controller, &EFI_IP4_CONFIG2_PROTOCOL_GUID, &iface,
                                         this->DriverBindingHandle, controller,
                                         EFI_OPEN_PROTOCOL_BY_DRIVER);
    if (EFI_ERROR(st)) {
        return st;
    }
    st = lic_bs->AllocatePool(EfiBootServicesData, sizeof(*n), (VOID **)&n);
    if (!EFI_ERROR(st)) {
        lic_bs->SetMem(n, sizeof(*n), 0);
        n->signature   = LIC_SIGNATURE;
        n->ip4.Start   = lic_ip4_start;
        n->ip4.Stop    = lic_ip4_stop;
        n->ip4.GetData = lic_ip4_get_data;
        n->nic.GetName = lic_nic_get_name;
        n->nic.GetInfo = lic_nic_get_info;
        n->nic.SetInfo = lic_nic_set_info;
        n->controller  = controller;
        n->started     = TRUE;
        st = lic_bs->InstallMultipleProtocolInterfaces(&controller,
                 &gEfiIp4ConfigProtocolGuid, &n->ip4,
                 &gEfiNicIp4ConfigProtocolGuid, &n->nic, NULL);
        if (EFI_ERROR(st)) {
            lic_bs->FreePool(n);
        }
    }
    if (EFI_ERROR(st)) {
        lic_bs->CloseProtocol(controller, &EFI_IP4_CONFIG2_PROTOCOL_GUID,
                              this->DriverBindingHandle, controller);
    }
    return st;
}

static EFI_STATUS EFIAPI
lic_stop(
    EFI_DRIVER_BINDING_PROTOCOL *this,
    EFI_HANDLE                   controller,
    UINTN                        children,
    EFI_HANDLE                  *child_handles
    )
{
    EFI_IP4_CONFIG_PROTOCOL *ip4 = NULL;
    (void)children;
    (void)child_handles;
    if (EFI_ERROR(lic_bs->HandleProtocol(controller, &gEfiIp4ConfigProtocolGuid,
                                         (VOID **)&ip4)) || ip4 == NULL) {
        return EFI_DEVICE_ERROR;
    }
    LicNic *n = LIC_CONTAINER(ip4, ip4);
    if (n->signature != LIC_SIGNATURE || n->ip4.GetData != lic_ip4_get_data) {
        return EFI_DEVICE_ERROR;   /* not ours: never touch another's protocol */
    }
    /* Uninstall FIRST: a refused uninstall leaves the NIC served exactly as
       it was, its started state's events still registered with IP4Config2.
       Only a NIC really leaving gives them up. */
    EFI_STATUS st = lic_bs->UninstallMultipleProtocolInterfaces(controller,
                        &gEfiIp4ConfigProtocolGuid, &n->ip4,
                        &gEfiNicIp4ConfigProtocolGuid, &n->nic, NULL);
    if (EFI_ERROR(st)) {
        return st;
    }
    lic_unregister(n, &n->reconfig);
    lic_done_cancel(n);
    lic_bs->CloseProtocol(controller, &EFI_IP4_CONFIG2_PROTOCOL_GUID,
                          this->DriverBindingHandle, controller);
    n->signature = 0;
    lic_bs->FreePool(n);
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// New NICs, unload, entry
// ---------------------------------------------------------------------------

/* An IP4Config2 appeared somewhere (or the entry point is catching up on the
   ones already there): attach to every NIC this driver supports. Starting a
   NIC directly, rather than through ConnectController, binds this driver and
   nothing else -- a connect would offer the handle to every driver. */
static VOID EFIAPI
lic_on_ip4cfg2(
    EFI_EVENT  event,
    VOID      *ctx
    )
{
    UINTN       count = 0;
    EFI_HANDLE *handles = NULL;
    (void)event;
    (void)ctx;
    if (EFI_ERROR(lic_bs->LocateHandleBuffer(ByProtocol, &EFI_IP4_CONFIG2_PROTOCOL_GUID,
                                             NULL, &count, &handles))) {
        return;
    }
    for (UINTN i = 0; i < count; i++) {
        if (lic_supported(&lic_binding, handles[i], NULL) == EFI_SUCCESS) {
            lic_start(&lic_binding, handles[i], NULL);
        }
    }
    lic_bs->FreePool(handles);
}

/* One line on standard error: the driver has no log of its own, and what
   it reports here (a refused unload it could not fully undo) leaves NICs
   unserved, which nothing else would tell anyone. */
static void
lic_say(
    CHAR16 *what
    )
{
    static CHAR16 who[] = L"AxlLegacyIp4Config: ";
    static CHAR16 eol[] = L"\r\n";
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *err = lic_st->StdErr;
    if (err == NULL) {
        return;
    }
    err->OutputString(err, who);
    err->OutputString(err, what);
    err->OutputString(err, eol);
}

/* Watch for IP4Config2 appearing, and catch up on the NICs that already have
   it: the signal runs the notify before SignalEvent returns. */
static EFI_STATUS
lic_arm_notify(void)
{
    EFI_STATUS s = lic_bs->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, lic_on_ip4cfg2,
                                       NULL, &lic_notify);
    if (!EFI_ERROR(s)) {
        s = lic_bs->RegisterProtocolNotify(&EFI_IP4_CONFIG2_PROTOCOL_GUID, lic_notify,
                                           &lic_notify_reg);
    }
    if (EFI_ERROR(s)) {
        if (lic_notify != NULL) {
            lic_bs->CloseEvent(lic_notify);
            lic_notify = NULL;
        }
        return s;
    }
    lic_bs->SignalEvent(lic_notify);
    return EFI_SUCCESS;
}

/* Is the EFI_IP4_CONFIG_PROTOCOL on @p controller this driver's? */
static BOOLEAN
lic_serves(
    EFI_HANDLE controller
    )
{
    EFI_IP4_CONFIG_PROTOCOL *ip4 = NULL;
    if (EFI_ERROR(lic_bs->HandleProtocol(controller, &gEfiIp4ConfigProtocolGuid,
                                         (VOID **)&ip4)) || ip4 == NULL) {
        return FALSE;
    }
    LicNic *n = LIC_CONTAINER(ip4, ip4);
    return n->signature == LIC_SIGNATURE && n->ip4.GetData == lic_ip4_get_data;
}

/* Stop every NIC this driver serves; how many it still serves afterwards.
   Calls the binding's Stop itself: the unload has already taken the binding
   down, so the firmware's DisconnectController could not reach it. */
static UINTN
lic_stop_all(void)
{
    UINTN       count = 0;
    UINTN       left = 0;
    EFI_HANDLE *handles = NULL;
    if (EFI_ERROR(lic_bs->LocateHandleBuffer(ByProtocol, &gEfiIp4ConfigProtocolGuid,
                                             NULL, &count, &handles))) {
        return 0;
    }
    for (UINTN i = 0; i < count; i++) {
        if (lic_serves(handles[i])) {
            lic_stop(&lic_binding, handles[i], 0, NULL);
            if (lic_serves(handles[i])) {
                left++;
            }
        }
    }
    lic_bs->FreePool(handles);
    return left;
}

/* Stop every NIC, then remove the driver -- or, if any NIC refuses to stop,
   change nothing: the driver must never unload with a protocol of its own
   still installed, which would point into freed memory.

   Everything that could start a NIC goes down FIRST: the notify, then the
   binding and the marker, so no ConnectController from elsewhere can attach
   a NIC between the count below and the unload. The driver then counts what
   it still serves (the firmware's DisconnectController status was not
   trusted for this: a Stop that failed has been seen to come back as
   success). On a refusal the binding and marker go back and the notify is
   armed again, which also re-attaches the NICs that did stop. */
static EFI_STATUS EFIAPI
lic_unload(
    EFI_HANDLE image
    )
{
    if (lic_notify != NULL) {
        lic_bs->CloseEvent(lic_notify);
        lic_notify = NULL;
    }
    EFI_STATUS st = lic_bs->UninstallMultipleProtocolInterfaces(image,
                        &EFI_DRIVER_BINDING_PROTOCOL_GUID, &lic_binding,
                        &lic_marker_guid, &lic_marker, NULL);
    if (EFI_ERROR(st)) {
        if (EFI_ERROR(lic_arm_notify())) {
            lic_say(L"unload refused, and new NICs are no longer watched for");
        }
        return st;
    }
    if (lic_stop_all() == 0) {
        return EFI_SUCCESS;
    }
    /* Refused. Each undo that fails is said, and returned in place of the
       plain refusal, so the caller learns the driver is NOT whole. */
    EFI_STATUS back = lic_bs->InstallMultipleProtocolInterfaces(&lic_image,
                          &EFI_DRIVER_BINDING_PROTOCOL_GUID, &lic_binding,
                          &lic_marker_guid, &lic_marker, NULL);
    EFI_STATUS arm = lic_arm_notify();
    if (EFI_ERROR(back)) {
        lic_say(L"unload refused, and its driver binding could not be put back");
        return back;
    }
    if (EFI_ERROR(arm)) {
        lic_say(L"unload refused, and the NICs it stopped could not be re-attached");
        return arm;
    }
    return EFI_ACCESS_DENIED;
}

EFI_STATUS EFIAPI
axl_legacy_ip4cfg_entry(
    EFI_HANDLE        image,
    EFI_SYSTEM_TABLE *st
    )
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    VOID *other = NULL;
    lic_st = st;
    lic_bs = st->BootServices;
    lic_image = image;

    /* One per boot: a second copy would bind nothing and only cost memory. */
    if (!EFI_ERROR(lic_bs->LocateProtocol(&lic_marker_guid, NULL, &other))) {
        return EFI_ALREADY_STARTED;
    }
    EFI_STATUS s = lic_bs->HandleProtocol(image, &EFI_LOADED_IMAGE_PROTOCOL_GUID,
                                          (VOID **)&li);
    if (EFI_ERROR(s)) {
        return s;
    }
    li->Unload = lic_unload;

    lic_binding.Supported           = lic_supported;
    lic_binding.Start               = lic_start;
    lic_binding.Stop                = lic_stop;
    lic_binding.Version             = LIC_BINDING_VER;
    lic_binding.ImageHandle         = image;
    lic_binding.DriverBindingHandle = image;
    s = lic_bs->InstallMultipleProtocolInterfaces(&lic_image,
            &EFI_DRIVER_BINDING_PROTOCOL_GUID, &lic_binding,
            &lic_marker_guid, &lic_marker, NULL);
    if (EFI_ERROR(s)) {
        return s;
    }
    s = lic_arm_notify();
    if (EFI_ERROR(s)) {
        lic_bs->UninstallMultipleProtocolInterfaces(image,
            &EFI_DRIVER_BINDING_PROTOCOL_GUID, &lic_binding,
            &lic_marker_guid, &lic_marker, NULL);
        return s;
    }
    return EFI_SUCCESS;
}
