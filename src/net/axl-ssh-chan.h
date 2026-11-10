/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ssh-chan.h
    Session channels, as pure bookkeeping: the per-connection channel table,
    both flow-control windows, the bounded output queue, the server-wide
    write budget with its fair-share floor, and the writable edge.

    Nothing here touches a socket, the loop or a hook. axl-ssh-server.c
    gathers the inputs -- a peer's message, an application's write, a
    transmit completion -- calls these, and acts on what they return: it
    sends the packet plan_send() sized, the WINDOW_ADJUST inbound() asked
    for, the writable hook take_writable() owes. So the unit test pins every
    decision directly, the shape axl-ssh-penalty.h set.

    The inbound rules are sshd's (OpenSSH 9.9p1 channels.c,
    channel_input_data): a session is confirmed with a window of 0 and gets
    its 2 MB at the start; a packet over our maximum packet is dropped
    uncounted; up to 10% past the window is tolerated; data after the
    peer's EOF is counted and discarded, extended data after it is fatal.

    A channel has two names. Its HANDLE (`id`) is what the application
    holds: image-wide, never 0, and retired the moment the server closes
    the channel, so a stale handle finds nothing. Its WIRE number
    (`local_id`) is what the peer addresses: the slot index, reused once
    both CLOSEs have passed, as sshd reuses its channel numbers -- and kept
    while the handle is retired, so messages that crossed our CLOSE still
    find the slot.
**/
#ifndef AXL_SSH_CHAN_H
#define AXL_SSH_CHAN_H

#include <axl/axl-ssh-channel.h>
#include <axl/axl-string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Inbound window granted when a shell or exec starts -- OpenSSH's
    CHAN_SES_WINDOW_DEFAULT, the public #AXL_SSH_CHANNEL_WINDOW_SIZE. */
#define AXL_SSH_CHAN_LOCAL_WINDOW   AXL_SSH_CHANNEL_WINDOW_SIZE

/** Largest inbound data packet accepted -- OpenSSH's CHAN_SES_PACKET_DEFAULT.
    A larger one is dropped uncounted. */
#define AXL_SSH_CHAN_LOCAL_MAXPKT   (32u * 1024u)

/** How far the inbound window may fall below its size before credit owed
    is granted back: three maximum packets, OpenSSH's channel_check_window()
    (`local_maxpacket*3`). */
#define AXL_SSH_CHAN_ADJUST_SLACK   (3u * AXL_SSH_CHAN_LOCAL_MAXPKT)

/** Largest outbound data packet sent, whatever the peer's maximum allows. */
#define AXL_SSH_CHAN_OUT_MAXPKT     (32u * 1024u)

/** Bytes a peer may send past the window before the connection ends:
    sshd's `local_window_max / 10`. */
#define AXL_SSH_CHAN_LOCAL_GRACE    (AXL_SSH_CHAN_LOCAL_WINDOW / 10u)

/** axl_ssh_chan_inbound(): counted against the window, not delivered. */
#define AXL_SSH_CHAN_IN_DISCARD     1

/** axl_ssh_chan_inbound(): over our maximum packet; dropped, not counted. */
#define AXL_SSH_CHAN_IN_DROP        2

/** The server-wide output budget. One per server; every channel of every
    connection charges its queued bytes here. */
typedef struct {
    size_t limit_per_channel; /**< bytes one channel may hold queued */
    size_t limit_total;       /**< bytes all channels may hold queued */
    size_t queued_total;      /**< bytes all channels hold queued now */
    size_t fair_share;        /**< a channel may always queue up to this,
                                   even at the total, so stalled channels
                                   cannot starve healthy ones; 0 = no floor */
} AxlSshChanBudget;

