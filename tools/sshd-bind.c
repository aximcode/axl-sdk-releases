/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * @file sshd-bind.c
 * @brief The pure half of the `sshd` tool's console binding; see sshd-bind.h.
 */

#include "sshd-bind.h"

#include <axl.h>

#define ESC_BYTE    0x1b
#define OUT_CAP     128

/* Where the reassembler collects the text run it is building.  Flushed when
   full, but only ever BETWEEN units (a whole sequence or character), so no
   emitted run splits one. */
typedef struct {
    char        buf[OUT_CAP];
    size_t      n;
    SshdInEmit  emit;
    void       *ud;
    bool        esc_carried;   /* the held ESC came in an earlier packet */
    bool        esc_esc;       /* an ESC was just released by a second ESC in this packet */
} Out;

static void out_flush(Out *o);
static void out_text(Out *o, const uint8_t *bytes, size_t len);
static void out_ctrl(Out *o, char letter);
static bool is_ctrl_key(uint8_t b);
static bool held_expired(const SshdInput *in, uint64_t now_ms);
static void resolve_expired(SshdInput *in, uint64_t now_ms, Out *o);
static void step(SshdInput *in, uint8_t b, uint64_t now_ms, Out *o);
static void begin_unit(SshdInput *in, uint8_t b, uint64_t now_ms, Out *o);
static bool continue_held(SshdInput *in, uint8_t b, uint64_t now_ms, Out *o);
static void hold(SshdInput *in, uint8_t b, uint64_t now_ms);
static size_t utf8_len(uint8_t lead);
static bool utf8_cont_ok(uint8_t lead, size_t have, uint8_t b);
static SshdSession *slot_for(SshdSessions *t, void *chan);
static size_t put_all(char *buf, size_t cap, int n);

/* ---- output ---------------------------------------------------------------- */

static void
out_flush(Out *o)
{
    if (o->n > 0) {
        o->emit(SSHD_IN_TEXT, o->buf, o->n, o->ud);
        o->n = 0;
    }
}

static void
out_text(Out *o, const uint8_t *bytes, size_t len)
{
    if (o->n + len > OUT_CAP) {
        out_flush(o);
    }
    axl_memcpy(o->buf + o->n, bytes, len);
    o->n += len;
}

static void
out_ctrl(Out *o, char letter)
{
    out_flush(o);
    o->emit(SSHD_IN_CTRL, &letter, 1, o->ud);
}

/* ---- input ----------------------------------------------------------------- */

/* A byte that is a control key of its own (SSHD_IN_CTRL): 0x01..0x1a other
   than BS, TAB, LF and CR, which are keys the Shell reads as text. */
static bool
is_ctrl_key(uint8_t b)
{
    return b >= 0x01 && b <= 0x1a && b != 0x08 && b != 0x09 && b != '\n' && b != '\r';
}

static bool
held_expired(const SshdInput *in, uint64_t now_ms)
{
    return now_ms < in->pend_since_ms
        || now_ms - in->pend_since_ms >= SSHD_ESC_TIMEOUT_MS;
}

/* Give up on whatever has waited too long.  The released bytes are a run of
   their own: a stale ESC glued to a later '[' would be read back as a
   sequence. */
static void
resolve_expired(SshdInput *in, uint64_t now_ms, Out *o)
{
    if ((in->npend == 0 && !in->discarding && !in->last_cr)
        || !held_expired(in, now_ms)) {
        return;
    }
    if (in->npend > 0 && in->pend[0] == ESC_BYTE) {
        out_text(o, in->pend, in->npend);
        out_flush(o);
    }
    in->npend      = 0;
    in->discarding = false;
    in->last_cr    = false;
}

static void
hold(SshdInput *in, uint8_t b, uint64_t now_ms)
{
    in->pend[in->npend++] = b;
    in->pend_since_ms     = now_ms;
}

static size_t
utf8_len(uint8_t lead)
{
    if (lead >= 0xf0) {
        return 4;
    }
    return lead >= 0xe0 ? 3 : 2;
}

