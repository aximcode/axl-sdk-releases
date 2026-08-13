/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file axl-driver-deps.h
 *
 * Transitive driver-dependency resolution over a JSON5 sidecar.
 *
 * Some firmware drivers only produce a usable device once another
 * driver is already resident — a USB-NIC whose personality driver
 * (RNDIS/CDC) must load first, an option-ROM that needs a bus shim, a
 * storage HBA that layers on a transport. A staging tool that loads
 * drivers one at a time has no way to know that ordering from the
 * `.efi` files alone. This module reads it from a small JSON5 *sidecar*
 * (see @ref axl-sidecar.h) that ships next to the drivers and declares,
 * per driver, the dependency driver(s) it needs co-resident:
 *
 * @code{.js}
 *   { schema: 1, drivers: [
 *       { name: 'Nic.efi',  requires: [ 'Comp.efi' ] },
 *       { name: 'Comp.efi', requires: [ 'SubComp.efi' ] }
 *   ] }
 * @endcode
 *
 * The library owns the two generic halves — parsing the sidecar into an
 * @ref AxlDriverDeps table, and walking a target driver's dependency
 * subtree in the correct (dependencies-first) order with cycle
 * protection. It owns *no* firmware policy: what "bring a dependency
 * resident" means (load, breadcrumb, quarantine-check, log) stays with
 * the caller, supplied as an @ref AxlDriverDepVisitor. That keeps the
 * on-disk sidecar format caller-defined too — the schema tag and the
 * filename are both parameters (@ref axl_driver_deps_load), so this is
 * not tied to any one tool's file.
 *
 * @code
 * static bool my_enter(const char *dep, const char *parent, void *ctx) {
 *     if (already_resident(dep)) return false;          // skip subtree
 *     return true;                                      // descend + load
 * }
 * static void my_load(const char *dep, const char *parent, void *ctx) {
 *     load_driver_from((const char *)ctx, dep);         // ctx = directory
 * }
 *
 * AxlDriverDeps deps;
 * if (axl_driver_deps_load("fs0:\\drivers\\x64",
 *                          "my-drivers.json5", "mytool",
 *                          &deps) == AXL_SIDECAR_OK) {
 *     AxlDriverDepVisitor v = { my_enter, my_load, (void *)dir };
 *     axl_driver_deps_walk(&deps, "Nic.efi", &v);       // loads Comp, then Nic's dep chain
 * }
 * @endcode
 */

#ifndef AXL_DRIVER_DEPS_H
#define AXL_DRIVER_DEPS_H

#include <axl/axl-macros.h>   /* AXL_CB_NOEXCEPT on callback declarations */

#include <stddef.h>
#include <stdbool.h>

#include <axl/axl-sidecar.h>   /* AxlSidecarStatus */

