/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-console-tee.c
    Fan one console producer's AxlConsoleOps out to several consumers.

    Mechanically dull for the eleven `void` ops — call each consumer that bound one —
    and deliberately careful for the two that return NEGOTIATION (`scrollrect`,
    `set_term_prop`). Those are the reason this lives in the SDK: split across
    consumers the answers can disagree, and answering wrong corrupts a grid with no
    diagnostic. See <axl/axl-console-tee.h> for the rule and why it points the way
    it does.
**/

#include <axl/axl-console-tee.h>
#include <axl/axl-mem.h>

typedef struct {
    const AxlConsoleOps *ops;
    void                *user;
} TeeSink;

struct AxlConsoleTee {
    TeeSink sinks[AXL_CONSOLE_TEE_MAX];
    size_t  count;
};

/* Every forwarder walks this. Consumers are called in the order they were added.
   `it` is a loop-variable NAME (a declarator and an increment target), not an
   expression, so it cannot be parenthesized — bugprone-macro-parentheses is a
   false positive here; `t` is parenthesized as it should be. */
// NOLINTBEGIN(bugprone-macro-parentheses)
#define TEE_FOR_EACH(t, it) \
    for (TeeSink *it = (t)->sinks; it < (t)->sinks + (t)->count; it++)
// NOLINTEND(bugprone-macro-parentheses)

// ---------------------------------------------------------------------------
// The void ops — forward to every consumer that bound them.
// ---------------------------------------------------------------------------

static void
tee_set_cell_rule(void *user, AxlConsoleCellRule rule)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    TEE_FOR_EACH(t, s) {
        if (s->ops->set_cell_rule != NULL) {
            s->ops->set_cell_rule(s->user, rule);
        }
    }
}

static void
tee_clear_screen(void *user)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    TEE_FOR_EACH(t, s) {
        if (s->ops->clear_screen != NULL) {
            s->ops->clear_screen(s->user);
        }
    }
}

static void
tee_set_cursor(void *user, int32_t row, int32_t col)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    TEE_FOR_EACH(t, s) {
        if (s->ops->set_cursor != NULL) {
            s->ops->set_cursor(s->user, row, col);
        }
    }
}

static void
tee_output_text(void *user, const char *utf8, size_t len)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    TEE_FOR_EACH(t, s) {
        if (s->ops->output_text != NULL) {
            s->ops->output_text(s->user, utf8, len);
        }
    }
}

static void
tee_set_pen(void *user, const AxlConsolePen *pen)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    TEE_FOR_EACH(t, s) {
        if (s->ops->set_pen != NULL) {
            s->ops->set_pen(s->user, pen);
        }
    }
}

static void
tee_set_mode(void *user, uint32_t mode)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    TEE_FOR_EACH(t, s) {
        if (s->ops->set_mode != NULL) {
            s->ops->set_mode(s->user, mode);
        }
    }
}

static void
tee_resize(void *user, uint32_t cols, uint32_t rows)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    TEE_FOR_EACH(t, s) {
        if (s->ops->resize != NULL) {
            s->ops->resize(s->user, cols, rows);
        }
    }
}

static void
tee_erase(void *user, AxlConsoleRect rect, bool selective)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    TEE_FOR_EACH(t, s) {
        if (s->ops->erase != NULL) {
            s->ops->erase(s->user, rect, selective);
        }
    }
}

static void
tee_moverect(void *user, AxlConsoleRect dest, AxlConsoleRect src)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    TEE_FOR_EACH(t, s) {
        if (s->ops->moverect != NULL) {
            s->ops->moverect(s->user, dest, src);
        }
    }
}

static void
tee_bell(void *user)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    TEE_FOR_EACH(t, s) {
        if (s->ops->bell != NULL) {
            s->ops->bell(s->user);
        }
    }
}

static void
tee_clear_scrollback(void *user)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    TEE_FOR_EACH(t, s) {
        if (s->ops->clear_scrollback != NULL) {
            s->ops->clear_scrollback(s->user);
        }
    }
}

// ---------------------------------------------------------------------------
// The negotiated ops — accepted only if EVERY consumer accepted.
// ---------------------------------------------------------------------------

static int
tee_scrollrect(void *user, AxlConsoleRect rect, int32_t downward, int32_t rightward)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    int            all_accepted = 1;
    TEE_FOR_EACH(t, s) {
        /* An unbound scrollrect means "I cannot scroll" — the same answer as an
           explicit decline, and it must be counted as one or the producer would skip
           the damage that consumer depends on. */
        if (s->ops->scrollrect == NULL || s->ops->scrollrect(s->user, rect, downward,
                                                             rightward) == 0) {
            all_accepted = 0;
        }
        /* Deliberately NO short-circuit: ask every consumer even once one has
           declined. A consumer that would have scrolled must still be given the
           chance to (it converges when the producer's damage repaint arrives), and
           short-circuiting would make the result depend on add order. */
    }
    return all_accepted;
}

static int
tee_set_term_prop(void *user, AxlConsoleProp prop, const AxlConsoleValue *val)
{
    AxlConsoleTee *t = (AxlConsoleTee *)user;
    int            all_accepted = 1;
    TEE_FOR_EACH(t, s) {
        if (s->ops->set_term_prop == NULL
            || s->ops->set_term_prop(s->user, prop, val) == 0) {
            all_accepted = 0;
        }
    }
    return all_accepted;
}

/* Every op is bound unconditionally. A producer inspects the vtable to decide what it
   may rely on (notably: erase must be bound whenever a scroll can be declined), and
   with a tee in the middle a scroll can ALWAYS be declined — so presenting a partial
   vtable that mirrored whatever the current consumers happened to bind would be a
   trap. Ops no consumer implements degrade to a no-op inside the forwarder. */
static const AxlConsoleOps tee_ops = {
    .set_cell_rule    = tee_set_cell_rule,
    .clear_screen     = tee_clear_screen,
    .set_cursor       = tee_set_cursor,
    .output_text      = tee_output_text,
    .set_pen          = tee_set_pen,
    .set_mode         = tee_set_mode,
    .resize           = tee_resize,
    .erase            = tee_erase,
    .moverect         = tee_moverect,
    .bell             = tee_bell,
    .scrollrect       = tee_scrollrect,
    .set_term_prop    = tee_set_term_prop,
    .clear_scrollback = tee_clear_scrollback,
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AxlConsoleTee *
axl_console_tee_new(void)
{
    return axl_calloc(1, sizeof(AxlConsoleTee));
}

void
axl_console_tee_free(AxlConsoleTee *t)
{
    axl_free(t);   /* borrowed consumers are the caller's to free */
}

int
axl_console_tee_add(AxlConsoleTee *t, const AxlConsoleOps *ops, void *user)
{
    if (t == NULL || ops == NULL || t->count >= AXL_CONSOLE_TEE_MAX) {
        return AXL_ERR;
    }
    t->sinks[t->count].ops  = ops;
    t->sinks[t->count].user = user;
    t->count++;
    return AXL_OK;
}

const AxlConsoleOps *
axl_console_tee_ops(AxlConsoleTee *t, void **user)
{
    if (t == NULL) {
        return NULL;
    }
    if (user != NULL) {
        *user = t;
    }
    return &tee_ops;
}
