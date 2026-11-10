/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-chan.c
    The channel table axl-ssh-chan.h declares: pure bookkeeping over slots,
    windows, one ordered output queue per channel and the server-wide
    budget.

    The output queue is ONE byte string of runs, [stream u8][remaining u32]
    [bytes], in write order, because a pty merges stdout and stderr and a
    terminal must show them as written: two queues would reorder them. A
    write onto a run of the same stream extends it, so a burst of small
    writes leaves as few packets as the window allows; a packet never spans
    two runs, because the two streams are different SSH messages.
**/

#include "axl-ssh-chan.h"
#include <axl/axl-macros.h>
#include <axl/axl-mem.h>
#include <axl/axl-str.h>

/* A run header: the stream byte, then the run's remaining length. */
#define CHAN_RUN_HDR  (1u + sizeof(uint32_t))

/* Below this capacity a drained queue keeps its buffer: shrinking it would
   only cost a reallocation for the next burst. */
#define CHAN_TRIM_MIN  4096u

/* OpenSSH's adjust rule has two clauses -- the window more than three
   maximum packets below its size, OR below half of it. The second only
   matters for a window smaller than six packets; ours is 64, so the first
   implies it and adjust_due() asks only the first. A change of either size
   that breaks that must restore the second clause. */
_Static_assert(AXL_SSH_CHAN_LOCAL_WINDOW / 2u > AXL_SSH_CHAN_ADJUST_SLACK,
               "adjust rule: below half must be implied by three packets");

/* The next handle axl_ssh_chan_next_id() issues. Image-wide, so a handle
   from one server never names a channel of another. */
static uint32_t chan_next_id = 1;

/* ------------------------------------------------------------------------ */
/* Budget and handles                                                        */
/* ------------------------------------------------------------------------ */

void
axl_ssh_chan_budget_init(
    AxlSshChanBudget *b,
    size_t            per_channel,
    size_t            total,
    unsigned          max_channels
)
{
    if (b == NULL) {
        return;
    }
    b->limit_per_channel = per_channel;
    b->limit_total       = total;
    b->queued_total      = 0;
    b->fair_share        = max_channels > 0 ? total / max_channels : 0;
}

uint32_t
axl_ssh_chan_next_id(void)
{
    if (chan_next_id == 0) {
        chan_next_id = 1;
    }
    return chan_next_id++;
}

void
axl_ssh_chan_set_next_id(
    uint32_t next
)
{
    chan_next_id = next;
}

/* ------------------------------------------------------------------------ */
/* The table                                                                 */
/* ------------------------------------------------------------------------ */

AxlSshChanSlot *
axl_ssh_chan_alloc(
    AxlSshChanSlot *tab,
    unsigned        cap,
    unsigned        max_sessions
)
{
    if (tab == NULL) {
        return NULL;
    }
    /* A retired slot is still in use: its wire number is reserved until
       the peer's CLOSE, so it counts against the session cap. */
    unsigned        used = 0;
    AxlSshChanSlot *free_slot = NULL;
    for (unsigned i = 0; i < cap; i++) {
        if (tab[i].id != 0) {
            used++;
        } else if (free_slot == NULL) {
            free_slot = &tab[i];
        }
    }
    if (free_slot == NULL || used >= max_sessions) {
        return NULL;
    }
    axl_memset(free_slot, 0, sizeof *free_slot);
    free_slot->id       = axl_ssh_chan_next_id();
    free_slot->local_id = (uint32_t)(free_slot - tab);
    return free_slot;
}

AxlSshChanSlot *
axl_ssh_chan_find(
    AxlSshChanSlot *tab,
    unsigned        cap,
    uint32_t        id
)
{
    if (tab == NULL || id == 0) {
        return NULL;
    }
    for (unsigned i = 0; i < cap; i++) {
        if (tab[i].id == id && !tab[i].retired) {
            return &tab[i];
        }
    }
    return NULL;
}

AxlSshChanSlot *
axl_ssh_chan_find_peer(
    AxlSshChanSlot *tab,
    unsigned        cap,
    uint32_t        local_id
)
{
    if (tab == NULL || local_id >= cap || tab[local_id].id == 0) {
        return NULL;
    }
    return &tab[local_id];
}

