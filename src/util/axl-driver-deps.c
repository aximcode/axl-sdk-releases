/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-driver-deps.c
    Transitive driver-dependency resolution over a JSON5 sidecar.

    The library owns the two generic halves: parsing the sidecar
    (`{ schema, drivers: [ { name, requires } ] }`) into an
    AxlDriverDeps table, and walking a target driver's dependency
    subtree in dependencies-first order with cycle protection. What
    "bring a dependency resident" means stays with the caller, driven
    through an AxlDriverDepVisitor. See axl/axl-driver-deps.h.
**/

#include <axl/axl-driver-deps.h>

#include <axl/axl-sidecar.h>
#include <axl/axl-json.h>
#include <axl/axl-str.h>
#include <axl/axl-string.h>
#include <axl/axl-mem.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("driver-deps");

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------

/* Walk a schema-validated reader at the document root into @out. @label names
   the source in overflow warnings (a filename for the file path, the schema
   tag for a buffer). @out is assumed already zeroed by the public entry. */
static AxlSidecarStatus
deps_parse_reader(
    AxlJsonReader  *r,
    const char     *schema_tag,
    const char     *label,
    AxlDriverDeps  *out
)
{
    static const uint64_t accepted[] = { 1 };
    uint64_t schema = 0;
    if (axl_sidecar_check_schema(r, schema_tag, accepted,
                                 sizeof accepted / sizeof accepted[0],
                                 &schema) != AXL_SIDECAR_OK) {
        return AXL_SIDECAR_PARSE_ERROR;   /* check_schema already warned */
    }

    AxlJsonArrayIter it;
    if (axl_json_array_begin(r, "drivers", &it)) {
        AxlJsonReader entry;
        while (axl_json_array_next(&it, &entry)) {
            if (out->n_rows >= AXL_DRIVER_DEPS_MAX) {
                axl_warning("%s: more than %d driver entries -- ignoring the rest",
                            label, AXL_DRIVER_DEPS_MAX);
                break;
            }
            AxlDriverDepRow *d = &out->rows[out->n_rows];
            d->name[0] = '\0';
            d->n_needs = 0;
            if (!axl_json_get_string(&entry, "name", d->name, sizeof d->name)
                || d->name[0] == '\0') {
                continue;                 /* skip a nameless entry */
            }
            AxlJsonArrayIter cit;
            if (axl_json_array_begin(&entry, "requires", &cit)) {
                AxlJsonReader celem;
                while (axl_json_array_next(&cit, &celem)) {
                    char c[AXL_DRIVER_DEP_NAME_MAX] = {0};
                    if (!axl_json_value_string(&celem, c, sizeof c) || !c[0]) {
                        continue;
                    }
                    if (d->n_needs >= AXL_DRIVER_DEPS_PER_NODE) {
                        axl_warning("%s: %s lists more than %d dependencies -- "
                                    "ignoring the rest", label, d->name,
                                    AXL_DRIVER_DEPS_PER_NODE);
                        break;
                    }
                    axl_strlcpy(d->needs[d->n_needs], c, AXL_DRIVER_DEP_NAME_MAX);
                    d->n_needs++;
                }
            }
            out->n_rows++;
        }
    }
    return AXL_SIDECAR_OK;
}

AxlSidecarStatus
axl_driver_deps_load(
    const char     *dir,
    const char     *filename,
    const char     *schema_tag,
    AxlDriverDeps  *out
)
{
    if (out == NULL) {
        return AXL_SIDECAR_PARSE_ERROR;
    }
    axl_memset(out, 0, sizeof *out);
    if (dir == NULL || filename == NULL || schema_tag == NULL) {
        return AXL_SIDECAR_PARSE_ERROR;
    }

    char path[300];
    axl_snprintf(path, sizeof path, "%s\\%s", dir, filename);

    AxlJsonReader r;
    void *raw = NULL;
    AxlSidecarStatus rc = axl_sidecar_open_file(path, &r, &raw);
    if (rc != AXL_SIDECAR_OK) {
        return rc;                        /* FILE_MISSING (silent) or PARSE_ERROR */
    }

    rc = deps_parse_reader(&r, schema_tag, filename, out);
    axl_json_free(&r);
    axl_free(raw);
    if (rc != AXL_SIDECAR_OK) {
        axl_memset(out, 0, sizeof *out);  /* leave the table empty on schema failure */
    }
    return rc;
}

