/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/*
 * lsproto — list the UEFI protocols present in the system, by their canonical
 * spec names.
 *
 * The gap this fills: the Shell's `dh decode` prints its own SHORT labels
 * ("RamDisk") from a curated table, and only names a curated subset — a
 * protocol it does not know shows as a bare GUID. lsproto walks the live
 * handle database (LocateHandleBuffer + ProtocolsPerHandle) and names each
 * GUID with the exact identifier the UEFI/PI headers use
 * ("EFI_RAM_DISK_PROTOCOL"), from a table generated from the same spec
 * sources as AXL's own headers.
 *
 * Views:
 *   (default)          every protocol present, once each: name, GUID, handle count
 *   <pattern>          filter the name by a case-insensitive substring
 *   -p <name|guid>     the handles that expose one protocol
 *   -H, --by-handle    group by handle: each handle and the protocols on it
 *   -u, --unknown      only GUIDs with no known name (vendor / OEM protocols)
 *   -a, --all          the full known dictionary (present or not) — spec-named
 * Modifiers: -v verbose (handle names), -j JSON, --sort name|guid|count.
 */

#include <axl.h>

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------

static const AxlArgDesc flags[] = {
    { .name = "protocol",  .short_name = 'p', .type = AXL_ARG_STRING,
      .help = "Show the handles that expose one protocol (name or GUID)" },
    { .name = "by-handle", .short_name = 'H', .type = AXL_ARG_BOOL,
      .help = "Group by handle: each handle and the protocols on it" },
    { .name = "unknown",   .short_name = 'u', .type = AXL_ARG_BOOL,
      .help = "Only GUIDs with no known name (vendor / OEM protocols)" },
    { .name = "all",       .short_name = 'a', .type = AXL_ARG_BOOL,
      .help = "The full known protocol dictionary, present or not" },
    { .name = "verbose",   .short_name = 'v', .type = AXL_ARG_BOOL,
      .help = "Verbose: name each handle (component name / device path)" },
    { .name = "json",      .short_name = 'j', .type = AXL_ARG_BOOL,
      .help = "JSON output" },
    { .name = "sort",                         .type = AXL_ARG_STRING,
      .help = "Sort key: name (default), guid, or count" },
    {0}
};

static const AxlArgDesc positional[] = {
    { .name = "pattern", .type = AXL_ARG_STRING,
      .help = "Case-insensitive substring filter on the protocol name" },
    {0}
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

#define GUID_STR_MAX  37   /* 8-4-4-4-12 + NUL */
#define NAME_MAX      96

/* Canonical textual GUID. AxlGuid stores data1/2/3 as host integers, which is
   exactly the on-the-wire textual order, so a direct print is correct. */
static void
guid_to_str(const AxlGuid *g, char *out)
{
    axl_snprintf(out, GUID_STR_MAX,
                 "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 g->data1, g->data2, g->data3,
                 g->data4[0], g->data4[1], g->data4[2], g->data4[3],
                 g->data4[4], g->data4[5], g->data4[6], g->data4[7]);
}

/* Parse the canonical 8-4-4-4-12 form into @p g. Hyphens required; the last
   two groups are a straight byte string. Returns true on a clean full parse. */
static bool
guid_parse(const char *s, AxlGuid *g)
{
    unsigned d1;
    unsigned d2;
    unsigned d3;
    unsigned b[8];
    int n = axl_sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
                       &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3],
                       &b[4], &b[5], &b[6], &b[7]);
    if (n != 11) {
        return false;
    }
    g->data1 = (uint32_t)d1;
    g->data2 = (uint16_t)d2;
    g->data3 = (uint16_t)d3;
    for (int i = 0; i < 8; i++) {
        g->data4[i] = (uint8_t)b[i];
    }
    return true;
}

/* Name @p g into @p out (>= NAME_MAX). Returns true if it is a known protocol;
   false (and writes the raw GUID string) if not. */
