/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/** @file axl-ssh-rekey.h
    Key re-exchange, as pure decisions: when a rekey is due, which messages
    may leave and which may be processed while one runs, and the per-
    direction STAGED keys that make the switch happen at the right packet.

    Nothing here touches a socket, the loop or a connection. axl-ssh-server.c
    counts packets in, reports the four kex events, asks before sealing and
    before dispatching, and acts on the answers -- the shape axl-ssh-penalty.h
    and axl-ssh-chan.h set, so the unit test pins every decision directly.

    Every rule is OpenSSH 9.9p1's, read from its source rather than from the
    RFC alone, because the two differ and sshd is the yardstick:

    - Triggers (packet.c, ssh_packet_need_rekeying): nothing while an
      exchange is running; more than the byte limit in either direction;
      more than 2^31 packets in either direction (MAX_PACKETS, RFC 4344
      section 3.1) whatever the limit. Strictly MORE than the limit,
      counted after each packet: one packet later than sshd's pre-send
      check, which counts cipher blocks. It also lets one packet through
      after each rekey.
    - Counters (packet.c, ssh_set_newkeys): each direction's counters reset
      at THAT direction's NEWKEYS -- outbound when ours is sent, inbound when
      the peer's arrives -- because the limits are about how much one KEY has
      carried, and the two directions change key at different packets.
    - Outbound (packet.c, ssh_packet_type_is_kex and ssh_packet_send2): from
      our KEXINIT until our NEWKEYS only transport messages 1-49 leave, and
      not SERVICE_REQUEST, SERVICE_ACCEPT or EXT_INFO (5, 6, 7) -- RFC 4253
      section 7.1 forbids the first two outright. Everything else is held.
    - Inbound (kex.c, kex_reset_dispatch / kex_input_kexinit /
      kex_send_newkeys; packet.c, ssh_packet_read_poll_seqnr): see
      axl_ssh_rekey_in_allowed().
**/
#ifndef AXL_SSH_REKEY_H
#define AXL_SSH_REKEY_H

#include "axl-ssh-internal.h"
#include <axl/axl-ssh-server.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Packets in one direction under one key before a rekey is forced,
    whatever the byte limit: OpenSSH's MAX_PACKETS. The wire's sequence
    number is 32 bits, so this leaves half its range as margin. */
#define AXL_SSH_REKEY_PACKETS  (1ull << 31)

/** One connection's rekey state. Lives inside the connection; no
    allocation, nothing to free. Zero-initialised by axl_ssh_rekey_init(). */
typedef struct {
    uint64_t limit_bytes;    /**< 0 = never by bytes */
    uint64_t bytes_out;      /**< sent under the current outbound key */
    uint64_t bytes_in;       /**< received under the current inbound key */
    uint64_t pkts_out;       /**< packets sent under the current outbound key */
    uint64_t pkts_in;        /**< packets received under the current inbound key */
    bool     ours_sent;      /**< our KEXINIT is out; non-kex output held */
    bool     peers_seen;     /**< peer's KEXINIT received; inbound restricted */
    bool     ours_newkeys;   /**< our NEWKEYS sent; output flows again */
} AxlSshRekey;

/** Reset @a r to "no exchange running, nothing counted" with @a limit_bytes
    as the byte trigger (0 = never by bytes). NULL-safe. */
void
axl_ssh_rekey_init(
    AxlSshRekey *r,            ///< the state
    uint64_t     limit_bytes   ///< bytes per direction per key; 0 = never
);

/** Count ONE packet of @a bytes sent. NULL-safe. */
void
axl_ssh_rekey_count_out(
    AxlSshRekey *r,     ///< the state
    size_t       bytes  ///< the packet's size on the wire
);

/** Count ONE packet of @a bytes received. NULL-safe. */
void
axl_ssh_rekey_count_in(
    AxlSshRekey *r,     ///< the state
    size_t       bytes  ///< the packet's size on the wire
);

/** Should we start a rekey now? True when no exchange is running and
    either direction has carried MORE than the byte limit (when set) or
    MORE than #AXL_SSH_REKEY_PACKETS packets. A trigger that fires while an
    exchange is running starts nothing -- sshd's "KEX in progress" refusal.

    @return true to send our KEXINIT; false for a NULL @a r. */
