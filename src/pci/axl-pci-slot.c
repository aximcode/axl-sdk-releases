/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-pci-slot.c
    PCIe Slot Capabilities and Slot Status decode.

    Its own translation unit rather than part of axl-pci-cap.c so
    that `--gc-sections` drops it from images that never ask about
    slots.

    Register layout, offsets relative to the PCI Express capability's
    own offset in the legacy capability chain (PCIe Base Spec, PCI
    Express Capability Structure):

        +0x02  PCI Express Capabilities (16-bit)
               bits 3:0  Capability Version
               bits 7:4  Device/Port Type
               bit  8    Slot Implemented
        +0x14  Slot Capabilities (32-bit)
        +0x1A  Slot Status (16-bit)
**/

#include <axl/axl-pci.h>
#include <axl/axl-macros.h>

/* Offsets within the PCI Express capability structure. */
#define EXPRESS_CAPS_REG    0x02   /* 16-bit */
#define SLOT_CAP_REG        0x14   /* 32-bit */
#define SLOT_STATUS_REG     0x1A   /* 16-bit */

/* PCI Express Capabilities register (+0x02). */
#define PORT_TYPE_SHIFT     4
#define PORT_TYPE_MASK      0x0Fu
#define SLOT_IMPLEMENTED    (1u << 8)

/* Slot Capabilities register (+0x14). */
#define SLOTCAP_ATTN_BUTTON       (1u << 0)
#define SLOTCAP_POWER_CONTROLLER  (1u << 1)
#define SLOTCAP_MRL_SENSOR        (1u << 2)
#define SLOTCAP_HOTPLUG_SURPRISE  (1u << 5)
#define SLOTCAP_HOTPLUG_CAPABLE   (1u << 6)
#define SLOTCAP_PWR_VALUE_SHIFT   7        /* bits 14:7  */
#define SLOTCAP_PWR_VALUE_MASK    0xFFu
#define SLOTCAP_PWR_SCALE_SHIFT   15       /* bits 16:15 */
#define SLOTCAP_PWR_SCALE_MASK    0x03u
#define SLOTCAP_INTERLOCK         (1u << 17)
#define SLOTCAP_NO_CMD_COMPLETED  (1u << 18)
#define SLOTCAP_PSN_SHIFT         19       /* bits 31:19 */
#define SLOTCAP_PSN_MASK          0x1FFFu

/* Slot Status register (+0x1A). Bits 0..4 are RW1C event flags and
   are deliberately not exposed; the three below are current state.

   PRESENCE_DETECT's bit position was verified against raw registers
   on real hardware, not taken from memory: reading CAP_EXP+0x1a on
   four root ports gave 0x0040 exactly where lspci reported PresDet+
   and 0x0000 where it reported PresDet-, which puts the state in bit
   6. An earlier draft of this file had it at bit 5.

   MRL_SENSOR_OPEN and INTERLOCK_ENGAGED are placed from the spec and
   are NOT empirically confirmed -- every port on both machines
   available here reports no MRL sensor and no interlock, so the
   captured data cannot discriminate those two bits. Treat them as
   the least-trusted fields in this file. */
#define SLOTSTA_MRL_SENSOR_OPEN   (1u << 5)  /* 1 == latch OPEN */
#define SLOTSTA_PRESENCE_DETECT   (1u << 6)
#define SLOTSTA_INTERLOCK_ENGAGED (1u << 7)

/**
 * @brief Locate the PCI Express capability in @p addr's legacy chain.
 *
 * @return AXL_OK with @p out_off set, or AXL_ERR if the function does
 *     not respond or has no PCI Express capability.
 */
static int
find_express_cap(
    AxlPciAddr   addr,
    uint16_t    *out_off
    )
{
    uint16_t off = 0;
    uint16_t id  = 0;

    /* axl_pci_cap_next rejects a pointer to ITSELF but deliberately
       allows a descending chain, so it cannot catch a multi-hop cycle
       (A -> B -> A) -- its docstring leaves that to the caller. A
       capability list lives in 0x40..0xFC on 4-byte boundaries, so 48
       hops is already more than the space allows. */
    for (unsigned hops = 0; hops < 48; hops++) {
        if (axl_pci_cap_next(addr, off, &off, &id) != AXL_OK) {
            break;
        }
        if (id == AXL_PCI_CAP_ID_EXPRESS) {
            *out_off = off;
            return AXL_OK;
        }
    }
    return AXL_ERR;
}

