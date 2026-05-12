/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-net-opts.c
    Canonical option bag + one-call DHCP bring-up helpers for AXL
    consumers, plus the AxlConfig group-injection helper that
    emits the matching descriptors into a consumer-owned table.
**/

#include <stddef.h>
#include <axl/axl-config.h>
#include <axl/axl-log.h>
#include <axl/axl-net.h>
#include <axl/axl-net-opts.h>

AXL_LOG_DOMAIN("net");

int
axl_net_init(
    uint64_t nic_index,
    size_t   timeout_sec)
{
    size_t nic = (nic_index == AXL_NET_NIC_AUTO)
                     ? SIZE_MAX : (size_t)nic_index;
    return axl_net_bring_up(nic, NULL, NULL, NULL, timeout_sec, NULL);
}

int
axl_net_init_from_opts(
    const AxlNetOpts *opts,
    size_t            timeout_sec)
{
    if (opts == NULL) {
        return AXL_ERR;
    }
    return axl_net_init(opts->nic_index, timeout_sec);
}

// ---------------------------------------------------------------------------
// AxlConfig group injection — axl_config_descs_net
// ---------------------------------------------------------------------------

/* Emits an entry into the accumulator. Centralizes the
   field-size + offset + base_offset addition so each kind branch
   below stays a single self-describing line. */
static void
emit_desc(
    AxlConfigDesc *out,
    size_t         idx,
    const char    *key,
    int            type,
    const char    *default_value,
    const char    *description,
    size_t         field_offset,
    size_t         field_size,
    size_t         base_offset,
    char           short_name)
{
    out[idx] = (AxlConfigDesc){
        .key           = key,
        .type          = type,
        .default_value = default_value,
        .description   = description,
        .offset        = base_offset + field_offset,
        .field_size    = field_size,
        .short_name    = short_name,
        .choices       = NULL,
    };
}

size_t
axl_config_descs_net(
    AxlConfigDesc *out,
    size_t         cap,
    uint32_t       kinds,
    size_t         base_offset)
{
    if (out == NULL) {
        axl_warning("axl_config_descs_net: NULL out");
        return 0;
    }

    /* Count requested entries first so under-capacity is detected
       before any partial write. */
    size_t need = 0;
    if (kinds & AXL_NET_OPT_NIC)       { need++; }
    if (kinds & AXL_NET_OPT_SOURCE_IP) { need++; }
    if (kinds & AXL_NET_OPT_PORT)      { need++; }
    if (kinds & AXL_NET_OPT_LISTEN_IP) { need++; }

    if (need > cap) {
        axl_warning("axl_config_descs_net: cap=%zu cannot hold %zu entries",
                    cap, need);
        return 0;
    }

    size_t n = 0;
    if (kinds & AXL_NET_OPT_NIC) {
        /* Default is the stringified AXL_NET_NIC_AUTO sentinel — not
           the friendlier literal "auto" — because AxlConfig's UINT
           auto-apply runs axl_str_to_u64 on the default at
           axl_config_new() time. "auto" fails parse and silently
           leaves the field at 0 ( = "first NIC by enumeration"),
           NOT the sentinel the consumer asked for. */
        emit_desc(out, n++, "nic", AXL_CFG_UINT, AXL_NET_NIC_AUTO_STR,
                  "NIC index (max = auto-select first usable)",
                  offsetof(AxlNetOpts, nic_index),
                  sizeof(((AxlNetOpts *)0)->nic_index),
                  base_offset, 0);
    }
    if (kinds & AXL_NET_OPT_SOURCE_IP) {
        emit_desc(out, n++, "source-ip", AXL_CFG_STRING, "",
                  "Outbound bind IPv4 (empty = kernel-chosen source)",
                  offsetof(AxlNetOpts, local_ip),
                  sizeof(((AxlNetOpts *)0)->local_ip),
                  base_offset, 0);
    }
    if (kinds & AXL_NET_OPT_PORT) {
        emit_desc(out, n++, "port", AXL_CFG_UINT, "0",
                  "TCP/UDP port (0 = consumer default)",
                  offsetof(AxlNetOpts, port),
                  sizeof(((AxlNetOpts *)0)->port),
                  base_offset, 0);
    }
    if (kinds & AXL_NET_OPT_LISTEN_IP) {
        emit_desc(out, n++, "listen-ip", AXL_CFG_STRING, "",
                  "Listen IPv4 (empty = bind any, 0.0.0.0)",
                  offsetof(AxlNetOpts, local_ip),
                  sizeof(((AxlNetOpts *)0)->local_ip),
                  base_offset, 0);
    }
    return n;
}