bool
axl_ssh_rekey_due(
    const AxlSshRekey *r  ///< the state
);

/** @return true from either side's KEXINIT until both NEWKEYS; false for a
        NULL @a r. */
bool
axl_ssh_rekey_active(
    const AxlSshRekey *r  ///< the state
);

/** Is @a msg one that may leave mid-exchange -- sshd's
    ssh_packet_type_is_kex(): 1-49 but SERVICE_REQUEST, SERVICE_ACCEPT and
    EXT_INFO (5, 6, 7)? A packet that is NOT is the kind sshd starts a due
    rekey ahead of (ssh_packet_send2) and holds until its NEWKEYS.

    @return the answer. */
bool
axl_ssh_rekey_is_kex(
    uint8_t msg  ///< a message number
);

/** What to do with one outbound message. */
typedef enum {
    AXL_SSH_REKEY_SEND = 0,  /**< seal and send it now */
    AXL_SSH_REKEY_HOLD = 1   /**< keep it, in order, until our NEWKEYS */
} AxlSshRekeyOut;

/** May message @a msg be sent now? Outside our KEXINIT..NEWKEYS window,
    always. Inside it, only 1-49 except 5, 6 and 7 -- sshd's
    ssh_packet_type_is_kex(), which also holds EXT_INFO.

    @return #AXL_SSH_REKEY_SEND or #AXL_SSH_REKEY_HOLD; SEND for a NULL
        @a r (no rekey state means no rekey). */
AxlSshRekeyOut
axl_ssh_rekey_out(
    const AxlSshRekey *r,   ///< the state
    uint8_t            msg  ///< the message number about to be sealed
);

/** May an inbound message @a msg be PROCESSED now?

    Before the peer's KEXINIT and after its NEWKEYS: everything (the
    session's own dispatch decides what each message means). Between them,
    as sshd's dispatch table stands during a rekey:

    - 1-4 (DISCONNECT, IGNORE, UNIMPLEMENTED, DEBUG): yes -- handled ahead
      of the table at any time but the strict initial exchange
      (ssh_packet_read_poll_seqnr).
    - 20 (KEXINIT): NO -- kex_input_kexinit disarms it, and RFC 4253
      section 7.1 forbids a second one.
    - 21 (NEWKEYS): only once OUR NEWKEYS is sent -- kex_send_newkeys is
      what arms it, so an early one is not a key switch.
    - 30-49 (method messages): yes, as a class; which one the exchange
      expects is the caller's kex sub-state, as it is kexgen.c's.
    - 5-19, 22-29: NO -- answered UNIMPLEMENTED, and the connection goes
      on. On a connection's FIRST rekey the answer comes from serverloop.c's
      dispatch_protocol_error (logged at INFO); after any rekey's
      kex_send_newkeys has run kex_reset_dispatch, from kex_protocol_error
      (logged at ERROR). Strict KEX's disconnect binds only the initial
      exchange.
    - 0: NO -- not a message at all. sshd disconnects ("Invalid ssh2 packet
      type", packet.c ssh_packet_read_poll2) before any table is consulted.
    - 50 and above: YES. sshd's kex never touches their dispatch entries,
      so channel traffic is processed under the old key mid-exchange. A
      compliant peer sends none after its KEXINIT (RFC 4253 section 7.1);
      sshd does not police it, and neither does this.

    Allowed is not the same as routed: axl_ssh_rekey_in_kex() says whether
    20, 21 and 30-49 belong to the exchange.

    @return true to process @a msg; false for a NULL @a r. What a refusal
        costs is the caller's choice -- see the bullets for sshd's. */
bool
axl_ssh_rekey_in_allowed(
    const AxlSshRekey *r,   ///< the state
    uint8_t            msg  ///< the inbound message number
);

/** Is the PEER mid-exchange -- its KEXINIT received, its NEWKEYS not yet?
    While true, an allowed 21 or 30-49 goes to the key-exchange sub-state;
    while false, 20 STARTS an exchange (on_peer_kexinit) and 21 or 30-49
    are not the exchange's, whatever the session dispatch makes of them.

    @return the answer; false for a NULL @a r. */