static bool
utf8_cont_ok(uint8_t lead, size_t have, uint8_t b)
{
    uint8_t lo = 0x80;
    uint8_t hi = 0xbf;

    if (have == 1) {
        if (lead == 0xe0) {
            lo = 0xa0;
        } else if (lead == 0xed) {
            hi = 0x9f;
        } else if (lead == 0xf0) {
            lo = 0x90;
        } else if (lead == 0xf4) {
            hi = 0x8f;
        }
    }
    return b >= lo && b <= hi;
}

/* Feed b to the unit being held.  Returns true if b was consumed; false if
   the held bytes were abandoned and b must be processed afresh. */
static bool
continue_held(SshdInput *in, uint8_t b, uint64_t now_ms, Out *o)
{
    if (in->pend[0] != ESC_BYTE) {
        /* A UTF-8 character: only a continuation byte extends it, and the
           second byte is narrowed after E0, ED, F0 and F4 so that overlong
           forms, surrogates and values past U+10FFFF never get through. */
        if (!utf8_cont_ok(in->pend[0], in->npend, b)) {
            in->npend = 0;
            return false;
        }
        hold(in, b, now_ms);
        if (in->npend == utf8_len(in->pend[0])) {
            out_text(o, in->pend, in->npend);
            in->npend = 0;
        }
        return true;
    }

    if (in->npend == 1) {
        /* An Alt chord is ESC and its key in ONE write, as a terminal sends
           it; an ESC from an earlier packet is a bare Esc and its own run,
           whatever follows it (so a Ctrl-C typed after Esc is still the
           break, and Esc then x is two keys, as at a USB keyboard).  Only a
           sequence ('[' or 'O') may span packets. */
        if (o->esc_carried && b != '[' && b != 'O') {
            out_text(o, in->pend, 1);
            out_flush(o);
            in->npend = 0;
            return false;
        }
        if (is_ctrl_key(b)) {
            /* ESC and a control key: one Alt chord (Ctrl-Alt-X), never Esc
               and then a control key of its own. */
            uint8_t chord[2] = { ESC_BYTE, b };
            out_text(o, chord, 2);
            in->npend = 0;
            return true;
        }
        if (b == ESC_BYTE) {
            /* ESC ESC: Alt+Esc if the packet ends here (sshd_input_feed
               releases the second ESC with it); otherwise the second ESC may
               start a sequence of its own (Esc, then an arrow). */
            o->esc_esc = true;
        }
        if (b != '[' && b != 'O') {
            /* ESC and any other byte coalesce in this run, where the decoder
               reads them as one Alt chord (Alt-x, Alt-Enter); b starts over. */
            out_text(o, in->pend, 1);
            in->npend = 0;
            return false;
        }
        hold(in, b, now_ms);
        return true;
    }

    if (in->pend[1] == 'O') {
        /* ESC O final */
        if (b < 0x20 || b > 0x7e) {
            out_text(o, in->pend, in->npend);
            out_flush(o);
            in->npend = 0;
            return false;
        }
        hold(in, b, now_ms);
        out_text(o, in->pend, in->npend);
        in->npend = 0;
        return true;
    }

    /* ESC [ params final: parameter and intermediate bytes are 0x20-0x3f,
       the final byte 0x40-0x7e. */
    if (b >= 0x20 && b <= 0x3f) {
        if (in->npend == SSHD_INPUT_PEND_MAX) {
            in->npend         = 0;
            in->discarding    = true;
            in->ndiscard      = 1;
            in->pend_since_ms = now_ms;
        } else {
            hold(in, b, now_ms);
        }
        return true;
    }
    if (b >= 0x40 && b <= 0x7e) {
        hold(in, b, now_ms);
        out_text(o, in->pend, in->npend);
        in->npend = 0;
        return true;
    }
    out_text(o, in->pend, in->npend);
    out_flush(o);
    in->npend = 0;
    return false;
}

