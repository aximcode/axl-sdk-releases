/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-args.c
    AxlArgs — declarative command-line parser for AXL tools.

    Replaces the per-tool AxlConfig + axl_subcommand_dispatch +
    hand-rolled positional-arg parsing with a single data-driven
    entry point. Tools declare a static @ref AxlArgsApp tree; the
    framework parses argv, validates types and bounds, generates
    `--help` output, and dispatches to the matching verb handler.

    Design choices:
      - Linear flag/positional storage (N is small; no hash table).
      - Parsing is single-pass; the verb is resolved when the first
        non-flag positional is encountered, after which subsequent
        flags can come from the verb's own descriptor list.
      - Validation runs at parse time so handlers see only good data.
**/

#include <axl/axl-args.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>
#include <axl/axl-array.h>
#include <axl/axl-io.h>
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
    const AxlArgsApp *app;
    const AxlVerb    *verb;          ///< NULL in single-verb mode

    /* Linear arrays of parsed slots: one per descriptor in
       global_flags + verb->flags + (verb->positionals OR app->positionals).
       Indexed in the order the descriptors appear. */
    ParsedArg        *slots;
    int               slot_count;

    AxlArray         *variadic;      ///< collected variadic tail (const char *)
    int               next_named_pos; ///< next positional descriptor to fill
};

// ---------------------------------------------------------------------------
// Descriptor-list helpers
// ---------------------------------------------------------------------------

static int
desc_count(
    const AxlArgDesc  *list
    )
{
    int n = 0;
    if (list != NULL) {
        while (list[n].name != NULL) {
            n++;
        }
    }
    return n;
}