/* Give every queued byte back to the budget and empty the queue. */
static void
chan_discard_output(
    AxlSshChanSlot   *ch,
    AxlSshChanBudget *b
)
{
    if (b != NULL) {
        b->queued_total = b->queued_total >= ch->queued
                              ? b->queued_total - ch->queued
                              : 0;
    }
    ch->queued = 0;
    axl_string_free(ch->out);
    ch->out = NULL;
}

/* Forget a refusal: the writable it owed is paid, or no longer owed. */
static void
chan_clear_busy(
    AxlSshChanSlot *ch
)
{
    ch->busy_on_chan   = false;
    ch->busy_on_total  = false;
    ch->busy_had_queue = false;
    ch->busy_len       = 0;
}

void
axl_ssh_chan_retire(
    AxlSshChanSlot   *ch,
    AxlSshChanBudget *b
)
{
    if (ch == NULL) {
        return;
    }
    chan_discard_output(ch, b);
    chan_clear_busy(ch);
    ch->retired = true;
}

void
axl_ssh_chan_free(
    AxlSshChanSlot   *ch,
    AxlSshChanBudget *b
)
{
    if (ch == NULL) {
        return;
    }
    chan_discard_output(ch, b);
    axl_memset(ch, 0, sizeof *ch);
}

/* ------------------------------------------------------------------------ */
/* Output                                                                    */
/* ------------------------------------------------------------------------ */

/* Append @a len bytes of @a stream to @a ch's queue, extending the last
   run when it is the same stream. All or nothing: on failure the string
   is truncated back to where it was. */
static int
chan_append(
    AxlSshChanSlot *ch,
    uint8_t         stream,
    const void     *buf,
    size_t          len
)
{
    if (ch->out == NULL) {
        ch->out = axl_string_new("");
        if (ch->out == NULL) {
            return AXL_NO_RESOURCES;
        }
    }
    size_t was  = axl_string_len(ch->out);
    size_t last = ch->out_last;
    if (was > 0 && (uint8_t)axl_string_str(ch->out)[last] == stream) {
        if (axl_string_append_len(ch->out, (const char *)buf, len) != AXL_OK) {
            axl_string_truncate(ch->out, was);
            return AXL_NO_RESOURCES;
        }
        /* The data string may have moved; re-read through data(). */
        uint8_t *d = (uint8_t *)axl_string_data(ch->out);
        uint32_t run;
        axl_memcpy(&run, d + last + 1, sizeof run);
        run += (uint32_t)len;
        axl_memcpy(d + last + 1, &run, sizeof run);
        return AXL_OK;
    }
    uint8_t  hdr[CHAN_RUN_HDR];
    uint32_t run = (uint32_t)len;
    hdr[0]       = stream;
    axl_memcpy(hdr + 1, &run, sizeof run);
    if (axl_string_append_len(ch->out, (const char *)hdr, sizeof hdr) != AXL_OK
        || axl_string_append_len(ch->out, (const char *)buf, len) != AXL_OK) {
        axl_string_truncate(ch->out, was);
        return AXL_NO_RESOURCES;
    }
    ch->out_last = was;
    return AXL_OK;
}

int
axl_ssh_chan_write(
    AxlSshChanSlot   *ch,
    AxlSshChanBudget *b,
    bool              stderr_,
    const void       *buf,
    size_t            len
)
{
    if (ch == NULL || b == NULL) {
        return AXL_INVALID;
    }
    if (ch->id == 0 || ch->retired) {
        return AXL_NOT_FOUND;
    }
    if (ch->exit_pending) {
        return AXL_ERR;
    }
    if (len == 0) {
        return AXL_OK;
    }
    if (buf == NULL) {
        return AXL_INVALID;
    }
    /* Longer than the channel limit can never succeed: an AXL_BUSY would
       leave the caller waiting for a writable that cannot help. */
    if (len > b->limit_per_channel) {
        return AXL_ERR;
    }

    /* The total is judged even when the channel limit already refused:
       a refusal on both owes both conditions, or the writable could fire
       on the channel's drain alone and hand back a retry the total still
       refuses. */
    bool on_chan  = ch->queued + len > b->limit_per_channel;
    bool on_total = ch->queued + len > b->fair_share
                    && b->queued_total + len > b->limit_total;
    if (on_chan || on_total) {
        ch->busy_on_chan   = ch->busy_on_chan || on_chan;
        ch->busy_on_total  = ch->busy_on_total || on_total;
        ch->busy_had_queue = ch->busy_had_queue || (on_total && ch->queued > 0);
        if (len > ch->busy_len) {
            ch->busy_len = len;
        }
        return AXL_BUSY;
    }

    int r = chan_append(ch, stderr_ ? 1u : 0u, buf, len);
    if (r != AXL_OK) {
        return r;
    }
    ch->queued      += len;
    b->queued_total += len;
    return AXL_OK;
}

