/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-net-opts.h:
 *
 * Canonical option bag and bring-up helper for AXL-consuming tools
 * and services that take the same NIC / local-IP / port options on
 * the command line. Every networked consumer (web servers, REST /
 * IPMI clients, one-shot fetch utilities) was reinventing the same
 * flag-set + sentinel-mapping + bring-up plumbing; `AxlNetOpts`
 * factors it into a sub-struct the consumer embeds in its own
 * options type, and `axl_net_init` / `axl_net_init_from_opts` are
 * the matching one-call DHCP bring-up.
 *
 * Pair with the `axl_config_descs_net` group-injection helper in
 * `<axl/axl-config.h>` to also pull the matching CLI / config
 * descriptors into a consumer's own table without copy-paste.
 *
 * **Out of scope by design**: setting a static IPv4 onto the NIC
 * (the `ifconfig`-equivalent layer). Call `axl_net_set_static_ip`
 * directly if a tool genuinely needs to mutate `IP4Config2`
 * policy — that's stateful system config, not a per-invocation
 * connection option. Source / listen IP selection (`local_ip`
 * below) is the connection-side knob and stays here.
 *
 * IPv4 only for v1. An IPv6 / family-tagged variant is a clean
 * future addition.
 */

#ifndef AXL_NET_OPTS_H
#define AXL_NET_OPTS_H

#include <stddef.h>
#include <stdint.h>
#include <axl/axl-macros.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Sentinels
// ---------------------------------------------------------------------------

/**
 * @brief NIC index sentinel meaning "auto-detect first usable NIC".
 *
 * Maps to `SIZE_MAX` when passed through to the bring-up call.
 * `axl_config_descs_net` uses `AXL_NET_NIC_AUTO_STR` (the
 * stringified value of this sentinel) as the descriptor default,
 * so `axl_config_new` auto-applies the sentinel into the embedded
 * `nic_index` field with no extra wiring from the consumer.
 */
#define AXL_NET_NIC_AUTO  ((uint64_t)-1)

/**
 * @brief Decimal-string form of `AXL_NET_NIC_AUTO`, suitable as
 *     an `AxlConfigDesc.default_value` for an `AXL_CFG_UINT` field.
 *
 * The string is the unsigned decimal representation of `UINT64_MAX`
 * — pre-stringified rather than computed at runtime because
 * `AxlConfigDesc.default_value` is a `const char *` that must be a
 * compile-time constant. Surfaced on the public API so consumers
 * synthesizing their own descriptors keep the "auto" semantics
 * consistent with `axl_config_descs_net`.
 */
#define AXL_NET_NIC_AUTO_STR  "18446744073709551615"

// ---------------------------------------------------------------------------
// Options bag
// ---------------------------------------------------------------------------

/**
 * @brief Canonical network options bag.
 *
 * Embed as a sub-struct of the consumer's own options type, then
 * use `axl_config_descs_net(out, cap, kinds, offsetof(MyOpts, net))`
 * to emit matching CLI / config descriptors without copy-paste.
 *
 * Field semantics:
 *   - `nic_index` — `AXL_NET_NIC_AUTO` (the default) means
 *     auto-detect the first usable NIC; any other value is a 0-based
 *     index into the SNP handle list as returned by
 *     `axl_net_list_interfaces`. Used at bring-up time to pick which
 *     NIC to run DHCP against. Post-bring-up, prefer `local_ip`
 *     for routing — you can name the interface you want by its
 *     station IP without knowing its handle index.
 *   - `local_ip` — IPv4 to bind the local end of the socket to.
 *     Empty string means "let the kernel pick" (`0.0.0.0`). For
 *     clients this is the outbound source IP (curl `--interface`);
 *     for servers this is the listen / accept address. One field
 *     because both roles `bind(2)` the same way — the role is
 *     implied by whether the consumer subsequently calls
 *     `connect()` or `listen()`. CLI vocabulary is selected by
 *     the `AXL_NET_OPT_SOURCE_IP` / `_LISTEN_IP` enum bits below.
 *   - `port` — `uint16_t` so it round-trips through the descriptor's
 *     `AXL_CFG_UINT` parser without truncation. `0` means
 *     "consumer-defined default" — AXL ships no canonical port,
 *     since every consumer's domain default differs.
 *
 * Sub-struct (not flat fields) so future option additions don't
 * collide with the consumer's own field names.
 */