/** One channel slot. Fields ordered widest first (no padding). */
typedef struct {
    AxlString *out;           /**< queued output in WRITE order: runs of
                                   [stream u8][remaining u32, native][bytes],
                                   stream 0 = stdout, 1 = stderr; NULL while
                                   empty -- a drained queue is freed */
    void      *user_data;     /**< the application's, for the wiring */
    size_t     queued;        /**< data bytes in `out` (headers excluded) */
    size_t     busy_len;      /**< largest refused write since the last
                                   writable: every trigger requires it fit */
    size_t     out_last;      /**< offset of the LAST run's header in
                                   `out` (meaningful while `out` is not
                                   empty): a write of the same stream
                                   extends that run */
    uint32_t   id;            /**< handle; 0 = free slot */
    uint32_t   local_id;      /**< our wire channel number (the slot index) */
    uint32_t   peer_id;       /**< the peer's channel number */
    uint32_t   peer_window;   /**< outbound credit */
    uint32_t   peer_maxpkt;   /**< outbound packet bound the peer set */
    uint32_t   local_window;  /**< inbound credit the peer has left */
    uint32_t   credit;        /**< inbound bytes owed back to the peer
                                   and not yet granted by a WINDOW_ADJUST
                                   (sshd's local_consumed): every byte
                                   counted, less those still unconsumed */
    uint32_t   unconsumed;    /**< ON_CONSUME: bytes delivered to the data
                                   hook and not yet reported consumed;
                                   never above #AXL_SSH_CHANNEL_UNCONSUMED_MAX */
    uint32_t   exceeded;      /**< bytes past the window since the last
                                   in-window packet (sshd's
                                   local_window_exceeded) */
    uint32_t   exit_status;   /**< for exit_pending's `exit-status` */
    unsigned   hook_depth;    /**< hooks of this channel on the stack */
    bool       started;       /**< shell/exec accepted; window granted */
    bool       on_consume;    /**< #AXL_SSH_CHANNEL_WINDOW_ON_CONSUME: a
                                   delivered byte is credited only once
                                   axl_ssh_chan_consume() reports it */
    bool       eof_in;        /**< the peer sent EOF */
    bool       eof_out;       /**< we sent EOF */
    bool       exit_pending;  /**< axl_ssh_channel_exit(): draining */
    bool       close_sent;    /**< we sent CLOSE */
    bool       close_in;      /**< the peer sent CLOSE */
    bool       retired;       /**< handle gone from the application; the
                                   slot is kept until BOTH CLOSEs */
    bool       close_deferred;/**< a close waits for hook_depth to reach 0 */
    bool       busy_on_chan;  /**< refused on the channel limit since the
                                   last writable */
    bool       busy_on_total; /**< refused on the server total since the
                                   last writable */
    bool       busy_had_queue;/**< a refusal on the total found this queue
                                   NON-empty: draining to empty also earns
                                   the writable */
} AxlSshChanSlot;

/** Fill @a b: both limits, nothing queued, and the fair-share floor
    `total / max_channels` -- the total divided by the most channels the
    server can ever hold (connection cap x session cap). A quotient of 0
    means no floor. A @a max_channels of 0 also means no floor. */
void
axl_ssh_chan_budget_init(
    AxlSshChanBudget *b,            ///< the budget
    size_t            per_channel,  ///< bytes one channel may hold queued
    size_t            total,        ///< bytes all channels may hold queued
    unsigned          max_channels  ///< divisor of the fair-share floor
);

/** @return a new handle: image-wide, never 0; wraps from UINT32_MAX to 1. */
uint32_t
axl_ssh_chan_next_id(void);

/** For the unit test: the next axl_ssh_chan_next_id() returns @a next (or 1
    for 0), so the wrap can be reached. */
void
axl_ssh_chan_set_next_id(
    uint32_t next  ///< handle the next call returns
);

/** Take a free slot: zeroed, a fresh handle, its wire number, window 0.
    Retired slots still count against @a max_sessions.

    @return the slot; NULL when @a max_sessions (or @a cap) slots are in
        use, or on a NULL @a tab. */
AxlSshChanSlot *
axl_ssh_chan_alloc(
    AxlSshChanSlot *tab,          ///< the connection's table
    unsigned        cap,          ///< slots in @a tab
    unsigned        max_sessions  ///< the server's session cap
);

/** @return the open channel with handle @a id; NULL for 0, a freed id or a
        RETIRED one -- a stale handle finds nothing. */
AxlSshChanSlot *
axl_ssh_chan_find(
    AxlSshChanSlot *tab,  ///< the connection's table
    unsigned        cap,  ///< slots in @a tab
    uint32_t        id    ///< the application's handle
);

/** @return the slot the peer addresses as @a local_id (the recipient
        channel of its message), retired or not; NULL for a free slot or a
        number out of range. */