bool
axl_ssh_chan_take_writable(
    AxlSshChanSlot         *ch,
    const AxlSshChanBudget *b
)
{
    if (ch == NULL || b == NULL || !(ch->busy_on_chan || ch->busy_on_total)) {
        return false;
    }
    if (ch->exit_pending || ch->retired) {
        chan_clear_busy(ch);
        return false;
    }
    size_t q = ch->queued;
    size_t t = b->queued_total;
    size_t l = ch->busy_len;

    /* Every trigger requires the largest refused write to FIT: "below half"
       alone can hold at the refusal itself, and firing then would hand the
       application a writable with nothing changed. */
    /* "Below half" as 2q < limit, not q < limit / 2: integer halving makes
       a limit of 1 unreachable and fires an odd limit half a byte late.
       Both limits are at most 64 MB, so the doubling cannot overflow. */
    bool chan_ok = !ch->busy_on_chan
                   || (2 * q < b->limit_per_channel
                       && q + l <= b->limit_per_channel);
    /* The drain release needs a REAL drain: a queue that was already empty
       at every refusal on the total cannot drain further, and releasing on
       it would spin. */
    bool total_ok = !ch->busy_on_total
                    || (2 * t < b->limit_total && t + l <= b->limit_total)
                    || (ch->busy_had_queue && q == 0);
    if (!chan_ok || !total_ok) {
        return false;
    }
    chan_clear_busy(ch);
    return true;
}

/* Give back the memory a drained queue no longer needs. The budget bounds
   QUEUED bytes, but a string keeps its high-water capacity: without this,
   channels taking turns at the per-channel limit would each retain it, and
   the heap would grow to channels x limit -- the very figure the server
   total exists to prevent. An emptied queue is freed outright; one at under
   half its capacity (and past CHAN_TRIM_MIN) is shrunk to fit. */
static void
chan_trim(
    AxlSshChanSlot *ch
)
{
    if (ch->out == NULL) {
        return;
    }
    size_t len = axl_string_len(ch->out);
    if (len == 0) {
        axl_string_free(ch->out);
        ch->out = NULL;
        return;
    }
    size_t cap = axl_string_capacity(ch->out);
    if (cap > CHAN_TRIM_MIN && cap / 2 > len) {
        axl_string_shrink_to_fit(ch->out);
    }
}

const uint8_t *
axl_ssh_chan_head(
    const AxlSshChanSlot *ch
)
{
    if (ch == NULL || ch->out == NULL
        || axl_string_len(ch->out) <= CHAN_RUN_HDR) {
        return NULL;
    }
    return (const uint8_t *)axl_string_str(ch->out) + CHAN_RUN_HDR;
}

size_t
axl_ssh_chan_plan_send(
    const AxlSshChanSlot *ch,
    bool                 *stderr_
)
{
    const uint8_t *data = axl_ssh_chan_head(ch);
    if (data == NULL) {
        return 0;
    }
    const uint8_t *hdr = data - CHAN_RUN_HDR;
    uint32_t       run;
    axl_memcpy(&run, hdr + 1, sizeof run);

    size_t n = run;
    if (n > ch->peer_window) {
        n = ch->peer_window;
    }
    if (n > ch->peer_maxpkt) {
        n = ch->peer_maxpkt;
    }
    if (n > AXL_SSH_CHAN_OUT_MAXPKT) {
        n = AXL_SSH_CHAN_OUT_MAXPKT;
    }
    if (n > 0 && stderr_ != NULL) {
        *stderr_ = hdr[0] != 0;
    }
    return n;
}

