/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * 9p-common.h - shared surface of the `9p` tool's translation units.
 *
 * The tool is split the way axl-webfs splits: 9p.c is the verb tree,
 * 9p-cmd-*.c are the verb handlers, and 9p-{serve,mount}-svc.c are the
 * dual-compiled services (each also linked into its own DXE driver
 * image). This header carries what more than one of those TUs needs.
 */

#ifndef AXL_TOOLS_9P_COMMON_H
#define AXL_TOOLS_9P_COMMON_H

#include <stddef.h>

#include <axl.h>
#include <axl/axl-net-opts.h>

/* The 9P well-known port. Needed by the host[:port] splitter and, as a
   string, by both services' AxlConfigDesc defaults - which take a
   compile-time const char *, so the two forms are kept adjacent to make a
   mismatch obvious. */
#define AXL_9P_PORT_DEFAULT      564
#define AXL_9P_PORT_DEFAULT_STR  "564"

/* Buffer size for the host part of a `host[:port]` address. Comfortably
   above the 253-byte DNS name limit is unnecessary here - the library
   resolves through AxlNet, which takes a dotted quad or a short name - and
   128 matches what every other AXL tool gives a host field. */
#define AXL_9P_HOST_MAX          128

/* The `nic` and `port` rows of a service's AxlConfigDesc table.
 *
 * Both services take the same two networking options, and both tables are
 * hand-authored rather than composed with axl_config_descs_net(): that
 * helper writes into a RUNTIME accumulator, while AxlService.opts_descs
 * must be a static table - AXL_SERVICE_DRIVER's DriverEntry reads it with
 * no consumer hook to run a builder first. The KEYS and the AUTO sentinel
 * are kept identical to what the helper emits, so the CLI vocabulary does
 * not fork from every other networked AXL tool.
 *
 * `nic` and `port` are the WHOLE of what crosses LoadOptions. AxlNetOpts'
 * other fields - `local_ip` above all - are deliberately absent: neither
 * service offers a static-IP flag, so a launcher-side assignment to them
 * would be dead code that reads like a configured value. Adding one here
 * is what makes such a field crossable.
 *
 * @p T is the consumer's opts type; each table needs its own offsetof, so
 * the type cannot be hidden inside the macro. */
#define AXL_9P_NET_CFG_DESCS(T)                                            \
    { "nic",  AXL_CFG_UINT, AXL_NET_NIC_AUTO_STR,                          \
      "NIC ordinal to bring up (default: first usable)",                   \
      offsetof(T, net.nic_index), sizeof(uint64_t) },                      \
    { "port", AXL_CFG_UINT, AXL_9P_PORT_DEFAULT_STR,                       \
      "TCP port",                                                          \
      offsetof(T, net.port),      sizeof(uint16_t) }

/// The `--nic` row of a verb's AxlArgDesc table. EVERY verb that touches
/// the network carries it, one-shot and resident alike, so it is its own
/// macro rather than half of a two-row block the one-shot verbs would
/// have to copy by hand.
#define AXL_9P_NET_ARG_NIC                                                 \
    { .name = "nic",  .short_name = 'n', .type = AXL_ARG_U64,              \
      .default_value = AXL_NET_NIC_AUTO_STR,                               \
      .help = "NIC ordinal to bring up (default: first usable)" }

/// The `--port` row, carried by the two RESIDENT verbs (the one-shot verbs
/// take the port inline, as `host:port`).
///
/// `.min`/`.max` bound the value to what fits the driver-side `uint16_t`
/// field it is serialized into (AXL_9P_NET_CFG_DESCS's `port` row,
/// `field_size = sizeof(uint16_t)`). Without this bound `axl_args_run`
/// accepts any AXL_ARG_U64 value and the launcher's own `(uint16_t)` cast
/// on the parsed value silently truncates it (70000 becomes 4464) BEFORE
/// serialization, so the driver-side auto_apply range check on the far end
/// of LoadOptions never sees the bad value at all. Bounding here instead
/// makes `axl_args_run` reject the out-of-range port with a clear error
/// before the handler (or the driver) ever runs.
#define AXL_9P_NET_ARG_PORT                                                \
    { .name = "port", .short_name = 'p', .type = AXL_ARG_U64,              \
      .default_value = AXL_9P_PORT_DEFAULT_STR,                            \
      .min = 1, .max = 65535,                                              \
      .help = "TCP port (1-65535)" }

// ---------------------------------------------------------------------------
// Shared helpers - implemented in 9p-common.c
// ---------------------------------------------------------------------------

/// The three lines a "unload this resident service" verb prints, one per
/// outcome.
///
/// Named members rather than positional parameters: the exact text is the
/// tool's user-facing contract (the integration harness asserts these lines
/// whole), so a call site must not be able to hand three anonymous strings
/// over in the wrong order. Each includes its own trailing newline.
typedef struct {
    const char *idle;   ///< stdout, when the service was not running
    const char *fail;   ///< stderr, when the stop itself failed
    const char *done;   ///< stdout, after the service really stopped
} Axl9pStopMsgs;