#ifdef __cplusplus
extern "C" {
#endif

/// Maximum driver entries (rows) a single sidecar may declare.
#define AXL_DRIVER_DEPS_MAX        32
/// Maximum dependency drivers one driver may declare in its `requires` list.
#define AXL_DRIVER_DEPS_PER_NODE   4
/// Bound (including NUL) on a driver basename in the table.
#define AXL_DRIVER_DEP_NAME_MAX    64

/**
 * @brief One parsed sidecar row: a driver plus the driver(s) it requires.
 *
 * A row is produced for every `{ name, requires }` object in the
 * sidecar's `drivers` array. @c needs is the list of dependency
 * driver basenames (the JSON `requires` array) that must be brought
 * resident before @c name binds. A driver with an empty or absent
 * `requires` still yields a row (with @c n_needs == 0) so callers can
 * tell "declared, self-contained" from "not in the sidecar at all".
 */
typedef struct {
    char   name[AXL_DRIVER_DEP_NAME_MAX];                           ///< the declaring driver's basename
    char   needs[AXL_DRIVER_DEPS_PER_NODE][AXL_DRIVER_DEP_NAME_MAX]; ///< dependency basenames, in declared order (the JSON `requires` list)
    size_t n_needs;                                                 ///< number of valid entries in @c needs
} AxlDriverDepRow;

/**
 * @brief A parsed dependency table — the whole sidecar, as fixed rows.
 *
 * Bounded and caller-allocated (stack or static), matching AXL's other
 * bounded-parse types: no heap, no cleanup call. Populate it with
 * @ref axl_driver_deps_load (from a file) or
 * @ref axl_driver_deps_load_buffer (from memory), then query it with
 * @ref axl_driver_deps_lookup / @ref axl_driver_deps_is_required or
 * traverse it with @ref axl_driver_deps_walk. A zeroed table
 * (@c n_rows == 0) is valid and means "no sidecar / no dependencies" —
 * every query returns the empty answer and every walk is a no-op.
 */
typedef struct {
    AxlDriverDepRow rows[AXL_DRIVER_DEPS_MAX];  ///< parsed rows
    size_t          n_rows;                     ///< number of valid rows
} AxlDriverDeps;

/**
 * @brief Caller policy for a dependency walk — what "bring resident" means.
 *
 * @ref axl_driver_deps_walk owns the graph traversal (order, cycle
 * protection, dedup within the walk); this struct owns everything
 * firmware-specific. Both callbacks receive @c parent — the driver
 * whose `requires` list named @c name — so the caller can attribute a
 * dependency to the exact driver that needs it (progress messages,
 * "needed by X" wording).
 */
typedef struct {
    /**
     * Decide whether to process dependency @p name (required by
     * @p parent). Called once per edge, BEFORE @p name's own
     * dependencies are visited. Return @c true to descend into
     * @p name's subtree and then @c load it; return @c false to skip
     * @p name AND its whole subtree — the caller's hook for "already
     * resident", "quarantined", or any other reason not to bring it in.
     * Optional (NULL ⇒ always descend + load). The walk protects
     * against cycles on its own, so returning @c true on a cyclic graph
     * is safe.
     */
    bool (*enter)(const char *name, const char *parent, void *ctx) AXL_CB_NOEXCEPT;

    /**
     * Bring dependency @p name (required by @p parent) resident. Called
     * AFTER @p name's own dependencies have been walked (post-order), so
     * the whole subtree is resident before @p name — and, ultimately,
     * before the walk's target — binds. Called only for names @c enter
     * returned @c true for (or all names, if @c enter is NULL). Required.
     */
    void (*load)(const char *name, const char *parent, void *ctx) AXL_CB_NOEXCEPT;

    void *ctx;   ///< borrowed context passed to both callbacks (NULL if unused)
} AxlDriverDepVisitor;

/**
 * @brief Parse a driver-dependency sidecar from a file into @p out.
 *
 * Opens `<@p dir>\<@p filename>` via @ref axl_sidecar_open_file,
 * validates the required `schema` field against @p schema_tag (schema
 * version 1), and walks the `drivers` array into @p out. Each array
 * object contributes one @ref AxlDriverDepRow "AxlDriverDepRow": its `name` string and
 * its `requires` array of dependency basenames.
 *
 * The on-disk format is caller-defined: @p filename and @p schema_tag
 * are both parameters, so two tools with different sidecars never
 * collide and neither name is baked into the library. Bounds
 * (@ref AXL_DRIVER_DEPS_MAX rows, @ref AXL_DRIVER_DEPS_PER_NODE
 * dependencies each, @ref AXL_DRIVER_DEP_NAME_MAX "AXL_DRIVER_DEP_NAME_MAX"-byte names) are
 * enforced by dropping the overflow with a warning; a nameless entry or
 * an empty dependency string is skipped. @p out is zeroed first, so a
 * missing or malformed file leaves it empty (@c n_rows == 0) — the safe
 * "no dependencies" state.
 *
 * @return @c AXL_SIDECAR_OK on a successful parse (possibly zero rows),
 *     @c AXL_SIDECAR_FILE_MISSING if the sidecar does not exist (not an
 *     error — the common standalone case), @c AXL_SIDECAR_PARSE_ERROR if
 *     the file exists but is malformed JSON5 or fails schema validation.
 *     @p out is left empty on any non-OK return.
 */
AxlSidecarStatus
axl_driver_deps_load(
    const char     *dir,         ///< directory holding the sidecar (UTF-8)
    const char     *filename,    ///< sidecar basename, e.g. "netload-drivers.json5"
    const char     *schema_tag,  ///< schema tag the file must declare (diagnostic + version gate)
    AxlDriverDeps  *out          ///< [out] parsed table; zeroed first
);

/**
 * @brief Parse a driver-dependency sidecar from a memory buffer.
 *
 * Buffer-source counterpart to @ref axl_driver_deps_load — no
 * filesystem, so there is no @c AXL_SIDECAR_FILE_MISSING return. Useful
 * for embedded fixtures and unit tests. Same schema/shape/bounds rules.
 *
 * @return @c AXL_SIDECAR_OK on a successful parse (possibly zero rows),
 *     @c AXL_SIDECAR_PARSE_ERROR on bad arguments, malformed JSON5, or
 *     schema-validation failure. @p out is left empty on a non-OK return.
 */
AxlSidecarStatus
axl_driver_deps_load_buffer(
    const char     *json5,       ///< JSON5 source (no NUL required)
    size_t          len,         ///< buffer length in bytes
    const char     *schema_tag,  ///< schema tag the file must declare
    AxlDriverDeps  *out          ///< [out] parsed table; zeroed first
);

/**
 * @brief Find the row a driver basename declares, or NULL.
 *
 * Returns the @ref AxlDriverDepRow whose @c name equals @p name — i.e.
 * the driver's own dependency declaration. NULL means @p name is not a
 * declaring driver in this table (it may still appear as *another*
 * driver's dependency; use @ref axl_driver_deps_is_required for that).
 *
 * @return borrowed pointer into @p deps (valid while @p deps is), or
 *     NULL if @p deps is NULL, @p name is NULL, or no row matches.
 */
const AxlDriverDepRow *
axl_driver_deps_lookup(
    const AxlDriverDeps  *deps,  ///< parsed table
    const char           *name   ///< driver basename to look up
);

/**
 * @brief Is @p name required by some other driver in the table?
 *
 * True when @p name appears in any row's `requires` list — i.e. it is a
 * dependency driver, auto-loaded on demand rather than a top-level pick.
 * This holds even for a *mid-tree* node: a dependency that itself
 * declares dependencies (so also appears as a row `name`) is still a
 * dependency, because being required by something makes it auto-loaded.
 *
 * @return true if @p name is listed as a dependency anywhere in @p deps;
 *     false otherwise (including NULL arguments).
 */
bool
axl_driver_deps_is_required(
    const AxlDriverDeps  *deps,  ///< parsed table
    const char           *name   ///< driver basename to test
);

/**
 * @brief Walk @p target's transitive dependencies, dependencies-first.
 *
 * Resolves the dependency subtree rooted at @p target and drives
 * @p visitor over it in post-order: for each dependency the subtree is
 * fully processed before the dependency's own @c load fires, so a driver
 * is always brought resident after everything it requires and before
 * anything that requires it. @p target itself is not loaded — the caller
 * loads it after the walk returns; the walk only handles what must come
 * first.
 *
 * Traversal is driven by @p visitor: @c enter decides per edge whether
 * to descend into and load a dependency (skip its whole subtree on
 * @c false), and @c load brings an entered dependency resident. See
 * @ref AxlDriverDepVisitor. Both receive the immediate @c parent.
 *
 * **Cycle protection.** The walk keeps its own visited set and never
 * descends into a node twice, so a sidecar that (mis)declares a cycle
 * (A requires B requires A) terminates instead of recursing forever —
 * independent of whatever bookkeeping @c enter does. A node already
 * visited in this walk is silently skipped (no @c enter, no @c load).
 *
 * No-op when @p deps, @p target, or @p visitor->load is NULL, or when
 * @p target declares no dependencies.
 */
void
axl_driver_deps_walk(
    const AxlDriverDeps        *deps,    ///< parsed table
    const char                 *target,  ///< driver whose dependencies to bring resident
    const AxlDriverDepVisitor  *visitor  ///< caller policy (load required; enter optional)
);

#ifdef __cplusplus
}
#endif

#endif /* AXL_DRIVER_DEPS_H */
