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
#include <axl/axl-str.h>
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
    AxlConfigType  type,
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

// ---------------------------------------------------------------------------
// AxlConfig group injection — axl_config_descs_net_static (policy group)
// ---------------------------------------------------------------------------

/// The two-value picker the `mode` descriptor advertises (a UI renders from
/// these; AxlConfig does not enforce choices on the set path — init_static is
/// the validator).
static const char *const net_mode_choices[] = { "dhcp", "static", NULL };

#define AXL_NET_STATIC_DESC_COUNT  7u

/* Emit one const-char* string field of AxlNetStaticOpts. */
#define EMIT_STATIC_STR(out, idx, key, member, desc, base)                  \
    emit_desc((out), (idx), (key), AXL_CFG_STRING, "", (desc),              \
              offsetof(AxlNetStaticOpts, member),                          \
              sizeof(((AxlNetStaticOpts *)0)->member), (base), 0)

size_t
axl_config_descs_net_static(
    AxlConfigDesc *out,
    size_t         cap,
    size_t         base_offset)
{
    if (out == NULL) {
        axl_warning("axl_config_descs_net_static: NULL out");
        return 0;
    }
    if (AXL_NET_STATIC_DESC_COUNT > cap) {
        axl_warning("axl_config_descs_net_static: cap=%zu cannot hold %u entries",
                    cap, AXL_NET_STATIC_DESC_COUNT);
        return 0;
    }

    size_t n = 0;
    /* mode default "dhcp"; the choices list is attached after emit (emit_desc
       zeroes .choices, shared with axl_config_descs_net). */
    emit_desc(out, n, "mode", AXL_CFG_STRING, "dhcp",
              "Address mode (dhcp|static)",
              offsetof(AxlNetStaticOpts, mode),
              sizeof(((AxlNetStaticOpts *)0)->mode), base_offset, 0);
    out[n].choices = net_mode_choices;
    n++;

    EMIT_STATIC_STR(out, n++, "ip",       ip,       "Static IPv4 address (mode=static)",     base_offset);
    EMIT_STATIC_STR(out, n++, "netmask",  netmask,  "Subnet mask (mode=static)",             base_offset);
    EMIT_STATIC_STR(out, n++, "gateway",  gateway,  "Default gateway (empty = none)",        base_offset);
    EMIT_STATIC_STR(out, n++, "dns",      dns,      "Primary DNS server (empty = leave)",    base_offset);
    EMIT_STATIC_STR(out, n++, "dns2",     dns2,     "Secondary DNS server (empty = none)",   base_offset);
    EMIT_STATIC_STR(out, n++, "hostname", hostname, "Hostname (empty = leave unchanged)",    base_offset);
    return n;
}

#undef EMIT_STATIC_STR

// ---------------------------------------------------------------------------
// axl_net_init_static — apply an AxlNetStaticOpts policy bag
// ---------------------------------------------------------------------------

/* Parse a config field that must be a dotted-quad. Returns 1 on a parsed
   address, 0 when the field is NULL/empty (caller decides if that's ok), -1
   on a malformed non-empty value. */
static int
parse_opt_ipv4(const char *s, uint8_t out[4])
{
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    return (axl_ipv4_parse(s, out) == AXL_OK) ? 1 : -1;
}

/* Apply cfg->dns / dns2 to @p nic. Returns AXL_ERR on a parse error; a NULL/
   empty primary is a no-op (AXL_OK). The actual SetData failure is the
   caller's to treat as fatal (static) or best-effort (dhcp). */
static int
apply_dns(const AxlNetStaticOpts *cfg, size_t nic, bool *out_attempted)
{
    *out_attempted = false;
    uint8_t d1[4], d2[4];
    int r1 = parse_opt_ipv4(cfg->dns, d1);
    if (r1 <= 0) {
        return (r1 < 0) ? AXL_ERR : AXL_OK;   /* malformed vs absent */
    }
    int r2 = parse_opt_ipv4(cfg->dns2, d2);
    if (r2 < 0) {
        return AXL_ERR;
    }
    *out_attempted = true;
    return axl_net_set_dns(nic, d1, (r2 == 1) ? d2 : NULL);
}

int
axl_net_init_static(
    const AxlNetStaticOpts *cfg,
    uint64_t                nic_index,
    size_t                  timeout_sec)
{
    if (cfg == NULL) {
        return AXL_ERR;
    }

    const char *mode = (cfg->mode != NULL) ? cfg->mode : "";
    bool is_static;
    if (mode[0] == '\0' || axl_strcmp(mode, "dhcp") == 0) {
        is_static = false;
    } else if (axl_strcmp(mode, "static") == 0) {
        is_static = true;
    } else {
        axl_warning("axl_net_init_static: unrecognized mode '%s'", mode);
        return AXL_ERR;
    }

    /* Concrete index for the per-NIC setters -- AUTO passes through
       unmapped (SIZE_MAX, same as axl_net_init above) so it resolves
       through the registry's AUTO ladder instead of being clamped to
       NIC 0 here. See axl_net_get_dhcp_lease in axl-net.h for the
       ladder every net API taking AXL_NET_NIC_AUTO resolves through. */
    size_t nic = (nic_index == AXL_NET_NIC_AUTO)
                     ? SIZE_MAX : (size_t)nic_index;
    bool   dns_attempted = false;

    if (is_static) {
        uint8_t ip[4], mask[4], gw[4];
        if (parse_opt_ipv4(cfg->ip, ip) != 1
            || parse_opt_ipv4(cfg->netmask, mask) != 1) {
            axl_warning("axl_net_init_static: static mode needs valid ip + netmask");
            return AXL_ERR;
        }
        int gwr = parse_opt_ipv4(cfg->gateway, gw);
        if (gwr < 0) {
            return AXL_ERR;
        }

        if (axl_net_drivers_up() != AXL_OK) {
            return AXL_ERR;
        }
        if (axl_net_set_static_ip(nic, ip, mask, (gwr == 1) ? gw : NULL) != AXL_OK) {
            return AXL_ERR;
        }
        if (apply_dns(cfg, nic, &dns_attempted) != AXL_OK) {
            return AXL_ERR;   /* parse error or (static path) a real DNS-set failure */
        }
        if (cfg->hostname != NULL && cfg->hostname[0] != '\0') {
            axl_net_set_hostname(cfg->hostname);
        }
        /* Settle on the exact address we set (a stale prior address won't
           satisfy it), so a read-back is valid. */
        axl_net_wait_ip_settled(nic, ip, 0);
    } else {
        if (axl_net_init(nic_index, timeout_sec) != AXL_OK) {
            return AXL_ERR;
        }
        /* DNS / hostname override on a DHCP box is best-effort: IP4Config2
           makes the DNS list read-only under the DHCP policy, so a SetData
           rejection here is not fatal to the (successful) DHCP bring-up. */
        if (apply_dns(cfg, nic, &dns_attempted) == AXL_ERR && !dns_attempted) {
            return AXL_ERR;   /* only a malformed dns string is fatal */
        }
        if (cfg->hostname != NULL && cfg->hostname[0] != '\0') {
            axl_net_set_hostname(cfg->hostname);
        }
    }
    return AXL_OK;
}