/**
 * @brief Split a `host` or `host:port` server address into its parts.
 *
 * IPv4 / hostname only (9P over IPv6 is not offered by the library), so the
 * FIRST colon is the separator - there is no bracketed-literal form to
 * disambiguate.
 *
 * @a port is IN/OUT, which is what lets one splitter serve every verb that
 * names a server: on entry it carries the port to KEEP when @a spec names
 * none of its own - a resident verb's parsed `--port`, or
 * AXL_9P_PORT_DEFAULT for the one-shot verbs, which have no such flag. An
 * inline `:port` in @a spec OVERRIDES that value. The more specific form
 * wins, and it wins the same way for every verb, which is the guarantee
 * `src/9p/README.md` states.
 *
 * @return true on success (@a host and @a port are set); false on a NULL or
 *     empty @a spec, a port that does not parse or is zero, or a host part
 *     that is empty or does not fit @a host_cap. @a host and @a port are
 *     left indeterminate on false - callers report and bail.
 */
bool
axl9p_split_host_port(
    const char *spec,      ///< server address, `host` or `host:port`
    char       *host,      ///< [out] host part, NUL-terminated
    size_t      host_cap,  ///< capacity of @a host, in bytes
    uint16_t   *port       ///< [in,out] fallback port in, resolved port out
);

/**
 * @brief Print "<label>: running" or "<label>: stopped" for a service.
 * @return true if the service is running.
 */
bool
axl9p_report_service(
    const char       *label,   ///< name printed before the colon
    const AxlService *svc      ///< service whose residency to query
);

/**
 * @brief Unload a resident 9P service, reporting only what actually happened.
 *
 * Queries residency first, stops only a service that is running, and claims
 * success only after axl_service_stop returns AXL_OK - that ordering is the
 * point, so "stopped" is never printed for a stop that did not occur.
 * Not-running is not an error: it reports @a msgs->idle and succeeds, which
 * is what makes the verb idempotent.
 *
 * @return 0 on success or when it was not running, 1 when the stop failed.
 */
int
axl9p_stop_service(
    const AxlService    *svc,   ///< service whose driver image to unload
    const Axl9pStopMsgs *msgs   ///< the three outcome lines
);

// ---------------------------------------------------------------------------
// One-shot verbs - implemented in 9p-cmd-file.c
// ---------------------------------------------------------------------------

/// `--nic`, shared by all three one-shot verbs (they take the port inline).
extern const AxlArgDesc axl9p_file_flags[];

extern const AxlArgDesc axl9p_ls_positional[];
extern const AxlArgDesc axl9p_get_positional[];
extern const AxlArgDesc axl9p_put_positional[];

/// @brief `9p ls <host>[:port] [path]` - list a remote directory.
/// @return 0 on success, 1 on failure.
int
axl9p_ls_handler(
    AxlArgs *a   ///< parsed `ls` verb arguments
);

/// @brief `9p get <host>[:port] <path> [outfile]` - read a remote file.
/// @return 0 on success, 1 on failure.
int
axl9p_get_handler(
    AxlArgs *a   ///< parsed `get` verb arguments
);

/// @brief `9p put <infile> <host>[:port] <path>` - write a remote file.
/// @return 0 on success, 1 on failure.
int
axl9p_put_handler(
    AxlArgs *a   ///< parsed `put` verb arguments
);

// ---------------------------------------------------------------------------
// serve - handlers in 9p-cmd-serve.c
// ---------------------------------------------------------------------------

extern const AxlArgDesc axl9p_serve_flags[];
extern const AxlArgDesc axl9p_serve_positional[];

/// @brief `9p serve [root]` - deploy the resident 9P server driver.
/// @return 0 on success (or already serving), 1 on failure.
int
axl9p_serve_handler(
    AxlArgs *a   ///< parsed `serve` verb arguments
);

/// @brief `9p serve-stop` - unload the resident 9P server driver.
/// @return 0 on success or when it was not running, 1 on failure.
int
axl9p_serve_stop_handler(
    AxlArgs *a   ///< parsed `serve-stop` verb arguments (unused)
);

// ---------------------------------------------------------------------------
// mount - handlers in 9p-cmd-mount.c
// ---------------------------------------------------------------------------

extern const AxlArgDesc axl9p_mount_flags[];
extern const AxlArgDesc axl9p_mount_positional[];

/// @brief `9p mount <host>` - deploy the resident 9P mount driver.
/// @return 0 on success (or already mounted), 1 on failure.
int
axl9p_mount_handler(
    AxlArgs *a   ///< parsed `mount` verb arguments
);

/// @brief `9p umount` - unload the mount driver, tearing the volume down.
/// @return 0 on success or when it was not mounted, 1 on failure.
int
axl9p_umount_handler(
    AxlArgs *a   ///< parsed `umount` verb arguments (unused)
);

#endif /* AXL_TOOLS_9P_COMMON_H */