bool
axl_ssh_rekey_in_kex(
    const AxlSshRekey *r  ///< the state
);

/** Our KEXINIT is going out: output is held from here.

    The four events are state-checked, because a call in the wrong order
    would otherwise leave a flag set across exchanges -- a stray NEWKEYS
    would make the NEXT exchange send channel data mid-window or process
    everything inbound. A refused event changes nothing.

    @return #AXL_OK; #AXL_ERR for a NULL @a r or when ours is already out
        this exchange. */
int
axl_ssh_rekey_on_our_kexinit(
    AxlSshRekey *r  ///< the state
);

/** The peer's KEXINIT arrived: inbound is restricted from here.

    @return #AXL_OK, with @a send_ours (when not NULL) true when the peer
        started the exchange and ours must go out now -- sshd's
        kex_input_kexinit sends its own unless already sent; #AXL_ERR for a
        NULL @a r or a second KEXINIT in one exchange, which sshd treats as
        a protocol error (kex_input_kexinit disarms the message). */
int
axl_ssh_rekey_on_peer_kexinit(
    AxlSshRekey *r,         ///< the state
    bool        *send_ours  ///< receives "send our KEXINIT now", or NULL
);

/** Our NEWKEYS is out: output flows again and the outbound counters
    restart, since everything from here is sealed under the new key.

    @return #AXL_OK; #AXL_ERR for a NULL @a r, or unless both KEXINITs are
        in and ours has not already been sent -- NEWKEYS needs a negotiated
        exchange to end. */
int
axl_ssh_rekey_on_our_newkeys(
    AxlSshRekey *r  ///< the state
);

/** The peer's NEWKEYS arrived: inbound is unrestricted, the inbound
    counters restart, and the exchange is over.

    @return #AXL_OK; #AXL_ERR for a NULL @a r, outside the peer's exchange,
        or before our NEWKEYS -- sshd arms the message only in
        kex_send_newkeys, so an earlier one is not a key switch. */
int
axl_ssh_rekey_on_peer_newkeys(
    AxlSshRekey *r  ///< the state
);

/** One direction's keys. `cur` is the key in use; `next` is the key a
    finished exchange produced, waiting for that direction's NEWKEYS. A
    receive key installed at exchange time instead would decrypt the
    peer's in-flight OLD-key packets with the new key. */
typedef struct {
    AxlSshAead cur;       /**< seals or opens this direction's packets now */
    AxlSshAead next;      /**< installed into `cur` by the matching NEWKEYS */
    bool       has_next;  /**< `next` holds a key */
} AxlSshDirKey;

/** Stage @a key as @a k's next key, leaving `cur` untouched. The bytes are
    copied; scrubbing the caller's buffer stays the caller's job.

    @return #AXL_OK; #AXL_ERR on a NULL argument or when a key is already
        staged -- a second exchange cannot finish before the first one's
        NEWKEYS, so a second stage is a caller bug, and replacing the key
        silently would hide it. */
int
axl_ssh_dirkey_stage(
    AxlSshDirKey  *k,                          ///< the direction
    const uint8_t  key[AXL_SSH_AEAD_KEY_LEN]   ///< the new key material
);

/** The matching NEWKEYS: install the staged key as `cur`, scrub the old
    one, and leave nothing staged.

    @return #AXL_OK; #AXL_ERR on a NULL @a k or with nothing staged, @a k
        unchanged. */
int
axl_ssh_dirkey_switch(
    AxlSshDirKey *k  ///< the direction
);

/** Scrub both keys and forget the staged one. NULL-safe. */
void
axl_ssh_dirkey_clear(
    AxlSshDirKey *k  ///< the direction
);

/** The byte limit a live server will copy into the connections it accepts
    next (axl_ssh_server_set_rekey_limit), for the unit test only. Defined in
    axl-ssh-server.c, which owns the struct.

    @return #AXL_OK with @a out written; #AXL_ERR when @a s is not live or
        @a out is NULL. */
int
axl_ssh_server_rekey_limit(
    const AxlSshServer *s,   ///< server from axl_ssh_server_new()
    uint64_t           *out  ///< receives the limit
);

#endif /* AXL_SSH_REKEY_H */
