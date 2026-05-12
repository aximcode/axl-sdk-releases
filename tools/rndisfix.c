/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file rndisfix.c
    Workaround for the EDK2 UsbRndis data-plane bug.

    The vendored EDK2 `UsbRndis.efi` driver has a stub
    `SetUsbRndisPacketFilter` that never actually sends a
    `REMOTE_NDIS_SET_MSG / OID_GEN_CURRENT_PACKET_FILTER` to the
    device. Without that step the RNDIS endpoint stays in its post-
    INITIALIZE default state with packet filter = 0; SNP comes UP
    but no inbound frames are delivered to the host.

    This tool walks USB interfaces, finds RNDIS comms interfaces by
    class triplet, and issues the missing SET_MSG directly via the
    USB-IO control-transfer pipe. After running it, ICMP / TCP /
    DHCP traffic to the BMC USB-NIC at 169.254.1.1 starts
    flowing.

    Usage:
      rndisfix.efi               apply to every RNDIS interface
      rndisfix.efi --debug       enable axl-sdk debug logging

    Returns 0 on success (or no RNDIS interfaces present), non-zero
    if any RNDIS interface failed to accept the SET_MSG.
**/

#include <axl.h>

// ---------------------------------------------------------------------------
// RNDIS protocol constants (RNDIS Spec 1.0 §3, also EDK2 UsbRndis.h)
// ---------------------------------------------------------------------------

#define RNDIS_SET_MSG                   0x00000005
#define RNDIS_SET_CMPLT                 0x80000005
#define RNDIS_STATUS_SUCCESS            0x00000000

#define OID_GEN_CURRENT_PACKET_FILTER   0x0001010E

#define NDIS_PACKET_TYPE_DIRECTED       0x0001
#define NDIS_PACKET_TYPE_MULTICAST      0x0002
#define NDIS_PACKET_TYPE_ALL_MULTICAST  0x0004
#define NDIS_PACKET_TYPE_BROADCAST      0x0008

/* Linux's RNDIS_DEFAULT_FILTER from drivers/net/usb/rndis_host.c —
   matches what cdc_rndis sends and what every RNDIS BMC is
   known to accept. */
#define RNDIS_DEFAULT_FILTER (NDIS_PACKET_TYPE_DIRECTED      \
                              | NDIS_PACKET_TYPE_BROADCAST   \
                              | NDIS_PACKET_TYPE_ALL_MULTICAST)

// ---------------------------------------------------------------------------
// USB CDC class request codes (USB CDC Spec §6.2)
// ---------------------------------------------------------------------------

#define CDC_REQ_SEND_ENCAPSULATED_COMMAND  0x00
#define CDC_REQ_GET_ENCAPSULATED_RESPONSE  0x01

// bmRequestType bytes for class-targeted control transfers
#define USB_REQ_TYPE_CLASS_INTERFACE_OUT   0x21
#define USB_REQ_TYPE_CLASS_INTERFACE_IN    0xA1

// ---------------------------------------------------------------------------
// RNDIS message layouts — packed for wire format
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

typedef struct {
    uint32_t  message_type;             /* RNDIS_SET_MSG */
    uint32_t  message_length;           /* sizeof(this) including info_buffer */
    uint32_t  request_id;
    uint32_t  oid;                      /* OID_GEN_CURRENT_PACKET_FILTER */
    uint32_t  information_buffer_length;
    uint32_t  information_buffer_offset;/* offset from request_id */
    uint32_t  reserved;
    uint32_t  filter_value;             /* the actual data */
} RndisSetMsg;

typedef struct {
    uint32_t  message_type;             /* RNDIS_SET_CMPLT */
    uint32_t  message_length;
    uint32_t  request_id;
    uint32_t  status;                   /* RNDIS_STATUS_* */
} RndisSetCmplt;

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Class triplet matchers
// ---------------------------------------------------------------------------

