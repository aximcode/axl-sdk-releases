/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-args.c
    AxlArgs — declarative command-line parser for AXL tools.

    A tool declares a static @ref AxlArgsNode tree (program name +
    flags + (positionals + handler) for leaves, or (verbs) for
    branches) and calls @ref axl_args_run from main. The framework
    parses argv, validates types and bounds, generates `--help`,
    and dispatches to the matching leaf handler.

    Recursion model: the same AxlArgsNode shape describes the root
    and every inner branch and leaf. axl_args_run is a thin wrapper
    around an internal `args_run_internal` that carries a
    breadcrumb path and a parent AxlArgs pointer for accessor walks.
    A branch consumes flags + the verb-name positional, then
    recurses into the matched child node with the remaining argv
    slice; a leaf consumes flags + positionals at this level and
    invokes its handler.

    Design choices kept from v1:
      - Linear flag/positional storage (N is small at every level;
        no hash table).
      - Single-pass parse; verb resolved on first non-flag.
      - Validation runs at parse time so handlers see good data only.
**/

#include <axl/axl-args.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-array.h>
#include <axl/axl-stream.h>
#include <axl/axl-log.h>

AXL_LOG_DOMAIN("args");

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

typedef struct {
    const AxlArgDesc *desc;          ///< back-pointer to descriptor
    bool              set;           ///< was this flag/pos provided?
    const char       *str_value;     ///< STRING / first MULTI value
    uint64_t          uint_value;    ///< U8..U64
    int64_t           int_value;     ///< S64
    AxlArray         *multi_values;  ///< MULTI: array of const char *
} ParsedArg;

struct AxlArgs {
    const AxlArgsNode *node;          ///< the node being parsed at this level
    const char        *path;          ///< full breadcrumb ("mytool bios pci"), borrowed
    AxlArgs           *parent;        ///< enclosing level, NULL at root

    /* Linear arrays of parsed slots: one per descriptor in
       node->flags + (node->positionals if leaf). */
    ParsedArg         *slots;
    int                slot_count;

    AxlArray          *variadic;      ///< collected variadic tail (const char *)
    int                next_named_pos; ///< next positional descriptor to fill
};

// ---------------------------------------------------------------------------
// Forward decls
// ---------------------------------------------------------------------------

static int args_run_internal(int argc, char **argv,
                             const AxlArgsNode *node,
                             const char *parent_path,
                             AxlArgs *parent_args);

static void print_help_for(const AxlArgsNode *node, const char *path);

static bool parse_typed(ParsedArg *slot, const char *value, const char *path);

// ---------------------------------------------------------------------------
// Descriptor / verb helpers
// ---------------------------------------------------------------------------

static int
desc_count(const AxlArgDesc *list)
{
    int n = 0;
    if (list != NULL) {
        while (list[n].name != NULL) {
            n++;
        }
    }
    return n;
}

static const AxlArgsNode *
find_verb(const AxlArgsNode *node, const char *name)
{
    if (node->verbs == NULL || name == NULL) {
        return NULL;
    }
    for (int i = 0; node->verbs[i].name != NULL; i++) {
        if (axl_strcmp(node->verbs[i].name, name) == 0) {
            return &node->verbs[i];
        }
    }
    return NULL;
}

static bool
node_is_leaf(const AxlArgsNode *node)
{
    return node->handler != NULL;
}

static bool
node_is_branch(const AxlArgsNode *node)
{
    return node->verbs != NULL;
}

// ---------------------------------------------------------------------------
// Slot lookup (linear; N is small)
// ---------------------------------------------------------------------------

static ParsedArg *
slot_by_name_local(AxlArgs *a, const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < a->slot_count; i++) {
        if (a->slots[i].desc != NULL
            && axl_strcmp(a->slots[i].desc->name, name) == 0)
        {
            return &a->slots[i];
        }
    }
    return NULL;
}

/* Walk this level + every parent for a slot matching @p name.
   Parent visibility is the whole point of the nested model — a
   leaf can read --verbose declared on the root via the same
   accessor it uses for its own positionals. */
static ParsedArg *
slot_by_name(AxlArgs *a, const char *name)
{
    while (a != NULL) {
        ParsedArg *s = slot_by_name_local(a, name);
        if (s != NULL) {
            return s;
        }
        a = a->parent;
    }
    return NULL;
}

static ParsedArg *
slot_by_long_local(AxlArgs *a, const char *key, size_t key_len)
{
    for (int i = 0; i < a->slot_count; i++) {
        const AxlArgDesc *d = a->slots[i].desc;
        if (d == NULL || d->name == NULL) {
            continue;
        }
        size_t n = axl_strlen(d->name);
        if (n == key_len && axl_strncmp(d->name, key, n) == 0) {
            return &a->slots[i];
        }
    }
    return NULL;
}