AxlSshChanSlot *
axl_ssh_chan_find_peer(
    AxlSshChanSlot *tab,      ///< the connection's table
    unsigned        cap,      ///< slots in @a tab
    uint32_t        local_id  ///< our wire channel number
);

/** Queue @a len bytes on one stream, all or nothing, judged in order: the
    channel limit (refused on the CHANNEL), the fair-share floor (accepted
    whatever the total), the server total (refused on the TOTAL). The total
    is judged even when the channel limit refused, and recorded too. A refusal
    records what take_writable() needs: which limit, the largest refused
    length, and whether the queue held bytes at a refusal on the total.

    @return #AXL_OK queued, and for @a len 0 (nothing queued, @a buf may be
        NULL); #AXL_BUSY refused, nothing queued; #AXL_ERR for @a len above
        the channel limit, or after exit_pending; #AXL_NO_RESOURCES when the
        bytes cannot be copied (nothing queued); #AXL_NOT_FOUND on a free or
        retired slot; #AXL_INVALID on a NULL @a ch or @a b, or a NULL @a buf
        with @a len above 0. */
int
axl_ssh_chan_write(
    AxlSshChanSlot   *ch,       ///< the channel
    AxlSshChanBudget *b,        ///< the server's budget
    bool              stderr_,  ///< true for extended data (stderr)
    const void       *buf,      ///< bytes, copied
    size_t            len       ///< how many
);

/** The writable EDGE: true at most once per run of refusals, and only once
    every limit that refused has eased AND the largest refused write fits
    (L = busy_len, Q = queued, T = queued_total):
      - channel: 2Q < limit and Q + L <= limit;
      - total: (2T < total and T + L <= total), or the queue drained to
        empty when a refusal on the total found it non-empty;
      - both kinds since the last writable: both conditions -- and a write
        over the channel limit that the total would ALSO refuse counts as
        both, so a writable's retry is never refused by the other limit,
        except when the drain release above is what eased the total: an
        emptied queue admits a retry only up to the fair-share floor.
    Consumes the edge when true. Owes nothing (and forgets the refusal) on
    an exit_pending or retired channel. */
bool
axl_ssh_chan_take_writable(
    AxlSshChanSlot         *ch,  ///< the channel
    const AxlSshChanBudget *b    ///< the server's budget
);

/** Size ONE outbound data packet from the head of the queue, never
    spanning two runs: min(head run, peer window, peer max packet,
    #AXL_SSH_CHAN_OUT_MAXPKT).

    @return the bytes to send (0 = nothing may leave now); @a stderr_
        receives the head run's stream when non-zero. The bytes start at
        axl_ssh_chan_head(). */
size_t
axl_ssh_chan_plan_send(
    const AxlSshChanSlot *ch,      ///< the channel
    bool                 *stderr_  ///< receives the stream
);

/** @return the first data byte of the head run, or NULL when nothing is
        queued. */
const uint8_t *
axl_ssh_chan_head(
    const AxlSshChanSlot *ch  ///< the channel
);

/** @a n bytes of the head run (as planned) were handed to the transmit
    queue: debit the peer window, the channel and the budget by exactly
    @a n. A call that does not match the head run (wrong stream, more than
    it holds, more than the window) changes nothing. A queue drained to
    empty is freed, and one far below its buffer is shrunk, so the heap
    follows the queued bytes the budget bounds rather than each channel's
    high-water mark. */
void
axl_ssh_chan_sent(
    AxlSshChanSlot   *ch,       ///< the channel
    AxlSshChanBudget *b,        ///< the server's budget
    bool              stderr_,  ///< the stream plan_send() reported
    size_t            n         ///< bytes sent, as plan_send() sized
);

/** The peer's WINDOW_ADJUST: add @a adj to the outbound credit.

    @return #AXL_OK; #AXL_ERR when it would take the window past 2^32 - 1
        (sshd's "adjust overflows remote window", fatal), window unchanged;
        #AXL_OK and no change on a retired slot, whose adjust crossed our
        CLOSE; #AXL_INVALID on a NULL @a ch. */
int
axl_ssh_chan_peer_adjust(
    AxlSshChanSlot *ch,  ///< the channel
    uint32_t        adj  ///< bytes the peer added
);