/* b is the first byte of a unit: nothing is held. */
static void
begin_unit(SshdInput *in, uint8_t b, uint64_t now_ms, Out *o)
{
    if (b == ESC_BYTE || (b >= 0xc2 && b <= 0xf4)) {
        hold(in, b, now_ms);
    } else if (b == '\r') {
        out_text(o, &b, 1);
        in->last_cr       = true;
        in->pend_since_ms = now_ms;
    } else if (b == '\n') {
        static const uint8_t cr = '\r';
        out_text(o, &cr, 1);
    } else if (b == 0x00 || b == 0x08 || b == 0x09 || b >= 0x1c) {
        if (b < 0x80) {
            out_text(o, &b, 1);
        }
        /* else a stray continuation or an invalid lead: dropped */
    } else {
        out_ctrl(o, (char)('a' + (b - 1)));   /* every byte left is_ctrl_key() */
    }
}

static void
step(SshdInput *in, uint8_t b, uint64_t now_ms, Out *o)
{
    bool was_cr = in->last_cr;
    in->last_cr = false;
    o->esc_esc  = false;

    if (in->discarding) {
        if (b >= 0x20 && b <= 0x3f) {
            /* The clock restarts per byte, as for a held sequence: it is the
               gap between bytes that says the sender has stopped.  The count
               bounds a sender that never does. */
            in->pend_since_ms = now_ms;
            if (++in->ndiscard >= SSHD_DISCARD_MAX) {
                in->discarding = false;
            }
            return;
        }
        in->discarding = false;
        if (b >= 0x40 && b <= 0x7e) {
            return;
        }
    } else if (in->npend > 0 && continue_held(in, b, now_ms, o)) {
        return;
    }

    if (b == '\n' && was_cr) {
        return;
    }
    begin_unit(in, b, now_ms, o);
}

void
sshd_input_feed(
    SshdInput     *in,
    const uint8_t *buf,
    size_t         len,
    uint64_t       now_ms,
    SshdInEmit     emit,
    void          *ud)
{
    Out o = { .n = 0, .emit = emit, .ud = ud };

    resolve_expired(in, now_ms, &o);
    o.esc_carried = in->npend == 1 && in->pend[0] == ESC_BYTE;
    for (size_t i = 0; i < len; i++) {
        step(in, buf[i], now_ms, &o);
        o.esc_carried = false;
    }
    if (o.esc_esc && in->npend == 1 && in->pend[0] == ESC_BYTE) {
        out_text(&o, in->pend, 1);   /* ESC ESC ending the packet: Alt+Esc */
        in->npend = 0;
    }
    out_flush(&o);
}

void
sshd_input_tick(
    SshdInput *in,
    uint64_t   now_ms,
    SshdInEmit emit,
    void      *ud)
{
    Out o = { .n = 0, .emit = emit, .ud = ud };

    resolve_expired(in, now_ms, &o);
}

/* ---- sessions -------------------------------------------------------------- */

static SshdSession *
slot_for(SshdSessions *t, void *chan)
{
    for (size_t i = 0; i < SSHD_SESSIONS_MAX; i++) {
        if (t->slots[i].used && t->slots[i].chan == chan) {
            return &t->slots[i];
        }
    }
    return NULL;
}

SshdSession *
sshd_sessions_add(SshdSessions *t, void *chan)
{
    if (chan == NULL || slot_for(t, chan) != NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SSHD_SESSIONS_MAX; i++) {
        SshdSession *s = &t->slots[i];
        if (!s->used) {
            axl_memset(s, 0, sizeof(*s));
            s->chan   = chan;
            s->behind = true;
            s->used   = true;
            s->seq    = t->next_seq++;
            return s;
        }
    }
    return NULL;
}

bool
sshd_sessions_remove(SshdSessions *t, void *chan)
{
    SshdSession *s = slot_for(t, chan);
    if (s == NULL) {
        return false;
    }
    axl_memset(s, 0, sizeof(*s));
    return true;
}