void
axl_ssh_chan_sent(
    AxlSshChanSlot   *ch,
    AxlSshChanBudget *b,
    bool              stderr_,
    size_t            n
)
{
    const uint8_t *data = axl_ssh_chan_head(ch);
    if (data == NULL || b == NULL || n == 0) {
        return;
    }
    const uint8_t *hdr = data - CHAN_RUN_HDR;
    uint32_t       run;
    axl_memcpy(&run, hdr + 1, sizeof run);
    if ((hdr[0] != 0) != stderr_ || n > run || n > ch->peer_window
        || n > ch->queued) {
        return;
    }

    if (n == run) {
        axl_string_erase(ch->out, 0, CHAN_RUN_HDR + n);
        /* Every later run moved up by what was erased; an emptied queue
           has no last run, and out_last is then unused. */
        ch->out_last = ch->out_last >= CHAN_RUN_HDR + n
                           ? ch->out_last - (CHAN_RUN_HDR + n)
                           : 0;
    } else {
        /* Keep the header, drop the sent bytes behind it. */
        uint8_t *d = (uint8_t *)axl_string_data(ch->out);
        run       -= (uint32_t)n;
        axl_memcpy(d + 1, &run, sizeof run);
        axl_string_erase(ch->out, CHAN_RUN_HDR, n);
        if (ch->out_last > 0) {
            ch->out_last -= n;
        }
    }
    ch->peer_window -= (uint32_t)n;
    ch->queued      -= n;
    b->queued_total  = b->queued_total >= n ? b->queued_total - n : 0;
    chan_trim(ch);
}

int
axl_ssh_chan_peer_adjust(
    AxlSshChanSlot *ch,
    uint32_t        adj
)
{
    if (ch == NULL) {
        return AXL_INVALID;
    }
    /* Crossed our CLOSE: nothing will be sent again, so it is ignored --
       even one that would overflow, which only matters for a channel that
       can still send. */
    if (ch->retired) {
        return AXL_OK;
    }
    if (adj > UINT32_MAX - ch->peer_window) {
        return AXL_ERR;
    }
    ch->peer_window += adj;
    return AXL_OK;
}

/* ------------------------------------------------------------------------ */
/* Input                                                                     */
/* ------------------------------------------------------------------------ */

void
axl_ssh_chan_start(
    AxlSshChanSlot *ch,
    uint32_t       *adjust
)
{
    if (adjust != NULL) {
        *adjust = 0;
    }
    if (ch == NULL || ch->started) {
        return;
    }
    ch->started      = true;
    ch->local_window = AXL_SSH_CHAN_LOCAL_WINDOW;
    ch->credit       = 0;
    ch->exceeded     = 0;
    if (adjust != NULL) {
        *adjust = AXL_SSH_CHAN_LOCAL_WINDOW;
    }
}

/* Count @a n bytes against the window. A peer that sends past it is
   tracked in `exceeded`, sshd's local_window_exceeded: more than the grace
   is "peer ignored channel window". Where the bytes go -- owed back at
   once, or held until the application consumes them -- is the caller's. */
static void
chan_count(
    AxlSshChanSlot *ch,
    uint32_t        n
)
{
    if (n <= ch->local_window) {
        ch->local_window -= n;
        ch->exceeded      = 0;
        return;
    }
    ch->exceeded     += n - ch->local_window;
    ch->local_window  = 0;
}

/* @a n counted bytes are owed back to the peer now. Saturating: credit past
   the window is a peer's overrun, which take_adjust() cuts anyway. */
static void
chan_credit(
    AxlSshChanSlot *ch,
    uint32_t        n
)
{
    ch->credit = n > UINT32_MAX - ch->credit ? UINT32_MAX : ch->credit + n;
}