/** Account for @a n inbound data bytes (sshd's rules, in this order):
    over #AXL_SSH_CHAN_LOCAL_MAXPKT dropped uncounted; before the start
    ignored uncounted; then counted against the window -- for a retired or
    closing channel, or after the peer's EOF, counted and discarded and
    never fatal (but extended data after EOF is #AXL_ERR); otherwise past
    the window by more than #AXL_SSH_CHAN_LOCAL_GRACE is #AXL_ERR, and
    extended data is discarded.

    Bytes to be delivered to a data hook (@a hooked) on an ON_CONSUME
    channel become `unconsumed` -- and a packet that would take that past
    #AXL_SSH_CHANNEL_UNCONSUMED_MAX is #AXL_ERR, checked first and
    whatever `exceeded` says, so an empty packet cannot reset it. Every
    other counted byte becomes `credit` at once.

    @return #AXL_OK deliver to the data hook; #AXL_SSH_CHAN_IN_DISCARD do
        not; #AXL_SSH_CHAN_IN_DROP over our max packet; #AXL_ERR end the
        connection. @a adjust receives the WINDOW_ADJUST to send now
        (axl_ssh_chan_take_adjust()), 0 for none. */
int
axl_ssh_chan_inbound(
    AxlSshChanSlot *ch,      ///< the channel
    uint32_t        n,       ///< data bytes in the packet
    bool            ext,     ///< CHANNEL_EXTENDED_DATA
    bool            hooked,  ///< a data hook will be handed the bytes
    uint32_t       *adjust   ///< receives the adjust to send, or 0
);

/** Switch the channel's window mode. To ON_ARRIVAL, every unconsumed byte
    becomes credit at once; to ON_CONSUME, nothing already counted moves.
    NULL-safe. */
void
axl_ssh_chan_set_mode(
    AxlSshChanSlot *ch,          ///< the channel
    bool            on_consume   ///< true for ON_CONSUME
);

/** The application consumed @a n delivered bytes: they move from
    `unconsumed` to `credit`. All or nothing.

    @return #AXL_OK, and for @a n 0; #AXL_INVALID when @a n exceeds
        `unconsumed` (nothing moves) or on a NULL @a ch. */
int
axl_ssh_chan_consume(
    AxlSshChanSlot *ch,  ///< the channel
    size_t          n    ///< bytes consumed
);

/** Is a WINDOW_ADJUST due -- credit owed, on a started channel we have not
    closed, whose window has fallen more than #AXL_SSH_CHAN_ADJUST_SLACK
    below #AXL_SSH_CHAN_LOCAL_WINDOW (OpenSSH's rule)? NULL-safe (false). */
bool
axl_ssh_chan_adjust_due(
    const AxlSshChanSlot *ch  ///< the channel
);

/** Take the WINDOW_ADJUST that is due: min(credit, window size - window),
    added to the window; the credit is cleared -- what the cap cuts is a
    peer's overrun, never owed back.

    @return the bytes to grant, 0 when none is due (nothing changes). */
uint32_t
axl_ssh_chan_take_adjust(
    AxlSshChanSlot *ch  ///< the channel
);

/** The shell or exec started: grant #AXL_SSH_CHAN_LOCAL_WINDOW.
    @a adjust receives the WINDOW_ADJUST to send (0 if already started). */
void
axl_ssh_chan_start(
    AxlSshChanSlot *ch,      ///< the channel
    uint32_t       *adjust   ///< receives the adjust to send
);

/** Retire the handle (the server closed first): discard queued output back
    to the budget and forget any owed writable. The slot -- its wire number
    and inbound accounting -- stays until axl_ssh_chan_free(). */
void
axl_ssh_chan_retire(
    AxlSshChanSlot   *ch,  ///< the channel
    AxlSshChanBudget *b    ///< the server's budget
);

/** Release the slot: its queued bytes leave the budget, the queue is
    freed, the slot is zeroed (free). NULL-safe. */
void
axl_ssh_chan_free(
    AxlSshChanSlot   *ch,  ///< the channel
    AxlSshChanBudget *b    ///< the server's budget
);

#endif /* AXL_SSH_CHAN_H */