SshdSession *
sshd_sessions_find(SshdSessions *t, void *chan)
{
    return chan == NULL ? NULL : slot_for(t, chan);
}

bool
sshd_sessions_mark_behind(SshdSessions *t, void *chan)
{
    SshdSession *s = sshd_sessions_find(t, chan);
    if (s == NULL) {
        return false;
    }
    s->behind = true;
    return true;
}

bool
sshd_sessions_clear_behind(SshdSessions *t, void *chan)
{
    SshdSession *s = sshd_sessions_find(t, chan);
    if (s == NULL) {
        return false;
    }
    s->behind = false;
    return true;
}

void
sshd_sessions_each(
    SshdSessions *t,
    SshdSessSel   sel,
    SshdSessFn    fn,
    void         *ud)
{
    /* Slots are reused, so array order is not admission order: walk by
       ascending seq.  `limit` fences out sessions added during the walk. */
    uint32_t limit = t->next_seq;
    uint32_t seq   = 0;
    bool     first = true;

    for (;;) {
        SshdSession *next = NULL;
        for (size_t i = 0; i < SSHD_SESSIONS_MAX; i++) {
            SshdSession *s = &t->slots[i];
            if (!s->used || s->seq >= limit || (!first && s->seq <= seq)) {
                continue;
            }
            if (next == NULL || s->seq < next->seq) {
                next = s;
            }
        }
        if (next == NULL) {
            return;
        }
        first = false;
        seq   = next->seq;
        if (sel == SSHD_SESS_ANY
            || (sel == SSHD_SESS_BEHIND) == next->behind) {
            fn(next, ud);
        }
    }
}

size_t
sshd_sessions_count(const SshdSessions *t)
{
    size_t n = 0;
    for (size_t i = 0; i < SSHD_SESSIONS_MAX; i++) {
        if (t->slots[i].used) {
            n++;
        }
    }
    return n;
}

bool
sshd_bind_snapshot_allowed(unsigned in_op)
{
    return in_op == 0;
}

void
sshd_repaint_begin(SshdRepaint *r, size_t len, uint64_t gen)
{
    r->len    = len;
    r->off    = 0;
    r->gen    = gen;
    r->active = true;
}

SshdRpResult
sshd_repaint_drain(SshdRepaint *r, const char *bytes, uint64_t now_gen, size_t piece,
                   SshdRpWrite write, void *ud)
{
    if (!r->active) {
        return SSHD_RP_FAILED;
    }
    while (r->off < r->len) {
        size_t       n  = r->len - r->off < piece ? r->len - r->off : piece;
        SshdWrResult rc = write(bytes + r->off, n, ud);
        if (rc == SSHD_WR_BUSY) {
            return SSHD_RP_BLOCKED;
        }
        if (rc != SSHD_WR_OK) {
            r->active = false;
            return SSHD_RP_FAILED;
        }
        r->off += n;
    }
    r->active = false;
    return r->gen == now_gen ? SSHD_RP_REJOIN : SSHD_RP_RETAKE;
}

bool
sshd_bind_close_on(SshdWrResult r)
{
    return r == SSHD_WR_FAILED;
}

SshdRelease
sshd_bind_release_of(int uninstall_rc)
{
    switch (uninstall_rc) {
    case AXL_BUSY:
        return SSHD_REL_RETRY;
    case AXL_OK:
        return SSHD_REL_RELEASED;
    default:
        return SSHD_REL_STUCK;
    }
}

SshdSetupFail
sshd_bind_setup_failed(SshdRelease r)
{
    return r == SSHD_REL_RELEASED ? SSHD_SETUP_FAIL : SSHD_SETUP_STAY;
}

const char *
sshd_bind_goodbye(SshdRelease r)
{
    switch (r) {
    case SSHD_REL_RELEASED:
        return "[sshd] the UEFI Shell exited; console released";
    case SSHD_REL_STUCK:
        return "[sshd] the UEFI Shell exited; the console could not be released";
    case SSHD_REL_RETRY:
    default:
        return NULL;
    }
}