AxlSidecarStatus
axl_driver_deps_load_buffer(
    const char     *json5,
    size_t          len,
    const char     *schema_tag,
    AxlDriverDeps  *out
)
{
    if (out == NULL) {
        return AXL_SIDECAR_PARSE_ERROR;
    }
    axl_memset(out, 0, sizeof *out);
    if (json5 == NULL || schema_tag == NULL) {
        return AXL_SIDECAR_PARSE_ERROR;
    }

    AxlJsonReader r;
    AxlSidecarStatus rc = axl_sidecar_open_buffer(json5, len, &r);
    if (rc != AXL_SIDECAR_OK) {
        return rc;
    }

    rc = deps_parse_reader(&r, schema_tag, schema_tag, out);
    axl_json_free(&r);
    if (rc != AXL_SIDECAR_OK) {
        axl_memset(out, 0, sizeof *out);
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

const AxlDriverDepRow *
axl_driver_deps_lookup(
    const AxlDriverDeps  *deps,
    const char           *name
)
{
    if (deps == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < deps->n_rows; i++) {
        if (axl_strcmp(deps->rows[i].name, name) == 0) {
            return &deps->rows[i];
        }
    }
    return NULL;
}

bool
axl_driver_deps_is_required(
    const AxlDriverDeps  *deps,
    const char           *name
)
{
    if (deps == NULL || name == NULL) {
        return false;
    }
    for (size_t i = 0; i < deps->n_rows; i++) {
        for (size_t j = 0; j < deps->rows[i].n_needs; j++) {
            if (axl_strcmp(deps->rows[i].needs[j], name) == 0) {
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Transitive walk
// ---------------------------------------------------------------------------

/* Upper bound on distinct nodes a single walk can enter -- every declared
   dependency edge across the whole table. Sized so the visited set can never
   overflow, which is what makes cycle termination unconditional. */
#define DEPS_VISITED_MAX  (AXL_DRIVER_DEPS_MAX * AXL_DRIVER_DEPS_PER_NODE)

static void
deps_walk_node(
    const AxlDriverDeps        *deps,
    const char                 *node,
    const AxlDriverDepVisitor  *v,
    char                        visited[][AXL_DRIVER_DEP_NAME_MAX],
    size_t                     *n_visited
)
{
    const AxlDriverDepRow *row = axl_driver_deps_lookup(deps, node);
    if (row == NULL) {
        return;                           /* a leaf: nothing to bring first */
    }
    for (size_t i = 0; i < row->n_needs; i++) {
        const char *dep = row->needs[i];

        /* Cycle guard: never descend into a node twice in one walk. */
        bool seen = false;
        for (size_t k = 0; k < *n_visited; k++) {
            if (axl_strcmp(visited[k], dep) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }

        /* Caller decides whether to bring this dependency (and its subtree)
           in at all -- the skip / quarantine / already-resident hook. */
        if (v->enter != NULL && !v->enter(dep, node, v->ctx)) {
            continue;
        }

        /* Mark visited BEFORE descending so a back-edge terminates. */
        if (*n_visited < DEPS_VISITED_MAX) {
            axl_strlcpy(visited[*n_visited], dep, AXL_DRIVER_DEP_NAME_MAX);
            (*n_visited)++;
        }
        deps_walk_node(deps, dep, v, visited, n_visited);   /* subtree first */
        v->load(dep, node, v->ctx);                          /* then this dep */
    }
}

void
axl_driver_deps_walk(
    const AxlDriverDeps        *deps,
    const char                 *target,
    const AxlDriverDepVisitor  *visitor
)
{
    if (deps == NULL || target == NULL || visitor == NULL || visitor->load == NULL) {
        return;
    }
    char   visited[DEPS_VISITED_MAX][AXL_DRIVER_DEP_NAME_MAX];
    size_t n_visited = 0;
    deps_walk_node(deps, target, visitor, visited, &n_visited);
}