static bool
name_of(const AxlGuid *g, char *out)
{
    if (axl_protocol_guid_name(g, out, NAME_MAX) == AXL_OK) {
        return true;
    }
    guid_to_str(g, out);
    return false;
}

/* Case-insensitive substring test. A NULL/empty needle matches everything. */
static bool
name_matches(const char *hay, const char *needle)
{
    if (needle == NULL || needle[0] == '\0') {
        return true;
    }
    return axl_strcasestr(hay, needle) != NULL;
}

/* Resolve a -p argument (a GUID string, else a protocol name) to a GUID.
   Returns true with @p out set. */
static bool
spec_to_guid(const char *spec, AxlGuid *out)
{
    if (guid_parse(spec, out)) {
        return true;
    }
    /* Not a GUID — match it against the dictionary by exact name,
       case-insensitively (a substring could name several protocols; -p wants
       exactly one). */
    size_t count = axl_protocol_name_count();
    for (size_t i = 0; i < count; i++) {
        const AxlGuid *g = NULL;
        const char    *nm = NULL;
        if (axl_protocol_name_at(i, &g, &nm) == AXL_OK
            && axl_strcasecmp(nm, spec) == 0)
        {
            *out = *g;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Present-protocol collection
// ---------------------------------------------------------------------------

typedef struct {
    AxlGuid   guid;
    uint32_t  handles;   ///< how many handles expose it
} ProtoRow;

/* Every handle in the database (heap; caller frees). */
static AxlHandle *
all_handles(size_t *count)
{
    size_t n = 0;
    if (axl_handle_list(NULL, NULL, 0, &n) != AXL_OK || n == 0) {
        *count = 0;
        return NULL;
    }
    AxlHandle *h = axl_malloc(n * sizeof(*h));
    if (h == NULL) {
        *count = 0;
        return NULL;
    }
    size_t got = 0;
    if (axl_handle_list(NULL, h, n, &got) != AXL_OK) {
        axl_free(h);
        *count = 0;
        return NULL;
    }
    *count = (got < n) ? got : n;
    return h;
}

/* Accumulate unique protocol GUIDs across all handles, counting how many
   handles expose each. Returns a heap array (caller frees) of @p *nrows rows. */
static ProtoRow *
collect_present(size_t *nrows)
{
    size_t     nh = 0;
    AxlHandle *handles = all_handles(&nh);
    *nrows = 0;
    if (handles == NULL) {
        return NULL;
    }

    ProtoRow *rows = NULL;
    size_t    cap = 0;
    size_t    used = 0;

    for (size_t i = 0; i < nh; i++) {
        size_t np = 0;
        if (axl_handle_protocols(handles[i], NULL, 0, &np) != AXL_OK || np == 0) {
            continue;
        }
        AxlGuid *pg = axl_malloc(np * sizeof(*pg));
        if (pg == NULL) {
            continue;
        }
        size_t got = 0;
        if (axl_handle_protocols(handles[i], pg, np, &got) != AXL_OK) {
            axl_free(pg);
            continue;
        }
        if (got > np) {
            got = np;
        }
        for (size_t k = 0; k < got; k++) {
            size_t j = 0;
            for (; j < used; j++) {
                if (axl_guid_equal(&rows[j].guid, &pg[k])) {
                    rows[j].handles++;
                    break;
                }
            }
            if (j < used) {
                continue;
            }
            if (used == cap) {
                size_t ncap = (cap == 0) ? 64 : cap * 2;
                ProtoRow *nr = axl_realloc(rows, ncap * sizeof(*nr));
                if (nr == NULL) {
                    break;
                }
                rows = nr;
                cap = ncap;
            }
            rows[used].guid = pg[k];
            rows[used].handles = 1;
            used++;
        }
        axl_free(pg);
    }

    axl_free(handles);
    *nrows = used;
    return rows;
}

// ---------------------------------------------------------------------------
// Sorting
// ---------------------------------------------------------------------------

typedef enum { SORT_NAME, SORT_GUID, SORT_COUNT } SortKey;

static SortKey g_sort = SORT_NAME;

static int
cmp_rows(const void *pa, const void *pb)
{
    const ProtoRow *a = pa;
    const ProtoRow *b = pb;
    if (g_sort == SORT_GUID) {
        return axl_guid_cmp(&a->guid, &b->guid);
    }
    if (g_sort == SORT_COUNT) {
        /* Descending by count; ties fall through to name for stability. */
        if (a->handles != b->handles) {
            return (a->handles > b->handles) ? -1 : 1;
        }
    }
    char na[NAME_MAX];
    char nb[NAME_MAX];
    name_of(&a->guid, na);
    name_of(&b->guid, nb);
    int c = axl_strcmp(na, nb);
    if (c != 0) {
        return c;
    }
    return axl_guid_cmp(&a->guid, &b->guid);
}

// ---------------------------------------------------------------------------
// Renderers
// ---------------------------------------------------------------------------

/* Default / -u / pattern view: one line per present protocol. */
static int
render_present(const char *pattern, bool unknown_only, bool verbose, bool json)
{
    (void)verbose;
    size_t    n = 0;
    ProtoRow *rows = collect_present(&n);
    if (rows == NULL && n == 0) {
        if (json) {
            axl_printf("[]\n");
        } else {
            axl_printf("No protocols found.\n");
        }
        return 0;
    }
    axl_qsort(rows, n, sizeof(*rows), cmp_rows);

    AxlString *js = NULL;
    AxlJsonWriter jw;
    if (json) {
        js = axl_string_new("");
        axl_json_writer_init(&jw, js, 0);
        axl_json_arr_begin(&jw);
    }

    size_t shown = 0;
    for (size_t i = 0; i < n; i++) {
        char name[NAME_MAX];
        bool known = name_of(&rows[i].guid, name);
        if (unknown_only && known) {
            continue;
        }
        if (!name_matches(name, pattern)) {
            continue;
        }
        char gs[GUID_STR_MAX];
        guid_to_str(&rows[i].guid, gs);
        shown++;

        if (json) {
            axl_json_obj_begin(&jw);
            axl_json_key(&jw, "name");
            axl_json_str(&jw, known ? name : "");
            axl_json_key(&jw, "guid");
            axl_json_str(&jw, gs);
            axl_json_key(&jw, "handles");
            axl_json_uint(&jw, rows[i].handles);
            axl_json_obj_end(&jw);
        } else {
            axl_printf("%-44s %s  %u\n", name, gs, rows[i].handles);
        }
    }

    if (json) {
        axl_json_arr_end(&jw);
        axl_json_writer_finish(&jw);
        axl_printf("%s\n", axl_string_str(js));
        axl_string_free(js);
    } else if (shown == 0) {
        axl_printf("No matching protocols.\n");
    }
    axl_free(rows);
    return 0;
}

/* -p <spec>: the handles that expose one protocol. */
static int
render_by_protocol(const char *spec, bool verbose, bool json)
{
    AxlGuid g;
    if (!spec_to_guid(spec, &g)) {
        axl_printerr("lsproto: not a known protocol name or GUID: %s\n", spec);
        return 1;
    }
    char name[NAME_MAX];
    name_of(&g, name);
    char gs[GUID_STR_MAX];
    guid_to_str(&g, gs);

    size_t n = 0;
    if (axl_handle_list(&g, NULL, 0, &n) != AXL_OK) {
        axl_printerr("lsproto: enumeration failed\n");
        return 1;
    }
    AxlHandle *h = (n > 0) ? axl_malloc(n * sizeof(*h)) : NULL;
    size_t got = 0;
    if (h != NULL) {
        axl_handle_list(&g, h, n, &got);
        if (got > n) {
            got = n;
        }
    }

    if (json) {
        AxlString *js = axl_string_new("");
        AxlJsonWriter jw;
        axl_json_writer_init(&jw, js, 0);
        axl_json_obj_begin(&jw);
        axl_json_key(&jw, "name");
        axl_json_str(&jw, name);
        axl_json_key(&jw, "guid");
        axl_json_str(&jw, gs);
        axl_json_key(&jw, "count");
        axl_json_uint(&jw, (uint32_t)got);
        axl_json_key(&jw, "handles");
        axl_json_arr_begin(&jw);
        for (size_t i = 0; i < got; i++) {
            char hn[128];
            axl_json_obj_begin(&jw);
            axl_json_key(&jw, "index");
            axl_json_uint(&jw, (uint32_t)i);
            if (axl_handle_name(h[i], hn, sizeof(hn)) == AXL_OK) {
                axl_json_key(&jw, "name");
                axl_json_str(&jw, hn);
            }
            axl_json_obj_end(&jw);
        }
        axl_json_arr_end(&jw);
        axl_json_obj_end(&jw);
        axl_json_writer_finish(&jw);
        axl_printf("%s\n", axl_string_str(js));
        axl_string_free(js);
    } else {
        axl_printf("%s  %s\n", name, gs);
        axl_printf("%zu handle%s\n", got, got == 1 ? "" : "s");
        for (size_t i = 0; i < got; i++) {
            char hn[128];
            if (verbose && axl_handle_name(h[i], hn, sizeof(hn)) == AXL_OK) {
                axl_printf("  [%zu] %s\n", i, hn);
            } else {
                axl_printf("  [%zu]\n", i);
            }
        }
    }
    axl_free(h);
    return 0;
}

/* -H: group by handle. */
static int
render_by_handle(const char *pattern, bool verbose, bool json)
{
    size_t     nh = 0;
    AxlHandle *handles = all_handles(&nh);
    if (handles == NULL) {
        axl_printf(json ? "[]\n" : "No handles found.\n");
        return 0;
    }

    AxlString *js = NULL;
    AxlJsonWriter jw;
    if (json) {
        js = axl_string_new("");
        axl_json_writer_init(&jw, js, 0);
        axl_json_arr_begin(&jw);
    }

    for (size_t i = 0; i < nh; i++) {
        size_t np = 0;
        if (axl_handle_protocols(handles[i], NULL, 0, &np) != AXL_OK || np == 0) {
            continue;
        }
        AxlGuid *pg = axl_malloc(np * sizeof(*pg));
        if (pg == NULL) {
            continue;
        }
        size_t got = 0;
        axl_handle_protocols(handles[i], pg, np, &got);
        if (got > np) {
            got = np;
        }

        /* When a pattern is set, only show handles that carry a matching
           protocol — and only those protocols. */
        char hn[128];
        bool have_hn = (axl_handle_name(handles[i], hn, sizeof(hn)) == AXL_OK);

        if (json) {
            /* Buffer the handle's matching protocols first so an empty match
               under a pattern does not emit a hollow handle object. */
            size_t matched = 0;
            for (size_t k = 0; k < got; k++) {
                char nm[NAME_MAX];
                name_of(&pg[k], nm);
                if (name_matches(nm, pattern)) {
                    matched++;
                }
            }
            if (matched > 0) {
                axl_json_obj_begin(&jw);
                axl_json_key(&jw, "index");
                axl_json_uint(&jw, (uint32_t)i);
                if (have_hn) {
                    axl_json_key(&jw, "handle");
                    axl_json_str(&jw, hn);
                }
                axl_json_key(&jw, "protocols");
                axl_json_arr_begin(&jw);
                for (size_t k = 0; k < got; k++) {
                    char nm[NAME_MAX];
                    bool known = name_of(&pg[k], nm);
                    if (!name_matches(nm, pattern)) {
                        continue;
                    }
                    char gs[GUID_STR_MAX];
                    guid_to_str(&pg[k], gs);
                    axl_json_obj_begin(&jw);
                    axl_json_key(&jw, "name");
                    axl_json_str(&jw, known ? nm : "");
                    axl_json_key(&jw, "guid");
                    axl_json_str(&jw, gs);
                    axl_json_obj_end(&jw);
                }
                axl_json_arr_end(&jw);
                axl_json_obj_end(&jw);
            }
        } else {
            size_t matched = 0;
            for (size_t k = 0; k < got; k++) {
                char nm[NAME_MAX];
                name_of(&pg[k], nm);
                if (name_matches(nm, pattern)) {
                    matched++;
                }
            }
            if (matched > 0) {
                if (verbose && have_hn) {
                    axl_printf("[%zu] %s\n", i, hn);
                } else {
                    axl_printf("[%zu]\n", i);
                }
                for (size_t k = 0; k < got; k++) {
                    char nm[NAME_MAX];
                    name_of(&pg[k], nm);
                    if (!name_matches(nm, pattern)) {
                        continue;
                    }
                    axl_printf("    %s\n", nm);
                }
            }
        }
        axl_free(pg);
    }

    if (json) {
        axl_json_arr_end(&jw);
        axl_json_writer_finish(&jw);
        axl_printf("%s\n", axl_string_str(js));
        axl_string_free(js);
    }
    axl_free(handles);
    return 0;
}

/* -a: the full known dictionary (present or not). */
static int
render_dictionary(const char *pattern, bool json)
{
    size_t count = axl_protocol_name_count();

    AxlString *js = NULL;
    AxlJsonWriter jw;
    if (json) {
        js = axl_string_new("");
        axl_json_writer_init(&jw, js, 0);
        axl_json_arr_begin(&jw);
    }
    for (size_t i = 0; i < count; i++) {
        const AxlGuid *g = NULL;
        const char    *nm = NULL;
        if (axl_protocol_name_at(i, &g, &nm) != AXL_OK) {
            continue;
        }
        if (!name_matches(nm, pattern)) {
            continue;
        }
        char gs[GUID_STR_MAX];
        guid_to_str(g, gs);
        if (json) {
            axl_json_obj_begin(&jw);
            axl_json_key(&jw, "name");
            axl_json_str(&jw, nm);
            axl_json_key(&jw, "guid");
            axl_json_str(&jw, gs);
            axl_json_obj_end(&jw);
        } else {
            axl_printf("%-44s %s\n", nm, gs);
        }
    }
    if (json) {
        axl_json_arr_end(&jw);
        axl_json_writer_finish(&jw);
        axl_printf("%s\n", axl_string_str(js));
        axl_string_free(js);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

static int
run_lsproto(AxlArgs *a)
{
    const char *pattern  = axl_args_get_string(a, "pattern");
    const char *protocol = axl_args_get_string(a, "protocol");
    bool        by_hand  = axl_args_get_bool(a, "by-handle");
    bool        unknown  = axl_args_get_bool(a, "unknown");
    bool        all      = axl_args_get_bool(a, "all");
    bool        verbose  = axl_args_get_bool(a, "verbose");
    bool        json     = axl_args_get_bool(a, "json");

    const char *sort = axl_args_get_string(a, "sort");
    if (sort != NULL) {
        if (axl_strcmp(sort, "name") == 0) {
            g_sort = SORT_NAME;
        } else if (axl_strcmp(sort, "guid") == 0) {
            g_sort = SORT_GUID;
        } else if (axl_strcmp(sort, "count") == 0) {
            g_sort = SORT_COUNT;
        } else {
            axl_printerr("lsproto: bad --sort '%s' (name|guid|count)\n", sort);
            return 1;
        }
    }

    if (protocol != NULL) {
        return render_by_protocol(protocol, verbose, json);
    }
    if (all) {
        return render_dictionary(pattern, json);
    }
    if (by_hand) {
        return render_by_handle(pattern, verbose, json);
    }
    return render_present(pattern, unknown, verbose, json);
}

AXL_TOOL_MAIN(lsproto)
{
    return axl_args_run(argc, argv, &(AxlArgsNode){
        .name        = "lsproto",
        .help        = "List UEFI protocols present in the system, by spec name",
        .flags       = flags,
        .positionals = positional,
        .handler     = run_lsproto,
    });
}