size_t
sshd_bind_goodbye_text(char *buf, size_t cap, bool region, const char *text)
{
    /* DECSC, reset the region (which homes the cursor), DECRC. */
    static const char reset[] = "\x1b" "7" "\x1b[r" "\x1b" "8";
    static const char open[]  = "\r\n";
    static const char close[] = "\x1b[K\r\n";
    const char *parts[4] = { region ? reset : "", open, text, close };
    size_t      n        = 0;

    for (size_t i = 0; i < 4; i++) {
        n += axl_strlen(parts[i]);
    }
    if (n + 1 > cap) {
        if (cap > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    n = 0;
    for (size_t i = 0; i < 4; i++) {
        size_t len = axl_strlen(parts[i]);
        axl_memcpy(buf + n, parts[i], len);
        n += len;
    }
    buf[n] = '\0';
    return n;
}

SshdRelease
sshd_bind_con_drop(SshdCon *c, int rc)
{
    SshdRelease r = sshd_bind_release_of(rc);

    if (r == SSHD_REL_RELEASED) {
        *c = SSHD_CON_NONE;
    } else if (r == SSHD_REL_STUCK) {
        *c = SSHD_CON_STUCK;
    }
    return r;
}

const char *
sshd_bind_status(bool loaded, bool taken, bool stuck)
{
    if (!loaded) {
        return "stopped";
    }
    if (stuck) {
        return "stuck - " SSHD_STUCK_REASON;
    }
    return taken ? "running" : "stopped (the Shell it served exited)";
}

SshdFanResult
sshd_fan_write(const char *bytes, size_t len, size_t limit, SshdRpWrite write, void *ud)
{
    if (len == 0) {
        return SSHD_FAN_SENT;
    }
    if (len > limit) {
        return SSHD_FAN_TOO_BIG;
    }
    /* ONE write: a flush split into pieces could be accepted part-way and
       then refused, leaving the session's stream inside an escape sequence. */
    switch (write(bytes, len, ud)) {
    case SSHD_WR_OK:
        return SSHD_FAN_SENT;
    case SSHD_WR_BUSY:
        return SSHD_FAN_BEHIND;
    case SSHD_WR_GONE:
        return SSHD_FAN_GONE;
    case SSHD_WR_FAILED:
    default:
        return SSHD_FAN_FAILED;
    }
}

/* ---- pending input ----------------------------------------------------------
   Records are laid end to end from head: kind (1 byte), length (2 bytes,
   little-endian), then the bytes.  Taking part of the front record moves its
   header forward over the bytes just taken, so the rest is again one whole
   record at the (new) head. */

#define PEND_HDR       3u
#define PEND_RUN_MAX   0xFFFFu

static size_t
pend_rec_len(const SshdPend *p, size_t at)
{
    return (size_t)p->buf[at + 1] | ((size_t)p->buf[at + 2] << 8);
}

static void
pend_put_hdr(SshdPend *p, size_t at, uint8_t kind, size_t len)
{
    p->buf[at]     = kind;
    p->buf[at + 1] = (uint8_t)(len & 0xFFu);
    p->buf[at + 2] = (uint8_t)(len >> 8);
}

bool
sshd_pend_push(SshdPend *p, SshdInKind kind, const char *bytes, size_t len)
{
    size_t need = PEND_HDR + len;

    if (len == 0 || len > PEND_RUN_MAX || bytes == NULL || need > SSHD_PEND_BYTES - p->used) {
        p->dropped += len;
        return false;
    }
    if (p->buf == NULL) {
        p->buf = axl_malloc(SSHD_PEND_BYTES);
        if (p->buf == NULL) {
            p->dropped += len;
            return false;
        }
    }
    if (p->head + p->used + need > SSHD_PEND_BYTES) {
        /* Room exists, but behind the head: slide what is pending to the front. */
        for (size_t i = 0; i < p->used; i++) {
            p->buf[i] = p->buf[p->head + i];
        }
        p->head = 0;
    }
    size_t at = p->head + p->used;
    pend_put_hdr(p, at, (uint8_t)kind, len);
    for (size_t i = 0; i < len; i++) {
        p->buf[at + PEND_HDR + i] = (uint8_t)bytes[i];
    }
    p->used += need;
    return true;
}

bool
sshd_pend_empty(const SshdPend *p)
{
    return p->used == 0;
}

bool
sshd_pend_drain(SshdPend *p, SshdPendTake take, void *ud)
{
    while (p->used > 0) {
        SshdInKind kind = (SshdInKind)p->buf[p->head];
        size_t     len  = pend_rec_len(p, p->head);
        size_t     got  = take(kind, (const char *)p->buf + p->head + PEND_HDR, len, ud);

        if (got >= len) {
            p->head += PEND_HDR + len;
            p->used -= PEND_HDR + len;
            continue;
        }
        if (got > 0) {
            p->head += got;
            p->used -= got;
            pend_put_hdr(p, p->head, (uint8_t)kind, len - got);
        }
        return false;
    }
    p->head = 0;
    return true;
}

bool
sshd_ctrl_bypasses(char letter)
{
    return letter == 'c';
}

bool
sshd_ctrl_may_enter(char letter, size_t unread_keys)
{
    return sshd_ctrl_bypasses(letter) || unread_keys == 0;
}

bool
sshd_input_route(SshdPend *p, SshdInKind kind, const char *bytes, size_t len,
                 SshdPendTake take, void *ud)
{
    if (kind == SSHD_IN_CTRL && sshd_ctrl_bypasses(bytes[0])) {
        take(kind, bytes, len, ud);
        return true;
    }
    if (sshd_pend_empty(p)) {
        size_t used = take(kind, bytes, len, ud);
        if (used >= len) {
            return true;
        }
        bytes += used;
        len   -= used;
    }
    return sshd_pend_push(p, kind, bytes, len);
}

void
sshd_pend_free(SshdPend *p)
{
    if (p != NULL) {
        axl_free(p->buf);
        p->buf  = NULL;
        p->head = 0;
        p->used = 0;
    }
}

bool
sshd_ratelimit(SshdRateLimit *r, uint64_t now_ms, uint64_t period_ms, uint32_t *suppressed)
{
    if (r->ever && now_ms - r->last_ms < period_ms && now_ms >= r->last_ms) {
        r->suppressed++;
        return false;
    }
    if (suppressed != NULL) {
        *suppressed = r->suppressed;
    }
    r->suppressed = 0;
    r->last_ms    = now_ms;
    r->ever       = true;
    return true;
}

SshdFit
sshd_fit(uint32_t con_cols, uint32_t con_rows, uint32_t term_cols, uint32_t term_rows)
{
    if (term_cols == 0 || term_rows == 0) {
        return SSHD_FIT_EXACT;   /* no size given: taken as the console's */
    }
    if (term_cols < con_cols || term_rows < con_rows) {
        return SSHD_FIT_SMALLER;
    }
    if (term_cols == con_cols && term_rows == con_rows) {
        return SSHD_FIT_EXACT;
    }
    return SSHD_FIT_LARGER;
}

/* snprintf into buf, all or nothing: 0 (and buf untouched past [0]) when it
   does not fit. */
static size_t
put_all(char *buf, size_t cap, int n)
{
    if (n < 0 || (size_t)n >= cap) {
        if (cap > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    return (size_t)n;
}

size_t
sshd_fit_prefix(char *buf, size_t cap, SshdFit fit, uint32_t con_rows, bool region_set)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    if (fit == SSHD_FIT_LARGER) {
        return put_all(buf, cap, axl_snprintf(buf, cap, "\x1b[1;%ur", (unsigned)con_rows));
    }
    if (fit == SSHD_FIT_EXACT && region_set) {
        return put_all(buf, cap, axl_snprintf(buf, cap, "\x1b[r"));
    }
    buf[0] = '\0';
    return 0;
}

size_t
sshd_held_notice(char *buf, size_t cap, uint32_t con_cols, uint32_t con_rows,
                 uint32_t term_cols, uint32_t term_rows)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    return put_all(buf, cap, axl_snprintf(buf, cap,
        "\x1b[r\x1b[m\x1b[2J\x1b[H"
        "[sshd] this terminal is %ux%u; the console is %ux%u.\r\n"
        "[sshd] enlarge it to at least %ux%u to see the console.\r\n"
        "[sshd] what you type still reaches the console.\r\n",
        (unsigned)term_cols, (unsigned)term_rows, (unsigned)con_cols, (unsigned)con_rows,
        (unsigned)con_cols, (unsigned)con_rows));
}

size_t
sshd_banner_compose(char *out, size_t cap, const char *user, size_t user_len,
                    uint32_t cols, uint32_t rows)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    if (user == NULL) {
        user_len = 0;
    }
    bool   nl = user_len == 0 || user[user_len - 1] == '\n';
    size_t at = user_len + (nl ? 0u : 1u);

    if (at >= cap) {
        out[0] = '\0';
        return 0;
    }
    size_t n = put_all(out + at, cap - at, axl_snprintf(out + at, cap - at,
        "sshd: the console is %ux%u; a smaller terminal waits until it is enlarged\n",
        (unsigned)cols, (unsigned)rows));
    if (n == 0) {
        out[0] = '\0';
        return 0;
    }
    axl_memcpy(out, user, user_len);
    if (!nl) {
        out[user_len] = '\n';
    }
    return at + n;
}

bool
sshd_bind_can_log_in(bool pubkey, bool have_keys, bool pwauth, bool have_password)
{
    return (pubkey && have_keys) || (pwauth && have_password);
}


void
sshd_bind_addr_list(char *out, size_t cap, const char *const *ips, size_t n)
{
    size_t len  = 0;
    size_t more = 0;

    out[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        char   tail[32];
        size_t sep     = len > 0 ? 2u : 0u;
        size_t need    = len + sep + axl_strlen(ips[i]);
        size_t reserve = 0;
        if (i + 1 < n) {
            reserve = (size_t)axl_snprintf(tail, sizeof(tail), ", +%zu more", n - i - 1);
        }
        if (need + reserve >= cap) {
            more = n - i;
            break;
        }
        if (sep > 0) {
            axl_strlcat(out, ", ", cap);
        }
        axl_strlcat(out, ips[i], cap);
        len = need;
    }
    if (more > 0) {
        char tail[32];
        axl_snprintf(tail, sizeof(tail), "%s+%zu more", len > 0 ? ", " : "", more);
        axl_strlcat(out, tail, cap);
    } else if (len == 0) {
        axl_strlcpy(out, "0.0.0.0", cap);
    }
}

int
sshd_bind_log_level(uint32_t level)
{
    static const int8_t axl[] = {
        [SSHD_LOG_QUIET]   = AXL_LOG_ERROR,
        [SSHD_LOG_FATAL]   = AXL_LOG_ERROR,
        [SSHD_LOG_ERROR]   = AXL_LOG_ERROR,
        [SSHD_LOG_INFO]    = AXL_LOG_INFO,
        [SSHD_LOG_VERBOSE] = AXL_LOG_DEBUG,
        [SSHD_LOG_DEBUG1]  = AXL_LOG_DEBUG,
        [SSHD_LOG_DEBUG2]  = AXL_LOG_TRACE,
        [SSHD_LOG_DEBUG3]  = AXL_LOG_TRACE,
    };

    if (level == SSHD_LOG_UNSET || level >= AXL_ARRAY_SIZE(axl)) {
        return -1;
    }
    return axl[level];
}

bool
sshd_bind_log_console(bool serving, bool moved, bool have_serial, bool before, bool now)
{
    if (!serving && !moved) {
        return now;
    }
    return before && (!serving || have_serial);
}

SshdBannerRead
sshd_bind_banner_text(const void *raw, size_t len, size_t max, char **out, size_t *out_len)
{
    char  *text = NULL;
    size_t n    = 0;
    int    rc   = axl_text_to_utf8(raw, len, &text, &n, NULL);

    *out = NULL;
    if (rc == AXL_NO_RESOURCES) {
        return SSHD_BANNER_NO_MEMORY;
    }
    if (rc != AXL_OK || !axl_utf8_validate(text, n, NULL)) {
        axl_free(text);
        return SSHD_BANNER_NOT_TEXT;
    }
    if (n > max) {
        axl_free(text);
        return SSHD_BANNER_TOO_LARGE;
    }
    *out     = text;
    *out_len = n;
    return SSHD_BANNER_OK;
}

// ---------------------------------------------------------------------------
// AxlLogPort
// ---------------------------------------------------------------------------

SshdLogPick
sshd_bind_log_port_pick(uint64_t want, const bool *console, size_t n, size_t *index)
{
    if (want == SSHD_LOG_PORT_NONE) {
        return SSHD_LOG_OFF_NONE;
    }
    if (want != SSHD_LOG_PORT_AUTO) {
        if (want >= n) {
            return SSHD_LOG_NO_SUCH_PORT;
        }
        *index = (size_t)want;
        return SSHD_LOG_TO_PORT;
    }
    for (size_t i = 0; i < n; i++) {
        if (!console[i]) {
            *index = i;
            return SSHD_LOG_TO_PORT;
        }
    }
    return SSHD_LOG_OFF_CONSOLE;
}

size_t
sshd_bind_log_append(char *buf, size_t cap, size_t len, const char *line, size_t n)
{
    if (n > cap) {
        return len;
    }
    if (len > cap) {
        len = cap;   /* defence: never trust a length past the buffer */
    }
    if (len + n > cap) {
        /* Drop up to the first line start at or past both the half and
           what the new line needs; none such drops everything. */
        size_t need = len + n - cap;
        size_t from = need > cap / 2 ? need : cap / 2;
        size_t cut  = len;
        for (size_t i = from; i < len; i++) {
            if (buf[i - 1] == '\n') {
                cut = i;
                break;
            }
        }
        axl_memmove(buf, buf + cut, len - cut);
        len -= cut;
    }
    axl_memcpy(buf + len, line, n);
    /* One append is one line (the caller counts appends): a message's own
       line breaks become spaces, so they cannot split it in the text. */
    for (size_t i = len; i + 1 < len + n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            buf[i] = ' ';
        }
    }
    return len + n;
}