AXL_WARN_UNUSED int
axl_pci_read_slot_caps(
    AxlPciAddr       addr,
    AxlPciSlotCaps  *out
    )
{
    uint16_t cap_off  = 0;
    uint16_t express  = 0;
    uint32_t slot_cap = 0;
    uint16_t slot_sta = 0;
    uint8_t  port_type;

    if (out == NULL) {
        return AXL_ERR;
    }
    if (find_express_cap(addr, &cap_off) != AXL_OK) {
        return AXL_ERR;
    }
    if (axl_pci_read_config_16(addr, (uint16_t)(cap_off + EXPRESS_CAPS_REG),
                               &express) != AXL_OK) {
        return AXL_ERR;
    }

    /* Slot Capabilities is only meaningful on a root or downstream
       port WITH Slot Implemented set. An endpoint that merely has a
       PCI Express capability is not a slot, which is the case a
       cap-ID-only check gets wrong. */
    port_type = (uint8_t)((express >> PORT_TYPE_SHIFT) & PORT_TYPE_MASK);
    if (port_type != AXL_PCI_PORT_TYPE_ROOT_PORT
        && port_type != AXL_PCI_PORT_TYPE_DOWNSTREAM_PORT) {
        return AXL_ERR;
    }
    if ((express & SLOT_IMPLEMENTED) == 0) {
        return AXL_ERR;
    }

    if (axl_pci_read_config_32(addr, (uint16_t)(cap_off + SLOT_CAP_REG),
                               &slot_cap) != AXL_OK) {
        return AXL_ERR;
    }
    if (axl_pci_read_config_16(addr, (uint16_t)(cap_off + SLOT_STATUS_REG),
                               &slot_sta) != AXL_OK) {
        return AXL_ERR;
    }

    /* Every read succeeded; only now is `out` written, so a caller
       that gets AXL_ERR knows its buffer is untouched. */
    out->port_type             = port_type;
    out->attention_button      = (slot_cap & SLOTCAP_ATTN_BUTTON) != 0;
    out->power_controller      = (slot_cap & SLOTCAP_POWER_CONTROLLER) != 0;
    out->mrl_sensor            = (slot_cap & SLOTCAP_MRL_SENSOR) != 0;
    out->hotplug_surprise      = (slot_cap & SLOTCAP_HOTPLUG_SURPRISE) != 0;
    out->hotplug_capable       = (slot_cap & SLOTCAP_HOTPLUG_CAPABLE) != 0;
    out->power_limit_value     = (uint8_t)((slot_cap >> SLOTCAP_PWR_VALUE_SHIFT)
                                           & SLOTCAP_PWR_VALUE_MASK);
    out->power_limit_scale     = (uint8_t)((slot_cap >> SLOTCAP_PWR_SCALE_SHIFT)
                                           & SLOTCAP_PWR_SCALE_MASK);
    out->electromech_interlock = (slot_cap & SLOTCAP_INTERLOCK) != 0;
    out->no_command_completed  = (slot_cap & SLOTCAP_NO_CMD_COMPLETED) != 0;
    out->physical_slot_number  = (uint16_t)((slot_cap >> SLOTCAP_PSN_SHIFT)
                                            & SLOTCAP_PSN_MASK);

    out->presence_detect       = (slot_sta & SLOTSTA_PRESENCE_DETECT) != 0;
    /* The raw bit is 1 when the latch is OPEN; invert so both MRL
       booleans read in the same direction. */
    out->mrl_sensor_closed     = (slot_sta & SLOTSTA_MRL_SENSOR_OPEN) == 0;
    out->interlock_engaged     = (slot_sta & SLOTSTA_INTERLOCK_ENGAGED) != 0;
    return AXL_OK;
}