static const AxlVerb *
find_verb(
    const AxlArgsApp  *app,
    const char        *name
    )
{
    if (app->verbs == NULL || name == NULL) {
        return NULL;
    }
    for (int i = 0; app->verbs[i].name != NULL; i++) {
        if (axl_strcmp(app->verbs[i].name, name) == 0) {
            return &app->verbs[i];
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Slot lookup (linear; N is small)
// ---------------------------------------------------------------------------

static ParsedArg *
slot_by_name(
    AxlArgs     *a,
    const char  *name
    )
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

static ParsedArg *
slot_by_long(
    AxlArgs     *a,
    const char  *key,
    size_t       key_len
    )
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

static ParsedArg *
slot_by_short(
    AxlArgs  *a,
    char      shortc
    )
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

// ---------------------------------------------------------------------------
// Slot table construction
// ---------------------------------------------------------------------------

static int
register_descs(
    AxlArgs            *a,
    int                 base,
    const AxlArgDesc   *list
    )
{
    if (list == NULL) {
        return base;
    }
    for (int i = 0; list[i].name != NULL; i++) {
        a->slots[base + i].desc      = &list[i];
        a->slots[base + i].set       = false;
        a->slots[base + i].str_value = list[i].default_value;
        a->slots[base + i].uint_value = 0;
        a->slots[base + i].int_value  = 0;
        a->slots[base + i].multi_values = NULL;
    }
    return base + desc_count(list);
}

static bool
build_slots(
    AxlArgs              *a,
    const AxlArgsApp     *app,
    const AxlVerb        *verb
    )
{
    int total = desc_count(app->global_flags);
    if (verb != NULL) {
        total += desc_count(verb->flags);
        total += desc_count(verb->positionals);
    } else {
        total += desc_count(app->positionals);
    }
    a->slots = (ParsedArg *)axl_calloc(total > 0 ? (size_t)total : 1,
                                       sizeof(ParsedArg));
    if (a->slots == NULL) {
        return false;
    }
    a->slot_count = total;
    int base = 0;
    base = register_descs(a, base, app->global_flags);
    if (verb != NULL) {
        base = register_descs(a, base, verb->flags);
        register_descs(a, base, verb->positionals);
    } else {
        register_descs(a, base, app->positionals);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Type parsing
// ---------------------------------------------------------------------------

static bool
parse_typed(
    ParsedArg   *slot,
    const char  *value,
    const char  *prog
    )
{
    const AxlArgDesc *d = slot->desc;
    switch (d->type) {
        case AXL_ARG_BOOL:
            /* Bools come from presence; this path handles
               --flag=true / --flag=false / --flag=1 / --flag=0. */
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

        case AXL_ARG_MULTI:
            slot->str_value = value;   /* head, for convenience */
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
            if (axl_str_to_u64(value, d->base, &v, NULL) != 0) {
                axl_print("%s: '%s' for --%s is not a valid integer\n",
                          prog, value != NULL ? value : "(missing)", d->name);
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
                          prog, value, d->name);
                return false;
            }
            if (d->min != 0 && v < d->min) {
                axl_print("%s: '%s' for --%s is below min %llu\n",
                          prog, value, d->name, (unsigned long long)d->min);
                return false;
            }
            if (d->max != 0 && v > d->max) {
                axl_print("%s: '%s' for --%s exceeds max %llu\n",
                          prog, value, d->name, (unsigned long long)d->max);
                return false;
            }
            slot->uint_value = v;
            slot->str_value  = value;   /* keep raw for logging */
            slot->set = true;
            return true;
        }

        case AXL_ARG_S64: {
            /* Dogfood axl_str_to_s64 — it handles INT64_MIN correctly,
               which a hand-rolled "strip minus, parse u64, negate" path
               does not (signed-overflow UB for the most-negative value;
               same bug class as the v0.5.0 axl_sscanf review caught). */
            int64_t v = 0;
            if (axl_str_to_s64(value, d->base, &v, NULL) != 0) {
                axl_print("%s: '%s' for --%s is not a valid integer\n",
                          prog, value != NULL ? value : "(missing)", d->name);
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

static void
print_flag_line(
    const AxlArgDesc  *d
    )
{
    char short_buf[8] = "    ";
    if (d->short_name != 0) {
        short_buf[0] = '-';
        short_buf[1] = d->short_name;
        short_buf[2] = ',';
    }
    const char *value_hint = "";
    switch (d->type) {
        case AXL_ARG_BOOL:                                 break;
        case AXL_ARG_STRING:                               value_hint = " <str>";   break;
        case AXL_ARG_MULTI:                                value_hint = " <str>*";  break;
        case AXL_ARG_U8:  case AXL_ARG_U16:
        case AXL_ARG_U32: case AXL_ARG_U64:                value_hint = " <uint>";  break;
        case AXL_ARG_S64:                                  value_hint = " <int>";   break;
    }
    axl_print("  %s --%-16s%s  %s\n",
              short_buf, d->name, value_hint,
              d->help != NULL ? d->help : "");
}

static void
print_positional_line(
    const AxlArgDesc  *d
    )
{
    const char *suffix = (d->type == AXL_ARG_MULTI) ? "..." : "";
    const char *req    = d->required ? "" : " (optional)";
    axl_print("  <%s>%s              %s%s\n",
              d->name, suffix,
              d->help != NULL ? d->help : "",
              req);
}

static void
print_help_for(
    const AxlArgsApp  *app,
    const AxlVerb     *verb
    )
{
    if (app->help != NULL) {
        axl_print("%s — %s\n\n", app->name, app->help);
    }

    if (verb != NULL) {
        axl_print("Usage: %s %s [flags]", app->name, verb->name);
        for (int i = 0; verb->positionals != NULL
                        && verb->positionals[i].name != NULL; i++) {
            const AxlArgDesc *d = &verb->positionals[i];
            if (d->type == AXL_ARG_MULTI) {
                axl_print(" [<%s>...]", d->name);
            } else if (d->required) {
                axl_print(" <%s>", d->name);
            } else {
                axl_print(" [<%s>]", d->name);
            }
        }
        axl_print("\n");
        if (verb->help != NULL) {
            axl_print("\n%s\n", verb->help);
        }
    } else if (app->verbs != NULL) {
        axl_print("Usage: %s [flags] <verb> [args]\n", app->name);
        if (app->usage != NULL) {
            axl_print("       %s %s\n", app->name, app->usage);
        }
        axl_print("\nVerbs:\n");
        for (int i = 0; app->verbs[i].name != NULL; i++) {
            axl_print("  %-12s  %s\n",
                      app->verbs[i].name,
                      app->verbs[i].help != NULL ? app->verbs[i].help : "");
        }
    } else {
        axl_print("Usage: %s [flags]", app->name);
        for (int i = 0; app->positionals != NULL
                        && app->positionals[i].name != NULL; i++) {
            const AxlArgDesc *d = &app->positionals[i];
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

    bool any_flags = (app->global_flags != NULL && app->global_flags[0].name != NULL)
                  || (verb != NULL && verb->flags != NULL && verb->flags[0].name != NULL);
    if (any_flags) {
        axl_print("\nFlags:\n");
        for (int i = 0; app->global_flags != NULL
                        && app->global_flags[i].name != NULL; i++) {
            print_flag_line(&app->global_flags[i]);
        }
        if (verb != NULL && verb->flags != NULL) {
            for (int i = 0; verb->flags[i].name != NULL; i++) {
                print_flag_line(&verb->flags[i]);
            }
        }
        axl_print("  -h, --help              Show this help\n");
    } else {
        axl_print("\n  -h, --help              Show this help\n");
    }

    /* Positional descriptions. */
    const AxlArgDesc *pos = (verb != NULL) ? verb->positionals : app->positionals;
    if (pos != NULL && pos[0].name != NULL) {
        axl_print("\nArguments:\n");
        for (int i = 0; pos[i].name != NULL; i++) {
            print_positional_line(&pos[i]);
        }
    }
}

void
axl_args_print_help(
    AxlArgs *args
    )
{
    if (args != NULL) {
        print_help_for(args->app, args->verb);
    }
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

static void
free_args(
    AxlArgs *a
    )
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
is_help_flag(
    const char *tok
    )
{
    /* `-h` / `--help` are help anywhere on the command line. The bare
       word `help` is NOT a help token here — it would shadow legitimate
       positional args (e.g. `grep help file.txt` looking for the word
       "help"). The verb-name path handles `help` as a synonym for
       `--help` only when no verb has been attached yet. */
    return tok != NULL
        && (axl_strcmp(tok, "-h") == 0
            || axl_strcmp(tok, "--help") == 0);
}

/** Extend the slot table to include the verb's flags + positionals
    once the verb is resolved. Preserves the existing global-flag
    slots (and their already-parsed state) — naively rebuilding here
    would discard any --flag values seen before the verb token. */
static bool
attach_verb(
    AxlArgs           *a,
    const AxlVerb     *verb
    )
{
    a->verb = verb;
    int extra = desc_count(verb->flags) + desc_count(verb->positionals);
    if (extra == 0) {
        return true;
    }
    int new_count = a->slot_count + extra;
    ParsedArg *grown = (ParsedArg *)axl_calloc((size_t)new_count, sizeof(ParsedArg));
    if (grown == NULL) {
        return false;
    }
    for (int i = 0; i < a->slot_count; i++) {
        grown[i] = a->slots[i];
    }
    axl_free(a->slots);
    a->slots = grown;
    int base = a->slot_count;
    a->slot_count = new_count;
    base = register_descs(a, base, verb->flags);
    register_descs(a, base, verb->positionals);
    return true;
}

static bool
consume_positional(
    AxlArgs     *a,
    const char  *value
    )
{
    const AxlArgDesc *pos_list = (a->verb != NULL)
                                 ? a->verb->positionals
                                 : a->app->positionals;
    int n = desc_count(pos_list);

    /* If we have a non-variadic descriptor with a slot still open,
       fill it. */
    if (a->next_named_pos < n && pos_list[a->next_named_pos].type != AXL_ARG_MULTI) {
        ParsedArg *slot = slot_by_name(a, pos_list[a->next_named_pos].name);
        if (slot == NULL) {
            return false;
        }
        if (!parse_typed(slot, value, a->app->name)) {
            return false;
        }
        a->next_named_pos++;
        return true;
    }
    /* Otherwise this is part of the variadic tail. */
    if (n > 0 && pos_list[n - 1].type == AXL_ARG_MULTI) {
        if (a->variadic == NULL) {
            a->variadic = axl_array_new(sizeof(const char *));
            if (a->variadic == NULL) {
                return false;
            }
        }
        (void)axl_array_append_ptr(a->variadic, (void *)value);
        /* Mark the variadic slot as set so help/required checks pass. */
        ParsedArg *slot = slot_by_name(a, pos_list[n - 1].name);
        if (slot != NULL) {
            slot->set = true;
        }
        return true;
    }
    /* The tool declared positionals and they're all filled (no MULTI
       tail): reject the extra. Silently accepting it would let
       `memspd show 0x53 garbage` run with `garbage` invisible to the
       handler. Tools that genuinely want unbounded positionals
       declare AXL_ARG_MULTI as their tail. */
    if (pos_list != NULL && n > 0) {
        axl_print("%s: unexpected argument '%s'\n", a->app->name, value);
        return false;
    }
    /* No descriptor at all — tool just wants to read positionals via
       axl_args_get_pos. Stash in variadic. */
    if (a->variadic == NULL) {
        a->variadic = axl_array_new(sizeof(const char *));
        if (a->variadic == NULL) {
            return false;
        }
    }
    (void)axl_array_append_ptr(a->variadic, (void *)value);
    return true;
}

/** Parse one --flag or -f token; returns the number of argv slots
    consumed (1 or 2), or -1 on error. */
static int
parse_flag_token(
    AxlArgs   *a,
    int        i,
    int        argc,
    char     **argv
    )
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
                      a->app->name, (int)key_len, key);
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
                axl_print("%s: --%s requires a value\n", a->app->name, slot->desc->name);
                return -1;
            }
        }
        if (!parse_typed(slot, value, a->app->name)) {
            return -1;
        }
        /* BOOL with =value consumed only one slot; otherwise (with
           value from next argv) two. */
        return (slot->desc->type == AXL_ARG_BOOL || eq != NULL) ? 1 : 2;
    }

    /* Short form: -f or -f value. Reject compact short groups
       (-vh, -abc) — they silently dropped trailing chars in the
       initial impl, which would surprise anyone trying tar-style
       invocation. Force the explicit form: `-v -h`. */
    char shortc = arg[1];
    if (arg[2] != '\0') {
        axl_print("%s: compact short-flag groups (-%c%c...) "
                  "are not supported; use -%c -%c instead\n",
                  a->app->name, shortc, arg[2], shortc, arg[2]);
        return -1;
    }
    ParsedArg *slot = slot_by_short(a, shortc);
    if (slot == NULL) {
        axl_print("%s: unknown flag -%c\n", a->app->name, shortc);
        return -1;
    }
    if (slot->desc->type == AXL_ARG_BOOL) {
        if (!parse_typed(slot, "true", a->app->name)) {
            return -1;
        }
        return 1;
    }
    if (i + 1 >= argc) {
        axl_print("%s: -%c requires a value\n", a->app->name, shortc);
        return -1;
    }
    if (!parse_typed(slot, argv[i + 1], a->app->name)) {
        return -1;
    }
    return 2;
}

static int
validate_required(
    AxlArgs *a
    )
{
    const AxlArgDesc *pos_list = (a->verb != NULL)
                                 ? a->verb->positionals
                                 : a->app->positionals;
    if (pos_list == NULL) {
        return 0;
    }
    for (int i = 0; pos_list[i].name != NULL; i++) {
        if (!pos_list[i].required) {
            continue;
        }
        ParsedArg *slot = slot_by_name(a, pos_list[i].name);
        if (slot == NULL || !slot->set) {
            axl_print("%s: missing required argument <%s>\n",
                      a->app->name, pos_list[i].name);
            return -1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

int
axl_args_run(
    int                   argc,
    char                **argv,
    const AxlArgsApp     *app
    )
{
    if (app == NULL || app->name == NULL) {
        return 1;
    }
    if ((app->verbs != NULL) == (app->handler != NULL)) {
        /* Both or neither — caller misconfigured the app. */
        axl_print("%s: app misconfigured (set verbs or handler, not both)\n",
                  app->name);
        return 1;
    }

    AxlArgs *a = (AxlArgs *)axl_calloc(1, sizeof(AxlArgs));
    if (a == NULL) {
        return 1;
    }
    a->app = app;

    /* Initial slot table: globals + (single-verb positionals). Verb
       slots get merged in once the verb is identified. */
    if (!build_slots(a, app, NULL)) {
        free_args(a);
        return 1;
    }

    int  rc          = 0;
    bool parse_error = false;   /* distinguish from handler return value */
    int  i           = 1;
    while (i < argc) {
        const char *arg = argv[i];
        if (is_help_flag(arg)) {
            print_help_for(app, a->verb);
            free_args(a);
            return 0;
        }
        /* In multi-verb mode before a verb is selected, the bare word
           `help` is also accepted as a synonym for --help. */
        if (app->verbs != NULL && a->verb == NULL
            && axl_strcmp(arg, "help") == 0)
        {
            print_help_for(app, NULL);
            free_args(a);
            return 0;
        }
        if (arg[0] == '-' && arg[1] != '\0' && arg[1] != '-') {
            int consumed = parse_flag_token(a, i, argc, argv);
            if (consumed < 0) {
                parse_error = true;
                rc = 1;
                goto out;
            }
            i += consumed;
            continue;
        }
        if (arg[0] == '-' && arg[1] == '-' && arg[2] != '\0') {
            int consumed = parse_flag_token(a, i, argc, argv);
            if (consumed < 0) {
                parse_error = true;
                rc = 1;
                goto out;
            }
            i += consumed;
            continue;
        }
        /* Positional — verb name in multi-verb mode (first one), or
           a positional value for the current verb / single-verb. */
        if (app->verbs != NULL && a->verb == NULL) {
            const AxlVerb *v = find_verb(app, arg);
            if (v == NULL) {
                axl_print("%s: unknown verb '%s'\n", app->name, arg);
                parse_error = true;
                rc = 1;
                goto out;
            }
            if (!attach_verb(a, v)) {
                parse_error = true;
                rc = 1;
                goto out;
            }
            i++;
            continue;
        }
        if (!consume_positional(a, arg)) {
            parse_error = true;
            rc = 1;
            goto out;
        }
        i++;
    }

    /* No verb supplied → show help. */
    if (app->verbs != NULL && a->verb == NULL) {
        print_help_for(app, NULL);
        free_args(a);
        return 1;
    }
    if (validate_required(a) != 0) {
        parse_error = true;
        rc = 1;
        goto out;
    }
    if (app->pre_run != NULL) {
        app->pre_run(a);
    }

    AxlVerbHandler handler = (a->verb != NULL) ? a->verb->handler : app->handler;
    if (handler == NULL) {
        axl_print("%s: no handler for verb\n", app->name);
        rc = 1;
        goto out;
    }
    rc = handler(a);

out:
    /* Only show usage on parse / validation errors. Handler-returned
       error codes are runtime failures (file not found, BMC offline,
       etc.) — flooding the screen with usage after a real error
       message is hostile, and was the loudest UX regression vs the
       prior AxlConfig-based tools. */
    if (parse_error) {
        axl_print("\n");
        print_help_for(app, a->verb);
    }
    free_args(a);
    return rc;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const char *
axl_args_get_string(
    AxlArgs    *args,
    const char *name
    )
{
    if (args == NULL) {
        return NULL;
    }
    ParsedArg *s = slot_by_name(args, name);
    return (s != NULL) ? s->str_value : NULL;
}

bool
axl_args_get_bool(
    AxlArgs    *args,
    const char *name
    )
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
    /* Fall back to default_value if it was provided. */
    return s->str_value != NULL
        && (axl_strcmp(s->str_value, "true") == 0
            || axl_strcmp(s->str_value, "1") == 0
            || axl_strcmp(s->str_value, "yes") == 0);
}

uint64_t
axl_args_get_uint(
    AxlArgs    *args,
    const char *name
    )
{
    if (args == NULL) {
        return 0;
    }
    ParsedArg *s = slot_by_name(args, name);
    return (s != NULL) ? s->uint_value : 0;
}

int64_t
axl_args_get_int(
    AxlArgs    *args,
    const char *name
    )
{
    if (args == NULL) {
        return 0;
    }
    ParsedArg *s = slot_by_name(args, name);
    return (s != NULL) ? s->int_value : 0;
}

int
axl_args_get_pos_count(
    AxlArgs *args
    )
{
    if (args == NULL || args->variadic == NULL) {
        return 0;
    }
    return (int)axl_array_len(args->variadic);
}

const char *
axl_args_get_pos(
    AxlArgs *args,
    int      index
    )
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
axl_args_get_multi_count(
    AxlArgs    *args,
    const char *name
    )
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
axl_args_get_multi(
    AxlArgs    *args,
    const char *name,
    int         index
    )
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
axl_args_user_data(
    AxlArgs *args
    )
{
    return (args != NULL && args->app != NULL) ? args->app->user_data : NULL;
}

const char *
axl_args_program_name(
    AxlArgs *args
    )
{
    return (args != NULL && args->app != NULL) ? args->app->name : NULL;
}