typedef struct {
    uint64_t    nic_index;   ///< AXL_NET_NIC_AUTO = auto-detect first usable NIC
    const char *local_ip;    ///< IPv4 to bind() the local socket end; "" = any
    uint16_t    port;        ///< 0 = consumer-defined default
} AxlNetOpts;

// ---------------------------------------------------------------------------
// Option-group selectors (for axl_config_descs_net)
// ---------------------------------------------------------------------------

/**
 * @brief Bitmask of which `AxlNetOpts` fields a consumer wants
 *     descriptors for. Pass to `axl_config_descs_net`.
 *
 * `AXL_NET_OPT_SOURCE_IP` and `AXL_NET_OPT_LISTEN_IP` both target
 * the same underlying `local_ip` field — they differ only in CLI
 * vocabulary (`--source-ip` for clients, `--listen-ip` for
 * servers). A consumer normally sets at most one of the two,
 * matching its role; the CLIENT and SERVER presets below pick the
 * conventional name for each.
 */
typedef enum {
    AXL_NET_OPT_NIC        = 1u << 0,  ///< --nic           → nic_index
    AXL_NET_OPT_SOURCE_IP  = 1u << 1,  ///< --source-ip     → local_ip (client)
    AXL_NET_OPT_PORT       = 1u << 2,  ///< --port          → port
    AXL_NET_OPT_LISTEN_IP  = 1u << 3,  ///< --listen-ip     → local_ip (server)
} AxlNetOptKind;

/// Client-side preset: NIC selector + outbound source-IP bind.
#define AXL_NET_OPT_CLIENT  (AXL_NET_OPT_NIC | AXL_NET_OPT_SOURCE_IP)

/// Server-side preset: NIC selector + port + listen-IP bind.
#define AXL_NET_OPT_SERVER  (AXL_NET_OPT_NIC | AXL_NET_OPT_PORT \
                                             | AXL_NET_OPT_LISTEN_IP)

// ---------------------------------------------------------------------------
// One-call bring-up
// ---------------------------------------------------------------------------

/**
 * @brief Bring up networking via DHCP on a chosen NIC.
 *
 * Maps `nic_index == AXL_NET_NIC_AUTO` to `SIZE_MAX` and delegates
 * to `axl_net_bring_up` with a NULL static address (DHCP path).
 * Address read-back is implicit; this helper does not surface the
 * resolved IP — call `axl_net_get_ip_address` separately if you
 * need it.
 *
 * Static-IP configuration is intentionally out of scope: it
 * mutates `IP4Config2` policy, which is the firmware's `ifconfig`
 * layer, not a per-tool connection option. Tools that need to
 * install a static IP call `axl_net_set_static_ip` directly, or
 * defer to UEFI Shell `ifconfig`.
 *
 * @return AXL_OK on success, AXL_ERR on driver-load / link / DHCP
 *     failure.
 */
int
axl_net_init(
    uint64_t nic_index,    ///< AXL_NET_NIC_AUTO or 0-based NIC index
    size_t   timeout_sec   ///< DHCP wait (0 = 10 s default)
);

/**
 * @brief Bring up networking from an `AxlNetOpts` bag.
 *
 * Thin thunk over `axl_net_init` reading `opts->nic_index`. The
 * `local_ip` / `port` fields are consumer-owned and unused at
 * bring-up — they parameterize subsequent socket operations.
 *
 * @return AXL_OK on success, AXL_ERR on bring-up failure (or if
 *     @p opts is NULL).
 */
int
axl_net_init_from_opts(
    const AxlNetOpts *opts,        ///< options bag
    size_t            timeout_sec  ///< DHCP wait (0 = 10 s default)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_NET_OPTS_H */