bool
sshd_bind_newest_error(const char *text, size_t len, size_t newest, const char **line,
                       size_t *line_len)
{
    size_t end = len;

    /* Walk the lines newest first: [start, end) without its '\n'. */
    while (end > 0 && newest > 0) {
        size_t stop = end;
        if (text[stop - 1] == '\n') {
            stop--;
        }
        size_t start = stop;
        while (start > 0 && text[start - 1] != '\n') {
            start--;
        }
        if (stop - start >= 8 && axl_memcmp(text + start, "[ERROR] ", 8) == 0) {
            *line     = text + start;
            *line_len = stop - start;
            return true;
        }
        end = start;
        newest--;
    }
    return false;
}

bool
sshd_bind_hostkey_next(const char **cursor, char *out, size_t cap)
{
    const char *p = *cursor;

    if (p == NULL || *p == '\0' || cap == 0) {
        return false;
    }
    size_t n = 0;
    while (p[n] != '\0' && p[n] != SSHD_HOSTKEY_SEP) {
        n++;
    }
    size_t keep = n < cap - 1 ? n : cap - 1;
    axl_memcpy(out, p, keep);
    out[keep] = '\0';
    *cursor = p[n] == SSHD_HOSTKEY_SEP ? p + n + 1 : p + n;
    return true;
}