int
axl_ssh_chan_inbound(
    AxlSshChanSlot *ch,
    uint32_t        n,
    bool            ext,
    bool            hooked,
    uint32_t       *adjust
)
{
    if (adjust != NULL) {
        *adjust = 0;
    }
    if (ch == NULL) {
        return AXL_ERR;
    }
    if (n > AXL_SSH_CHAN_LOCAL_MAXPKT) {
        return AXL_SSH_CHAN_IN_DROP;
    }
    /* Not yet open, to sshd: ignored, and not counted -- the window is
       still 0, and counting would make an early byte fatal. */
    if (!ch->started) {
        return AXL_SSH_CHAN_IN_DISCARD;
    }

    /* We closed first: whatever crossed our CLOSE is counted and ignored,
       and none of it ends the connection. Nothing is re-opened either --
       the channel will never take data again (take_adjust refuses). */
    if (ch->retired || ch->close_sent) {
        chan_count(ch, n);
        chan_credit(ch, n);
        return AXL_SSH_CHAN_IN_DISCARD;
    }

    int r = AXL_OK;
    if (ch->eof_in) {
        /* sshd: extended data after EOF disconnects; plain data is counted
           and discarded -- before its window check, so never fatal. */
        if (ext) {
            return AXL_ERR;
        }
        chan_count(ch, n);
        chan_credit(ch, n);
        r = AXL_SSH_CHAN_IN_DISCARD;
    } else {
        /* What a data hook will be handed on an ON_CONSUME channel is held
           by the application until it reports it, so its bound is checked
           directly, first: sshd's grace counter below is reset by any
           packet that fits -- an empty one always does -- so on its own it
           would let a peer overrun again and again. */
        bool held = hooked && ch->on_consume && !ext;
        if (held && n > AXL_SSH_CHANNEL_UNCONSUMED_MAX - ch->unconsumed) {
            return AXL_ERR;
        }
        chan_count(ch, n);
        if (ch->exceeded > AXL_SSH_CHAN_LOCAL_GRACE) {
            return AXL_ERR;
        }
        if (held) {
            ch->unconsumed += n;
        } else {
            chan_credit(ch, n);
        }
        /* No client sends stderr; it is counted and dropped. */
        if (ext) {
            r = AXL_SSH_CHAN_IN_DISCARD;
        }
    }

    /* Re-open the window as the rule allows -- after EOF too, because the
       peer's view of the window shrinks either way and sshd fakes the
       consumption for exactly that reason. */
    uint32_t a = axl_ssh_chan_take_adjust(ch);
    if (adjust != NULL) {
        *adjust = a;
    }
    return r;
}

void
axl_ssh_chan_set_mode(
    AxlSshChanSlot *ch,
    bool            on_consume
)
{
    if (ch == NULL || ch->on_consume == on_consume) {
        return;
    }
    if (!on_consume) {
        chan_credit(ch, ch->unconsumed);
        ch->unconsumed = 0;
    }
    ch->on_consume = on_consume;
}

int
axl_ssh_chan_consume(
    AxlSshChanSlot *ch,
    size_t          n
)
{
    /* Compared as size_t BEFORE any narrowing: 2^32 + 5 must be refused,
       not taken as 5. */
    if (ch == NULL || n > (size_t)ch->unconsumed) {
        return AXL_INVALID;
    }
    ch->unconsumed -= (uint32_t)n;
    chan_credit(ch, (uint32_t)n);
    return AXL_OK;
}

bool
axl_ssh_chan_adjust_due(
    const AxlSshChanSlot *ch
)
{
    if (ch == NULL || !ch->started || ch->retired || ch->close_sent
        || ch->close_in || ch->credit == 0
        || ch->local_window >= AXL_SSH_CHAN_LOCAL_WINDOW) {
        return false;
    }
    /* OpenSSH's channel_check_window(): the window more than three maximum
       packets below its size, or below half of it. At our fixed sizes the
       first implies the second (the assertion at the top of the file), so
       it is the one asked. */
    return AXL_SSH_CHAN_LOCAL_WINDOW - ch->local_window
           > AXL_SSH_CHAN_ADJUST_SLACK;
}

uint32_t
axl_ssh_chan_take_adjust(
    AxlSshChanSlot *ch
)
{
    if (!axl_ssh_chan_adjust_due(ch)) {
        return 0;
    }
    uint32_t room = AXL_SSH_CHAN_LOCAL_WINDOW - ch->local_window;
    uint32_t a    = ch->credit < room ? ch->credit : room;
    ch->local_window += a;
    ch->credit        = 0;
    return a;
}