/* Long-form flag lookup walks parents too — `--verbose` declared on
   the root remains settable on the inner verb's command line. */
static ParsedArg *
slot_by_long(AxlArgs *a, const char *key, size_t key_len)
{
    while (a != NULL) {
        ParsedArg *s = slot_by_long_local(a, key, key_len);
        if (s != NULL) {
            return s;
        }
        a = a->parent;
    }
    return NULL;
}

static ParsedArg *
slot_by_short_local(AxlArgs *a, char shortc)
{
    if (shortc == 0) {
        return NULL;
    }
    for (int i = 0; i < a->slot_count; i++) {
        if (a->slots[i].desc != NULL
            && a->slots[i].desc->short_name == shortc)
        {
            return &a->slots[i];
        }
    }
    return NULL;
}

static ParsedArg *
slot_by_short(AxlArgs *a, char shortc)
{
    while (a != NULL) {
        ParsedArg *s = slot_by_short_local(a, shortc);
        if (s != NULL) {
            return s;
        }
        a = a->parent;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Slot table construction
// ---------------------------------------------------------------------------

static int
register_descs(AxlArgs *a, int base, const AxlArgDesc *list)
{
    if (list == NULL) {
        return base;
    }
    for (int i = 0; list[i].name != NULL; i++) {
        a->slots[base + i].desc        = &list[i];
        a->slots[base + i].set         = false;
        a->slots[base + i].str_value   = list[i].default_value;
        a->slots[base + i].uint_value  = 0;
        a->slots[base + i].int_value   = 0;
        a->slots[base + i].multi_values = NULL;
        /* If a default_value is configured for a numeric type, parse it
         * now so axl_args_get_uint/get_int return the default when the
         * flag is unset. parse_typed sets .set=true unconditionally; we
         * restore it to false so the slot still looks "user did not
         * pass this flag" — distinguishes default from explicit-set
         * for callers that care (e.g., --port not given vs --port 8080
         * explicitly). String/bool/multi types already work correctly
         * via str_value alone, so we only run this for numeric types. */
        if (list[i].default_value != NULL) {
            switch (list[i].type) {
                case AXL_ARG_U8:  case AXL_ARG_U16:
                case AXL_ARG_U32: case AXL_ARG_U64:
                case AXL_ARG_S64:
                    parse_typed(&a->slots[base + i],
                                list[i].default_value, "default");
                    a->slots[base + i].set = false;
                    break;
                default:
                    break;
            }
        }
    }
    return base + desc_count(list);
}

static bool
build_slots(AxlArgs *a, const AxlArgsNode *node)
{
    int total = desc_count(node->flags);
    if (node_is_leaf(node)) {
        total += desc_count(node->positionals);
    }
    a->slots = (ParsedArg *)axl_calloc(total > 0 ? (size_t)total : 1,
                                       sizeof(ParsedArg));
    if (a->slots == NULL) {
        return false;
    }
    a->slot_count = total;
    int base = 0;
    base = register_descs(a, base, node->flags);
    if (node_is_leaf(node)) {
        register_descs(a, base, node->positionals);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Type parsing
// ---------------------------------------------------------------------------

static bool
parse_typed(ParsedArg *slot, const char *value, const char *path)
{
    const AxlArgDesc *d = slot->desc;
    switch (d->type) {
        case AXL_ARG_BOOL:
            slot->uint_value = (value != NULL
                                && (axl_strcmp(value, "true") == 0
                                    || axl_strcmp(value, "1") == 0
                                    || axl_strcmp(value, "yes") == 0));
            slot->set = true;
            return true;

        case AXL_ARG_STRING:
            slot->str_value = value;
            slot->set = true;
            return true;

        case AXL_ARG_CHOICE:
            /* NULL or empty choices array → behave like AXL_ARG_STRING
               (unconstrained). Otherwise the value must match one of
               the entries. Comparison is byte-equal by default; set
               choices_case_insensitive = true to relax to ASCII
               case-folded match (axl_strcasecmp). */
            if (d->choices != NULL && d->choices[0] != NULL) {
                bool ok = false;
                for (size_t i = 0; d->choices[i] != NULL; i++) {
                    if (value == NULL) { break; }
                    int cmp = d->choices_case_insensitive
                                  ? axl_strcasecmp(value, d->choices[i])
                                  : axl_strcmp(value, d->choices[i]);
                    if (cmp == 0) {
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    /* Build a human-readable choice list inline. The
                       buffer is sized for ~16 short choices; longer
                       sets get truncated with an ellipsis, which is
                       fine for an error message. */
                    char  list[256];
                    size_t off = 0;
                    list[0] = '\0';
                    for (size_t i = 0; d->choices[i] != NULL; i++) {
                        size_t left = sizeof(list) - off;
                        if (left <= 4) {
                            /* Not enough room for one more choice
                               plus the trailing NUL. Replace the
                               last 3 chars of the existing content
                               with "..." in place; the NUL snprintf
                               placed at list[off] stays valid (the
                               buffer never reaches sizeof(list) by
                               construction — snprintf reserves its
                               own NUL byte). */
                            if (off >= 3) {
                                list[off - 3] = '.';
                                list[off - 2] = '.';
                                list[off - 1] = '.';
                            }
                            break;
                        }
                        const char *sep = (off == 0) ? "" : ", ";
                        int n = axl_snprintf(list + off, left, "%s%s",
                                             sep, d->choices[i]);
                        if (n <= 0 || (size_t)n >= left) {
                            break;
                        }
                        off += (size_t)n;
                    }
                    axl_print("%s: '%s' for --%s is not one of: %s\n",
                              path,
                              value != NULL ? value : "(missing)",
                              d->name, list);
                    return false;
                }
            }
            slot->str_value = value;
            slot->set = true;
            return true;

        case AXL_ARG_MULTI:
            slot->str_value = value;
            if (slot->multi_values == NULL) {
                slot->multi_values = axl_array_new(sizeof(const char *));
                if (slot->multi_values == NULL) {
                    return false;
                }
            }
            (void)axl_array_append_ptr(slot->multi_values, (void *)value);
            slot->set = true;
            return true;

        case AXL_ARG_U8:
        case AXL_ARG_U16:
        case AXL_ARG_U32:
        case AXL_ARG_U64: {
            uint64_t v = 0;
            if (axl_str_to_u64(value, d->base, &v, NULL) != AXL_OK) {
                axl_print("%s: '%s' for --%s is not a valid integer\n",
                          path, value != NULL ? value : "(missing)", d->name);
                return false;
            }
            uint64_t cap = 0;
            switch (d->type) {
                case AXL_ARG_U8:  cap = 0xFFu;        break;
                case AXL_ARG_U16: cap = 0xFFFFu;      break;
                case AXL_ARG_U32: cap = 0xFFFFFFFFu;  break;
                default:          cap = 0;            break;   /* U64 unbounded */
            }
            if (cap != 0 && v > cap) {
                axl_print("%s: '%s' for --%s exceeds the type's range\n",
                          path, value, d->name);
                return false;
            }
            if (d->min != 0 && v < d->min) {
                axl_print("%s: '%s' for --%s is below min %llu\n",
                          path, value, d->name, (unsigned long long)d->min);
                return false;
            }
            if (d->max != 0 && v > d->max) {
                axl_print("%s: '%s' for --%s exceeds max %llu\n",
                          path, value, d->name, (unsigned long long)d->max);
                return false;
            }
            slot->uint_value = v;
            slot->str_value  = value;
            slot->set = true;
            return true;
        }

        case AXL_ARG_S64: {
            int64_t v = 0;
            if (axl_str_to_s64(value, d->base, &v, NULL) != 0) {
                axl_print("%s: '%s' for --%s is not a valid integer\n",
                          path, value != NULL ? value : "(missing)", d->name);
                return false;
            }
            /* Reuse the uint64 min/max fields cast as int64. Same
               "0 = no bound" convention as the unsigned variants —
               for negative lower bounds, the descriptor sets
               .min = (uint64_t)(int64_t)-N which round-trips via
               two's complement. */
            int64_t s_min = (int64_t)d->min;
            int64_t s_max = (int64_t)d->max;
            if (d->min != 0 && v < s_min) {
                axl_print("%s: '%s' for --%s is below min %lld\n",
                          path, value, d->name, (long long)s_min);
                return false;
            }
            if (d->max != 0 && v > s_max) {
                axl_print("%s: '%s' for --%s exceeds max %lld\n",
                          path, value, d->name, (long long)s_max);
                return false;
            }
            slot->int_value  = v;
            slot->str_value  = value;
            slot->set = true;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------

/* Cap on the aligned left-column width. Keys longer than this overflow and
   push their help text right (same as an over-long flag/positional name),
   so one long choice-list hint can't blow the whole column out. */
#define HELP_KEY_MAX 24

/* Build the left-column "key" for a flag into @p out — "-x, --name <hint>",
   or "    --name <hint>" (4-space pad) when there is no short name, so
   long-only flags line up under the short ones. Returns its length. */
static size_t
flag_key(const AxlArgDesc *d, char *out, size_t outsz)
{
    char short_buf[8] = "    ";
    if (d->short_name != 0) {
        short_buf[0] = '-';
        short_buf[1] = d->short_name;
        short_buf[2] = ',';
    }
    const char *value_hint = "";
    /* Choice-aware hint buffer; lives on the stack with enough room
       for "<a|b|c|...>" up to ~80 chars. CHOICE with no list falls
       back to the generic " <str>" hint. */
    char choice_hint[96];
    switch (d->type) {
        case AXL_ARG_BOOL:                                 break;
        case AXL_ARG_STRING:                               value_hint = " <str>";   break;
        case AXL_ARG_MULTI:                                value_hint = " <str>*";  break;
        case AXL_ARG_CHOICE:
            value_hint = " <str>";
            if (d->choices != NULL && d->choices[0] != NULL) {
                size_t off = 0;
                int n = axl_snprintf(choice_hint, sizeof(choice_hint), " <");
                if (n > 0) { off = (size_t)n; }
                for (size_t i = 0; d->choices[i] != NULL && off + 1 < sizeof(choice_hint); i++) {
                    n = axl_snprintf(choice_hint + off,
                                     sizeof(choice_hint) - off,
                                     "%s%s", (i == 0 ? "" : "|"),
                                     d->choices[i]);
                    if (n <= 0 || (size_t)n >= sizeof(choice_hint) - off) {
                        /* Truncate gracefully — close the bracket and stop. */
                        if (off + 4 < sizeof(choice_hint)) {
                            axl_snprintf(choice_hint + off,
                                         sizeof(choice_hint) - off, "|...");
                            off += 4;
                        }
                        break;
                    }
                    off += (size_t)n;
                }
                if (off + 1 < sizeof(choice_hint)) {
                    choice_hint[off++] = '>';
                    choice_hint[off]   = '\0';
                }
                /* Mark case-insensitive CHOICE in help so users know
                   that e.g. "DD_CFG" and "dd_cfg" are both valid.
                   Append " (case-insensitive)" if there's room; on
                   buffers already near capacity, drop the marker
                   silently rather than ellipsize. */
                if (d->choices_case_insensitive) {
                    const char *suffix = " (case-insensitive)";
                    size_t      slen   = axl_strlen(suffix);
                    if (off + slen + 1 < sizeof(choice_hint)) {
                        /* Per-iteration bound is redundant given the
                           guard above, but makes the safety explicit
                           for the static analyzer. */
                        for (size_t i = 0;
                             i < slen && off + 1 < sizeof(choice_hint);
                             i++) {
                            choice_hint[off++] = suffix[i];
                        }
                        choice_hint[off] = '\0';
                    }
                }
                value_hint = choice_hint;
            }
            break;
        case AXL_ARG_U8:  case AXL_ARG_U16:
        case AXL_ARG_U32: case AXL_ARG_U64:                value_hint = " <uint>";  break;
        case AXL_ARG_S64:                                  value_hint = " <int>";   break;
    }
    axl_snprintf(out, outsz, "%s--%s%s", short_buf, d->name, value_hint);
    return axl_strlen(out);
}

/* Build the left-column key for a positional into @p out: "<name>" (or
   "<name>..." for a variadic). No "(optional)" suffix — the [<name>]
   brackets in the Usage line already convey it. Returns its length. */
static size_t
positional_key(const AxlArgDesc *d, char *out, size_t outsz)
{
    const char *suffix = (d->type == AXL_ARG_MULTI) ? "..." : "";
    axl_snprintf(out, outsz, "<%s>%s", d->name, suffix);
    return axl_strlen(out);
}

/* Print one aligned help row: "  <key>  <help>", left-justifying @p key
   to width @p w so every row in the block shares a help column. */
static void
print_help_row(const char *key, const char *help, int w)
{
    axl_print("  %-*s  %s\n", w, key, help != NULL ? help : "");
}

/* Render help for one node. @p path is the breadcrumb used in the
   "Usage:" line ("mytool bios" rather than "bios"). */
static void
print_help_for(const AxlArgsNode *node, const char *path)
{
    if (node->help != NULL) {
        /* ASCII '-' separator, not a Unicode em-dash: a UEFI text console has
           no UTF-8, so U+2014 would render as a white block. */
        axl_print("%s - %s\n\n", path, node->help);
    }

    if (node->help_prolog != NULL) {
        /* Print verbatim, then a single blank-line separator before
           the auto-generated `Usage:` block. The docstring on
           AxlArgsNode.help_prolog tells consumers not to include
           leading or trailing newlines themselves. */
        axl_print("%s\n\n", node->help_prolog);
    }

    if (node_is_branch(node)) {
        axl_print("Usage: %s [flags] <verb> [args]\n", path);
        if (node_is_leaf(node)) {
            axl_print("       %s [flags]"
                      "                "
                      "(no verb runs the default handler)\n",
                      path);
        }
        axl_print("\nVerbs:\n");
        for (int i = 0; node->verbs[i].name != NULL; i++) {
            const char *marker = node_is_branch(&node->verbs[i]) ? "*" : " ";
            const char *help = node->verbs[i].help != NULL
                               ? node->verbs[i].help : "";
            bool is_default = (node_is_leaf(node)
                               && node->verbs[i].handler != NULL
                               && node->verbs[i].handler == node->handler);
            if (is_default) {
                axl_print("  %s %-12s  %s (default)\n",
                          marker, node->verbs[i].name, help);
            } else {
                axl_print("  %s %-12s  %s\n",
                          marker, node->verbs[i].name, help);
            }
        }
        axl_print("\n  (* indicates a verb with sub-verbs; "
                  "run `<verb> --help` for details)\n");
    } else {
        axl_print("Usage: %s [flags]", path);
        for (int i = 0; node->positionals != NULL
                        && node->positionals[i].name != NULL; i++) {
            const AxlArgDesc *d = &node->positionals[i];
            if (d->type == AXL_ARG_MULTI) {
                axl_print(" [<%s>...]", d->name);
            } else if (d->required) {
                axl_print(" <%s>", d->name);
            } else {
                axl_print(" [<%s>]", d->name);
            }
        }
        axl_print("\n");
    }

    /* Options + arguments as ONE aligned list — no "Flags:" / "Arguments:"
       section headers — so the help reads like a hand-written legacy usage
       block: a Usage line, then positionals, flags, and a single terse
       --help row, all sharing one help column. */
    {
        bool any_flags = (node->flags != NULL && node->flags[0].name != NULL);
        bool any_pos   = (!node_is_branch(node)
                          && node->positionals != NULL
                          && node->positionals[0].name != NULL);
        char keybuf[96];

        /* Pass 1: the common left-column width (capped; "-h, --help" is
           always present so it sets the floor). */
        int w = (int)axl_strlen("-h, --help");
        if (any_pos) {
            for (int i = 0; node->positionals[i].name != NULL; i++) {
                int k = (int)positional_key(&node->positionals[i],
                                            keybuf, sizeof keybuf);
                if (k > w) w = k;
            }
        }
        if (any_flags) {
            for (int i = 0; node->flags[i].name != NULL; i++) {
                int k = (int)flag_key(&node->flags[i], keybuf, sizeof keybuf);
                if (k > w) w = k;
            }
        }
        if (w > HELP_KEY_MAX) w = HELP_KEY_MAX;

        /* Pass 2: print the block (positionals, then flags, then --help). */
        axl_print("\n");
        if (any_pos) {
            for (int i = 0; node->positionals[i].name != NULL; i++) {
                positional_key(&node->positionals[i], keybuf, sizeof keybuf);
                print_help_row(keybuf, node->positionals[i].help, w);
            }
        }
        if (any_flags) {
            for (int i = 0; node->flags[i].name != NULL; i++) {
                flag_key(&node->flags[i], keybuf, sizeof keybuf);
                print_help_row(keybuf, node->flags[i].help, w);
            }
        }
        print_help_row("-h, --help", "Show this help", w);
    }

    if (node->help_epilog != NULL) {
        /* Blank-line separator before the epilog, then verbatim
           text plus a closing newline. Consumer doesn't include
           leading or trailing newlines. */
        axl_print("\n%s\n", node->help_epilog);
    }
}

void
axl_args_print_help(AxlArgs *args)
{
    if (args != NULL && args->node != NULL) {
        print_help_for(args->node, args->path);
    }
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

static void
free_args(AxlArgs *a)
{
    if (a == NULL) {
        return;
    }
    for (int i = 0; i < a->slot_count; i++) {
        axl_array_free(a->slots[i].multi_values);
    }
    axl_free(a->slots);
    axl_array_free(a->variadic);
    axl_free(a);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

static bool
is_help_flag(const char *tok)
{
    return tok != NULL
        && (axl_strcmp(tok, "-h") == 0
            || axl_strcmp(tok, "--help") == 0
            || axl_strcmp(tok, "?") == 0);   /* legacy-tool help alias */
}

static bool
consume_positional(AxlArgs *a, const char *value)
{
    /* Branches don't take positionals — the first non-flag positional
       is the verb name and is consumed by the run loop, not here. */
    const AxlArgDesc *pos_list = a->node->positionals;
    int n = desc_count(pos_list);

    if (a->next_named_pos < n
        && pos_list[a->next_named_pos].type != AXL_ARG_MULTI)
    {
        ParsedArg *slot = slot_by_name_local(a, pos_list[a->next_named_pos].name);
        if (slot == NULL) {
            return false;
        }
        if (!parse_typed(slot, value, a->path)) {
            return false;
        }
        a->next_named_pos++;
        return true;
    }
    if (n > 0 && pos_list[n - 1].type == AXL_ARG_MULTI) {
        if (a->variadic == NULL) {
            a->variadic = axl_array_new(sizeof(const char *));
            if (a->variadic == NULL) {
                return false;
            }
        }
        (void)axl_array_append_ptr(a->variadic, (void *)value);
        ParsedArg *slot = slot_by_name_local(a, pos_list[n - 1].name);
        if (slot != NULL) {
            slot->set = true;
        }
        return true;
    }
    if (pos_list != NULL && n > 0) {
        axl_print("%s: unexpected argument '%s'\n", a->path, value);
        return false;
    }
    /* No positional descriptor — accumulate into variadic for tools
       that just want to read positionals via axl_args_get_pos. */
    if (a->variadic == NULL) {
        a->variadic = axl_array_new(sizeof(const char *));
        if (a->variadic == NULL) {
            return false;
        }
    }
    (void)axl_array_append_ptr(a->variadic, (void *)value);
    return true;
}

/* A token is a flag iff it starts with '-', isn't a bare "-", and
   isn't a negative number ("-1", "-.5"). A numeric short-flag can't
   be registered (slot_by_short needs a C-identifier char), so
   treating leading-dash-then-digit as a positional is unambiguous —
   and lets consumers pass negative numeric operands (calculator
   expressions, signed offsets) without escaping. The "--"
   end-of-options marker is handled by the caller, before this
   check. */
static bool
token_is_flag(const char *arg)
{
    if (arg[0] != '-' || arg[1] == '\0') {
        return false;
    }
    if (axl_isdigit((unsigned char)arg[1])) {
        return false;
    }
    if (arg[1] == '.' && axl_isdigit((unsigned char)arg[2])) {
        return false;
    }
    return true;
}

static int
parse_flag_token(AxlArgs *a, int i, int argc, char **argv)
{
    const char *arg = argv[i];

    /* Long form. */
    if (arg[0] == '-' && arg[1] == '-') {
        const char *key = arg + 2;
        const char *eq  = NULL;
        for (const char *p = key; *p != '\0'; p++) {
            if (*p == '=') {
                eq = p;
                break;
            }
        }
        size_t key_len = (eq != NULL) ? (size_t)(eq - key) : axl_strlen(key);
        ParsedArg *slot = slot_by_long(a, key, key_len);
        if (slot == NULL) {
            axl_print("%s: unknown flag --%.*s\n",
                      a->path, (int)key_len, key);
            return -1;
        }
        const char *value = NULL;
        if (slot->desc->type == AXL_ARG_BOOL) {
            value = (eq != NULL) ? (eq + 1) : "true";
        } else {
            if (eq != NULL) {
                value = eq + 1;
            } else if (i + 1 < argc) {
                value = argv[i + 1];
            } else {
                axl_print("%s: --%s requires a value\n", a->path, slot->desc->name);
                return -1;
            }
        }
        if (!parse_typed(slot, value, a->path)) {
            return -1;
        }
        return (slot->desc->type == AXL_ARG_BOOL || eq != NULL) ? 1 : 2;
    }

    /* Short form: -f or -f value. */
    char shortc = arg[1];
    if (arg[2] != '\0') {
        axl_print("%s: compact short-flag groups (-%c%c...) "
                  "are not supported; use -%c -%c instead\n",
                  a->path, shortc, arg[2], shortc, arg[2]);
        return -1;
    }
    ParsedArg *slot = slot_by_short(a, shortc);
    if (slot == NULL) {
        axl_print("%s: unknown flag -%c\n", a->path, shortc);
        return -1;
    }
    if (slot->desc->type == AXL_ARG_BOOL) {
        if (!parse_typed(slot, "true", a->path)) {
            return -1;
        }
        return 1;
    }
    if (i + 1 >= argc) {
        axl_print("%s: -%c requires a value\n", a->path, shortc);
        return -1;
    }
    if (!parse_typed(slot, argv[i + 1], a->path)) {
        return -1;
    }
    return 2;
}

static int
validate_required(AxlArgs *a)
{
    const AxlArgDesc *pos_list = a->node->positionals;
    if (pos_list == NULL) {
        return 0;
    }
    for (int i = 0; pos_list[i].name != NULL; i++) {
        if (!pos_list[i].required) {
            continue;
        }
        ParsedArg *slot = slot_by_name_local(a, pos_list[i].name);
        if (slot == NULL || !slot->set) {
            axl_print("%s: missing required argument <%s>\n",
                      a->path, pos_list[i].name);
            return -1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Internal entry point — recursive
// ---------------------------------------------------------------------------

/* Build the breadcrumb path for this node into @p out. Returns true
   on success; false if the buffer would overflow. */
static bool
build_path(char *out, size_t cap, const char *parent_path, const char *name)
{
    if (parent_path == NULL) {
        size_t n = axl_strlen(name);
        if (n + 1 > cap) return false;
        for (size_t i = 0; i <= n; i++) out[i] = name[i];
        return true;
    }
    size_t pn = axl_strlen(parent_path);
    size_t nn = axl_strlen(name);
    if (pn + 1 + nn + 1 > cap) return false;
    for (size_t i = 0; i < pn; i++) out[i] = parent_path[i];
    out[pn] = ' ';
    for (size_t i = 0; i <= nn; i++) out[pn + 1 + i] = name[i];
    return true;
}

static bool
validate_node_shape(const AxlArgsNode *node, const char *path)
{
    bool leaf   = node_is_leaf(node);
    bool branch = node_is_branch(node);
    /* leaf, branch, and leaf+branch are all valid:
       - leaf only: a normal handler-bearing terminal node.
       - branch only: a category that always requires a sub-verb.
       - branch + leaf: a category whose handler is the no-verb
         default ("mytool bios" with no further verb invokes the
         handler with parsed branch-level flags). The first non-
         flag still must match a verb; only the no-verb path
         falls through to the default handler. */
    if (!leaf && !branch) {
        axl_print("%s: misconfigured (no verbs and no handler)\n", path);
        return false;
    }
    if (branch && node->positionals != NULL && node->positionals[0].name != NULL) {
        axl_print("%s: misconfigured (branch nodes cannot have positionals)\n",
                  path);
        return false;
    }
    return true;
}

static int
args_run_internal(int argc, char **argv,
                  const AxlArgsNode *node,
                  const char *parent_path,
                  AxlArgs *parent_args)
{
    if (node == NULL || node->name == NULL) {
        return 1;
    }

    char path_buf[256];
    if (!build_path(path_buf, sizeof(path_buf), parent_path, node->name)) {
        axl_print("args: command path too long\n");
        return 1;
    }

    if (!validate_node_shape(node, path_buf)) {
        return 1;
    }

    AxlArgs *a = (AxlArgs *)axl_calloc(1, sizeof(AxlArgs));
    if (a == NULL) {
        return 1;
    }
    a->node   = node;
    a->path   = path_buf;
    a->parent = parent_args;

    if (!build_slots(a, node)) {
        free_args(a);
        return 1;
    }

    int  rc          = 0;
    bool parse_error = false;
    int  i           = 1;   /* skip argv[0] (program/verb name) */
    bool end_of_opts = false;  /* set by the POSIX "--" marker */

    /* Branches: first non-flag positional is the verb name. Once
       found, recurse with the remaining argv slice. */
    const AxlArgsNode *next_branch = NULL;
    int                next_argc   = 0;
    char             **next_argv   = NULL;

    while (i < argc) {
        const char *arg = argv[i];
        /* Once "--" is seen, every remaining token is positional —
           no help detection, no flag parsing. */
        if (!end_of_opts) {
            if (is_help_flag(arg)) {
                print_help_for(node, path_buf);
                free_args(a);
                return 0;
            }
            /* Bare `help` is a help synonym only at branches before a
               verb is selected (so `grep help file` still searches for
               the word "help" in a leaf). */
            if (node_is_branch(node) && axl_strcmp(arg, "help") == 0) {
                print_help_for(node, path_buf);
                free_args(a);
                return 0;
            }
            /* POSIX end-of-options marker: consume "--" and treat the
               rest as positional unconditionally. */
            if (arg[0] == '-' && arg[1] == '-' && arg[2] == '\0') {
                end_of_opts = true;
                i++;
                continue;
            }
            if (token_is_flag(arg)) {
                int consumed = parse_flag_token(a, i, argc, argv);
                if (consumed < 0) {
                    parse_error = true;
                    rc = 1;
                    goto out;
                }
                i += consumed;
                continue;
            }
        }
        if (node_is_branch(node)) {
            const AxlArgsNode *child = find_verb(node, arg);
            if (child == NULL) {
                axl_print("%s: unknown verb '%s'\n", path_buf, arg);
                parse_error = true;
                rc = 1;
                goto out;
            }
            next_branch = child;
            next_argc   = argc - i;     /* slice starting at the verb name */
            next_argv   = argv + i;     /* inner skips index 0 (verb name) */
            break;                      /* recursion happens after pre_run */
        }
        if (!consume_positional(a, arg)) {
            parse_error = true;
            rc = 1;
            goto out;
        }
        i++;
    }

    /* Branch with no verb supplied:
       - if the branch has a default handler (branch + leaf shape),
         fall through to invoke it with parsed branch-level flags;
       - otherwise show help and exit. */
    if (node_is_branch(node) && next_branch == NULL && !node_is_leaf(node)) {
        print_help_for(node, path_buf);
        free_args(a);
        return 1;
    }

    /* Leaf: validate required positionals before invoking handler.
       (A branch with default handler has no positionals — guarded
       by validate_node_shape — so this only runs for true leaves.) */
    if (node_is_leaf(node) && !node_is_branch(node)
        && validate_required(a) != 0)
    {
        parse_error = true;
        rc = 1;
        goto out;
    }

    if (node->pre_run != NULL) {
        node->pre_run(a);
    }

    if (next_branch != NULL) {
        /* Recurse into the matched child, carrying our path as
           parent_path and our AxlArgs as parent_args so the child's
           accessors can walk up to read our flags. */
        rc = args_run_internal(next_argc, next_argv, next_branch,
                               path_buf, a);
    } else {
        /* Leaf, or branch falling through to its default handler. */
        rc = node->handler(a);
    }

out:
    if (parse_error) {
        axl_print("\n");
        print_help_for(node, path_buf);
    }
    free_args(a);
    return rc;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

int
axl_args_run(int argc, char **argv, const AxlArgsNode *root)
{
    return args_run_internal(argc, argv, root, NULL, NULL);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const char *
axl_args_get_string(AxlArgs *args, const char *name)
{
    if (args == NULL) {
        return NULL;
    }
    ParsedArg *s = slot_by_name(args, name);
    return (s != NULL) ? s->str_value : NULL;
}

bool
axl_args_get_bool(AxlArgs *args, const char *name)
{
    if (args == NULL) {
        return false;
    }
    ParsedArg *s = slot_by_name(args, name);
    if (s == NULL) {
        return false;
    }
    if (s->set) {
        return s->uint_value != 0;
    }
    return s->str_value != NULL
        && (axl_strcmp(s->str_value, "true") == 0
            || axl_strcmp(s->str_value, "1") == 0
            || axl_strcmp(s->str_value, "yes") == 0);
}

uint64_t
axl_args_get_uint(AxlArgs *args, const char *name)
{
    if (args == NULL) {
        return 0;
    }
    ParsedArg *s = slot_by_name(args, name);
    return (s != NULL) ? s->uint_value : 0;
}

int64_t
axl_args_get_int(AxlArgs *args, const char *name)
{
    if (args == NULL) {
        return 0;
    }
    ParsedArg *s = slot_by_name(args, name);
    return (s != NULL) ? s->int_value : 0;
}

int
axl_args_get_uint_offset(AxlArgs *args, const char *name, uint64_t *out_value)
{
    if (args == NULL || name == NULL || out_value == NULL) {
        return AXL_ERR;
    }
    ParsedArg *s = slot_by_name(args, name);
    if (s == NULL || s->str_value == NULL) {
        return AXL_ERR;
    }
    return axl_strtou64_with_offset(s->str_value, out_value);
}

int
axl_args_get_pos_count(AxlArgs *args)
{
    if (args == NULL || args->variadic == NULL) {
        return 0;
    }
    return (int)axl_array_len(args->variadic);
}

const char *
axl_args_get_pos(AxlArgs *args, int index)
{
    if (args == NULL || args->variadic == NULL || index < 0) {
        return NULL;
    }
    if ((size_t)index >= axl_array_len(args->variadic)) {
        return NULL;
    }
    void *p = axl_array_get_ptr(args->variadic, (size_t)index);
    return (const char *)p;
}

int
axl_args_get_multi_count(AxlArgs *args, const char *name)
{
    if (args == NULL) {
        return 0;
    }
    ParsedArg *s = slot_by_name(args, name);
    if (s == NULL || s->multi_values == NULL) {
        return 0;
    }
    return (int)axl_array_len(s->multi_values);
}

const char *
axl_args_get_multi(AxlArgs *args, const char *name, int index)
{
    if (args == NULL || index < 0) {
        return NULL;
    }
    ParsedArg *s = slot_by_name(args, name);
    if (s == NULL || s->multi_values == NULL) {
        return NULL;
    }
    if ((size_t)index >= axl_array_len(s->multi_values)) {
        return NULL;
    }
    void *p = axl_array_get_ptr(s->multi_values, (size_t)index);
    return (const char *)p;
}

void *
axl_args_user_data(AxlArgs *args)
{
    while (args != NULL) {
        if (args->node != NULL && args->node->user_data != NULL) {
            return args->node->user_data;
        }
        args = args->parent;
    }
    return NULL;
}

const char *
axl_args_program_name(AxlArgs *args)
{
    while (args != NULL && args->parent != NULL) {
        args = args->parent;
    }
    return (args != NULL && args->node != NULL) ? args->node->name : NULL;
}