/// True if this interface descriptor looks like a Microsoft RNDIS
/// communications interface. RNDIS appears in two flavors:
///   - Class 0xE0 / Sub 0x01 / Proto 0x03 (Wireless Controller / RF /
///     Microsoft RNDIS) — Microsoft's reserved triplet
///   - Class 0x02 / Sub 0x02 / Proto 0xFF (Communications / ACM /
///     Vendor-specific) — Linux/Android-style
/// Either one corresponds to a comms interface that takes RNDIS
/// control messages over endpoint 0.
static bool
is_rndis_comms_interface(uint8_t cls, uint8_t sub, uint8_t proto)
{
    if (cls == 0xE0 && sub == 0x01 && proto == 0x03) {
        return true;
    }
    if (cls == 0x02 && sub == 0x02 && proto == 0xFF) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Per-interface fix
// ---------------------------------------------------------------------------

/// Issue REMOTE_NDIS_SET_MSG for OID_GEN_CURRENT_PACKET_FILTER on the
/// given interface, then read back the SET_CMPLT and verify status.
/// Returns 0 on success, -1 on transfer error or non-success status.
static int
apply_packet_filter(AxlUsbAddr addr)
{
    static uint32_t request_id_counter = 0x12340001;

    RndisSetMsg msg = {
        .message_type             = RNDIS_SET_MSG,
        .message_length           = sizeof(RndisSetMsg),
        .request_id               = ++request_id_counter,
        .oid                      = OID_GEN_CURRENT_PACKET_FILTER,
        .information_buffer_length = 4,
        /* Offset is from request_id (byte 8) to filter_value
           (byte 28) = 20 bytes per RNDIS spec. */
        .information_buffer_offset = 20,
        .reserved                 = 0,
        .filter_value             = RNDIS_DEFAULT_FILTER,
    };

    int rc = axl_usb_control_transfer(
        addr,
        USB_REQ_TYPE_CLASS_INTERFACE_OUT,
        CDC_REQ_SEND_ENCAPSULATED_COMMAND,
        0,                /* wValue */
        addr.intf,        /* wIndex = interface number */
        AXL_USB_DATA_OUT,
        5000,             /* 5s timeout — RNDIS responses are usually
                             fast but BMCs vary */
        &msg,
        sizeof(msg));
    if (rc != AXL_OK) {
        axl_printf("  SEND_ENCAPSULATED_COMMAND failed\n");
        return -1;
    }

    RndisSetCmplt resp = { 0 };
    rc = axl_usb_control_transfer(
        addr,
        USB_REQ_TYPE_CLASS_INTERFACE_IN,
        CDC_REQ_GET_ENCAPSULATED_RESPONSE,
        0,
        addr.intf,
        AXL_USB_DATA_IN,
        5000,
        &resp,
        sizeof(resp));
    if (rc != AXL_OK) {
        axl_printf("  GET_ENCAPSULATED_RESPONSE failed\n");
        return -1;
    }

    if (resp.message_type != RNDIS_SET_CMPLT) {
        axl_printf("  unexpected response type 0x%08x (want 0x%08x)\n",
                   (unsigned)resp.message_type,
                   (unsigned)RNDIS_SET_CMPLT);
        return -1;
    }
    if (resp.request_id != msg.request_id) {
        axl_printf("  request_id mismatch (sent 0x%08x, got 0x%08x)\n",
                   (unsigned)msg.request_id,
                   (unsigned)resp.request_id);
        return -1;
    }
    if (resp.status != RNDIS_STATUS_SUCCESS) {
        axl_printf("  device reported RNDIS status 0x%08x\n",
                   (unsigned)resp.status);
        return -1;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Argument table + handler
// ---------------------------------------------------------------------------

static const AxlArgDesc flags[] = {
    { .name = "debug", .type = AXL_ARG_BOOL,
      .help = "Set log level to DEBUG" },
    {0}
};

static int
run_rndisfix(AxlArgs *a)
{
    if (axl_args_get_bool(a, "debug")) {
        axl_log_set_level(AXL_LOG_DEBUG);
    }

    /* Make sure the NIC drivers (UsbRndis + NetworkCommon) are loaded
       and bound, so the RNDIS device is in the INITIALIZED state and
       will accept a SET message. */
    if (axl_net_ensure_drivers() != AXL_OK) {
        axl_printf("rndisfix: failed to bring up network drivers\n");
        return 1;
    }

    AxlUsbAddr *u = NULL;
    int    found    = 0;
    int    succeeded = 0;
    int    failed    = 0;

    while ((u = axl_usb_next(u)) != NULL) {
        uint8_t cls, sub, proto;
        if (axl_usb_get_class(*u, &cls, &sub, &proto) != AXL_OK) {
            continue;
        }
        if (!is_rndis_comms_interface(cls, sub, proto)) {
            continue;
        }

        uint16_t vid = 0, pid = 0;
        axl_usb_get_vid_pid(*u, &vid, &pid);
        axl_printf("RNDIS interface %u-%u-%u  (vid:pid %04x:%04x  class %02x/%02x/%02x)\n",
                   (unsigned)u->bus, (unsigned)u->addr, (unsigned)u->intf,
                   (unsigned)vid, (unsigned)pid,
                   (unsigned)cls, (unsigned)sub, (unsigned)proto);
        found++;

        if (apply_packet_filter(*u) == 0) {
            axl_printf("  packet filter set to 0x%04x  OK\n",
                       (unsigned)RNDIS_DEFAULT_FILTER);
            succeeded++;
        } else {
            failed++;
        }
    }

    if (found == 0) {
        axl_printf("rndisfix: no RNDIS interfaces found\n");
        return 0;
    }
    axl_printf("rndisfix: %d interface(s), %d ok, %d failed\n",
               found, succeeded, failed);
    return failed == 0 ? 0 : 1;
}

AXL_TOOL_MAIN(rndisfix)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name         = "rndisfix",
        .help         = "Send RNDIS_SET_MSG/OID_GEN_CURRENT_PACKET_FILTER to "
                        "RNDIS USB-NICs (workaround for EDK2 UsbRndis stub).",
        .flags        = flags,
        .handler      = run_rndisfix,
    });
}
