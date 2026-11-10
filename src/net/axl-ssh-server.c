/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/** @file axl-ssh-server.c
    The server role: TCP accept, and the connection state machine.

    Everything protocol-shaped lives in the role-neutral files beside this one
    (axl-ssh-buf.c, axl-ssh-packet.c, axl-ssh-kex.c, axl-ssh-crypt.c). What is
    here is the part a client would NOT reuse: proving the host key, and the
    order in which a server expects messages.

    The connection completes key exchange,
    switches to the AEAD cipher, answers SERVICE_REQUEST, and then
    authenticates by RFC 4252 publickey -- against the server's authorized-key
    set, with the application's publickey hook naming the principal -- or by
    RFC 4252 password, which only the application's password hook can check.
    Either verdict may arrive inside the hook or later, through
    axl_ssh_auth_complete; the connection reads nothing more until it does
    (RFC 4252 section 5.1), and a failure is answered no earlier than a
    per-user minimum. A hook may run the loop itself, which is why the server
    counts the application calls on its stack (app_call_begin): one hook runs
    at a time and the rest of the connections wait their turn, conn_drop
    leaves a connection whose own hook is running for the frame beneath it to
    drop, and axl_ssh_server_free stops the server at once but finishes from
    the loop. An authenticated connection reaches ST_SESSION, where
    GLOBAL_REQUEST is answered with RFC 4254 section 4's REQUEST_FAILURE --
    AxlSsh implements no global request, so every one that asked for a reply
    gets refused, including a client's keepalive@openssh.com liveness probe
    -- and CHANNEL_OPEN opens a `session` channel when the application has
    installed channel hooks (axl_ssh_server_set_channel_ops), and is refused
    otherwise. "Session channels" below is that half: the per-connection
    channel table (axl-ssh-chan.c, pure bookkeeping) driven from the parser,
    the application's channel hooks, and the deferred flush that turns
    queued channel output into packets.
**/

#include "axl-ssh-admit.h"
#include "axl-ssh-auth.h"
#include "axl-ssh-chan.h"
#include "axl-ssh-idle.h"
#include "axl-ssh-internal.h"
#include "axl-ssh-penalty.h"
#include "axl-ssh-reap.h"
#include "axl-ssh-rekey.h"
#include "axl-tcp-internal.h"   /* _axl_tcp_recv_fin */
#include <axl/axl-crypto.h>
#include <axl/axl-debug.h>
#include <axl/axl-defer.h>
#include <axl/axl-fs.h>
#include <axl/axl-log.h>
#include <axl/axl-mem.h>
#include <axl/axl-net.h>
#include <axl/axl-rng.h>
#include <axl/axl-ssh-channel.h>
#include <axl/axl-ssh-server.h>
#include <axl/axl-str.h>
#include <axl/axl-tcp.h>
#include <axl/axl-time.h>

AXL_LOG_DOMAIN("ssh");

/** Bytes read per recv. One MTU-ish chunk; the packet reader loops over
    whatever accumulates, so this bounds syscalls, not messages. */
#define RX_CHUNK        2048u

/** Ceiling on unparsed input held for one connection. A peer that sends a
    valid length prefix and then stops must not be able to make us buffer
    without limit, and a peer that never sends a parseable packet must not
    either -- so this is checked on every append, not only when a packet
    completes. Two max packets of slack, so a legal packet split across
    reads is never the thing that trips it. */
#define RX_BACKLOG_MAX  (2u * AXL_SSH_PACKET_MAX)

/** Output backpressure. Past TXQ_HIGH bytes queued to send -- pending plus
    in flight, axl_ssh_txq_bytes -- a connection stops parsing and stops
    reading; once its sends bring the queue under TXQ_LOW it reads again.

    Every reply is made by parsing one of the peer's requests, so a peer
    that sends requests and never reads the replies would otherwise grow
    the queue without limit. Before authentication the request caps
    (MAX_SERVICE_REQUESTS, MAX_UNIMPLEMENTED_REPLIES) bound it; after it,
    nothing did. Stopping the PARSE bounds the queue at TXQ_HIGH plus one
    reply, and leaving the receive unarmed lets TCP's window close on the
    peer, so nothing is dropped: the input waits in the network, not here.

    THAT BOUND HOLDS ONLY WHILE EVERY FRAME IS A REPLY TO A PARSED REQUEST,
    and channel data is not one: the server originates it from what the
    application wrote. It keeps the bound anyway because the channel flush
    (chan_flush) packetizes queued channel output into this queue ONLY while
    it holds fewer than TXQ_LOW bytes, and resumes from on_sent once a send
    brings it back under -- so channel data alone never reaches TXQ_HIGH, and
    what does is still at most one reply past it. A rekey's held flush is
    the one exception (REKEY_HELD_MAX). The application's own
    queue behind that is bounded by the channel write limits.

    128 KiB is sshd's figure for the same question,
    ssh_packet_not_very_much_data_to_write (packet.c) in its non-interactive
    case. sshd asks it before channel_output_poll packetizes more CHANNEL
    data, and never stops reading the network; here the input that makes
    output is the peer's own requests, so it is the peer that is paused. The
    gap down to TXQ_LOW keeps a peer near the line from pausing and resuming
    on every send.

    A PAUSED PEER IS BOUNDED BY CLIENT-ALIVE, NOT WATCHED: with the receive
    unarmed, a half-close (FIN) is not seen until the queue drains, so a peer
    that stops reading and shuts down its sending side would hold its slot
    for as long as it does not read. The idle tick (axl-ssh-idle.h) ends it
    instead: a keepalive due while tx_paused is not queued, which would grow
    a dead peer's queue, but it still advances the unanswered count, so the
    peer is ended within (count_max + 1) x (interval + tick) of its last
    bytes (spec section 12a.7, deviation 8). A full close or reset still
    ends it sooner, through the send that then fails. */
#define TXQ_HIGH  (128u * 1024u)
#define TXQ_LOW   (64u * 1024u)

/** Largest payload we will hand to the dispatcher. */
#define PAYLOAD_MAX     AXL_SSH_PACKET_MAX

/** Concurrent connections, capped WELL below AxlLoop's AXL_MAX_SOURCES (64).

    Each connection holds at least one loop source for its receive, plus one
    per queued send and one per deferred close. Unbounded, 64 idle sockets
    from an unauthenticated peer exhaust the table -- and the failure is not
    confined to SSH: every other consumer of that loop stops being able to
    register, and the close path cannot register its own event either, so
    teardown falls back to a multi-second inline wait per connection. Measured
    before this cap existed: connection 63 was served, 64 got nothing, and the
    server did not recover within 60 s of the flood ending.

    Axl9pServer and AxlHttpServer use a fixed slot array for the same reason;
    this is the same decision with a linked list behind it. 16 leaves the
    majority of the table for everything else in the image, which for a
    firmware command channel is the right side to err on.

    REACHING IT DOES NOT PAUSE THE ACCEPT. Admission is by eviction (spec
    section 12a.2, on_accept): an arrival over this cap, or over
    max_startups unauthenticated, evicts the unauthenticated connection
    least far along in key exchange (eviction_victim), and is refused only
    when none is still in it. The accept used to pause here instead, which let 16 silent peers
    hold every slot until the login grace ran out, and made every lost
    re-arm a server that had gone deaf. */
#define MAX_CONNS       16u

/* AXL_SSH_MAX_STARTUPS_MAX (public, axl-ssh-server.h) is the only way a
   caller learns this cap, and its own doc comment promises it equals
   MAX_CONNS -- a raised pre-auth ceiling above the connection cap would be
   unreachable, since every unauthenticated connection is also a
   connection. This is the compile-time link, the same role the asserts
   below play for AXL_SSH_PASSWORD_FAILURES_MAX. */
_Static_assert(AXL_SSH_MAX_STARTUPS_MAX == MAX_CONNS,
               "the pre-auth cap ceiling must equal the connection cap");

/** Consecutive failed accepts before the listener is paused. A failed accept
    re-arms immediately, so a listener stuck in a permanently-failing state is
    a tight callback loop rather than an idle server -- bounded here so it
    ends up refusing connections instead of pegging a core. */
#define MAX_ACCEPT_ERRS 32u

/** Userauth requests one connection may send before it is dropped.

    RFC 4252 section 7's query form costs the server no crypto and, by
    design, does not end the connection when the key is unknown -- that is a
    client walking its key list. Unbounded, it is also a peer that can talk
    to us forever without ever authenticating. Twenty is far above any real
    client: OpenSSH sends one `none` plus one query per candidate identity,
    and a user with twenty keys is already past what ssh will offer before
    its own limit.

    THE PUBLIC AXL_SSH_PASSWORD_FAILURES_MAX DEPENDS ON THIS NUMBER, and the
    asserts below are the compile-time link (the precedent is
    axl-http-conn.c:18). Twenty requests are SERVED -- the check below is
    `++auth_requests > MAX_AUTH_REQUESTS`, so the twenty-first is what ends
    the connection. A password is also an authentication request, so a cap of
    N is only ever REACHED by a client with 20 - N other requests to spare;
    past that this bound ends the connection first and the server would
    advertise one number while enforcing another. Lowering MAX_AUTH_REQUESTS
    without lowering the public maximum breaks that. */
#define MAX_AUTH_REQUESTS  20u

_Static_assert(AXL_SSH_PASSWORD_FAILURES_MAX + 10u <= MAX_AUTH_REQUESTS,
               "the none probe, nine key offers and the largest cap must fit");
_Static_assert(AXL_SSH_PASSWORD_FAILURES_DEFAULT
                   <= AXL_SSH_PASSWORD_FAILURES_MAX,
               "the default password cap must be inside the settable range");

/** Longest user name handed to a hook. Longer names are refused without
    being examined. */
#define AUTH_USER_MAX          256u

/** SERVICE_REQUESTs one connection may send before it is dropped.

    RFC 4253 section 10 has one service request per connection: a client asks
    for ssh-userauth, gets SERVICE_ACCEPT, and never asks again. Unbounded,
    the request is the other half of the pre-auth amplifier the size cap on
    the reply already closed -- axl_tcp_send_async is documented unrefusable
    and unbounded by design (axl-tcp-internal.h), so bounding what we queue
    is the CALLER's job, and every unanswered reply grows the connection's
    transmit queue (axl-ssh-txq.c) against a peer that simply never reads:
    its one send in flight never completes, so nothing queued behind it ever
    drains. MAX_AUTH_REQUESTS
    bounds the userauth free form next door for exactly this reason. */
#define MAX_SERVICE_REQUESTS  1u

/** SSH_MSG_UNIMPLEMENTED replies one connection may draw out of us.

    RFC 4253 section 11.4 makes the reply a MUST, and an unbounded
    reply-per-unknown-message is the same pre-auth amplifier the two caps
    above exist to close: a 16-byte packet buys a 44-byte frame in the
    transmit queue, against a peer that never reads its socket -- so never
    lets the queue drain -- and a transport that will never refuse on our
    behalf.

    EIGHT. A conforming client draws ZERO -- measured, not assumed: a full
    ssh(1) session against this server sends SERVICE_REQUEST, three
    USERAUTH_REQUESTs, CHANNEL_OPEN and DISCONNECT, every one of which is
    handled or deliberately unanswered. Eight leaves a genuinely mismatched
    client its first few diagnostics (which is the only reason the message
    exists) and caps what a hostile one can queue at about 350 bytes.

    Past the cap the message is dropped SILENTLY rather than ending the
    connection, which is where this differs from MAX_SERVICE_REQUESTS. A
    repeated SERVICE_REQUEST is a peer we cannot serve; an unrecognised
    message is something RFC 4253 section 11 explicitly says a peer may send
    and an implementation must tolerate. So the cap bounds the REPLY and
    restores exactly the behaviour that shipped before it -- ignoring --
    rather than inventing a new way to drop a connection. */
#define MAX_UNIMPLEMENTED_REPLIES  8u

/** Bytes of a peer-supplied user name we will put in a log line
    (#AXL_SSH_LOG_USER_MAX, shared with axl-ssh-auth.h's line formats).

    The logged copy decides nothing: what an application's hooks decide on is
    the full, NUL-terminated auth_user, never this truncated one. So this is
    a diagnostic, and it is copied rather than printed in place
    because the bytes are the peer's choice: anything not printable ASCII
    would go to a firmware console as a control sequence, and no printf
    conversion sanitises. It was written for a second reason that no longer
    holds -- AxlFormat's `%.*s` used to scan for a NUL BEFORE applying the
    precision, so pointing it at a non-terminated slice of a received packet
    read past the payload allocation. The scan is bounded by the precision
    now, so the copy is here for the sanitising alone. */
#define AUTH_USER_LOG_MAX  AXL_SSH_LOG_USER_MAX

/** Bytes of the peer string peer_for_log writes, "A port P" (NUL
    included; #AXL_SSH_LOG_PEER_MAX). */
#define PEER_LOG_MAX       AXL_SSH_LOG_PEER_MAX
_Static_assert(sizeof "255.255.255.255 port 65535" <= PEER_LOG_MAX,
               "peer_for_log's longest output must fit its buffer");

/** Ceiling on how often the reaper runs, in milliseconds. Coarse on purpose:
    it walks every connection, and a pre-auth deadline does not need
    precision. axl_ssh_server_listen() may arm the timer FINER than this --
    see axl_ssh_reap_tick_ms() (axl-ssh-reap.h) -- for a short
    login_grace_sec, never coarser. */
#define REAP_INTERVAL_MS     5000u

/* The kinds of audit line that are rate-limited, one window each -- see
   AxlSshServer's auth_log. */
typedef enum {
    AUTH_LOG_REFUSED = 0, /**< an ordinary refused attempt ("Failed ...") */
    AUTH_LOG_FORGED,      /**< publickey (bad signature) */
    AUTH_LOG_EXCEEDED,    /**< maximum authentication attempts exceeded */
    AUTH_LOG_TIMEOUT,     /**< the login grace ran out */
    AUTH_LOG_USER_CHANGE, /**< a request named a different user */
    /* A login grace that ran out on a connection that was STILL QUEUED --
       waiting its turn to parse. A window of its own, not TIMEOUT's: the
       file's rule is that whichever kind is cheaper hides the other's first
       line, and an ordinary timeout is the cheapest event there is. A peer
       opening and abandoning connections would otherwise hide the first
       line of the one kind that names the server's OWN fault. */
    AUTH_LOG_TIMEOUT_QUEUED,
    AUTH_LOG_HOOK_SLOW,    /**< a hook RETURNED over its budget */
    /* ...and one still running over it. Separate from HOOK_SLOW for the
       same reason as above: a stalled hook always goes on to produce a
       slow-return line too, so a shared window would let the first hide
       the second. */
    AUTH_LOG_HOOK_STALLED,
    /* Admission past max_startups (spec section 12a.2): a connection
       EVICTED to make room, and one REFUSED because there was no victim.
       Two windows, not one, by the same rule: an eviction costs a flood one
       silent TCP connection, so a shared window would let a flood's
       evictions hide the first refusal -- the one line that says the
       server's pre-auth slots are held by peers that finished a real key
       exchange, which is the attack eviction cannot answer. */
    AUTH_LOG_STARTUP_EVICT,
    AUTH_LOG_STARTUP_REFUSE,
    /* An authenticated connection ended by the idle rules (axl-ssh-idle.h):
       sshd's UnusedConnectionTimeout and ClientAliveCountMax lines. One
       window each, by the same rule: neither may hide the other's first
       line, nor share a window with any pre-authentication kind. */
    AUTH_LOG_IDLE_UNUSED,
    AUTH_LOG_IDLE_SILENT,
    AUTH_LOG_KINDS
} AuthLogKind;

typedef enum {
    ST_IDENT,     /**< waiting for the peer's identification line */
    ST_KEXINIT,   /**< waiting for the peer's KEXINIT */
    ST_KEX,       /**< waiting for KEX_ECDH_INIT */
    ST_NEWKEYS,   /**< waiting for the peer's NEWKEYS */
    ST_AUTH,      /**< encrypted; answering service/userauth */
    ST_SESSION,   /**< authenticated; session channels, when hooks are set */
    ST_DEAD,      /**< closing; ignore everything */
    /* A DISCONNECT was queued on a healthy stream: nothing more is sealed or
       parsed, the transmit queue keeps flushing, and the connection waits up
       to AXL_SSH_DISCONNECT_LINGER_MS for the peer to hang up (conn_linger).
       FINISHED, like ST_DEAD -- conn_over() is true for both, and every
       check that means "is this connection done" asks that, not == ST_DEAD.
       Declared after ST_DEAD so the two are the tail of the order. */
    ST_LINGER
} SshState;

/* The per-channel flag words in SshConn hold one bit per slot. */
_Static_assert(AXL_SSH_MAX_SESSIONS_MAX <= 16u,
               "a uint16_t must hold one bit per channel slot");

/* eviction_victim ranks connections still in key exchange by this order. */
_Static_assert(ST_IDENT < ST_KEXINIT && ST_KEXINIT < ST_KEX
               && ST_KEX < ST_NEWKEYS && ST_NEWKEYS < ST_AUTH,
               "key exchange states must be declared in protocol order");

typedef struct SshConn {
    AxlSshServer   *srv;
    AxlTcp         *sock;
    struct SshConn *next;

    uint8_t   rx[RX_CHUNK];   /* must stay valid across recv re-arms */
    AxlString *in;            /* unparsed input */
    /* Where every ENCRYPTED packet on this connection is decrypted, PAYLOAD_MAX
       bytes, allocated once at the NEWKEYS transition and freed by conn_free.

       35,000 is right as a limit (RFC 4253 section 6.1) and wrong as an
       allocation unit, which is what it used to be: pump allocated it per
       packet, before the plaintext length was known, and freed it after
       dispatch. Per PACKET understates it -- measured, it was TWICE per
       packet, because pump's loop only learns the input is exhausted by
       calling axl_ssh_aead_open, which needs the buffer to be there first and
       answers AXL_INCOMPLETE. So every parse round trip through the firmware
       allocator cost 70,000 bytes, and a keystroke on a P4 channel would have
       cost the same as a full-size packet.

       PER CONNECTION AND NOT PER SERVER, and that is not a cost decision. An
       authentication hook may dispatch the loop (axl-loop.h documents a
       nested wait from any source callback as safe, and the deferred drop in
       conn_drop exists because of it), so another connection can be parsed
       while this one's payload is still live -- on_userauth hands the
       publickey hook a `blob` that points INTO this buffer. A server-wide
       buffer would let that second connection overwrite it under the hook.
       One connection cannot do that to itself: pump refuses to parse anything
       while a verdict is owed (`c->auth_req != 0 || c->reply_armed`), and
       auth_req is set before the hook runs (issue_request).

       THAT GUARD COVERS THE AUTHENTICATION PATH ONLY, and session channels
       need their own, because the data hook is handed a pointer INTO this
       buffer with no copy -- application code with no auth_req set, which
       may dispatch the loop. So pump has a second gate beside it: nothing is
       parsed for a connection while any of its channel hooks is on the stack
       (`c->chan_hooks != 0`, counted by chan_hook_enter/chan_hook_leave around
       every one, checked before decryption). A pump that stops there marks
       the connection (`chan_gated`), and the last hook to leave puts it on
       the resume queue, so the input it held back is parsed from the loop --
       never nested inside the frame that called the hook, where it would
       answer a later request before the reply to the one still on the stack.

       NOT for the pre-NEWKEYS packets: those still copy per packet (pump's
       else arm). There are at most four of them per connection -- KEXINIT, the
       one packet a wrong kex guess makes us drop (drop_next), KEX_ECDH_INIT and
       NEWKEYS -- and each copies exactly its own length, whereas allocating
       this buffer at accept would hand every scan and every penalised knock
       35 KB of firmware heap: MAX_CONNS of them is 560 KB. NEWKEYS is the
       cheapest gate that costs a peer a real key exchange.

       WHAT IT COSTS, stated because the 560 KB above is only the avoided
       half: after NEWKEYS these 35,000 bytes are RESIDENT for the
       connection's life, where they used to be transient. A connection's
       ceiling goes from about 72.7 KB (sizeof(SshConn) ~2,680 plus
       RX_BACKLOG_MAX 70,000) to about 107.7 KB, so MAX_CONNS of them is
       ~1.72 MB rather than ~1.16 MB -- pinnable for the login grace by
       peers that complete a key exchange. That is the trade: a bounded,
       earned increase in resident memory against 70,000 bytes of allocator
       traffic on every packet forever. */
    uint8_t   *payload;

    SshState  state;
    AxlString *v_c;           /* peer ident line, no CRLF */
    AxlString *i_c;           /* peer KEXINIT payload */
    AxlString *i_s;           /* our KEXINIT payload */

    uint8_t   session_id[AXL_SSH_HASH_LEN];
    uint64_t  tx_seq, rx_seq;
    bool      tx_on, rx_on;   /* cipher active in each direction */
    bool      have_session_id;
    /* The peer offered kex-strict-c-v00@openssh.com, so both sequence
       numbers reset to zero at NEWKEYS (OpenSSH PROTOCOL section 1.10 rule
       b; the Terrapin mitigation, CVE-2023-48795). Set from the INITIAL
       KEXINIT only -- section 1.10 says the pseudo-names are valid nowhere
       else -- and never cleared, because rule b holds for the life of the
       connection, not just the first NEWKEYS. Off for a peer that did not
       ask: resetting unilaterally desynchronises the AEAD nonce against a
       client that is still counting.

       THIS IS THE PEER'S HALF ONLY, and that is safe solely because OUR half
       is unconditional: axl_ssh_kexinit_build always carries
       AXL_SSH_KEX_STRICT_S. Strict KEX is in force when BOTH sides advertise,
       so if our offer ever becomes conditional this flag must stop being set
       from the peer's list alone -- otherwise we reset and a client that
       never saw our name does not, which is a bad MAC on the first encrypted
       packet. The unit test "ssh strict kex: our KEXINIT offers
       kex-strict-s-v00@openssh.com" is what holds our half in place;
       removing the name from the built list fails it AND takes the whole
       encrypted transport down, which is how this coupling was measured. */
    bool      strict_kex;
    bool      drop_next;      /* discard one packet: a wrong kex guess */
    unsigned  auth_requests;  /* userauth messages seen; bounds the free form */
    unsigned  svc_requests;   /* SERVICE_REQUESTs seen; bounds that free form */
    unsigned  unimpl_replies; /* UNIMPLEMENTEDs sent; bounds that free form */
    /* The login grace, as THREE distinct states rather than one sentinel.
       born_ms == 0 used to mean "authenticated, never reap" -- and 0 is also
       what axl_time_get_ms() returns when the monotonic clock is
       unreadable, so on such a platform every connection looked
       authenticated and the grace silently ceased to exist. Separate fields
       cannot collide that way: authenticated is set at USERAUTH_SUCCESS,
       deadline_armed only by a clock reading that actually succeeded. The
       reaper's decision over these three is axl_ssh_reap_reason()
       (axl-ssh-reap.h), which the unit test pins directly. */
    bool      authenticated;  /* reached USERAUTH_SUCCESS; exempt from reaping */
    bool      deadline_armed; /* deadline_ms came from a real clock reading */
    uint64_t  deadline_ms;    /* monotonic ms after which the grace has run out */
    /* --- P3: one authentication request, from arrival to its reply ------
       A request is "pending" from the moment its hook is called until its
       reply is sent: auth_req != 0 while the application owes a verdict,
       reply_armed while a failure waits for its minimum time. pump() reads
       nothing while either holds (RFC 4252 section 5.1).

       Ordered by alignment, widest first, not by the story above: the order
       that reads best leaves 41 bytes of padding, which the lint gate's
       clang-analyzer-optin.performance.Padding check refuses. */
    void     *auth_principal;        /* recorded with ACCEPT until applied */
    char     *auth_banner;           /* axl_malloc'd copy, or NULL */
    uint64_t  auth_start_us;         /* when the request arrived */
    size_t    auth_user_len;
    uint64_t  reply_due_ms;
    /* Where this connection sits in the server-wide resume queue: the order
       it joined (never 0 while it waits), so on_reply_timer serves the queue
       first-in first-out rather than in list order -- which is the order
       connections were ACCEPTED, reversed. See auth_may_parse. */
    uint64_t  resume_seq;
    /* The order this connection was ACCEPTED in (never 0), stamped where it
       is linked. Admission breaks a tie between victims at the same key
       exchange state by age (eviction_victim), and the list cannot say
       which is older:
       it is newest-first, and a connection's position there is no record
       of when it arrived once others have been dropped around it. */
    uint64_t  accept_seq;
    /* When the CURRENT wait began, for the reason the reaper now puts on a
       queued connection's timeout line. Written in queue_join's fresh-join
       guard and so overwritten on every REJOIN: a timestamp kept from the
       first join ever would report every period this connection was NOT
       waiting as part of the wait. Paired with queued_armed rather than
       using 0 as "never joined" -- read_now_ms can fail, and this struct
       already carries the scar of born_ms meaning two things. */
    uint64_t  queued_ms;
    void     *principal;             /* adopted at USERAUTH_SUCCESS */
    /* When the linger began and when it ends in a reset. The deadline is
       fixed when the linger starts and nothing the peer sends moves it. */
    uint64_t  linger_start_ms;
    uint64_t  linger_due_ms;
    /* Padding pool for send_packet's axl_ssh_aead_seal (P4b): one firmware
       RNG call per AXL_SSH_RNG_POOL_LEN bytes instead of one per packet.
       Placed HERE, in the run of 8-byte-aligned fields above: its own size
       (264 bytes) is a multiple of 8, so inserting it costs nothing, where
       the run of single-byte bools below would have padded it to a 4-byte
       boundary twice over (the clang-analyzer-optin.performance.Padding
       check the lint gate runs refuses a layout that pads). */
    AxlSshRngPool pad_pool;
    /* Key re-exchange (axl-ssh-rekey.h): the byte and packet counters, which
       side's KEXINIT and NEWKEYS have passed, and so what may be sent and
       what may be processed now. Its limit is the server's rekey_limit AS IT
       STOOD AT ACCEPT, like max_password_failures. 8-byte aligned, so it
       sits in this run (the lint gate's Padding check). */
    AxlSshRekey    rekey;
    /* Idle handling (axl-ssh-idle.h): the silence clock, the unanswered
       probe count and the unused expiry, started at USERAUTH_SUCCESS from
       the server's settings AS THEY STOOD THEN (they cannot change after
       listen anyway). Unarmed -- every tick KEEP -- until then, and for
       good if the clock could not be read at that moment. 8-byte aligned,
       so it sits in this run (the lint gate's Padding check). */
    AxlSshIdle     idle;
    /* Connection-layer packets produced between our KEXINIT and our NEWKEYS,
       which RFC 4253 section 7.1 keeps off the wire until then: each as a
       uint32 length and its PLAINTEXT payload, in production order, sealed
       under the new key by held_flush right after our NEWKEYS. Allocated
       with the connection (on_accept, beside i_c and i_s), so no exchange
       allocates it -- and none can fail to -- in the middle of a rekey; kept,
       emptied and scrubbed, for the connection's life, its capacity
       settling at the largest run ever held. See held_push for its bound. */
    AxlString     *held;
    /* Session channels, one slot per channel this connection may hold (the
       session cap), indexed by our wire channel number -- axl-ssh-chan.h
       has the table's rules. Here, in the 8-byte-aligned run, because each
       slot is pointer-aligned and a multiple of 8 bytes long. */
    AxlSshChanSlot chans[AXL_SSH_MAX_SESSIONS_MAX];
    /* The outbound queue and the one send it may have on the wire -- see
       "Sending" for the lifetime rules. tx_ticket is the in-flight send's
       ticket (NULL when nothing is in flight), kept so tx_detach can cut its
       back-pointer; tx_flush is the scheduled deferred flush's handle, 0
       when none is scheduled. */
    AxlSshTxq            txq;
    struct SshTxTicket  *tx_ticket;
    /* The most the queue held during the current input pause (TXQ_HIGH),
       from the pause line's figure up; logged when the pause ends. */
    size_t               tx_peak;
    AxlSshAuthRequest auth_req;      /* non-zero while a verdict is owed */
    AxlSshAuthVerdict auth_verdict;
    unsigned  password_failures;
    /* The first request's user name as it was on the WIRE, beside
       auth_user's copy of at most AUTH_USER_MAX of it: RFC 4252 section 5
       ends the connection when a later request names someone else
       (axl_ssh_userauth_user_changed), and past the store the length is the
       only part of the old name still known in full. */
    uint32_t  auth_user_wire_len;
    uint32_t  tx_flush;              /* axl_defer handle, 0 = none scheduled */
    /* The scheduled channel flush (chan_flush), 0 when none: at most one
       per connection, cancelled by tx_detach (lifetime rule 7). */
    uint32_t  chan_flush;
    /* Channel hooks of this connection on the stack now -- pump parses
       nothing while it is non-zero (the payload buffer's comment). */
    unsigned  chan_hooks;
    /* Where the flush's round robin over the channels starts next. */
    unsigned  chan_rr;
    /* The source port, read at accept beside peer_addr. Only the audit log
       reads it, and only so its lines carry what sshd's do ("from A port P")
       -- which is what lets an operator match an entry against a NAT or a
       packet capture. Placed HERE, in the run of 4-byte fields, rather than
       beside peer_addr[4] among the bools: the clang-analyzer Padding check
       the lint gate runs refuses a layout that pads, and this one does not. */
    uint16_t  peer_port;
    /* Per-channel flags, one bit per slot (index = wire channel number):
       the pty hook accepted a pty; axl_ssh_channel_close ran under one of
       the channel's own hooks, so its CLOSE goes out once that hook's reply
       has (chan_settle); a writable hook is owed (chan_budget_eased took
       the edge, chan_fire_writable calls it). */
    uint16_t  chan_pty;
    uint16_t  chan_close_want;
    uint16_t  writable_due;
    bool      auth_in_hook;          /* the application's hook is on the stack */
    bool      have_auth_user;        /* auth_user holds the first request's name */
    bool      auth_have_verdict;     /* recorded, not yet applied */
    bool      auth_is_password;      /* which method the request is for */
    bool      auth_have_start;       /* auth_start_us came from the clock */
    bool      reply_armed;           /* a reply or drop waits on the timer */
    bool      drop_due;              /* ...and it is a drop, not a reply */
    bool      drop_deferred;         /* dropped during its hook (conn_drop) */
    bool      dropping;              /* conn_drop is tearing it down now */
    /* --- P5: the linger (conn_linger). The rest of its state is
       linger_start_ms / linger_due_ms and in_recv. */
    bool      lingering;             /* entered: app released, still listed */
    bool      disc_sent;             /* the send carrying the DISCONNECT completed */
    bool      tx_broken;             /* a run was lost: the peer can read nothing after it */
    bool      releasing;             /* conn_linger is running application code */
    bool      pen_accrued;           /* penalty_accrue ran: never again */
    bool      resume_due;            /* waiting for its turn to parse */
    bool      queued_armed;          /* ...since queued_ms, a real reading */
    bool      resume_turn;           /* ...and on_reply_timer is giving it */
    bool      tx_paused;             /* past TXQ_HIGH: pump parses nothing */
    bool      rx_parked;             /* ...and on_recv left the receive unarmed */
    bool      chan_gated;            /* pump stopped for a channel hook */
    bool      chan_flush_lost;       /* chan_schedule found the defer ring
                                        full; the reaper retries it */
    bool      kex_replied;           /* this rekey's KEX_ECDH_REPLY is out:
                                        a second KEX_ECDH_INIT is not the
                                        exchange's */
    /* --- Task P: what the penalty ladder reads when this connection ends
       (axl_ssh_penalty_classify). One setter each: pen_refused at the top
       of schedule_failure, the path every refused real attempt that is owed
       a reply takes and the `none` probe does not (a forgery is the
       exception: it gets no reply, and sets pen_forged instead); pen_forged
       in the signature check; pen_grace_expired in on_reap.
       "authenticated" is the field above. pen_server_fault (ruling P6) on
       the two paths where the failure is the server's own: on_accept's
       setup and an unarmed login grace. */
    bool      have_peer;             /* peer_addr came from a real reading */
    bool      pen_refused;           /* a real attempt was refused */
    bool      pen_forged;            /* an authorized key's signature failed */
    bool      pen_grace_expired;     /* reaped with an ARMED deadline past */
    bool      pen_server_fault;      /* the SERVER failed it: charge nothing */
    /* The server's password cap AS IT STOOD AT ACCEPT (D4:
       axl_ssh_server_set_max_password_failures, which is settable while the
       server listens). send_due_reply compares against THIS, never against
       the server's field: sshd forks a process per connection with the
       configuration then in force, so a reload leaves open connections
       alone, and copying here is how that behaviour is reproduced.
       A uint8_t among the bools rather than beside login_grace_sec's
       relatives: the value is at most AXL_SSH_PASSWORD_FAILURES_MAX, and
       this run of single bytes is the one place it costs no padding (the
       lint gate's clang-analyzer-optin.performance.Padding check). */
    uint8_t   max_password_failures;
    /* The session cap AS IT STOOD AT ACCEPT, for the same reason as the
       password cap above (axl_ssh_server_set_max_sessions). */
    uint8_t   max_sessions;
    /* on_recv frames of THIS connection on the stack: a receive callback that
       returns true re-arms itself, so conn_linger must not arm a second one
       under it. A count, not a flag, because a hook may dispatch the loop and
       deliver a nested receive. */
    uint8_t   in_recv;
    uint8_t   peer_addr[4];          /* the source, read at accept */
    char      auth_user[AUTH_USER_MAX + 1]; /* NUL-terminated copy */
    /* Each direction's cipher: `cur` seals or opens its packets now, and a
       finished exchange STAGES the next key here until that direction's
       NEWKEYS (axl_ssh_dirkey_switch) -- ours, right after it is sealed, for
       sending; the peer's, when it arrives, for receiving. A receive key
       installed when the exchange finishes instead would open the peer's
       in-flight OLD-key packets with the new key. The initial exchange
       stages too, so there is one path for both. Byte-aligned, so they sit
       with auth_user at the end (the Padding check). */
    AxlSshDirKey tx_key;
    AxlSshDirKey rx_key;
} SshConn;

/* Finished: closing (ST_DEAD) or waiting out a DISCONNECT (ST_LINGER).
   Nothing is sealed, parsed or authenticated on either. */
static inline bool
conn_over(
    const SshConn *c  ///< connection
)
{
    return c->state >= ST_DEAD;
}

struct AxlSshServer {
    AxlLoop  *loop;
    uint16_t  port;
    AxlTcp     *listener;
    AxlSourceId reaper;
    AxlPkKey   *host_key;
    uint8_t   host_pub[32];
    AxlSshAuthKeys authkeys;   /* who may log in; empty means nobody */
    /* Seconds a connection may sit without AUTHENTICATING before it is
       dropped (Task G: axl_ssh_server_set_login_grace; default
       AXL_SSH_LOGIN_GRACE_DEFAULT_SEC). OpenSSH's LoginGraceTime is 120 s
       and does the same job.

       Before admission by eviction (spec section 12a.2) this was the only
       thing between the connection cap and a permanent denial of service:
       16 peers that complete a TCP handshake and then send NOTHING pinned
       nconns at the cap, the accept paused, and the only re-arm was in
       conn_drop -- which never ran, because nothing dropped them. Eviction
       now takes such a peer the moment a new arrival needs its slot; this
       still ends a peer that FINISHED key exchange and then stalls, which
       eviction never takes. */
    uint32_t  login_grace_sec;
    /* sshd's UnusedConnectionTimeout and ClientAliveInterval /
       ClientAliveCountMax (axl_ssh_server_set_unused_timeout,
       axl_ssh_server_set_client_alive; defaults 300 s and 60 s x 3, each
       0 = off, spec section 12a.7 deviation 2). Fixed at listen, like the
       grace: the reaper's tick is derived from them there. */
    uint32_t  unused_sec;
    uint32_t  alive_sec;
    uint32_t  alive_max;
    /* Per-source penalties (Task P: axl_ssh_server_set_penalties and
       axl_ssh_server_set_penalty_exempt). Every decision over it is a pure
       function in axl-ssh-penalty.c; this file only reads the clock and the
       peer address, checks at accept, and records when a connection ends. */
    AxlSshPenaltyTable penalty;
    /* --- P3: authentication hooks ---------------------------------------- */
    AxlSshAuthOps auth_ops;     /* copied; all NULL when none installed */
    void     *auth_ud;
    /* Keys the per-user minimum failure time (spec section 9a). Random per
       server, so one server's delays predict nothing about another's. */
    uint8_t   timing_secret[AXL_SSH_TIMING_SECRET_LEN];
    /* Bound how often a FAILED authentication is announced at INFO
       (axl_ssh_auth_log_due). Per server, not per source: a distributed
       flood is exactly the case the per-source penalty ladder cannot
       throttle, so a per-source limiter would leave the console unprotected
       against the one attacker the rest of the design cannot refuse.
       Successes are not rate-limited at all -- see auth_succeed.

       ONE WINDOW PER KIND OF LINE (AuthLogKind), never shared. There was
       one, and an ordinary refusal a moment before a forgery -- an
       unauthorized key, which any peer can offer for free -- pushed the
       forgery's line to DEBUG; with nothing after it, the forgery never
       reached INFO even as a count. That is an evasion for anyone holding a
       stolen .pub against an operator who alerts on "bad signature". The
       rule is general, not special to forgeries: every kind is cheap for
       SOMEBODY ("attempts exceeded" takes twenty-one `none` requests, a
       timeout takes a connection that says nothing), and whichever kinds
       share a window, the cheaper one hides the other's first line. Each
       window bounds only its own kind, so the console sees at most one line
       per kind a minute -- AUTH_LOG_KINDS lines in all, ten today: the
       five refusal kinds, the queued timeout the reaper distinguishes, the
       hook watchdog's two (which are not one event either -- a stalled
       hook always goes on to produce a slow-return line), and admission's
       two (an eviction and a refusal past max_startups). */
    AxlSshAuthLogRate auth_log[AUTH_LOG_KINDS];
    /* The pre-authentication banner (D9: axl_ssh_server_set_banner), an
       axl_malloc'd copy or NULL. sshd's `Banner`.

       WHERE IT IS READ: exactly one site, in on_userauth, after the
       `ssh-connection` service check and BEFORE the `none` reply, the
       user-name validation, the method dispatch and any issue_request. It
       is never cached in a connection, so a live set cannot leave a
       connection holding a pointer to a freed copy -- and because the send
       site precedes every application call on that path, a hook cannot be
       running underneath the read.

       ONCE is decided by `c->auth_requests == 1`, NOT by a per-connection
       flag. A flag would send a banner in the MIDDLE of an authentication
       -- between two password prompts -- to a connection whose first
       request found none set.

       WHY ONCE MATTERS beyond tidiness: at most MAX_CONNS x
       AXL_SSH_BANNER_MAX (64 KB) can be queued to peers that never read,
       before any of them authenticates. That is the same pre-auth
       amplifier argument MAX_SERVICE_REQUESTS and MAX_UNIMPLEMENTED_REPLIES
       make, and it is why a banner per request would need a different
       bound.

       THE SETTER stores the new pointer before freeing the old one, and
       axl_ssh_server_free releases it beside the keys. */
    char     *banner;
    /* Session channels (axl-ssh-channel.h): the application's hooks, copied
       by axl_ssh_server_set_channel_ops, and the write budget every channel
       of every connection charges its queued output to. */
    AxlSshChannelOps chan_ops;
    void            *chan_ud;
    AxlSshChanBudget budget;
    /* ONE timer for every owed reply -- see rearm_reply_timer(). */
    AxlSourceId reply_timer;
    uint64_t  reply_timer_due_ms;
    /* ONE timer for every lingering connection's deadline -- see
       rearm_linger_timer(). One source for the server, as the reply timer
       is, because the loop's table is shared (lifetime rule 6). */
    AxlSourceId linger_timer;
    uint64_t  linger_timer_due_ms;
    /* Last resume_seq handed out. Whole numbers of 64 bits: a queue position
       cannot recur while anything still holds an earlier one. */
    uint64_t  resume_last;
    /* Last accept_seq handed out, by the same argument as resume_last. */
    uint64_t  next_accept_seq;
    /* axl_ssh_server_set_rekey_limit (default AXL_SSH_REKEY_LIMIT_DEFAULT,
       0 = never by bytes). Copied into each connection's rekey state at
       accept, like max_password_failures, so a change reaches connections
       accepted afterwards. */
    uint64_t  rekey_limit;
    /* --- NEW-1: the authentication-hook watchdog. REPORTING ONLY; nothing
       here refuses, delays or cancels anything (see
       axl_ssh_server_set_auth_hook_budget_ms). Server-side rather than per
       connection because `hooks` is capped at 1, so there is exactly one
       hook to describe at any moment.

       hook_start_ms is paired with hook_start_armed for the same reason
       queued_ms is: a failed clock reading must not read as "started at
       0". */
    uint64_t  hook_start_ms;
    SshConn  *conns;
    /* The connection whose hook is on the stack, valid only while
       hooks > 0. CLEARED where hooks reaches 0, because conn_drop DEFERS
       the drop of a connection whose own hook is running -- the frame
       beneath issue_request frees it the moment the hook returns, and a
       pointer left here would be read by the next reap sweep. */
    SshConn  *hook_conn;
    unsigned  nconns;
    unsigned  accept_errs; /* consecutive failed accepts; reset on success */
    /* Application code on the stack right now -- see app_call_begin(). */
    unsigned  app_calls;
    unsigned  hooks;       /* ...of which authentication hooks (capped at 1) */
    /* What axl_ssh_server_set_auth_hook_budget_ms holds (0 = report
       neither), and the copy a running hook is measured against. LATCHED at
       hook entry, the rule set_max_password_failures follows for a
       connection already accepted: a change made while a hook runs reaches
       the NEXT one, which also stops a misbehaving hook disarming the
       report about itself. */
    uint32_t  auth_hook_budget_ms;
    uint32_t  hook_budget_ms;
    /* Wrong passwords that end a connection (D4:
       axl_ssh_server_set_max_password_failures; default
       AXL_SSH_PASSWORD_FAILURES_DEFAULT). NOTHING enforces it from here --
       on_accept copies it into each connection and send_due_reply reads that
       copy, so a change reaches connections accepted afterwards and leaves
       open ones alone. */
    uint8_t   max_password_failures;
    /* Unauthenticated connections allowed at once before admission starts
       evicting the one least far along in key exchange (spec section 12a.2:
       axl_ssh_server_set_max_startups; default
       AXL_SSH_MAX_STARTUPS_DEFAULT). Read at every arrival, by on_accept's
       admission check, which hands it to axl_ssh_admit_decide()
       (axl-ssh-admit.h) -- so a change reaches the next arrival and evicts
       nothing by itself. Same run of single bytes as
       max_password_failures, for the same padding reason. */
    uint8_t   max_startups;
    /* axl_ssh_server_set_max_sessions; copied into each connection at
       accept, like max_password_failures. */
    uint8_t   max_sessions;
    /* axl_ssh_server_set_channel_ops installed hooks: session channels
       open. False, every CHANNEL_OPEN is refused as before channels. */
    bool      have_chan_ops;
    /* The accept is armed. False only while paused after MAX_ACCEPT_ERRS
       consecutive failed accepts, or after a re-arm failed: capacity never
       pauses it (admission by eviction, spec section 12a.2), and
       accept_rearm restores it. */
    bool      accepting;
    /* accept_rearm has reported this episode's failure at ERROR; cleared
       when a re-arm succeeds. */
    bool      rearm_err_logged;
    bool      running;
    bool      free_pending;   /* stopped by axl_ssh_server_free, not yet freed */
    bool      free_scheduled; /* ...and the callback that frees it is queued */
    bool      hook_is_password;  /* the running hook's method, for its line */
    bool      hook_start_armed;  /* hook_start_ms came from a real reading */
    bool      hook_reported;     /* its mid-hook line has already gone out */
};

/* ------------------------------------------------------------------------ */
/* State shared by every server in the image                                 */
/* ------------------------------------------------------------------------ */

/* The last authentication request handle issued, by ANY server. One counter
   for the image, not one per server -- the decision src/loop/axl-loop.c made
   for source ids after the adbf5461 cross-instance bug, made here for the
   same reason (P3 security review MED-4). Handles that restart at 1 on every
   server put the same handle on every instance, so after a restart -- free,
   then new -- a completion for the OLD server's request H could match the
   NEW server's request H and apply an old ACCEPT, principal and all, to a
   different peer. From one counter a handle recurs nowhere before 2^32 - 1
   requests in total; 0 is skipped on the wrap (axl_ssh_auth_request_next).
   Single-threaded UEFI: no locking. */
static AxlSshAuthRequest g_last_request;

/* Every server from the end of axl_ssh_server_new to the START of its
   axl_ssh_server_free. axl_ssh_auth_complete looks a server up here before
   reading one byte of it, because the natural teardown order -- the server
   freed, then axl_async_free, whose drain runs done callbacks -- hands it a
   freed pointer, and walking that server's connections wrote a verdict into
   whatever had taken its place.

   A table of pointers, not a list threaded through the servers: the lookup
   compares the caller's pointer with static storage and reads no heap at
   all, stale or live. axl-registry.c does not fit: it exists to sweep leaked
   firmware resources at exit (it would start freeing leaked servers), it has
   no lookup by pointer, --minimal-runtime images never initialise it (every
   server would be refused), and _axl_registry_add does not report a failed
   append, so it cannot fail closed.

   THE BOUND, 8 (AXL_SSH_LIVE_SERVERS_MAX). A listening server holds its
   accept, its reaper and its reply timer, plus a receive per connection up
   to MAX_CONNS, from its loop's 64 sources: at most three fully loaded
   servers fit on one loop. Eight covers servers on several loops and servers
   that never listen, for 64 bytes of .bss. FULL, axl_ssh_server_new refuses:
   a server this table cannot record is one whose late completions could not
   be refused safely.

   A NEW SERVER AT A FREED ONE'S ADDRESS is harmless FOR A COMPLETION. A late
   completion for the freed server then finds this entry and searches the new
   server's connections for its handle -- which, issued from g_last_request
   above, no request on the new server carries -- so it still returns
   AXL_NOT_FOUND. It is NOT harmless for the other calls that read this
   table (axl_ssh_server_free, _listen, _set_auth_ops, _set_login_grace,
   _set_penalties, _set_penalty_exempt, _set_max_password_failures,
   _set_max_startups, _set_banner, and the authorized-key loaders):
   each of those acts on whatever server the address now names, so
   one freed and replaced at the same address is torn down, or written
   through, as if it were the new one. That is the table's real weakness --
   address reuse, not the pointer comparison below -- and it is what
   axl_ssh_server_free's own header comment now says plainly (P3 security
   re-review NEW-4).

   WHY THE COMPARISON g_live_servers[i] == s IS SOUND HERE, THOUGH s MAY BE
   FREED. After axl_free(s) the value of s is indeterminate by the standard
   (C11 6.2.4p2, 7.22.3p1), and a compiler that could see both the
   deallocation and this comparison would be entitled to fold the comparison
   to false regardless of what the table holds. It cannot see them together:
   axl_free is defined in src/mem/axl-mem.c, a different translation unit,
   reached through an ordinary external call; every caller here
   (axl_ssh_server_free, axl_ssh_auth_complete, and the rest) receives s as a
   parameter from OUTSIDE this TU, so its provenance is opaque at every
   comparison in this file; and this tree builds with no -flto --
   `grep -rn flto cmake/ scripts/` finds nothing, and the configuration flags
   are `-Os -g ...` / `-Og -g ...` with neither naming it
   (cmake/AxlFlags.cmake).
   A consumer's own -flto on ITS build does not change this either, since the
   comparison is already compiled into libaxl.a without it. Enabling any of
   the three -- LTO here, a custom allocator inlined into this file, or
   moving axl_free itself into this file -- would need this revisited.

   THREADS AND TPL. Only the BSP touches this table, from wherever the
   application runs the server: new, free, listen, set_auth_ops, and
   completions. AxlAsync runs done callbacks from its loop idle source or from
   axl_async_free's drain, never on an AP (axl-async.c, axl_task_pool_poll).
   A loop dispatches at TPL_APPLICATION under axl_loop_run, or at TPL_CALLBACK
   from axl_loop_attach_driver's notify, which never preempts itself. The one
   interleaving is application code at TPL_APPLICATION preempted by a
   driver-mode notify, and the server's connection list has no more
   protection against that than this table does, so a lock here would guard
   nothing the server does not already expose. What the table does ensure is
   that removal is a single pointer store made before anything is freed: a
   completion that preempts axl_ssh_server_free sees either a live server
   whose memory is still intact, or no server. */
static AxlSshServer *g_live_servers[AXL_SSH_LIVE_SERVERS_MAX];

/* ------------------------------------------------------------------------ */
/* Time                                                                      */
/* ------------------------------------------------------------------------ */

/* Read the monotonic clock, reporting whether the reading is REAL.

   axl_time_get_ms() answers 0 both for "0 ms since boot" and for "there is
   no readable clock", and this server cannot afford to confuse them: the
   counter frequency genuinely can be unreadable -- an unprogrammed
   CNTFRQ_EL0 on aarch64, a failed TSC calibration on x64 -- and a login
   grace written on an unchecked reading stops enforcing anything without
   saying so. Every caller here needs to know whether it got a time. */
static bool
read_now_ms(
    uint64_t *out  ///< receives milliseconds since boot; untouched on failure
)
{
    AxlTimespec ts;

    if (axl_clock_gettime(AXL_CLOCK_MONOTONIC, &ts) != AXL_OK) {
        return false;
    }
    *out = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    return true;
}

/* read_now_ms's microsecond twin, for the minimum failure time. */
static bool
read_now_us(
    uint64_t *out  ///< receives microseconds since boot; untouched on failure
)
{
    AxlTimespec ts;

    if (axl_clock_gettime(AXL_CLOCK_MONOTONIC, &ts) != AXL_OK) {
        return false;
    }
    *out = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Sending                                                                   */
/* ------------------------------------------------------------------------ */

/* conn_drop belongs to the connection lifecycle further down; a send that
   fails AFTER its producer returned has nobody to report to, so it ends the
   connection from here. */
static void
conn_drop(SshConn *c, AxlTeardown how);

/* The linger (defined with the connection lifecycle): conn_drop starts it for
   a connection whose DISCONNECT went out on a healthy stream. */
static void
conn_linger(SshConn *c);

/* ONE TCP SEND IN FLIGHT PER CONNECTION, AND EVERYTHING SEALED MEANWHILE
   LEAVES IN THE NEXT ONE.

   The expensive part of axl_tcp_send_async is its completion (~95 us on x64,
   measured), not its bytes, so sending each packet on its own made that the
   whole outbound cost. Producers -- send_packet and on_accept's
   identification line -- only PUSH into c->txq; tx_flush is the one place
   that calls axl_tcp_send_async, and it hands TCP the whole pending run as
   one buffer. The DEBUG line "tx: F frames, B bytes" is written once per
   such send, never at INFO: the integration test counts it.

   LIFETIME RULES -- every one of them is a use-after-free if broken:

   1. The send BORROWS its buffer until its callback. The buffer and a
      back-pointer to the connection live in a heap TICKET (SshTxTicket),
      and on_sent is the one free of both, whatever the status.

   2. The back-pointer is how a completion sends the next run, so it must
      never outlive the connection: tx_detach cuts it (ticket->c = NULL)
      through c->tx_ticket, and on_sent clears c->tx_ticket before freeing
      the ticket, so tx_detach never writes through a freed one. There is at
      most one ticket per connection, because there is at most one send.

   3. A completion does NOTHING but free and forget its ticket when it is
      orphaned (c == NULL), cancelled (AXL_CANCELLED), or its connection is
      ST_DEAD or its server stopping. The STATUS CANNOT SAY "TORN DOWN":
      axl_tcp_close runs every callback it still holds SYNCHRONOUSLY, and
      only the sends whose bytes it drops get AXL_CANCELLED (every close
      this server makes is RESET; a GRACEFUL one would also deliver a
      fully-armed send with AXL_OK) -- a send
      that had already FINISHED, whose callback was scheduled for the next
      loop iteration but has not run, is delivered from inside the close
      with the status it finished with, AXL_OK or AXL_ERR. conn_drop calls
      the close BEFORE conn_free, so that completion would find its
      connection still allocated and, flushing or dropping, re-enter the
      teardown already under way: a second penalty, a second conn_free.
      So conn_drop marks the connection ST_DEAD and runs tx_detach BEFORE
      it closes, and every completion the close delivers is an orphan.

   4. Flushes are scheduled, not immediate: the first push with nothing in
      flight defers ONE flush (axl_defer) to the next loop turn, so every
      packet a turn seals goes out together. At most one is scheduled
      (c->tx_flush holds its handle), and tx_detach CANCELS it -- from
      conn_drop before its close, and first thing in conn_free, before
      release_principal can run the loop -- since the deferred call holds
      `c` raw. If axl_defer answers 0 (its queue is full) the flush happens
      NOW: late is acceptable, never is not. A completion with more pending
      flushes directly; nothing else needs scheduling.

   5. disconnect() flushes immediately when nothing is in flight, so its
      DISCONNECT reaches TCP's queue ahead of the abortive close that follows
      it, as it did before the queue existed (see disconnect()).

   6. Progress depends on the completion being DELIVERED: a connection's
      next run leaves only from on_sent. AxlTcp schedules a delivery on the
      loop's defer ring (~42 entries), then on a one-shot timer from the
      loop's 64-entry source table -- and BOTH are SHARED with every other
      tenant of the loop: on SoftBMC's one loop that is its HTTP and
      WebSocket servers, their sockets and timers too. With both full the
      delivery comes only at the socket's close, and the connection sends
      nothing more until then. Rule 3 keeps that memory-safe; to the user
      it is a HANG -- a session that stops answering until it is closed.
      Each connection holds at most TWO ring entries at a time -- its
      scheduled flush or its finished send's delivery, never both, because
      a flush is scheduled only with nothing in flight, plus its scheduled
      channel flush (rule 7) -- so this server's own demand is 2 x MAX_CONNS
      plus schedule_server_free's one. test_ssh_defer_ring_bound
      (test/unit/axl-test-ssh.c) pins that an EMPTY ring holds that much;
      what the other tenants take is theirs to bound.

   7. Channel output reaches the queue only through ONE scheduled CHANNEL
      flush per connection (chan_flush, c->chan_flush holds its axl_defer
      handle), never inline: an axl_ssh_channel_write made inside a hook
      must leave after the reply to that hook's request, which is sealed
      when the hook returns. It is scheduled after a write, an exit, a
      WINDOW_ADJUST, a hook leaving work behind, and from on_sent once the
      queue is back under TXQ_LOW -- each only when chan_has_work says
      something could actually go out -- and never on a timer. Between our
      KEXINIT and our NEWKEYS of a rekey it is neither scheduled nor run
      (chan_paused): channel data waits in the channel queues, and our
      NEWKEYS schedules it (rekey_our_newkeys). The deferred
      call holds `c` raw, so tx_detach CANCELS it, from conn_drop before its
      close and first thing in conn_free. If axl_defer answers 0 the flush
      is NOT run inline (that would packetize under a hook, ahead of its
      reply): chan_flush_lost is set and the reaper's sweep retries.

   ORDER IS SACRED: frames leave in push order, each exactly once, because
   a sealed packet's sequence number is its AEAD nonce. The queue keeps that
   by construction (one pending run, taken whole), and a run TCP refuses is
   never re-sent out of turn -- its connection ends instead. */
typedef struct SshTxTicket {
    SshConn *c;     /* NULL once tx_detach has run */
    uint8_t *buf;   /* the run; the send borrows it until on_sent */
} SshTxTicket;

/* Hand the whole pending run to TCP as one send. AXL_OK when it was sent or
   there was nothing to send; AXL_ERR when it could not be -- the run is then
   gone and the connection must end, which the CALLER does (it knows whether
   it may drop the connection from where it stands). */
static int
tx_flush(
    SshConn *c  ///< connection; not ST_DEAD
);

/* End an output-backpressure pause (TXQ_LOW); defined after on_recv, which
   it re-arms. */
static int
rx_resume(
    SshConn *c  ///< paused connection; not ST_DEAD, its server running
);

/* Schedule the channel flush (lifetime rule 7) when there is something for
   it to do; defined with the channel code. */
static void
chan_schedule(
    SshConn *c  ///< connection whose channels may have output
);

/* End a connection the server cannot go on serving; defined with the queue
   helpers further down. */
static void
drop_failed(
    SshConn *c  ///< connection to drop
);

static bool
on_sent(AxlTcp *sock, AxlStatus st, void *data)
{
    SshTxTicket *t = data;
    SshConn     *c = t->c;

    (void)sock;
    axl_free(t->buf);
    axl_free(t);
    if (c == NULL) {
        return true;   /* rules 2 and 3: the connection is gone or going */
    }
    /* Bookkeeping only, and safe here: `c` is alive and not being dropped
       (tx_detach would have cut t->c), and this ticket is its one in-flight
       send. */
    c->tx_ticket = NULL;
    axl_ssh_txq_sent(&c->txq);
    if (st == AXL_OK && c->state == ST_LINGER
        && _axl_ssh_server_test_point("tx-complete-fail-linger")) {
        st = AXL_ERR;
    }
    if (st == AXL_CANCELLED || c->state == ST_DEAD || !c->srv->running) {
        return true;   /* rule 3 */
    }
    if (st != AXL_OK) {
        /* The transport lost bytes it had accepted. What follows on this
           stream would be out of sequence for the peer's cipher, so the
           connection ends here rather than carrying on desynchronised. On a
           connection in ST_LINGER too: tx_broken is what keeps conn_drop
           from lingering a DISCONNECT the peer cannot decrypt -- or, if the
           linger has started, this drop ends it with a reset. */
        axl_debug("tx: send failed (%d)", (int)st);
        c->tx_broken = true;
        conn_drop(c, AXL_TEARDOWN_RESET);
        return true;
    }
    if (tx_flush(c) != AXL_OK) {
        conn_drop(c, AXL_TEARDOWN_RESET);
        return true;
    }
    if (c->state == ST_LINGER) {
        /* Flushing is the ONE thing a lingering connection still does.
           Nothing is sealed after its DISCONNECT, so a queue with nothing
           pending and nothing in flight means the send carrying it has just
           completed -- what makes a later hang-up "delivered". */
        if (c->tx_ticket == NULL && c->txq.pending_frames == 0) {
            c->disc_sent = true;
        }
        return true;
    }
    /* Only from here, past rule 3: a completion on a dying connection must
       not re-arm or parse. */
    if (c->tx_paused && axl_ssh_txq_bytes(&c->txq) < TXQ_LOW
        && rx_resume(c) != AXL_OK) {
        drop_failed(c);
        return true;
    }
    /* Channel output waits for the queue to fall under TXQ_LOW (rule 7). */
    if (axl_ssh_txq_bytes(&c->txq) < TXQ_LOW) {
        chan_schedule(c);
    }
    return true;   /* one-shot: the return is ignored for sends */
}

static int
tx_flush(
    SshConn *c  ///< connection; not ST_DEAD
)
{
    uint8_t  *buf    = NULL;
    size_t    len    = 0;
    unsigned  frames = 0;

    if (!axl_ssh_txq_ready(&c->txq)) {
        return AXL_OK;
    }
    /* Every failure below ends the connection (see above), and a DISCONNECT
       sealed after it would follow a hole the peer cannot read past -- so it
       is marked, and disconnect() resets rather than lingers. */
    SshTxTicket *t = axl_malloc(sizeof *t);
    if (t == NULL) {
        c->tx_broken = true;
        return AXL_ERR;
    }
    if (axl_ssh_txq_take(&c->txq, &buf, &len, &frames) != AXL_OK) {
        axl_free(t);
        c->tx_broken = true;
        return AXL_ERR;
    }
    t->c   = c;
    t->buf = buf;
    if (_axl_ssh_server_test_point(c->state == ST_LINGER ? "tx-refuse-linger"
                                                         : "tx-refuse")
        || axl_tcp_send_async(c->sock, buf, len, c->srv->loop, NULL,
                              on_sent, t) != AXL_OK) {
        /* Refused outright, so no callback will come: this frame owns the
           ticket's free, and the run is lost. */
        axl_ssh_txq_sent(&c->txq);
        axl_free(buf);
        axl_free(t);
        c->tx_broken = true;
        return AXL_ERR;
    }
    c->tx_ticket = t;
    axl_debug("tx: %u frames, %zu bytes", frames, len);
    return AXL_OK;
}

static void
tx_flush_cb(
    void *data  ///< the SshConn; conn_free cancels this before freeing it
)
{
    SshConn *c = data;

    c->tx_flush = 0;
    if (c->state == ST_DEAD || !c->srv->running) {
        return;
    }
    if (tx_flush(c) != AXL_OK) {
        conn_drop(c, AXL_TEARDOWN_RESET);
    }
}

/* After a push: schedule the one deferred flush if nothing is in flight and
   none is scheduled (rule 4). AXL_ERR only from the immediate fallback. */
static int
tx_schedule(
    SshConn *c  ///< connection that just pushed
)
{
    if (c->tx_paused && axl_ssh_txq_bytes(&c->txq) > c->tx_peak) {
        c->tx_peak = axl_ssh_txq_bytes(&c->txq);
    }
    if (c->tx_flush != 0 || !axl_ssh_txq_ready(&c->txq)) {
        return AXL_OK;
    }
    c->tx_flush = axl_defer(c->srv->loop, tx_flush_cb, c);
    if (c->tx_flush != 0) {
        return AXL_OK;
    }
    return tx_flush(c);   /* the defer queue is full: now, not never */
}

/* Queue one frame that needs no sealing -- the identification line. */
static int
tx_queue_raw(
    SshConn    *c,     ///< connection
    const void *data,  ///< the frame
    size_t      len    ///< its length
)
{
    if (conn_over(c)
        || axl_ssh_txq_push(&c->txq, data, len) != AXL_OK) {
        return AXL_ERR;
    }
    return tx_schedule(c);
}

/* Defined with key exchange: start a rekey of our own -- send our KEXINIT. */
static int
rekey_start(
    SshConn *c  ///< connection in ST_SESSION, no exchange running
);

/* Keep one outbound payload until our NEWKEYS (the `held` field). */
static int
held_push(
    SshConn    *c,        ///< connection between our KEXINIT and NEWKEYS
    const void *payload,  ///< the plaintext payload
    size_t      len       ///< its length
);

/* Frame one payload and send it, advancing the transmit sequence number.
   EVERY packet counts, encrypted or not -- the sequence number is the AEAD
   nonce, and it starts counting from the first packet on the connection, not
   from NEWKEYS. Getting that wrong desynchronises the cipher at the exact
   moment it is switched on, which looks like a bad MAC rather than like a
   counting bug.

   REKEY, as sshd's ssh_packet_send2 does it. A packet that is not a
   key-exchange message, about to go out on an authenticated connection past
   its limit, starts the exchange FIRST -- our KEXINIT is sealed ahead of it
   -- and is then itself held. From our KEXINIT to our NEWKEYS such packets
   go to `held` as plaintext (axl_ssh_rekey_out), and leave, sealed under
   the new key, right after our NEWKEYS (held_flush). Every encrypted packet
   sealed is counted against the key that sealed it. */
static int
send_packet(SshConn *c, const void *payload, size_t len)
{
    if (conn_over(c) || len == 0) {
        return AXL_ERR;
    }
    uint8_t msg = ((const uint8_t *)payload)[0];
    if (c->state == ST_SESSION && !axl_ssh_rekey_is_kex(msg)
        && axl_ssh_rekey_due(&c->rekey) && rekey_start(c) != AXL_OK) {
        return AXL_ERR;
    }
    if (axl_ssh_rekey_out(&c->rekey, msg) == AXL_SSH_REKEY_HOLD) {
        return held_push(c, payload, len);
    }
    AxlString *pkt = axl_string_new("");
    if (pkt == NULL) {
        return AXL_ERR;
    }
    int rc = c->tx_on
        ? axl_ssh_aead_seal(&c->tx_key.cur, c->tx_seq, payload, len,
                            &c->pad_pool, pkt)
        : axl_ssh_packet_wrap(pkt, payload, len);
    size_t wire = axl_string_len(pkt);
    if (rc == AXL_OK) {
        rc = axl_ssh_txq_push(&c->txq, axl_string_data(pkt), wire);
    }
    axl_string_free(pkt);
    if (rc != AXL_OK) {
        return rc;
    }
    /* Counted at the PUSH: from here the frame is on its way, in order. */
    c->tx_seq++;
    if (c->tx_on) {
        axl_ssh_rekey_count_out(&c->rekey, wire);
    }
    return tx_schedule(c);
}

/* Send SSH_MSG_DISCONNECT and finish the connection.

   WHAT THE PEER SEES. With @a linger and the DISCONNECT queued, the
   connection becomes ST_LINGER: nothing is sealed after the DISCONNECT
   (send_packet and the channel flush refuse a finished connection), on_sent
   keeps flushing what is queued, and the caller's conn_drop starts the
   LINGER instead of a reset (conn_linger) -- the peer reads the reason, and
   ssh(1) prints it -- unless the stream has lost a run (tx_broken: before
   the DISCONNECT, in its own flush, or at a completion before the drop),
   where the peer could not decrypt it anyway, and conn_drop resets.
   Otherwise the connection becomes ST_DEAD and the caller's conn_drop resets
   it at once: an input flood (the one caller that passes @a linger false,
   sshd's cleanup_exit parity), or a DISCONNECT that could not be built or
   queued.

   Against sshd: ssh_packet_disconnect (packet.c:2055-2091) logs, queues the
   DISCONNECT, waits in ssh_packet_write_wait until the kernel has taken it
   -- bounded only by the packet timeout, ClientAlive x Count or the grace
   alarm -- then closes and exits. Its "delivered" is "in the socket
   buffer"; ours is "the peer hung up after the send carrying it completed",
   and our bound is AXL_SSH_DISCONNECT_LINGER_MS, not minutes. The reason
   codes stay RFC 4253's (sshd sends 2 for everything, packet.c:2848); the
   texts match sshd's where sshd has one.

   FLUSHED HERE, not on the next turn: a caller that resets (ST_DEAD) closes
   on this same stack, so a deferred flush would never run. With a send
   already in flight the DISCONNECT waits behind it in our queue, and on_sent
   sends it when that completes -- the case a CHANGE_FIRST banner run puts it
   in. */
static void
disconnect_as(
    SshConn                *c,       ///< connection
    AxlSshDisconnectReason  reason,  ///< RFC 4253 section 11.1 code
    const char             *why,     ///< description, as sent
    bool                    linger   ///< false: always reset (input flood)
)
{
    if (conn_over(c)) {
        return;
    }
    bool       sent = false;
    AxlString *m    = axl_string_new("");
    if (m != NULL) {
        char hdr = (char)AXL_SSH_MSG_DISCONNECT;
        if (axl_string_append_len(m, &hdr, 1) == AXL_OK
            && axl_ssh_put_u32(m, (uint32_t)reason) == AXL_OK
            && axl_ssh_put_string(m, why, axl_strlen(why)) == AXL_OK
            && axl_ssh_put_string(m, "", 0) == AXL_OK) {
            sent = send_packet(c, axl_string_data(m), axl_string_len(m)) == AXL_OK;
        }
        axl_string_free(m);
    }
    /* Finished from here: nothing more is sealed. ST_LINGER whenever the
       DISCONNECT was queued, before the flush, so that flush is a lingering
       connection's own. Whether the stream is healthy enough to linger is
       NOT decided here: a run lost before, in, or after this flush sets
       tx_broken, and conn_drop reads it -- ONE guard, so no second one can
       hide its removal (final review F2). */
    c->state = linger && sent ? ST_LINGER : ST_DEAD;
    if (axl_ssh_txq_ready(&c->txq)) {
        if (c->tx_flush != 0) {
            axl_defer_cancel(c->srv->loop, c->tx_flush);
            c->tx_flush = 0;
        }
        /* Its failure is recorded in tx_broken, and conn_drop resets for
           it: nothing to do with the status here. */
        tx_flush(c);
    }
    axl_debug("disconnect: %s", why);
}

static void
disconnect(SshConn *c, AxlSshDisconnectReason reason, const char *why)
{
    disconnect_as(c, reason, why, true);
}

/* ------------------------------------------------------------------------ */
/* Key exchange                                                              */
/* ------------------------------------------------------------------------ */

/* Derive one key from the exchange, per RFC 4253 section 7.2. K must already
   be mpint-encoded -- axl_ssh_kdf hashes exactly what it is given, and the
   RFC hashes K as an mpint. */
static int
derive(const uint8_t *k_mpint, size_t k_len, const uint8_t *h,
       const uint8_t *sid, char letter, uint8_t *out, size_t out_len)
{
    return axl_ssh_kdf(k_mpint, k_len, h, AXL_SSH_HASH_LEN, letter,
                       sid, AXL_SSH_HASH_LEN, out, out_len);
}

/* Ceiling on what `held` may hold, length prefixes included. What lands
   there is connection-layer output produced between our KEXINIT and our
   NEWKEYS: replies to what the peer sent in that window -- a WINDOW_ADJUST,
   a request's SUCCESS -- plus the one packet whose sending started the
   exchange, and whatever a channel's exit sealed behind it. A conforming
   peer sends nothing but kex once its own KEXINIT is out, so that is a few
   hundred bytes. sshd, which keeps processing channel messages there as this
   server does (axl-ssh-rekey.h), queues their replies without a bound; a
   peer that keeps sending requests mid-exchange would grow this until the
   exchange ends, so past the ceiling the connection is dropped instead.
   TXQ_HIGH, the transmit queue's high-water mark; a held flush can briefly
   add up to this much on top of a queue already near it, so about twice
   that is the worst case per connection. */
#define REKEY_HELD_MAX  TXQ_HIGH

static int
held_push(
    SshConn    *c,
    const void *payload,
    size_t      len
)
{
    if (len > REKEY_HELD_MAX - 4u
        || axl_string_len(c->held) > REKEY_HELD_MAX - 4u - len) {
        axl_debug("rekey: held output past %u bytes", (unsigned)REKEY_HELD_MAX);
        return AXL_ERR;
    }
    return (axl_ssh_put_u32(c->held, (uint32_t)len) == AXL_OK
            && axl_string_append_len(c->held, (const char *)payload, len)
                   == AXL_OK)
           ? AXL_OK : AXL_ERR;
}

/* Our NEWKEYS is out: seal every held payload, in the order it was produced,
   under the new key, then scrub the plaintext. The storage is kept -- empty
   -- for the next exchange. */
static int
held_flush(
    SshConn *c  ///< connection whose NEWKEYS has just been sealed
)
{
    if (axl_string_len(c->held) == 0) {
        return AXL_OK;
    }
    const uint8_t *d   = (const uint8_t *)axl_string_data(c->held);
    size_t         n   = axl_string_len(c->held);
    size_t         off = 0;
    unsigned       pkts = 0;
    int            rc   = AXL_OK;
    while (rc == AXL_OK && off < n) {
        uint32_t len = 0;
        rc = axl_ssh_get_u32(d, n, &off, &len);
        if (rc == AXL_OK && len > n - off) {
            rc = AXL_ERR;
        }
        if (rc == AXL_OK) {
            rc = send_packet(c, d + off, len);
            off += len;
            pkts++;
        }
    }
    axl_ssh_secure_zero(axl_string_data(c->held), n);
    axl_string_truncate(c->held, 0);
    axl_debug("rekey: sealed %u held after our NEWKEYS", pkts);
    return rc;
}

/* Our NEWKEYS of a rekey is sealed and the send key switched: output flows
   again. Held packets go first, then the channel flush the exchange paused
   is rescheduled. */
static int
rekey_our_newkeys(
    SshConn *c  ///< connection in ST_SESSION
)
{
    if (axl_ssh_rekey_on_our_newkeys(&c->rekey) != AXL_OK
        || held_flush(c) != AXL_OK) {
        return AXL_ERR;
    }
    /* Unobservable: on_sent already reschedules the flush whenever a send
       completion leaves the queue under TXQ_LOW, and our NEWKEYS is itself
       a send. Kept as the explicit resume point the chan_paused design
       block names. */
    chan_schedule(c);
    return AXL_OK;
}

static int
rekey_send_kexinit(
    SshConn *c
)
{
    /* The state check first, so a refused event leaves i_s -- the last
       exchange's KEXINIT, which nothing may rewrite while it could still be
       hashed -- untouched. Then i_s is replaced: the exchange hash covers
       THIS exchange's two KEXINITs. */
    if (axl_ssh_rekey_on_our_kexinit(&c->rekey) != AXL_OK) {
        return AXL_ERR;
    }
    axl_string_truncate(c->i_s, 0);
    if (axl_ssh_kexinit_build(c->i_s, false) != AXL_OK) {
        return AXL_ERR;
    }
    return send_packet(c, axl_string_data(c->i_s), axl_string_len(c->i_s));
}

/* The peer's KEXINIT on an authenticated connection: a rekey, which sshd
   answers with its own KEXINIT unless one is already out (kex_input_kexinit).
   Its i_c replaces the first exchange's, first_kex_packet_follows is read
   again, and its strict-KEX pseudo-name, valid only in the initial KEXINIT
   (OpenSSH PROTOCOL section 1.10), is ignored: strict KEX was settled then
   and holds for the life of the connection. */
static int
rekey_on_peer_kexinit(
    SshConn       *c,    ///< connection in ST_SESSION, no peer exchange open
    const uint8_t *p,    ///< the KEXINIT
    uint32_t       len   ///< its length
)
{
    AxlSshKexinitPeer peer = { .guessed_wrong = false, .strict_kex = false };
    bool              send_ours = false;

    if (axl_ssh_kexinit_select(p, len, &peer) != AXL_OK
        || axl_ssh_rekey_on_peer_kexinit(&c->rekey, &send_ours) != AXL_OK) {
        return AXL_ERR;
    }
    axl_string_truncate(c->i_c, 0);
    if (axl_string_append_len(c->i_c, (const char *)p, len) != AXL_OK) {
        return AXL_ERR;
    }
    c->drop_next   = peer.guessed_wrong;
    c->kex_replied = false;
    if (!send_ours) {
        return AXL_OK;
    }
    axl_debug("rekey: started by the peer");
    return rekey_send_kexinit(c);
}

/* The server's own trigger (axl_ssh_rekey_due), from send_packet or pump. */
static int
rekey_start(
    SshConn *c  ///< connection in ST_SESSION, no exchange running
)
{
    /* The counts that crossed, for the reader: which direction did it. */
    axl_debug("rekey: started by us, %llu bytes out, %llu in",
              (unsigned long long)c->rekey.bytes_out,
              (unsigned long long)c->rekey.bytes_in);
    c->kex_replied = false;
    return rekey_send_kexinit(c);
}

/* The peer's NEWKEYS of a rekey: its packets from here are under the staged
   key, and the exchange is over. */
static int
rekey_on_peer_newkeys(
    SshConn *c  ///< connection in ST_SESSION
)
{
    if (axl_ssh_rekey_on_peer_newkeys(&c->rekey) != AXL_OK
        || axl_ssh_dirkey_switch(&c->rx_key) != AXL_OK) {
        return AXL_ERR;
    }
    /* Strict KEX rule (b), receive half: pump has already counted this
       packet, so the next one opened is number 0. */
    if (c->strict_kex) {
        c->rx_seq = 0;
    }
    axl_debug("rekey: complete");
    return AXL_OK;
}

static int
do_kex(SshConn *c, const uint8_t *payload, uint32_t len)
{
    AxlSshServer *s = c->srv;

    /* KEX_ECDH_INIT: byte 30 || string Q_C */
    size_t         off = 1;
    const uint8_t *q_c = NULL;
    uint32_t       q_c_len = 0;
    if (len < 1 || payload[0] != AXL_SSH_MSG_KEX_ECDH_INIT
        || axl_ssh_get_string(payload, len, &off, &q_c, &q_c_len) != AXL_OK
        || q_c_len != AXL_SSH_X25519_LEN) {
        return AXL_ERR;
    }

    int      rc    = AXL_ERR;
    AxlEcdh *ecdh  = axl_ecdh_new(AXL_ECDH_X25519);
    AxlString *k_s = axl_string_new("");
    AxlString *sig_blob = axl_string_new("");
    AxlString *k_mp = axl_string_new("");
    AxlString *reply = axl_string_new("");
    if (ecdh == NULL || k_s == NULL || sig_blob == NULL || k_mp == NULL
        || reply == NULL) {
        goto out;
    }

    uint8_t q_s[AXL_SSH_X25519_LEN];
    size_t  q_s_len = sizeof q_s;
    uint8_t secret[AXL_SSH_X25519_LEN];
    size_t  secret_len = sizeof secret;
    if (axl_ecdh_get_public(ecdh, q_s, &q_s_len) != AXL_OK
        || q_s_len != AXL_SSH_X25519_LEN
        || axl_ecdh_compute(ecdh, q_c, q_c_len, secret, &secret_len) != AXL_OK
        || secret_len != AXL_SSH_X25519_LEN) {
        goto out;
    }

    if (axl_ssh_hostkey_blob(k_s, s->host_pub) != AXL_OK
        || axl_ssh_put_mpint(k_mp, secret, secret_len) != AXL_OK) {
        goto out;
    }

    uint8_t h[AXL_SSH_HASH_LEN];
    if (axl_ssh_exchange_hash(h, sizeof h,
                              axl_string_data(c->v_c), AXL_SSH_IDENT,
                              (const uint8_t *)axl_string_data(c->i_c),
                              axl_string_len(c->i_c),
                              (const uint8_t *)axl_string_data(c->i_s),
                              axl_string_len(c->i_s),
                              (const uint8_t *)axl_string_data(k_s),
                              axl_string_len(k_s),
                              q_c, q_c_len, q_s, q_s_len,
                              secret, secret_len) != AXL_OK) {
        goto out;
    }

    /* The exchange hash IS the session id, fixed at the FIRST exchange and
       unchanged by any later rekey (RFC 4253 section 7.2) -- so this is
       guarded rather than unconditional: an unconditional write would
       replace, at every rekey, the session id that every signature was bound
       to. */
    if (!c->have_session_id) {
        axl_memcpy(c->session_id, h, sizeof h);
        c->have_session_id = true;
    }

    uint8_t sig[64];
    size_t  sig_len = sizeof sig;
    if (axl_pk_key_sign(s->host_key, h, sizeof h, AXL_PK_SIG_RAW,
                        sig, &sig_len) != AXL_OK || sig_len != sizeof sig
        || axl_ssh_signature_blob(sig_blob, sig) != AXL_OK) {
        goto out;
    }

    /* Letters D and B are the SERVER-to-client key and IV; C and A are the
       client-to-server pair. Swapping them yields a connection that decrypts
       its own traffic and nothing the peer sends.

       STAGED, both, and neither installed here: each direction switches at
       its own NEWKEYS -- ours just below, once it is sealed under the old
       key; the peer's when it arrives (on_payload). Installing the receive
       key now would open the peer's packets still in flight under the OLD
       key with the new one. */
    const uint8_t *km = (const uint8_t *)axl_string_data(k_mp);
    size_t         kl = axl_string_len(k_mp);
    uint8_t s2c[AXL_SSH_AEAD_KEY_LEN];
    uint8_t c2s[AXL_SSH_AEAD_KEY_LEN];
    int     staged = (derive(km, kl, h, c->session_id, 'D', s2c, sizeof s2c)
                          == AXL_OK
                      && derive(km, kl, h, c->session_id, 'C', c2s, sizeof c2s)
                          == AXL_OK
                      && axl_ssh_dirkey_stage(&c->tx_key, s2c) == AXL_OK
                      && axl_ssh_dirkey_stage(&c->rx_key, c2s) == AXL_OK)
                     ? AXL_OK : AXL_ERR;
    axl_ssh_secure_zero(s2c, sizeof s2c);
    axl_ssh_secure_zero(c2s, sizeof c2s);
    if (staged != AXL_OK) {
        goto out;
    }

    char hdr = (char)AXL_SSH_MSG_KEX_ECDH_REPLY;
    if (axl_string_append_len(reply, &hdr, 1) != AXL_OK
        || axl_ssh_put_string(reply, axl_string_data(k_s),
                              axl_string_len(k_s)) != AXL_OK
        || axl_ssh_put_string(reply, q_s, q_s_len) != AXL_OK
        || axl_ssh_put_string(reply, axl_string_data(sig_blob),
                              axl_string_len(sig_blob)) != AXL_OK
        || send_packet(c, axl_string_data(reply),
                       axl_string_len(reply)) != AXL_OK) {
        goto out;
    }
    c->kex_replied = true;

    /* Our NEWKEYS is sealed under the OLD key (or none, the first time) and
       counted against it; only then does the send side switch, so
       everything we send after it is under the new key. The receive side
       switches only when the PEER's NEWKEYS arrives -- the two directions
       turn on independently and it is a real bug to conflate them. */
    char nk = (char)AXL_SSH_MSG_NEWKEYS;
    if (send_packet(c, &nk, 1) != AXL_OK
        || axl_ssh_dirkey_switch(&c->tx_key) != AXL_OK) {
        goto out;
    }
    c->tx_on = true;
    /* Strict KEX rule (b): reset the sequence number after SENDING NEWKEYS,
       so our first packet under the new key is number 0 -- at EVERY NEWKEYS,
       the rekeys' too, for the life of the connection. The two directions
       reset INDEPENDENTLY -- this is the send half; the receive half is in
       on_payload, when the peer's NEWKEYS arrives.

       This number is the chacha20-poly1305 nonce, which is the whole point:
       without the reset an attacker who deletes packets from the front of
       the encrypted stream leaves the two ends still agreeing on the count,
       so the deletion is invisible (Terrapin, CVE-2023-48795). It is also
       why getting it wrong is not subtle -- announce strict KEX without
       resetting and the client's very first decrypt fails its MAC. */
    if (c->strict_kex) {
        c->tx_seq = 0;
    }
    if (c->state == ST_KEX) {
        c->state = ST_NEWKEYS;
    } else if (rekey_our_newkeys(c) != AXL_OK) {
        goto out;
    }
    rc = AXL_OK;
out:
    axl_ssh_secure_zero(secret, sizeof secret);
    axl_string_free(reply);
    /* k_mp IS the shared secret, mpint-encoded. */
    axl_ssh_string_free_secure(k_mp);
    axl_string_free(sig_blob);
    axl_string_free(k_s);
    axl_ecdh_free(ecdh);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* Authentication                                                            */
/* ------------------------------------------------------------------------ */

/* RFC 4252 section 5.1. The continuation list is what a client uses to decide
   what to try next: naming publickey is what makes ssh(1) offer its keys, and
   naming password is what makes it prompt, rather than give up. P1 sent an
   EMPTY list here, which was correct for a phase that accepted nothing. */
static int
send_userauth_failure(SshConn *c)
{
    AxlString *m = axl_string_new("");
    if (m == NULL) {
        return AXL_ERR;
    }
    char hdr        = (char)AXL_SSH_MSG_USERAUTH_FAILURE;
    char no_partial = 0;
    /* The continuation list names what is actually configured: offering
       password with no hook, or publickey with no key, sends a client down a
       path that cannot succeed. listen() guarantees at least one. */
    AxlSshServer *s  = c->srv;
    bool          pk = s->authkeys.n > 0;
    bool          pw = s->auth_ops.password != NULL;
    const char *methods = (pk && pw) ? "publickey,password"
                          : pw       ? "password"
                                     : "publickey";
    int rc = (axl_string_append_len(m, &hdr, 1) == AXL_OK
              && axl_ssh_put_string(m, methods, axl_strlen(methods)) == AXL_OK
              && axl_string_append_len(m, &no_partial, 1) == AXL_OK)
             ? send_packet(c, axl_string_data(m), axl_string_len(m))
             : AXL_ERR;
    axl_string_free(m);
    return rc;
}

/* RFC 4252 section 7: echo back the algorithm name and key blob the client
   asked about, so it knows WHICH offer we accepted when several are in
   flight. Nothing is authenticated by this message. */
static int
send_pk_ok(SshConn *c, const uint8_t *alg, uint32_t alg_len,
           const uint8_t *blob, uint32_t blob_len)
{
    AxlString *m = axl_string_new("");
    if (m == NULL) {
        return AXL_ERR;
    }
    char hdr = (char)AXL_SSH_MSG_USERAUTH_PK_OK;
    int  rc  = (axl_string_append_len(m, &hdr, 1) == AXL_OK
                && axl_ssh_put_string(m, alg, alg_len) == AXL_OK
                && axl_ssh_put_string(m, blob, blob_len) == AXL_OK)
               ? send_packet(c, axl_string_data(m), axl_string_len(m))
               : AXL_ERR;
    axl_string_free(m);
    return rc;
}

/* RFC 4254 section 4: AxlSsh implements no global request, so every one that
   asked for a reply gets REQUEST_FAILURE -- the same answer sshd gives a
   request it does not recognise either. */
static int
send_request_failure(SshConn *c)
{
    AxlString *m = axl_string_new("");
    if (m == NULL) {
        return AXL_ERR;
    }
    int rc = axl_ssh_request_failure_build(m) == AXL_OK
             ? send_packet(c, axl_string_data(m), axl_string_len(m))
             : AXL_ERR;
    axl_string_free(m);
    return rc;
}

/* RFC 4253 section 11.4: answer a message we do not implement, naming the
   sequence number the peer used so it can match the reply to what it sent.
   Bounded per connection (MAX_UNIMPLEMENTED_REPLIES). Returns AXL_ERR only
   on a real failure -- a message that earns no reply, and one past the cap,
   are both AXL_OK and both leave the connection running. */
static int
send_unimplemented(SshConn *c, uint8_t msg, uint32_t seq, bool post_auth)
{
    if (!axl_ssh_unimplemented_due(msg, post_auth)
        || c->unimpl_replies >= MAX_UNIMPLEMENTED_REPLIES) {
        /* Counted up to the cap and no further: incrementing forever would
           wrap on a peer determined enough to send 2^32 of them, and a
           wrapped counter is a cap that stops capping. */
        return AXL_OK;
    }
    c->unimpl_replies++;

    AxlString *m = axl_string_new("");
    if (m == NULL) {
        return AXL_ERR;
    }
    int rc = axl_ssh_unimplemented_build(m, seq) == AXL_OK
             ? send_packet(c, axl_string_data(m), axl_string_len(m))
             : AXL_ERR;
    axl_string_free(m);
    return rc;
}

/* A message the peer may not send while its exchange is open
   (axl_ssh_rekey_in_allowed), or a method message the exchange does not
   expect. sshd's answers: type 0 is not a message at all and ends the
   connection ("Invalid ssh2 packet type", packet.c); the rest get
   UNIMPLEMENTED and the connection goes on (kex_protocol_error). */
static int
rekey_refuse(
    SshConn  *c,    ///< connection in ST_SESSION
    uint8_t   msg,  ///< the refused message number
    uint32_t  seq   ///< its sequence number
)
{
    if (msg == 0) {
        disconnect(c, AXL_SSH_DISCONNECT_PROTOCOL_ERROR,
                   "invalid message type 0");
        return AXL_ERR;
    }
    axl_debug("rekey: message %u refused mid-exchange", (unsigned)msg);
    return send_unimplemented(c, msg, seq, true);
}

/* RFC 4252 section 5.4: string message (UTF-8), string language tag. */
static int
send_banner(SshConn *c, const char *text)
{
    AxlString *m = axl_string_new("");
    if (m == NULL) {
        return AXL_ERR;
    }
    char hdr = (char)AXL_SSH_MSG_USERAUTH_BANNER;
    int  rc  = (axl_string_append_len(m, &hdr, 1) == AXL_OK
                && axl_ssh_put_string(m, text, axl_strlen(text)) == AXL_OK
                && axl_ssh_put_string(m, "", 0) == AXL_OK)
               ? send_packet(c, axl_string_data(m), axl_string_len(m))
               : AXL_ERR;
    axl_string_free(m);
    return rc;
}

/* Copy a peer-supplied user name into @a out as a NUL-terminated, printable
   ASCII string, truncating at @a out_size - 1 bytes. @a out_size must be at
   least 1.

   Anything outside printable ASCII becomes '.', because the bytes are the
   peer's choice and the sink is a firmware console; that is what a "%.*s"
   conversion cannot do and this helper exists for.

   It also used to be the only SAFE spelling: AxlFormat's %s found the
   argument's length by scanning for a NUL and only THEN applied the
   precision, so pointing it at a slice of a received packet read past the
   payload allocation before printing the bounded prefix. That is fixed --
   the scan is bounded by the precision -- so "%.*s" is no longer an
   out-of-bounds read here. It is still not a substitute, because it emits
   the peer's bytes unaltered. */
static void
copy_user_for_log(char *out, size_t out_size, const uint8_t *user,
                  uint32_t user_len)
{
    size_t n = user_len < out_size - 1 ? user_len : out_size - 1;
    for (size_t i = 0; i < n; i++) {
        out[i] = (user[i] >= 0x20 && user[i] < 0x7f) ? (char)user[i] : '.';
    }
    out[n] = '\0';
}

/* ------------------------------------------------------------------------ */
/* The authentication audit log                                              */
/*                                                                           */
/* sshd's most-consumed output, at its default LogLevel INFO:                */
/*   Accepted publickey for <user> from <address> port 54922 ssh2            */
/*   Failed password for invalid user bob from 10.0.2.2 port 55730 ssh2      */
/* (read off this box's own journal, and out of sshd-session's format string */
/* "%s %s%s%s for %s%.100s from %.200s port %d ssh2%s%s"). AxlSsh logged a    */
/* success at DEBUG and a FAILURE nowhere at any level, so a BMC could not    */
/* answer "who tried to log in and failed" -- the first question anyone asks  */
/* after an incident.                                                        */
/*                                                                           */
/* Shape borrowed, not the wording: AxlSsh has no user database, so it cannot */
/* say "invalid user", and it says WHY it refused instead, which is the thing */
/* an operator with no /var/log actually needs.                              */
/* ------------------------------------------------------------------------ */

/* A source as "A port P", or "? port P" when the address could not be read.
   Never decides anything -- it is a diagnostic, and have_peer is what the
   penalty ladder decides from. Takes the parts rather than a connection so
   on_accept can name an arrival it refuses before any SshConn exists. */
static void
peer_format(bool have_peer, const uint8_t peer_addr[4], uint16_t peer_port,
            char *out, size_t out_size)
{
    char addr[16];

    if (!have_peer || axl_ipv4_format(peer_addr, addr, sizeof addr)
                          != AXL_OK) {
        axl_strlcpy(addr, "?", sizeof addr);
    }
    axl_snprintf(out, out_size, "%s port %u", addr, (unsigned)peer_port);
}

/* This connection's source, as peer_format writes it. */
static void
peer_for_log(const SshConn *c, char *out, size_t out_size)
{
    peer_format(c->have_peer, c->peer_addr, c->peer_port, out, out_size);
}

/* The one way a connection becomes authenticated, for both methods. */
static int
auth_succeed(SshConn *c, const char *method)
{
    char user_log[AUTH_USER_LOG_MAX];
    char peer_log[PEER_LOG_MAX];
    copy_user_for_log(user_log, sizeof user_log,
                      (const uint8_t *)c->auth_user,
                      (uint32_t)c->auth_user_len);
    peer_for_log(c, peer_log, sizeof peer_log);
    /* log-level: an AUDIT record, not a success announcement. The rule
       check-log-levels enforces is that a library must not tell an operator
       at INFO what the caller already learns from its own return -- and the
       application's hook does learn this one. What the hook cannot do is
       leave a record where an operator looks, which on a BMC with no
       /var/log is the firmware console and nowhere else. "Who logged in,
       from where" is the thing an audit trail exists to hold; sshd logs it
       at its default level for exactly that reason, and the matching
       FAILURE line below is useless without it (a log with only failures
       cannot say whether the attack ended in a login).
       NOT rate-limited, unlike the failures: producing one of these requires
       a credential that already works, so it is not the unauthenticated
       amplifier the flood argument is about, and it is the line least
       affordable to lose. */
    axl_info("Accepted %s for %s from %s", method, user_log, peer_log);

    char ok = (char)AXL_SSH_MSG_USERAUTH_SUCCESS;
    if (send_packet(c, &ok, 1) != AXL_OK) {
        return AXL_ERR;
    }
    /* A FLAG, not a zeroed deadline -- see the SshConn field comments. */
    c->authenticated = true;
    /* The idle clocks start here: sshd arms both after authentication
       (serverloop.c server_loop2; auth2.c:441 sets the "user" log preamble
       the idle lines carry at the same point). An unreadable clock leaves
       them unarmed -- the connection is kept, as the reap rules keep one
       they cannot time. */
    uint64_t now      = 0;
    bool     have_now = read_now_ms(&now);
    axl_ssh_idle_start(&c->idle, c->srv->unused_sec, c->srv->alive_sec,
                       c->srv->alive_max, have_now, now);
    if (!have_now) {
        axl_debug("idle: no clock at authentication; idle rules unarmed");
    }
    /* ST_SESSION enforces RFC 4252's "no further userauth after success"
       structurally. */
    c->state = ST_SESSION;
    return AXL_OK;
}

/* rearm_reply_timer names on_reply_timer before its definition, and that
   callback resumes parsing (pump), defined further down the file. (It also
   drops connections; conn_drop's prototype is at "Sending".) */
static bool
on_reply_timer(void *data);
static int
pump(SshConn *c);

/* Arm the server's single reply timer for the earliest owed reply, or for
   NOW when a connection is waiting for its turn to parse and may have it.

   One timer for the server, not one per connection: connections own no loop
   source (P1's MAX_CONNS exists because per-connection sources exhaust the
   loop's table), and a freed connection needs no cancellation -- it is simply
   absent from the list this walks. The same timer carries the resume queue
   for the same reason, and because the two are the same question: what does
   this connection owe before it may read again. */
static void
rearm_reply_timer(AxlSshServer *s)
{
    if (!s->running) {
        return;   /* axl_ssh_server_free is tearing down; arm nothing */
    }
    bool     any      = false;
    uint64_t earliest = 0;
    for (SshConn *c = s->conns; c != NULL; c = c->next) {
        if (c->reply_armed && (!any || c->reply_due_ms < earliest)) {
            earliest = c->reply_due_ms;
            any      = true;
        }
        /* A connection whose turn has come is due at once, like a drop. It
           cannot come while a hook is on the stack: that is what it waits
           for, and app_call_end arms this again when the last one returns. */
        if (c->resume_due && s->hooks == 0) {
            earliest = 0;
            any      = true;
        }
    }
    if (s->reply_timer != 0 && (!any || s->reply_timer_due_ms != earliest)) {
        axl_loop_remove_source(s->loop, s->reply_timer);
        s->reply_timer = 0;
    }
    if (!any || s->reply_timer != 0) {
        return;
    }
    uint64_t now   = 0;
    uint64_t delay = 0;
    if (read_now_ms(&now) && earliest > now) {
        delay = earliest - now;
    }
    /* axl_loop_add_timeout REFUSES a zero delay (returns 0; src/loop/
       axl-loop.c), and zero is the common case here, not an edge: every
       schedule_drop is due at 0, a reply already overdue is due in the past,
       and an unreadable clock leaves the delay at 0. Unclamped, none of those
       would arm -- the failure reply would never be sent and the drop never
       done, and a drop owed to an ACCEPT whose resume failed would leave an
       authenticated connection, which the reaper exempts, holding its slot
       for ever. The whole cost of the clamp is one millisecond. */
    if (delay == 0) {
        delay = 1;
    }
    if (delay > UINT32_MAX) {
        delay = UINT32_MAX;
    }
    s->reply_timer = axl_loop_add_timeout(s->loop, (uint32_t)delay,
                                          on_reply_timer, s);
    s->reply_timer_due_ms = earliest;
    if (s->reply_timer == 0) {
        /* Not delivered early to compensate. on_reap retries the arm on its
           next tick; until then an owed reply waits, and the login grace
           still ends an unauthenticated connection. */
        axl_error("ssh: cannot arm the authentication reply timer");
    }
}

/* ------------------------------------------------------------------------ */
/* Application calls on the stack, and the resume queue                      */
/* ------------------------------------------------------------------------ */

/* The rest of axl_ssh_server_free, once nothing of ours is on the stack. */
static void
server_finish(AxlSshServer *s);

static void
on_free_due(AxlSshServer *s)
{
    s->free_scheduled = false;
    if (s->app_calls > 0) {
        /* Application code again -- a release_principal from the frames that
           were still unwinding when this was queued. Its return schedules
           this afresh. */
        return;
    }
    server_finish(s);
}

static void
free_due_defer_cb(void *data)
{
    on_free_due((AxlSshServer *)data);
}

static bool
free_due_timer_cb(void *data)
{
    on_free_due((AxlSshServer *)data);
    return AXL_SOURCE_REMOVE;
}

/* Ask the loop to finish a free that had to stop short.

   The defer ring first, then a one-shot timeout: two different pools, the
   shape tcp_send_retire (src/net/axl-tcp-async.c) uses for the same reason.
   The ring is the better first choice here because it is NOT the source
   table, which is the resource a peer can pressure. 1 ms because the loop
   refuses a zero delay. */
static void
schedule_server_free(AxlSshServer *s)
{
    if (s->free_scheduled) {
        return;
    }
    if (axl_defer(s->loop, free_due_defer_cb, s) != 0
        || axl_loop_add_timeout(s->loop, 1, free_due_timer_cb, s) != 0) {
        s->free_scheduled = true;
        return;
    }
    /* Both full. The server stays STOPPED -- not listening, no connections,
       keys zeroised -- but THIS STRUCT IS NOW LEAKED, not merely delayed.
       schedule_server_free is only ever reached from app_call_end at the
       instant app_calls has JUST counted down to 0 (P3 security re-review
       NEW-5), so the only application call left that could retry this is a
       release_principal for the one connection drop_all had to defer, and
       only if that connection actually held a principal. In the common
       case it does not, and nothing calls back here again. Leaking is still
       the right side to fail on: freeing the struct now would pull it out
       from under the very frames this exists to wait for. */
    axl_error("ssh: cannot schedule the rest of the free (defer queue and"
              " source table both full); the server stays stopped");
}

/* Every call into application code -- the two authentication hooks and both
   release_principal calls -- is bracketed by these two. The count answers the
   question the server could not ask before: is any frame of MINE on the stack
   beneath the frame running now?

   It can be, because a hook and release_principal may run the loop
   (AxlSshAuthOps), and everything that dispatch runs -- another connection's
   receive, an unrelated service's handler, a completion -- runs ABOVE frames
   of ours that still hold `s` and a connection. Two things turn on it.

   FREEING (P3 security review MED-1). With app_calls > 0 axl_ssh_server_free
   stops the server at once but does not free it; the free finishes from a
   loop callback once the count is back to 0, which is exactly the moment no
   frame of ours is on the stack. Before that, free spun for ever on the
   connection conn_drop had to defer, and freeing anyway was a use-after-free
   in every frame under the hook -- including an indirect call through a freed
   s->auth_ops.

   NESTING (MED-3). `hooks` counts the subset that can start another hook by
   parsing input, and pump refuses to parse authentication input while it is
   non-zero: one hook at a time server-wide, everything else queued in arrival
   order. release_principal is deliberately NOT in that subset. It parses
   nothing, so it cannot start a hook by itself; it cannot be made to wait
   either, since its connection is being freed and the principal is owed now;
   and a hook the loop runs above one is still the only hook on the stack, so
   the bound on nested PARSE frames -- the ~1.9 KB level MED-3 measured -- is
   the same either way. */
static void
app_call_begin(
    AxlSshServer *s,        ///< server whose hook or release_principal is next
    bool          is_hook,  ///< a password/publickey hook, not release_principal
    SshConn      *hook_conn ///< whose hook it is; NULL when @a is_hook is false
)
{
    s->app_calls++;
    if (is_hook) {
        s->hooks++;
        /* The watchdog's state, LATCHED (NEW-1). Read once here rather than
           at report time so a change to the budget cannot reach a hook that
           is already running -- including a change the hook itself makes. */
        s->hook_conn         = hook_conn;
        s->hook_is_password  = hook_conn != NULL && hook_conn->auth_is_password;
        s->hook_budget_ms    = s->auth_hook_budget_ms;
        s->hook_start_armed  = read_now_ms(&s->hook_start_ms);
        s->hook_reported     = false;
    }
}

/* Has the hook on the stack now held the one-hook slot for at least the
   budget it latched, and for how long?

   A pure test over values the callers already have, so on_reap does not read
   the clock a second time. False when reporting is off (a budget of 0),
   when either clock reading failed -- an unmeasured hold is not a reported
   one -- or when the readings run backwards, which a monotonic clock should
   not do and which would otherwise underflow into a colossal duration.
   `>=`, not `>`: a budget of 1000 ms means a hold of exactly 1000 ms is
   over it. */
static bool
hook_over_budget(
    const AxlSshServer *s,        ///< server with a hook on its stack
    bool                have_now, ///< the caller's clock reading succeeded
    uint64_t            now,      ///< ...and this is it, in monotonic ms
    uint64_t           *held_ms   ///< receives the hold; untouched on false
)
{
    if (s->hooks == 0 || s->hook_budget_ms == 0 || !s->hook_start_armed
        || s->hook_conn == NULL || !have_now || now < s->hook_start_ms) {
        return false;
    }
    *held_ms = now - s->hook_start_ms;
    return *held_ms >= (uint64_t)s->hook_budget_ms;
}

/* Write the watchdog's line for the hook on the stack: the STILL-RUNNING one
   when @a stalled, the one for a hook that has RETURNED otherwise. Defined
   down with the other audit lines; declared here because app_call_end is
   where a hook ends. */
static void
hook_report(AxlSshServer *s, bool stalled, uint64_t held_ms);

static void
app_call_end(
    AxlSshServer *s,       ///< the same server
    bool          is_hook  ///< the same answer as the matching begin
)
{
    if (is_hook) {
        /* Every app_call_begin(s, true) is paired with exactly one of these.
           A mismatch must never underflow s->hooks: that would pin it above
           zero for the rest of the server's life and auth_may_parse would
           refuse every authentication from then on, silently. */
        AXL_DEBUG_ASSERT_MSG(s->hooks > 0, "app_call_end: hooks underflow");
        if (s->hooks > 0) {
            /* BEFORE the decrement and before a single turn is handed out:
               the depth this line reports is what was waiting ON THIS HOOK,
               and rearm_reply_timer below starts giving those turns back.
               This report ALWAYS lands when the hold is over budget -- it is
               the mid-hook one that is best effort. */
            uint64_t now  = 0;
            uint64_t held = 0;
            /* The reading on its own line: `hook_over_budget(s,
               read_now_ms(&now), now, ...)` leaves the argument that READS
               `now` unsequenced against the call that WRITES it, and gcc
               evaluated it first -- so every hold was measured against 0
               and no slow-return line was ever written. The integration
               assertion caught it; nothing in the compiler did. */
            bool have_now = read_now_ms(&now);
            if (hook_over_budget(s, have_now, now, &held)) {
                hook_report(s, false, held);
            }
            s->hooks--;
            if (s->hooks == 0) {
                /* The same branch that decrements, deliberately: conn_drop
                   DEFERRED the drop of this connection if anything asked for
                   it while the hook ran, and the frame beneath issue_request
                   frees it as soon as the hook returns. A pointer left here
                   would be dereferenced by the next reap sweep. */
                s->hook_conn        = NULL;
                s->hook_start_armed = false;
                s->hook_reported    = false;
                /* Every connection that stopped before decrypting while
                   this hook ran is owed its turn. The timer is what hands
                   them out, one at a time. */
                rearm_reply_timer(s);
            }
        }
    }
    AXL_DEBUG_ASSERT_MSG(s->app_calls > 0, "app_call_end: app_calls underflow");
    if (s->app_calls > 0) {
        s->app_calls--;
        if (s->app_calls == 0 && s->free_pending) {
            schedule_server_free(s);
        }
    }
}

/* The connection that has waited longest for its turn to parse, or NULL if
   none waits.

   By join order, NOT list order: on_accept pushes each new connection at the
   HEAD, so the list is newest-first and serving it in order would answer the
   most recent arrival first -- which is the LIFO pinning MED-3 is about, one
   level down. */
static SshConn *
queue_head(AxlSshServer *s)
{
    SshConn *head = NULL;

    for (SshConn *c = s->conns; c != NULL; c = c->next) {
        if (c->resume_due
            && (head == NULL || c->resume_seq < head->resume_seq)) {
            head = c;
        }
    }
    return head;
}

/* How many connections are waiting their turn to parse right now.

   What the hook watchdog's lines carry, because the queue depth is what
   tells an operator the SERVER-WIDE cost of a slow hook rather than the
   local one. A walk rather than a counter: the list is at most MAX_CONNS
   long, and a counter would be a second truth to keep in step with
   resume_due through every join, every turn handed out and every drop. */
static unsigned
queue_depth(const AxlSshServer *s)
{
    unsigned n = 0;

    for (const SshConn *c = s->conns; c != NULL; c = c->next) {
        if (c->resume_due) {
            n++;
        }
    }
    return n;
}

/* Unauthenticated connections held now, in any state -- a walk, like
   queue_depth, so no counter has to be kept in step with every exit path. */
static unsigned
preauth_count(const AxlSshServer *s)
{
    unsigned n = 0;

    for (const SshConn *c = s->conns; c != NULL; c = c->next) {
        if (!c->authenticated && !c->lingering) {
            n++;
        }
    }
    return n;
}

/* The unauthenticated connection admission evicts, or NULL: of those that
   have not finished key exchange, the one LEAST FAR ALONG in it (ST_IDENT,
   then ST_KEXINIT, ST_KEX, ST_NEWKEYS -- SshState's own order), and the
   oldest among equals. ST_AUTH and later are never victims: a peer there has
   done a real key exchange, and a user typing a password sits there. A
   drop_deferred connection is ST_DEAD and already on its way out.

   WHY PROGRESS FIRST. Age alone gives a real client a clock: it survives
   about max_startups further arrivals, so a flood of R connections a second
   evicts it unless its key exchange finishes within roughly max_startups / R.
   A client that has sent its KEXINIT -- one round trip after connecting, for
   ssh(1) -- now outranks every peer that has said nothing, however many
   arrive; a flood has to do the same work to compete with it, and at equal
   progress age decides as before. */
static SshConn *
eviction_victim(const AxlSshServer *s)
{
    SshConn *victim = NULL;

    for (SshConn *c = s->conns; c != NULL; c = c->next) {
        bool in_kex = c->state == ST_IDENT || c->state == ST_KEXINIT
                      || c->state == ST_KEX || c->state == ST_NEWKEYS;
        if (c->authenticated || !in_kex) {
            continue;
        }
        if (victim == NULL || c->state < victim->state
            || (c->state == victim->state
                && c->accept_seq < victim->accept_seq)) {
            victim = c;
        }
    }
    return victim;
}

/* Put @a c at the back of the queue and make sure the timer that serves it is
   armed. A connection already waiting keeps its place. */
static void
queue_join(SshConn *c)
{
    if (!c->resume_due) {
        c->resume_due = true;
        c->resume_seq = ++c->srv->resume_last;
        /* Inside the fresh-join guard, so this is the start of the CURRENT
           wait and a connection already waiting keeps its own -- the same
           rule resume_seq follows one line up. */
        c->queued_armed = read_now_ms(&c->queued_ms);
    }
    rearm_reply_timer(c->srv);
}

/* May @a c read authentication input now? When not, it joins the queue and
   pump stops BEFORE decrypting -- the same place, and for the same reason, as
   the pending-verdict gate above it: input a connection has not read is input
   it has not committed to answering, and it costs nothing but the peer's own
   receive window (RX_BACKLOG_MAX bounds that).

   Two things make a connection wait, and both are the one-hook rule
   (P3 security review MED-3):
     - a hook is on the stack anywhere in the server, or
     - somebody has been waiting longer. That second test is what keeps the
       queue FIFO through the millisecond between a hook returning and the
       reply timer firing; without it a peer that times its packets to land
       in that window walks in front of a connection that has waited for
       every hook so far.
   on_reply_timer gives out the turn (resume_turn) after taking the connection
   off the queue, so the turn-holder passes here once and the queue cannot
   hand out the same place twice. */
static bool
auth_may_parse(SshConn *c)
{
    AxlSshServer *s = c->srv;

    if (c->state == ST_AUTH
        && (s->hooks > 0 || (!c->resume_turn && queue_head(s) != NULL))) {
        queue_join(c);
        return false;
    }
    /* Parsing is what waiting was for. */
    c->resume_due = false;
    return true;
}

/* End a connection the server cannot go on serving: tell the peer, then drop
   it -- deferred, if its own hook is on the stack (conn_drop). */
static void
drop_failed(SshConn *c)
{
    disconnect(c, AXL_SSH_DISCONNECT_PROTOCOL_ERROR, "protocol error");
    conn_drop(c, AXL_TEARDOWN_RESET);
}

/* Called where freeing the connection would pull it out from under a caller:
   axl_ssh_auth_complete can run from any hook, including release_principal
   during another connection's teardown. The reply timer's sweep drops it. */
static void
schedule_drop(SshConn *c)
{
    c->drop_due     = true;
    c->reply_armed  = true;
    c->reply_due_ms = 0;
    rearm_reply_timer(c->srv);
}

/* Longest audit line log_audit is handed. The user-change line is the
   longest by construction -- it carries THREE user names (the old one twice,
   the new one once) where every other line carries at most one -- so it
   sets the size, derived from its components in axl-ssh-auth.h. Checked
   here rather than trusted: the timeout line's fixed format, and a refusal
   line at the longest event-plus-reason this file passes (the 40 characters
   of "maximum authentication attempts exceeded"; "Failed " plus the longest
   reason is 42), both fit. axl_snprintf truncates rather than overruns, so
   an under-sized buffer would lose the line's tail, never the bound. */
#define AUDIT_LINE_MAX  AXL_SSH_USER_CHANGE_LINE_MAX
/* The timeout line's bound is #AXL_SSH_TIMEOUT_LINE_MAX rather than this
   file's own arithmetic over a bare format, because that line now carries a
   REASON (NEW-1) and the longest variant is the one that has to fit. A bound
   still written against the old reasonless format would be a check that had
   stopped checking. */
_Static_assert(AUDIT_LINE_MAX >= AXL_SSH_TIMEOUT_LINE_MAX,
               "the timeout audit line, reason included, must fit AUDIT_LINE_MAX");
_Static_assert(AUDIT_LINE_MAX >= AXL_SSH_HOOK_LINE_MAX,
               "a hook watchdog audit line must fit AUDIT_LINE_MAX");
_Static_assert(AUDIT_LINE_MAX
                   >= sizeof "Failed authentication (method not offered)"
                      + sizeof " for " + sizeof " from " + AUTH_USER_LOG_MAX
                      + PEER_LOG_MAX,
               "a refusal audit line must fit AUDIT_LINE_MAX");
_Static_assert(AUDIT_LINE_MAX
                   >= sizeof "terminating inactive connection"
                      + sizeof " from user " + AUTH_USER_LOG_MAX
                      + PEER_LOG_MAX,
               "an idle audit line must fit AUDIT_LINE_MAX");
_Static_assert(AUDIT_LINE_MAX
                   >= sizeof "Evicted connection from "
                      + sizeof ": past MaxStartups, key exchange unfinished"
                      + PEER_LOG_MAX,
               "an admission audit line must fit AUDIT_LINE_MAX");

/* Write one audit line, bounded by the window for its @a kind: at INFO when
   the window says so -- carrying the count of what the window held back
   since the last one, if any -- and at DEBUG otherwise. @a line is built by
   the callers below from this file's own words, sanitised peer bytes
   (copy_user_for_log) and the peer's address; nothing else reaches it. */
static void
log_audit(AxlSshServer *s, AuthLogKind kind, const char *line)
{
    uint64_t now        = 0;
    bool     have_now   = read_now_ms(&now);
    uint32_t suppressed = 0;

    if (!axl_ssh_auth_log_due(&s->auth_log[kind], now, have_now,
                              &suppressed)) {
        axl_debug("%s", line);
        return;
    }
    if (suppressed > 0) {
        /* log-level: an AUDIT record, and a count of how many of the same
           kind the rate limit has held back since the last one -- a
           bounded log that silently drops entries is a log an operator
           would read as calm. Nothing else reports either fact: the
           application never learns of most of these events (a hook's own
           DENY is the exception), and the count exists nowhere but this
           line. */
        axl_info("%s (and %u more since the previous line, at debug level"
                 " only)", line, (unsigned)suppressed);
        return;
    }
    /* log-level: an AUDIT record, the counterpart of auth_succeed's. This is
       the one a library cannot argue its caller already knows: a refused
       user name, an unknown method, an unauthorized key, a forged signature,
       an over-long or empty password, a connection out of attempts, a login
       grace that ran out -- every one refused or ended by the server itself,
       before or without any hook, so the application never hears about it
       at all. (A hook's own refusal is logged here as well; that one the
       application does know of, and it is logged alongside the rest because
       an audit trail missing the ordinary wrong password is not one.)
       TWO KINDS ARE NOT REFUSALS AND NOT NEWS TO NOBODY: the hook
       watchdog's (NEW-1) report how long the APPLICATION'S OWN hook held
       the server. The application knows how long its hook ran; what it
       cannot know from inside one call is that the hold is server-wide and
       how many other logins were queued behind it, which is the fact this
       line exists to carry -- and an operator reading the audit log is who
       needs it, not the hook.
       Rate-limited to one line per AXL_SSH_AUTH_LOG_WINDOW_MS per kind
       across the server, the same shape the penalty path's table-full
       notice uses, so a peer cannot turn its own failures into a
       firmware-console flood; every suppressed line is still emitted at
       DEBUG, and the next INFO line of its kind says how many there were.
       This function only LOGS: its callers are where each refusal was
       decided. */
    axl_info("%s", line);
}

/* A refused authentication, as "<event><why> for <user> from <A> port <P>".
   @a event and @a why are always this file's own literals -- never peer
   bytes, which only reach the log through copy_user_for_log. */
static void
log_auth_refusal(SshConn *c, AuthLogKind kind, const char *event,
                 const char *why)
{
    char user_log[AUTH_USER_LOG_MAX];
    char peer_log[PEER_LOG_MAX];
    char line[AUDIT_LINE_MAX];

    copy_user_for_log(user_log, sizeof user_log,
                      (const uint8_t *)c->auth_user,
                      (uint32_t)c->auth_user_len);
    peer_for_log(c, peer_log, sizeof peer_log);
    axl_snprintf(line, sizeof line, "%s%s for %s from %s", event, why,
                 user_log, peer_log);
    log_audit(c->srv, kind, line);
}

/* An ordinary refused attempt: sshd's "Failed <method>" line, with AxlSsh's
   reason in parentheses. */
static void
log_auth_failure(SshConn *c, const char *why)
{
    log_auth_refusal(c, AUTH_LOG_REFUSED, "Failed ", why);
}

/* The connection is being ended for running out of attempts -- the password
   cap, or MAX_AUTH_REQUESTS. sshd's words for the same event, its
   auth_maxtries_exceeded() line: "maximum authentication attempts exceeded
   for %s%.100s from %.200s port %d ssh2" (/usr/libexec/openssh/sshd-session,
   OpenSSH 9.9p1), which it logs at ERROR. */
static void
log_auth_exceeded(SshConn *c)
{
    log_auth_refusal(c, AUTH_LOG_EXCEEDED,
                     "maximum authentication attempts exceeded", "");
}

/* An admission decision past max_startups (spec section 12a.2), as
   "<event> from <A> port <P><why>". The source is passed as parts because a
   REFUSED arrival never becomes a connection. @a event and @a why are this
   file's own literals. After sshd's "drop connection #N from [A]:P on
   [L]:P past MaxStartups", in this log's own shape. */
static void
log_admission(AxlSshServer *s, AuthLogKind kind, const char *event,
              bool have_peer, const uint8_t peer_addr[4], uint16_t peer_port,
              const char *why)
{
    char peer_log[PEER_LOG_MAX];
    char line[AUDIT_LINE_MAX];

    peer_format(have_peer, peer_addr, peer_port, peer_log, sizeof peer_log);
    axl_snprintf(line, sizeof line, "%s from %s%s", event, peer_log, why);
    log_audit(s, kind, line);
}

/* The login grace ran out. sshd 9.9p1's listener logs this at INFO as
   "Timeout before authentication for connection from %s to %s, pid = %ld"
   (/usr/sbin/sshd; measured against a throwaway sshd with LoginGraceTime 3:
   "Timeout before authentication for connection from 127.0.0.1 to
   127.0.0.1, pid = 1089169"). AxlSsh has no pid and names the source port,
   as its other audit lines do. The cheapest event there is -- a connection
   that sends nothing -- so it has a window of its own like every kind.

   ...AND WHY, when the connection was still waiting its turn to parse
   (NEW-1). That is the one state where the reap is arguably the SERVER'S
   doing rather than the peer's, and it was the state the line said nothing
   about. `resume_due` as the reaper found it is exactly it, and s->hooks
   separates the two reasons auth_may_parse has for queueing: a hook on the
   stack somewhere, or somebody who has been waiting longer. Neither is an
   "ever waited" bit, and that is deliberate -- a connection that waited,
   got its turn, was refused and then sat idle has such a bit set, and the
   line would blame the server for a peer's silence.

   A connection that was NOT queued gets the line it always got, to the
   byte. A DIFFERENT KIND when it was: the two must not share a window (see
   AuthLogKind). */
static void
log_grace_expired(SshConn *c, bool have_now, uint64_t now)
{
    AxlSshServer *s = c->srv;
    char peer_log[PEER_LOG_MAX];
    char line[AUDIT_LINE_MAX];
    /* No duration when either reading failed, rather than a made-up one:
       the same rule axl_ssh_reap_reason follows about an unreadable clock. */
    bool     was_queued  = c->resume_due;
    bool     have_waited = was_queued && c->queued_armed && have_now
                           && now >= c->queued_ms;
    uint64_t waited_ms   = have_waited ? now - c->queued_ms : 0;

    peer_for_log(c, peer_log, sizeof peer_log);
    axl_ssh_format_timeout(line, sizeof line, peer_log, was_queued,
                           s->hooks > 0, have_waited, waited_ms);
    log_audit(s, was_queued ? AUTH_LOG_TIMEOUT_QUEUED : AUTH_LOG_TIMEOUT,
              line);
}

/* An authenticated connection ended by the idle rules, in sshd's words with
   sshd's "user" preamble (auth2.c:441 sets it at success, packet.c:1976
   prints it): "<event> from user USER A.B.C.D port N". @a event is one of
   this file's two literals; USER goes through copy_user_for_log. */
static void
log_idle_drop(SshConn *c, AuthLogKind kind, const char *event)
{
    char user_log[AUTH_USER_LOG_MAX];
    char peer_log[PEER_LOG_MAX];
    char line[AUDIT_LINE_MAX];

    copy_user_for_log(user_log, sizeof user_log,
                      (const uint8_t *)c->auth_user,
                      (uint32_t)c->auth_user_len);
    peer_for_log(c, peer_log, sizeof peer_log);
    axl_snprintf(line, sizeof line, "%s from user %s %s", event, user_log,
                 peer_log);
    log_audit(c->srv, kind, line);
}

/* The hook watchdog's line, declared up beside app_call_end. Here because it
   needs log_audit and the two sanitisers above it.

   IT MAY NAME A CONNECTION THE PEER HAS ALREADY LEFT, and that is correct,
   not a bug to fix later: conn_drop marks a connection whose own hook is
   running ST_DEAD and defers the drop, so the mid-hook report can describe
   a dead connection. The hook still holds the server and the stall is still
   real -- the line is about the HOLD, and the connection is how an operator
   identifies which hook. */
static void
hook_report(AxlSshServer *s, bool stalled, uint64_t held_ms)
{
    SshConn *c = s->hook_conn;
    char user_log[AUTH_USER_LOG_MAX];
    char peer_log[PEER_LOG_MAX];
    char line[AUDIT_LINE_MAX];

    if (c == NULL) {
        return;   /* hook_over_budget refuses this; belt and braces */
    }
    copy_user_for_log(user_log, sizeof user_log,
                      (const uint8_t *)c->auth_user,
                      (uint32_t)c->auth_user_len);
    peer_for_log(c, peer_log, sizeof peer_log);
    axl_ssh_format_hook_hold(line, sizeof line, stalled, s->hook_is_password,
                             held_ms, queue_depth(s), user_log, peer_log);
    log_audit(s, stalled ? AUTH_LOG_HOOK_STALLED : AUTH_LOG_HOOK_SLOW, line);
}

/* A request named a user other than the one this connection has been
   authenticating as. sshd's line for the same refusal, measured against a
   throwaway sshd (OpenSSH 9.9p1) with a client that asked as one user and
   then another:
     Disconnecting invalid user alice 127.0.0.1 port 36658: Change of
     username or service not allowed: (alice,ssh-connection) ->
     (bob,ssh-connection) [preauth]
   AxlSsh has no user database, so it is always sshd's "authenticating user"
   branch, and there is no privilege-separated child to mark [preauth]. The
   OLD name appears twice and the NEW one once, as in sshd's; all are the
   peer's bytes and go through copy_user_for_log. The service is always
   ssh-connection: any other was refused before this is reached. The format
   and its buffer size are axl-ssh-auth.h's, so the unit test pins the
   worst case. */
static void
log_user_change(SshConn *c, const uint8_t *user, uint32_t user_len)
{
    char was[AUTH_USER_LOG_MAX];
    char now[AUTH_USER_LOG_MAX];
    char peer_log[PEER_LOG_MAX];
    char line[AUDIT_LINE_MAX];

    copy_user_for_log(was, sizeof was, (const uint8_t *)c->auth_user,
                      (uint32_t)c->auth_user_len);
    copy_user_for_log(now, sizeof now, user, user_len);
    peer_for_log(c, peer_log, sizeof peer_log);
    axl_ssh_format_user_change(line, sizeof line, was, now, peer_log);
    log_audit(c->srv, AUTH_LOG_USER_CHANGE, line);
}

/* Send an owed failure: the banner if any, then FAILURE -- or, at the
   password cap, a disconnect instead. Returns AXL_ERR to close. */
static int
send_due_reply(SshConn *c)
{
    if (c->auth_banner != NULL) {
        int brc = send_banner(c, c->auth_banner);
        axl_free(c->auth_banner);
        c->auth_banner = NULL;
        if (brc != AXL_OK) {
            return AXL_ERR;
        }
    }
    if (c->password_failures >= c->max_password_failures) {
        /* After the "Failed password" line schedule_failure already logged
           (or counted) for this attempt, as sshd logs its MaxAuthTries line
           after the failure that tripped it. */
        log_auth_exceeded(c);
        disconnect(c, AXL_SSH_DISCONNECT_NO_MORE_AUTH_METHODS,
                   "Too many authentication failures");
        return AXL_ERR;
    }
    return send_userauth_failure(c);
}

/* A failure is owed. Reply no earlier than the per-user minimum measured from
   the request's arrival (spec section 9a); the connection reads nothing until
   then. @a why names the refusal for the audit log. Returns AXL_ERR to
   close. */
static int
schedule_failure(SshConn *c, const char *why)
{
    AxlSshServer *s      = c->srv;
    uint64_t      now_us = 0;

    /* Rule 3 of the penalty ladder (axl_ssh_server_set_penalties). Every
       refused real attempt the peer is owed a REPLY for comes through here
       -- a DENY from either hook, an unauthorized key, an unknown method, a
       refused user name -- and the `none` probe, answered by
       send_userauth_failure directly, does not. Neither does a forged
       signature: it is refused with no reply at all, sets pen_forged rather
       than pen_refused, and writes its own audit line (on_userauth). So
       this is where the audit log records the refusals that are answered,
       NOT the one place every refusal passes -- anything every refused
       attempt must do has to be done at the forgery exit as well. The two
       records that END a connection, a forgery and a connection out of
       attempts (log_auth_exceeded), are written where they happen, each
       against a limiter of its own. */
    c->pen_refused = true;
    log_auth_failure(c, why);

    if (!c->auth_have_start || !read_now_us(&now_us)) {
        /* No clock, so no minimum can be measured. Reply now rather than
           strand the client; only this reply loses its timing. */
        return send_due_reply(c);
    }
    uint64_t elapsed = now_us >= c->auth_start_us
                       ? now_us - c->auth_start_us : 0;
    uint32_t target  = axl_ssh_fail_delay_target_us(
                           s->timing_secret, sizeof s->timing_secret,
                           c->auth_user, c->auth_user_len);
    uint64_t wait_us = axl_ssh_fail_reply_delay_us(elapsed, target);

    /* Rounded UP as one sum. The sweep releases a reply once the millisecond
       clock reaches this value, so due * 1000 must not fall short of
       now_us + wait_us. Flooring now_us and rounding wait_us separately can
       fall short by up to 999 us -- a reply before its minimum -- and by an
       amount that moves with elapsed's sub-millisecond phase, which is the
       very quantity the minimum exists to hide. */
    c->reply_due_ms = (now_us + wait_us + 999u) / 1000u;
    c->reply_armed  = true;
    rearm_reply_timer(s);
    return AXL_OK;
}

/* Act on a recorded verdict. Returns AXL_ERR to close.

   Fails CLOSED on a verdict nobody recorded: AXL_SSH_AUTH_ACCEPT is 0 and the
   connection starts zeroed, so acting on an unrecorded auth_verdict would
   authenticate. Both callers check first today; this makes that structural
   rather than a property of the callers. */
static int
auth_apply(SshConn *c)
{
    if (!c->auth_have_verdict) {
        return AXL_ERR;
    }
    AxlSshAuthVerdict v = c->auth_verdict;

    c->auth_have_verdict = false;
    c->auth_req          = 0;

    if (v == AXL_SSH_AUTH_ACCEPT) {
        /* Adopt FIRST, so conn_free releases it on every later path. */
        c->principal      = c->auth_principal;
        c->auth_principal = NULL;
        if (c->auth_banner != NULL) {
            int brc = send_banner(c, c->auth_banner);
            axl_free(c->auth_banner);
            c->auth_banner = NULL;
            if (brc != AXL_OK) {
                return AXL_ERR;
            }
        }
        /* "publickey", not "public key": the audit line this feeds is read
           alongside sshd's, which spells the method as RFC 4252 names it. */
        return auth_succeed(c, c->auth_is_password ? "password"
                                                   : "publickey");
    }
    /* DENY and CHANGE_FIRST are both failures. Only a password counts
       toward the cap: a refused publickey principal is not a guess. */
    if (c->auth_is_password) {
        c->password_failures++;
    }
    /* The application's own verdict, so the log names the METHOD rather
       than a reason this file chose: only the hook knows why. */
    return schedule_failure(c, c->auth_is_password
                               ? (v == AXL_SSH_AUTH_CHANGE_FIRST
                                  ? "password (change required)" : "password")
                               : "publickey");
}

/* The receive callback, defined with the connection lifecycle; rx_unpark
   arms it again. */
static bool
on_recv(AxlTcp *sock, AxlStatus st, void *data);

/* Re-arm a receive that on_recv left unarmed (rx_parked), once neither
   reason to park holds: output backpressure (tx_paused) and a channel hook
   of this connection on the stack (chan_hooks, the parse gate). The ONE
   re-arm for both; only resume() calls it. Never from inside on_recv: a
   parked receive means no on_recv of this connection is running. */
static int
rx_unpark(
    SshConn *c  ///< connection, not ST_DEAD
)
{
    if (!c->rx_parked || c->tx_paused || c->chan_hooks > 0) {
        return AXL_OK;
    }
    c->rx_parked = false;
    return axl_tcp_recv_async(c->sock, c->rx, sizeof c->rx, c->srv->loop,
                              NULL, on_recv, c) == AXL_OK
           ? AXL_OK : AXL_ERR;
}

/* The reply for one request is out: parse what the client pipelined behind
   it. Returns AXL_ERR to close, like pump -- including when the server is
   being freed, which pump checks: a completion reached from
   axl_ssh_server_free's release_principal must not hand the application a
   new request. It re-arms a parked receive (rx_unpark) before the parse and
   again after it.

   Why after as well: the receive re-armed first can complete INSIDE a hook
   this pump runs -- a hook may dispatch the loop -- and the gate parks it
   there. That hook's leave queues the connection for a turn, but this pump
   is still on the stack beneath it: it goes on parsing what the nested
   receive appended, and auth_may_parse clears resume_due as it does, so the
   queued turn is consumed here and never reaches on_reply_timer. Nothing
   else would re-arm the receive, and the connection would never read
   again. rx_unpark does nothing unless the receive is parked with neither
   gate holding it, so the second call is free when nothing nested. */
static int
resume(SshConn *c)
{
    if (conn_over(c) || rx_unpark(c) != AXL_OK || pump(c) != AXL_OK) {
        return AXL_ERR;
    }
    return conn_over(c) ? AXL_ERR : rx_unpark(c);
}

/* Send every reply that is due, drop every connection marked for it, and
   then give out turns to parse -- the queue auth_may_parse and
   axl_ssh_auth_complete put connections on.

   Rescans from the head for each connection it handles and holds no next
   pointer: sending a reply resumes parsing, parsing can call a hook, and a
   hook can complete -- or by running the loop, or through
   release_principal, drop -- another connection. reply_armed is cleared
   before anything else, so a connection is handled once per due reply, and a
   drop conn_drop has to defer (its hook is on the stack further down) is not
   picked again. */
static bool
on_reply_timer(void *data)
{
    AxlSshServer *s        = (AxlSshServer *)data;
    uint64_t      now      = 0;
    bool          have_now = read_now_ms(&now);

    /* One-shot: returning AXL_SOURCE_REMOVE retires this source once the
       callback returns. Forget its id NOW, so a rearm below -- nested or
       final -- adds a fresh timeout instead of removing the one running. */
    s->reply_timer = 0;
    for (;;) {
        SshConn *c = s->conns;
        while (c != NULL
               && !(c->reply_armed
                    && (c->drop_due || !have_now || now >= c->reply_due_ms))) {
            c = c->next;
        }
        if (c != NULL) {
            c->reply_armed = false;
            if (c->drop_due || send_due_reply(c) != AXL_OK
                || resume(c) != AXL_OK) {
                drop_failed(c);
            }
            continue;
        }
        /* Nothing owed. Hand the longest-waiting connection its turn to
           parse -- but only with no hook on the stack: this callback runs
           inside one whenever a hook dispatches the loop, and the whole point
           of the queue is that the hook it would start must not nest. */
        if (s->hooks > 0) {
            break;
        }
        c = queue_head(s);
        if (c == NULL) {
            break;
        }
        /* OFF the queue before the resume, with the turn marked on it. A
           connection that came back still queued would be picked again here,
           for ever; taking it off first makes that unrepresentable rather
           than argued. It is still valid when resume returns: the only
           dispatch beneath that call runs inside this connection's own hook,
           and conn_drop defers a drop of the connection whose hook is on the
           stack. */
        c->resume_due  = false;
        c->resume_turn = true;
        int rc = resume(c);
        c->resume_turn = false;
        if (rc != AXL_OK) {
            drop_failed(c);
        }
    }
    rearm_reply_timer(s);
    return AXL_SOURCE_REMOVE;
}

/* Hand one request to the application and apply the verdict if it answered
   inside the hook. Returns AXL_ERR to close.

   The hook may run the loop (AxlSshAuthOps), and while it does, the client
   can leave or the login grace can expire. conn_drop defers the drop of a
   connection whose hook is on the stack and marks it ST_DEAD instead, so `c`
   is still valid when the hook returns -- but it is finished. It is not
   authenticated, sent on or parsed any further: the AXL_ERR goes up to
   whichever frame called pump, and that frame drops it on the ordinary path.
   A verdict the hook recorded before or after the drop stays recorded, so a
   principal that came with it is released by conn_free exactly once. The
   handle is withdrawn, so a completion that arrives later for it is refused
   as stale instead of acting on a dead connection. */
static int
issue_request(SshConn *c, bool is_password, const char *password,
              size_t password_len, const uint8_t *blob, size_t blob_len)
{
    AxlSshServer *s = c->srv;

    /* From the counter every server shares -- see g_last_request. */
    c->auth_req          = axl_ssh_auth_request_next(&g_last_request);
    c->auth_is_password  = is_password;
    c->auth_have_verdict = false;

    c->auth_in_hook = true;
    app_call_begin(s, true, c);
    if (is_password) {
        s->auth_ops.password(c->auth_req, c->auth_user, password,
                             password_len, s->auth_ud);
    } else {
        s->auth_ops.publickey(c->auth_req, c->auth_user, blob, blob_len,
                              s->auth_ud);
    }
    app_call_end(s, true);
    c->auth_in_hook = false;

    if (conn_over(c)) {
        c->auth_req = 0;
        return AXL_ERR;         /* dropped during the hook; see above */
    }
    if (c->auth_have_verdict) {
        return auth_apply(c);   /* answered synchronously */
    }
    return AXL_OK;              /* pending: pump stops at the top of its loop */
}

/* RFC 4252 section 8:
     boolean   FALSE
     string    plaintext password in ISO-10646 UTF-8
   or, to change it,
     boolean   TRUE
     string    old password
     string    new password
   The change form is refused unexamined (spec section 13.5): a second
   credential-handling path to review, for a need the web UI already meets.
   So is an EMPTY password, which is OpenSSH's `PermitEmptyPasswords no`
   default -- see axl_ssh_password_reaches_hook, which owns the whole
   admission decision so the unit test can pin each refusal on its own.

   The copy handed to the hook is on this stack and scrubbed on return; the
   decrypted payload it came from is scrubbed by pump(). Neither is logged. */
static int
on_userauth_password(SshConn *c, const uint8_t *p, uint32_t len, size_t off)
{
    AxlSshServer *s = c->srv;

    if (s->auth_ops.password == NULL) {
        return schedule_failure(c, "password (method not offered)");
    }
    if (off >= len) {
        return AXL_ERR;
    }
    bool change = p[off] != 0;
    off++;

    const uint8_t *pw     = NULL;
    uint32_t       pw_len = 0;
    if (axl_ssh_get_string(p, len, &off, &pw, &pw_len) != AXL_OK) {
        return AXL_ERR;
    }
    /* Refused without reaching the hook, and not counted toward the password
       cap: none of these is a guess the application ever checked, and
       MAX_AUTH_REQUESTS still bounds them. Each refusal is a REAL attempt
       otherwise -- schedule_failure owes it the per-user minimum failure
       time and accrues the same penalty as a wrong password. The four cases
       and their OpenSSH counterparts are
       axl_ssh_password_reaches_hook's contract. */
    if (!axl_ssh_password_reaches_hook(change, pw, pw_len)) {
        /* One reason for all four forms, on purpose: naming which one a
           peer tripped tells the peer's author more than it tells the
           operator, and none of the four is a credential the log should
           characterise. */
        return schedule_failure(c, "password (request not accepted)");
    }

    char copy[AXL_SSH_AUTH_PASSWORD_MAX + 1];
    axl_memcpy(copy, pw, pw_len);
    copy[pw_len] = '\0';
    int rc = issue_request(c, true, copy, pw_len, NULL, 0);
    axl_ssh_secure_zero(copy, sizeof copy);
    return rc;
}

/* RFC 4252 section 5:
     byte      SSH_MSG_USERAUTH_REQUEST
     string    user name
     string    service name
     string    method name
   and for "publickey" (section 7):
     boolean   has_signature
     string    public key algorithm name
     string    public key blob
     [string   signature]              -- present only when has_signature

   The two publickey forms are NOT two ways of saying the same thing, and the
   publickey failure policy turns on the difference. The query form costs no
   crypto and is how ssh(1) walks its key list, so an unknown key there is
   answered and the connection lives. The signed form carries proof, and a
   signature that does not verify under an AUTHORIZED key is forgery and
   ends the connection -- whether or not a query came first. ssh(1) does
   query first (9.9p1 even for a key file with no .pub beside it, measured:
   it reads the public half out of the private key file), but nothing here
   depends on that: RFC 4252 section 7 says the client "MAY send the
   signature directly without first verifying whether the key is
   acceptable", and this server keeps no record of whether one did (see
   below). Spec section 9, amended 2026-09-07. A valid signature is still
   only an authorized KEY: who logs in is the publickey hook's verdict when
   one is installed, and a refusal there is an ordinary failure.
   Password failures are counted apart, in auth_apply, and end the
   connection once they reach the cap the connection was ACCEPTED with
   (send_due_reply against c->max_password_failures; the default is
   AXL_SSH_PASSWORD_FAILURES_DEFAULT and the setter is
   axl_ssh_server_set_max_password_failures). Every failure of either
   method is answered no earlier than the per-user minimum
   (schedule_failure).

   Nothing is remembered BETWEEN the two forms on purpose: the signed form
   re-parses and re-authorizes the key it carries, so there is no "the client
   asked about this key earlier" state for a later request to trade on. */
static int
on_userauth(SshConn *c, const uint8_t *p, uint32_t len)
{
    AxlSshServer *s = c->srv;

    if (++c->auth_requests > MAX_AUTH_REQUESTS) {
        /* Refused before it is parsed, so the name logged is the one the
           previous request carried -- and since a change of name ends the
           connection (below), that is the one user this connection has
           ever authenticated as, which is what sshd names too. There is
           always one: MAX_AUTH_REQUESTS requests have been parsed before
           this, and each stored its name or ended the connection. */
        log_auth_exceeded(c);
        disconnect(c, AXL_SSH_DISCONNECT_NO_MORE_AUTH_METHODS,
                   "Too many authentication failures");
        return AXL_ERR;
    }

    size_t         off      = 1;
    const uint8_t *user     = NULL;
    const uint8_t *svc      = NULL;
    const uint8_t *meth     = NULL;
    uint32_t       user_len = 0;
    uint32_t       svc_len  = 0;
    uint32_t       meth_len = 0;
    if (axl_ssh_get_string(p, len, &off, &user, &user_len) != AXL_OK
        || axl_ssh_get_string(p, len, &off, &svc, &svc_len) != AXL_OK
        || axl_ssh_get_string(p, len, &off, &meth, &meth_len) != AXL_OK) {
        return AXL_ERR;
    }

    static const char CONN_SVC[] = "ssh-connection";
    if (svc_len != sizeof CONN_SVC - 1
        || axl_memcmp(svc, CONN_SVC, sizeof CONN_SVC - 1) != 0) {
        disconnect(c, AXL_SSH_DISCONNECT_SERVICE_NOT_AVAILABLE,
                   "no such service");
        return AXL_ERR;
    }

    /* The pre-authentication banner (D9), the ONLY site that reads
       s->banner -- see the field comment for the invariants this position
       is load-bearing for. Whatever is set at this instant, or nothing; the
       counter was incremented above, so `== 1` is this connection's FIRST
       request. FAIL CLOSED: a mandated notice exists to be shown before
       access, so a banner that cannot be queued drops the connection
       instead of authenticating it silently (send_due_reply already does
       this for a verdict's banner).

       It sets no pen_server_fault, so the dropped connection is charged what
       it earned before -- rule 4's noauth, here. That matches the verdict
       banner's path beside it and the parse-time allocation failure in the
       signature check, whose comment makes the argument: whether the ladder
       should exempt the server's own out-of-memory is ONE decision for all
       of those paths (ruling P6 names two, and neither is this), not one
       taken at whichever site was edited last. */
    if (c->auth_requests == 1 && s->banner != NULL
        && send_banner(c, s->banner) != AXL_OK) {
        return AXL_ERR;
    }

    /* RFC 4252 section 5: one user per connection. The server "MUST flush
       any accumulated authentication states if they change", or disconnect
       -- and every count here is per connection (the password cap, the
       request cap), so a new name keeping them would be the state the RFC
       says to flush, and flushing them would be a fresh CAPFUL of guesses
       per rename -- three with the defaults, up to ten
       (AXL_SSH_PASSWORD_FAILURES_MAX). sshd disconnects; so does this. A protocol refusal, not an
       attempt the server judged: it sets no penalty flag of its own, so the
       connection accrues what it earned before (rule 3 if an attempt was
       refused, otherwise rule 4's noauth) -- which is also what sshd
       charged when measured: "deferred penalty of 1 seconds for penalty:
       connections without attempting authentication" after `none` as one
       user and a password request as another. */
    if (c->have_auth_user
        && axl_ssh_userauth_user_changed((const uint8_t *)c->auth_user,
                                         c->auth_user_len,
                                         c->auth_user_wire_len, user,
                                         user_len)) {
        log_user_change(c, user, user_len);
        disconnect(c, AXL_SSH_DISCONNECT_PROTOCOL_ERROR,
                   "Change of username or service not allowed");
        return AXL_ERR;
    }

    /* Who is asking, for the per-user failure time and the log. A name longer
       than a hook accepts, or one with an embedded NUL -- which a
       NUL-terminated copy would silently turn into a different name -- is
       refused without being examined. The delay still hashes the bytes kept,
       so a refusal takes the same time as any other failure for that name. */
    size_t ucopy = user_len < AUTH_USER_MAX ? user_len : AUTH_USER_MAX;
    axl_memcpy(c->auth_user, user, ucopy);
    c->auth_user[ucopy]   = '\0';
    c->auth_user_len      = ucopy;
    c->auth_user_wire_len = user_len;
    c->have_auth_user     = true;
    bool user_ok = user_len <= AUTH_USER_MAX
                   && axl_memchr(user, 0, user_len) == NULL;
    c->auth_have_start  = read_now_us(&c->auth_start_us);
    c->auth_is_password = false;

    static const char NONE[] = "none";
    if (meth_len == sizeof NONE - 1
        && axl_memcmp(meth, NONE, sizeof NONE - 1) == 0) {
        /* RFC 4252 section 5.2: how a client asks what we accept. Not a
           failed attempt, so no minimum failure time -- OpenSSH exempts
           "none" the same way. */
        return send_userauth_failure(c);
    }
    if (!user_ok) {
        return schedule_failure(c, "authentication (user name refused)");
    }

    static const char PW[] = "password";
    if (meth_len == sizeof PW - 1
        && axl_memcmp(meth, PW, sizeof PW - 1) == 0) {
        return on_userauth_password(c, p, len, off);
    }

    static const char PK[] = "publickey";
    if (meth_len != sizeof PK - 1
        || axl_memcmp(meth, PK, sizeof PK - 1) != 0) {
        /* Every other method name (keyboard-interactive, hostbased, ...) is
           a failure like any other, and owed the same minimum time: "none",
           the one exempt query, was answered above. */
        return schedule_failure(c, "authentication (method not offered)");
    }

    if (off >= len) {
        return AXL_ERR;
    }
    bool has_sig = p[off] != 0;
    off++;

    const uint8_t *alg      = NULL;
    const uint8_t *blob     = NULL;
    uint32_t       alg_len  = 0;
    uint32_t       blob_len = 0;
    if (axl_ssh_get_string(p, len, &off, &alg, &alg_len) != AXL_OK
        || axl_ssh_get_string(p, len, &off, &blob, &blob_len) != AXL_OK) {
        return AXL_ERR;
    }
    /* The signature covers everything up to HERE -- captured before the
       signature field is read, so the blob is the bytes that arrived. */
    size_t signed_len = off;

    if (alg_len != sizeof AXL_SSH_HOSTKEY_NAME - 1
        || axl_memcmp(alg, AXL_SSH_HOSTKEY_NAME,
                      sizeof AXL_SSH_HOSTKEY_NAME - 1) != 0) {
        return schedule_failure(c, "publickey (unsupported algorithm)");
    }

    uint8_t pub[AXL_SSH_ED25519_PUB_LEN];
    if (axl_ssh_hostkey_blob_parse(blob, blob_len, pub) != AXL_OK
        || !axl_ssh_authkeys_contains(&s->authkeys, pub)) {
        return schedule_failure(c, "publickey (key not authorized)");
    }

    if (!has_sig) {
        /* The key IS authorized, so ask for a signature. Still nothing
           authenticated, and no crypto has run. */
        return send_pk_ok(c, alg, alg_len, blob, blob_len);
    }

    /* No check that off == len after the signature string: deliberate, and
       not an inconsistency with the blob parsers just above. A BLOB
       (axl_ssh_hostkey_blob_parse, axl_ssh_signature_blob_parse) is a
       self-describing unit, so bytes left over inside one are always
       malformed input and both parsers refuse them. A wire MESSAGE is a
       different layer: SSH messages are extensible by design, every sibling
       message handler in this file is equally permissive about trailing
       bytes (SERVICE_REQUEST, CHANNEL_OPEN), and any bytes trailing the
       signature here sit outside signed_len -- captured above, before the
       signature field was read -- so they are never hashed, never verified,
       and never read by anything else either. There is nothing past this
       point for a trailing byte to reach. */
    AxlSshSigCheck verdict = axl_ssh_userauth_sig_check(
        c->session_id, sizeof c->session_id, pub, p, len, signed_len);

    if (verdict == AXL_SSH_SIG_UNCHECKED) {
        /* OUR failure, not the peer's: memory ran out building or running
           the check, so there is no verdict to act on. Not a forgery --
           no pen_forged, no `bad signature` line, which would accuse the
           peer of this machine's state in the one audit line an operator is
           told to alert on. The connection ends the way every other
           allocation failure while parsing ends it (AXL_ERR, and the
           caller's drop), and accrues what those do: what the connection
           earned before this request (rule 3 if an attempt was refused),
           rule 4's noauth if nothing. The penalty ladder's "the SERVER
           failed it, charge nothing" is documented for exactly two paths --
           a connection it could not set up and one it could not time
           (AxlSshPenalties, rule 1) -- and a parse-time allocation failure
           is neither; whether it should be is one decision for all of them,
           not for this site alone. */
        axl_debug("publickey signature not checked: out of memory");
        return AXL_ERR;
    }
    if (verdict != AXL_SSH_SIG_VALID) {
        /* Forgery, not a client walking its key list: the peer offered a key
           that IS authorized and could not prove it holds the private half.
           Whether it asked about the key first is not known here and does
           not matter (see above). No USERAUTH_FAILURE is ever sent for it,
           so the penalty ladder learns of it here or not at all (rule 3). */
        c->pen_forged = true;
        /* Logged HERE rather than through schedule_failure, which every
           answered refusal takes. This path deliberately does neither of the
           things that funnel does -- no minimum failure time (there is no
           reply to delay) and a different penalty event -- so routing it
           through schedule_failure to get the log line would change its
           timing and its accrual. The audit record is separable from both,
           so it is taken separately.

           And it is the decision an operator most wants: a wrong password is
           usually a typo, while a bad signature under an authorized key is
           someone holding a public key they do not have the private half of.
           Without this line the one refusal that is unambiguous evidence of
           an attack was the only one logged nowhere above DEBUG -- and with
           it on the ordinary limiter, one free unauthorized-key refusal just
           before it kept it there. So it has a window of its own. */
        log_auth_refusal(c, AUTH_LOG_FORGED, "Failed ",
                         "publickey (bad signature)");
        disconnect(c, AXL_SSH_DISCONNECT_NO_MORE_AUTH_METHODS,
                   "authentication failed");
        return AXL_ERR;
    }

    /* A valid signature by an authorized key. Who the login IS belongs to
       the application when it asked to decide (spec section 13.2); a refusal
       there is an ordinary failure, not forgery. */
    if (s->auth_ops.publickey != NULL) {
        return issue_request(c, false, NULL, 0, blob, blob_len);
    }
    return auth_succeed(c, "publickey");
}

/* ------------------------------------------------------------------------ */
/* Session channels                                                          */
/* ------------------------------------------------------------------------ */

/* Defined with the live-server table; the public channel calls look a
   server up there before reading it. */
static bool
server_is_live(const AxlSshServer *s);

/* The channel table's capacity: one slot per channel a connection may hold. */
#define CHAN_CAP  AXL_SSH_MAX_SESSIONS_MAX

/* The fair-share floor's divisor: the most channels the server can hold. */
#define CHAN_MAX_CHANNELS  (MAX_CONNS * AXL_SSH_MAX_SESSIONS_MAX)

/* One bit of SshConn's per-channel flag words. */
static uint16_t
chan_bit(
    const AxlSshChanSlot *ch  ///< a slot of its connection's table
)
{
    return (uint16_t)(1u << ch->local_id);
}

/* Seal what a codec builder appended to @a m, then free @a m. @a built is
   the builder's own return: a builder that failed part-way may have left
   partial bytes in @a m, and those are never sent. Returns AXL_ERR to
   close. */
static int
chan_seal(
    SshConn   *c,     ///< connection to send on
    AxlString *m,     ///< the built message; freed here
    int        built  ///< what the builder returned
)
{
    int rc = built == AXL_OK
             ? send_packet(c, axl_string_data(m), axl_string_len(m))
             : AXL_ERR;
    axl_string_free(m);
    return rc;
}

/* RFC 4254 section 5.1: refuse an open, echoing the peer's sender channel.
   The reason is the RFC's code for the case -- unknown type, the session
   cap (RESOURCE_SHORTAGE) or the application's refusal
   (ADMINISTRATIVELY_PROHIBITED); sshd sends CONNECT_FAILED for the last two
   and the header says why AxlSsh does not. Refusing explicitly rather than
   ignoring matters for the same reason the rekey refusal does: a client
   that gets no answer waits forever. */
static int
send_channel_open_failure(
    SshConn  *c,          ///< connection
    uint32_t  recipient,  ///< the peer's sender channel
    uint32_t  reason      ///< an AXL_SSH_OPEN_* code
)
{
    AxlString *m = axl_string_new("");
    if (m == NULL) {
        return AXL_ERR;
    }
    return chan_seal(c, m, axl_ssh_chan_open_failure_build(m, recipient,
                                                           reason));
}

/* A type-plus-recipient message on a channel: EOF, CLOSE, SUCCESS, FAILURE. */
static int
chan_send_u32(
    SshConn  *c,          ///< connection
    uint8_t   type,       ///< AXL_SSH_MSG_CHANNEL_*
    uint32_t  recipient   ///< the peer's channel number
)
{
    AxlString *m = axl_string_new("");
    if (m == NULL) {
        return AXL_ERR;
    }
    return chan_seal(c, m, axl_ssh_chan_u32_msg_build(m, type, recipient));
}

static int
chan_send_adjust(
    SshConn  *c,          ///< connection
    uint32_t  recipient,  ///< the peer's channel number
    uint32_t  bytes       ///< window to add
)
{
    AxlString *m = axl_string_new("");
    if (m == NULL) {
        return AXL_ERR;
    }
    return chan_seal(c, m, axl_ssh_chan_adjust_build(m, recipient, bytes));
}

/* Our CLOSE, once: after it nothing more is sent on the channel. */
static int
chan_send_close(
    SshConn        *c,   ///< connection
    AxlSshChanSlot *ch   ///< the channel
)
{
    if (ch->close_sent) {
        return AXL_OK;
    }
    ch->close_sent = true;
    axl_debug("chan %u: our CLOSE queued", (unsigned)ch->id);
    return chan_send_u32(c, AXL_SSH_MSG_CHANNEL_CLOSE, ch->peer_id);
}

/* A channel message naming a channel this connection does not hold: sshd's
   "referred to nonexistent channel", which ends the connection. */
static int
chan_nonexistent(
    SshConn *c  ///< connection
)
{
    disconnect(c, AXL_SSH_DISCONNECT_PROTOCOL_ERROR,
               "channel: nonexistent channel");
    return AXL_ERR;
}

/* The slot holding the open channel @a ch of a LIVE server, and its
   connection; NULL for 0, a handle never issued, a retired one, one whose
   connection is being dropped, and any handle on a server that is not live
   -- which server_is_live answers without reading @a s. */
static AxlSshChanSlot *
chan_lookup(
    AxlSshServer   *s,    ///< server, may be freed
    AxlSshChannel   ch,   ///< the application's handle
    SshConn       **out   ///< receives the connection
)
{
    if (!server_is_live(s) || ch == 0) {
        return NULL;
    }
    for (SshConn *c = s->conns; c != NULL; c = c->next) {
        if (conn_over(c)) {
            continue;
        }
        AxlSshChanSlot *slot = axl_ssh_chan_find(c->chans, CHAN_CAP, ch);
        if (slot != NULL) {
            *out = c;
            return slot;
        }
    }
    return NULL;
}

/* Is channel output paused for a key re-exchange -- our KEXINIT out, our
   NEWKEYS not yet? RFC 4253 section 7.1 keeps connection-layer messages off
   the wire in that window, so channel data stays in the channel queues
   rather than being packetized into `held`; rekey_our_newkeys reschedules
   the flush. Asked of the module, as send_packet asks it about every
   packet. */
static bool
chan_paused(
    const SshConn *c  ///< connection
)
{
    return axl_ssh_rekey_out(&c->rekey, AXL_SSH_MSG_CHANNEL_DATA)
           == AXL_SSH_REKEY_HOLD;
}

/* Could the channel flush send anything on @a c right now -- a packet the
   peer's window admits, a WINDOW_ADJUST the application's consumption made
   due, an exit whose drain is done, or a writable hook it owes? This is
   what keeps a flush from being scheduled for nothing: a
   channel with queued output and a window of 0 is NOT work, so a peer that
   stops reading costs no flushes until its WINDOW_ADJUST: a zero window
   must never spin the loop. */
static bool
chan_has_work(
    const SshConn *c  ///< connection
)
{
    if (c->writable_due != 0) {
        return true;
    }
    for (unsigned i = 0; i < CHAN_CAP; i++) {
        const AxlSshChanSlot *ch = &c->chans[i];
        if (ch->id == 0 || ch->retired || ch->close_sent) {
            continue;
        }
        if (ch->exit_pending && ch->queued == 0) {
            return true;
        }
        if (axl_ssh_chan_adjust_due(ch)) {
            return true;
        }
        if (axl_ssh_chan_plan_send(ch, NULL) > 0) {
            return true;
        }
    }
    return false;
}

static void
chan_flush_cb(
    void *data  ///< the SshConn; tx_detach cancels this before it is freed
);

static void
chan_schedule(
    SshConn *c  ///< connection whose channels may have output
)
{
    if (c->chan_flush != 0 || conn_over(c) || !c->srv->running
        || chan_paused(c) || !chan_has_work(c)) {
        return;
    }
    c->chan_flush      = axl_defer(c->srv->loop, chan_flush_cb, c);
    c->chan_flush_lost = c->chan_flush == 0;
    if (c->chan_flush_lost) {
        axl_debug("chan: flush not scheduled (defer ring full);"
                  " the reaper retries");
    }
}

/* Bytes left the server's write budget: a channel refused on the TOTAL may
   now be owed its writable, and that channel can be on ANY connection. So
   every open channel of every connection is asked (the table's
   take_writable consumes the edge), and each connection owed one gets its
   flush, which is where the hook is called. @a self is the connection whose
   own flush is running and will fire its hooks itself, or NULL. */
static void
chan_budget_eased(
    AxlSshServer *s,     ///< server
    SshConn      *self   ///< connection not to schedule, or NULL
)
{
    for (SshConn *c = s->conns; c != NULL; c = c->next) {
        if (conn_over(c)) {
            continue;
        }
        for (unsigned i = 0; i < CHAN_CAP; i++) {
            AxlSshChanSlot *ch = &c->chans[i];
            if (ch->id != 0 && !ch->retired
                && axl_ssh_chan_take_writable(ch, &s->budget)) {
                c->writable_due |= chan_bit(ch);
            }
        }
        if (c != self) {
            chan_schedule(c);
        }
    }
}

/* Release a slot: its queued output leaves the budget, its flags clear. */
static void
chan_slot_free(
    SshConn        *c,   ///< connection
    AxlSshChanSlot *ch   ///< the slot
)
{
    uint16_t bit         = chan_bit(ch);
    bool     was_started = ch->started;

    c->chan_pty        &= (uint16_t)~bit;
    c->chan_close_want &= (uint16_t)~bit;
    c->writable_due    &= (uint16_t)~bit;
    axl_ssh_chan_free(ch, &c->srv->budget);
    /* The last channel in use is gone: the unused clock runs from NOW, as
       sshd's loop refreshes the expiry on the pass the peer's CLOSE wakes
       (serverloop.c:246-247). Without this the clock would run from the
       last reaper tick, up to a tick early. Hooks are not asked here: one
       on the stack still counts at the next tick (axl_ssh_idle_in_use).
       Not with the unused timeout off, where nothing runs, and not in
       teardown (conn_drop), where nothing is left to time: a restart line
       there would report one that never happened. */
    if (was_started && c->idle.unused_sec != 0 && !c->dropping
        && !axl_ssh_idle_in_use(c->chans, CHAN_CAP, 0u)) {
        uint64_t now      = 0;
        bool     have_now = read_now_ms(&now);
        axl_ssh_idle_channel_released(&c->idle, have_now, now);
        /* What the restart left, from the same reading: the unused period
           in ms exactly, when it happened. A seam for the test, which
           cannot time a restart that moves the drop by less than a tick. */
        if (have_now && c->idle.armed) {
            axl_debug("idle: last channel freed; unused clock restarts,"
                      " %llu ms",
                      (unsigned long long)(c->idle.unused_expiry_ms - now));
        }
    }
}

/* Bracket every channel hook. It is application code (app_call_begin, so a
   free waits for it) but not an authentication hook: channel hooks do not
   take the server's one-hook slot. chan_hooks is the parse gate; hook_depth
   is what keeps a close hook from running under another hook of the same
   channel. */
static void
chan_hook_enter(
    SshConn        *c,   ///< connection
    AxlSshChanSlot *ch   ///< the channel the hook is for
)
{
    app_call_begin(c->srv, false, NULL);
    c->chan_hooks++;
    ch->hook_depth++;
}

static void
chan_hook_leave(
    SshConn        *c,   ///< connection
    AxlSshChanSlot *ch   ///< the same channel
)
{
    ch->hook_depth--;
    c->chan_hooks--;
    bool resume = c->chan_hooks == 0 && c->chan_gated;
    if (c->chan_hooks == 0) {
        c->chan_gated = false;
    }
    app_call_end(c->srv, false);
    /* A pump stopped at the gate while the hook ran: what it held back is
       parsed from the loop, through the resume queue -- never from here,
       which may be inside the parse of the request this hook answers. */
    if (resume && !conn_over(c)) {
        queue_join(c);
    }
}

/* Run the close hook for a channel and release its slot -- the one place
   both happen, so a channel is closed at most once by construction: the
   slot is free afterwards and nothing finds it again. The handle is retired
   first, so every call on it from inside the hook is AXL_NOT_FOUND, and the
   hook gets the user data as @a chan_data instead.

   Never under another hook of the same channel: with one on the stack the
   close is recorded (close_deferred) and chan_settle delivers it once that
   hook has left. The parse gate makes that unreachable today -- a close
   comes from the peer's CLOSE, which is not parsed while any hook of the
   connection runs, or from conn_free, which conn_drop defers past them --
   so this is the guard, not a path. */
static void
chan_close_slot(
    SshConn        *c,   ///< connection
    AxlSshChanSlot *ch   ///< the channel
)
{
    AxlSshServer *s = c->srv;

    /* Unreachable today (see above): a DEBUG run fails loudly the first time
       a new caller reaches it, and a RELEASE build still defers safely. */
    AXL_DEBUG_ASSERT_MSG(ch->hook_depth == 0,
                         "chan_close_slot: a hook of the channel is on the"
                         " stack");
    if (ch->hook_depth > 0) {
        ch->close_deferred = true;
        return;
    }
    uint32_t id   = ch->id;
    void    *data = ch->user_data;
    axl_ssh_chan_retire(ch, &s->budget);
    ch->user_data = NULL;
    if (s->chan_ops.close != NULL) {
        chan_hook_enter(c, ch);
        s->chan_ops.close(id, data, s->chan_ud);
        chan_hook_leave(c, ch);
    }
    axl_debug("chan %u: closed", (unsigned)id);
    chan_slot_free(c, ch);
}

/* After a hook of @a ch has left and the reply to its request (if any) has
   been sealed: the CLOSE an axl_ssh_channel_close made under the hook goes
   out now, a deferred close hook runs, and the flush is scheduled for
   whatever the hook wrote. Returns AXL_ERR to close the connection. */
static int
chan_settle(
    SshConn        *c,   ///< connection
    AxlSshChanSlot *ch   ///< the channel whose hook returned
)
{
    if (ch->hook_depth > 0) {
        return AXL_OK;
    }
    uint16_t bit = chan_bit(ch);
    if ((c->chan_close_want & bit) != 0) {
        c->chan_close_want &= (uint16_t)~bit;
        if (chan_send_close(c, ch) != AXL_OK) {
            return AXL_ERR;
        }
    }
    if (ch->close_deferred) {
        chan_close_slot(c, ch);
        chan_budget_eased(c->srv, NULL);
    }
    if (c->chan_hooks == 0) {
        chan_schedule(c);
    }
    return AXL_OK;
}

/* Run every close hook of a connection being freed and release its slots.
   conn_free calls this BEFORE release_principal -- a principal a channel
   holds is still valid in its close hook -- and only with no hook of the
   connection on the stack (conn_drop defers until then). The connection is
   already unlinked, so a close hook that names another of its channels gets
   AXL_NOT_FOUND. */
static void
chan_close_all(
    SshConn *c  ///< connection being freed
)
{
    bool any = false;

    for (unsigned i = 0; i < CHAN_CAP; i++) {
        AxlSshChanSlot *ch = &c->chans[i];
        if (ch->id == 0) {
            continue;
        }
        AXL_DEBUG_ASSERT_MSG(ch->hook_depth == 0,
                             "chan_close_all: a hook of the channel is"
                             " on the stack");
        ch->hook_depth = 0;
        chan_close_slot(c, ch);
        any = true;
    }
    if (any) {
        chan_budget_eased(c->srv, NULL);
    }
}

/* ----- the flush (lifetime rule 7) ----- */

/* Packetize queued channel output, round robin one packet per channel per
   pass, while the transmit queue holds fewer than TXQ_LOW bytes. */
static int
chan_packetize(
    SshConn *c,       ///< connection
    bool    *eased    ///< set when bytes left the budget
)
{
    AxlSshServer *s        = c->srv;
    AxlString    *m        = NULL;
    bool          progress = true;

    /* chan_paused re-asked per packet: the send_packet below may be the one
       that starts a rekey (its trigger), and that packet is then itself the
       only channel data held. */
    while (progress && axl_ssh_txq_bytes(&c->txq) < TXQ_LOW
           && !chan_paused(c)) {
        progress = false;
        for (unsigned k = 0; k < CHAN_CAP && !chan_paused(c)
                             && axl_ssh_txq_bytes(&c->txq) < TXQ_LOW; k++) {
            AxlSshChanSlot *ch  = &c->chans[(c->chan_rr + k) % CHAN_CAP];
            bool            err = false;
            if (ch->id == 0 || ch->retired || ch->close_sent) {
                continue;
            }
            size_t n = axl_ssh_chan_plan_send(ch, &err);
            if (n == 0) {
                continue;
            }
            if (m == NULL) {
                m = axl_string_new("");
                if (m == NULL) {
                    return AXL_ERR;
                }
            }
            axl_string_truncate(m, 0);
            if (axl_ssh_chan_data_build(m, ch->peer_id, err,
                                        axl_ssh_chan_head(ch), n) != AXL_OK
                || send_packet(c, axl_string_data(m), axl_string_len(m))
                       != AXL_OK) {
                axl_string_free(m);
                return AXL_ERR;
            }
            axl_ssh_chan_sent(ch, &s->budget, err, n);
            *eased   = true;
            progress = true;
        }
        c->chan_rr = (c->chan_rr + 1u) % CHAN_CAP;
    }
    axl_string_free(m);
    return AXL_OK;
}

/* Every WINDOW_ADJUST that is due: credit the application reported with
   axl_ssh_channel_consumed() or released by switching a channel back to
   ON_ARRIVAL. Those calls only count and schedule; the adjust is sent here,
   from the loop, so a call can never re-enter a send (or a rekey it would
   trigger) from inside a hook, and many reports between two iterations
   cost one packet. Before the exits below, so it never follows a CLOSE --
   and adjust_due refuses a channel whose CLOSE is already out. */
static int
chan_send_adjusts(
    SshConn *c  ///< connection
)
{
    for (unsigned i = 0; i < CHAN_CAP && !chan_paused(c); i++) {
        AxlSshChanSlot *ch = &c->chans[i];
        if (ch->id == 0) {
            continue;
        }
        uint32_t a = axl_ssh_chan_take_adjust(ch);
        if (a > 0 && chan_send_adjust(c, ch->peer_id, a) != AXL_OK) {
            return AXL_ERR;
        }
    }
    return AXL_OK;
}

/* Every channel whose axl_ssh_channel_exit drain is complete: exit-status,
   EOF and CLOSE, in that order, and the handle is retired. */
static int
chan_finish_exits(
    SshConn *c,       ///< connection
    bool    *eased    ///< set when a channel was retired
)
{
    for (unsigned i = 0; i < CHAN_CAP; i++) {
        AxlSshChanSlot *ch = &c->chans[i];
        if (ch->id == 0 || ch->retired || ch->close_sent
            || !ch->exit_pending || ch->queued > 0) {
            continue;
        }
        AxlString *m = axl_string_new("");
        if (m == NULL
            || chan_seal(c, m, axl_ssh_chan_exit_status_build(
                                   m, ch->peer_id, ch->exit_status))
                   != AXL_OK) {
            return AXL_ERR;
        }
        if (!ch->eof_out) {
            ch->eof_out = true;
            if (chan_send_u32(c, AXL_SSH_MSG_CHANNEL_EOF, ch->peer_id)
                != AXL_OK) {
                return AXL_ERR;
            }
        }
        if (chan_send_close(c, ch) != AXL_OK) {
            return AXL_ERR;
        }
        c->writable_due &= (uint16_t)~chan_bit(ch);
        axl_ssh_chan_retire(ch, &c->srv->budget);
        *eased = true;
    }
    return AXL_OK;
}

/* Call every writable hook this connection is owed. The owed set is taken
   whole first, so a hook's own chan_settle sees only work that is NEW and
   does not schedule a flush for hooks this loop is about to call. */
static void
chan_fire_writable(
    SshConn *c  ///< connection; dropped here if a hook ended it
)
{
    AxlSshServer *s   = c->srv;
    uint16_t      due = c->writable_due;

    c->writable_due = 0;
    for (unsigned i = 0; due != 0; i++) {
        if ((due & (1u << i)) == 0) {
            continue;
        }
        due &= (uint16_t)~(1u << i);
        AxlSshChanSlot *ch = &c->chans[i];
        if (ch->id == 0 || ch->retired || ch->exit_pending
            || s->chan_ops.writable == NULL) {
            continue;
        }
        chan_hook_enter(c, ch);
        s->chan_ops.writable(ch->id, s->chan_ud);
        chan_hook_leave(c, ch);
        if (conn_over(c) || chan_settle(c, ch) != AXL_OK) {
            drop_failed(c);
            return;
        }
    }
}

/* The channel flush: due adjusts and packets first, then finished exits,
   then the writable hooks the bytes that left have earned -- on this
   connection, and (through chan_budget_eased) on any other one a refusal
   on the total was waiting on. Never with a hook of the connection on the stack: such a flush does
   nothing, and chan_settle schedules a fresh one when the last hook leaves,
   so nothing a hook wrote can leave ahead of the reply to its request. */
static void
chan_flush(
    SshConn *c  ///< connection; may be dropped here
)
{
    AxlSshServer *s     = c->srv;
    bool          eased = false;

    if (conn_over(c) || !s->running || c->chan_hooks > 0
        || chan_paused(c)) {
        return;
    }
    if (chan_send_adjusts(c) != AXL_OK
        || chan_packetize(c, &eased) != AXL_OK
        || chan_finish_exits(c, &eased) != AXL_OK) {
        drop_failed(c);
        return;
    }
    if (eased) {
        chan_budget_eased(s, c);
    }
    chan_fire_writable(c);
}

static void
chan_flush_cb(
    void *data  ///< the SshConn; tx_detach cancels this before it is freed
)
{
    SshConn *c = data;

    c->chan_flush = 0;
    chan_flush(c);
}

/* ----- inbound channel messages (RFC 4254 sections 5 and 6) ----- */

/* CHANNEL_OPEN. With no channel hooks installed every open is refused as it
   was before channels existed; with them, `session` alone opens. */
static int
chan_on_open(
    SshConn       *c,    ///< connection
    const uint8_t *p,    ///< the message
    uint32_t       len   ///< its length
)
{
    AxlSshServer   *s = c->srv;
    AxlSshChanOpen  o;
    static const char SESSION[] = "session";

    if (axl_ssh_chan_open_parse(p, len, &o) != AXL_OK) {
        return AXL_ERR;
    }
    /* Every other type -- x11, direct-tcpip, forwarded-tcpip, whatever a
       peer names -- is refused with its type-specific data unread, as sshd
       refuses a type it does not serve: ssh -L/-D/-W on a connection must
       cost the forward, never the session. */
    if (!s->have_chan_ops || o.type_len != sizeof SESSION - 1
        || axl_memcmp(o.type, SESSION, sizeof SESSION - 1) != 0) {
        return send_channel_open_failure(c, o.sender,
                                         AXL_SSH_OPEN_UNKNOWN_CHANNEL_TYPE);
    }
    /* A session (RFC 4254 section 6.1) carries nothing after max packet;
       anything there is a protocol error, as sshd's sshpkt_get_end in
       server_request_session makes it. */
    if (o.rest_len != 0) {
        return AXL_ERR;
    }
    AxlSshChanSlot *ch = axl_ssh_chan_alloc(c->chans, CHAN_CAP,
                                            c->max_sessions);
    if (ch == NULL) {
        axl_debug("chan: open refused, %u sessions open",
                  (unsigned)c->max_sessions);
        return send_channel_open_failure(c, o.sender,
                                         AXL_SSH_OPEN_RESOURCE_SHORTAGE);
    }
    ch->peer_id     = o.sender;
    ch->peer_window = o.window;
    ch->peer_maxpkt = o.max_packet;

    int hrc = AXL_OK;
    if (s->chan_ops.open != NULL) {
        chan_hook_enter(c, ch);
        hrc = s->chan_ops.open(ch->id, c->principal, c->auth_user,
                               s->chan_ud);
        chan_hook_leave(c, ch);
    }
    if (hrc != AXL_OK) {
        /* Never opened: no close hook, whatever it wrote is discarded, and a
           close it made is folded into the refusal. */
        uint32_t peer = ch->peer_id;
        axl_debug("chan %u: open refused by the application",
                  (unsigned)ch->id);
        chan_slot_free(c, ch);
        chan_budget_eased(s, NULL);
        if (conn_over(c)) {
            return AXL_ERR;
        }
        return send_channel_open_failure(
                   c, peer, AXL_SSH_OPEN_ADMINISTRATIVELY_PROHIBITED);
    }
    if (conn_over(c)) {
        return AXL_ERR;   /* opened: conn_free closes it */
    }
    axl_debug("chan %u: open", (unsigned)ch->id);
    AxlString *m = axl_string_new("");
    if (m == NULL
        || chan_seal(c, m, axl_ssh_chan_open_confirm_build(
                               m, ch->peer_id, ch->local_id, 0,
                               AXL_SSH_CHAN_LOCAL_MAXPKT)) != AXL_OK) {
        return AXL_ERR;
    }
    return chan_settle(c, ch);
}

/* Is the request's name @a name? */
static bool
chan_req_is(
    const AxlSshChanReq *r,    ///< the parsed request
    const char          *name  ///< a request name
)
{
    size_t n = axl_strlen(name);

    return r->name_len == n && axl_memcmp(r->name, name, n) == 0;
}

/* The shell, exec or subsystem hook accepted: the session starts and the
   channel gets its window. Returns the WINDOW_ADJUST to send behind the
   reply. */
static uint32_t
chan_start(
    AxlSshChanSlot *ch  ///< the channel
)
{
    uint32_t adjust = 0;

    axl_ssh_chan_start(ch, &adjust);
    return adjust;
}

/* "pty-req" (RFC 4254 section 6.2). Sets *ok to the answer; AXL_ERR ends
   the connection. */
static int
chan_req_pty(
    SshConn             *c,    ///< connection
    AxlSshChanSlot      *ch,   ///< the channel
    const AxlSshChanReq *r,    ///< the request
    bool                *ok    ///< receives the answer
)
{
    AxlSshServer *s = c->srv;

    /* After the start, or with no hook: refused UNPARSED, as sshd checks
       before it parses. */
    if (ch->started || ch->exit_pending || s->chan_ops.pty == NULL) {
        return AXL_OK;
    }
    if ((c->chan_pty & chan_bit(ch)) != 0) {
        disconnect(c, AXL_SSH_DISCONNECT_PROTOCOL_ERROR,
                   "channel: you already have a pty");
        return AXL_ERR;
    }
    char *term = axl_malloc(r->rest_len + 1);
    if (term == NULL) {
        return AXL_OK;   /* our own failure: refuse the request */
    }
    uint32_t dims[4];
    if (axl_ssh_chan_pty_parse(r->rest, r->rest_len, term, r->rest_len + 1,
                               dims) != AXL_OK) {
        axl_free(term);
        return AXL_ERR;
    }
    AxlSshPty pty = { term, dims[0], dims[1], dims[2], dims[3] };
    chan_hook_enter(c, ch);
    int hrc = s->chan_ops.pty(ch->id, &pty, s->chan_ud);
    chan_hook_leave(c, ch);
    axl_free(term);
    *ok = hrc == AXL_OK;
    if (*ok) {
        c->chan_pty |= chan_bit(ch);
    }
    return AXL_OK;
}

/* "shell" (section 6.5). */
static int
chan_req_shell(
    SshConn        *c,       ///< connection
    AxlSshChanSlot *ch,      ///< the channel
    bool           *ok,      ///< receives the answer
    uint32_t       *adjust   ///< receives the window to grant
)
{
    AxlSshServer *s = c->srv;

    if (ch->started || ch->exit_pending || s->chan_ops.shell == NULL) {
        return AXL_OK;
    }
    chan_hook_enter(c, ch);
    int hrc = s->chan_ops.shell(ch->id, s->chan_ud);
    chan_hook_leave(c, ch);
    *ok = hrc == AXL_OK;
    if (*ok) {
        *adjust = chan_start(ch);
    }
    return AXL_OK;
}

/* The exec or subsystem @a hook, handed a NUL-terminated copy of the
   request's string: on acceptance the session starts. */
static void
chan_req_start_hook(
    SshConn        *c,       ///< connection
    AxlSshChanSlot *ch,      ///< the channel
    const uint8_t  *str,     ///< the command or subsystem name, as sent
    uint32_t        len,     ///< its length
    int (*hook)(AxlSshChannel, const char *, size_t, void *),  ///< the hook
    bool           *ok,      ///< receives the answer
    uint32_t       *adjust   ///< receives the window to grant
)
{
    AxlSshServer *s    = c->srv;
    char         *copy = axl_malloc((size_t)len + 1);

    if (copy == NULL) {
        return;   /* our own failure: refuse the request */
    }
    axl_memcpy(copy, str, len);
    copy[len] = '\0';
    chan_hook_enter(c, ch);
    int hrc = hook(ch->id, copy, len, s->chan_ud);
    chan_hook_leave(c, ch);
    axl_free(copy);
    *ok = hrc == AXL_OK;
    if (*ok) {
        *adjust = chan_start(ch);
    }
}

/* "exec" (section 6.5). The command is parsed whether or not a hook is
   installed -- a NUL inside it ends the connection either way, as sshd's
   parse of the same request does. */
static int
chan_req_exec(
    SshConn             *c,       ///< connection
    AxlSshChanSlot      *ch,      ///< the channel
    const AxlSshChanReq *r,       ///< the request
    bool                *ok,      ///< receives the answer
    uint32_t            *adjust   ///< receives the window to grant
)
{
    AxlSshServer  *s       = c->srv;
    const uint8_t *cmd     = NULL;
    uint32_t       cmd_len = 0;

    if (ch->started || ch->exit_pending) {
        return AXL_OK;
    }
    if (axl_ssh_chan_exec_parse(r->rest, r->rest_len, &cmd, &cmd_len)
        != AXL_OK) {
        return AXL_ERR;
    }
    if (s->chan_ops.exec != NULL) {
        chan_req_start_hook(c, ch, cmd, cmd_len, s->chan_ops.exec, ok,
                            adjust);
    }
    return AXL_OK;
}

/* "subsystem" (section 6.5). With no hook it is refused UNPARSED, as it was
   before the hook existed; with one, the name is parsed as sshd's
   session_subsystem_req parses it -- a NUL inside it or bytes after it end
   the connection -- and the hook alone decides whether it exists. */
static int
chan_req_subsystem(
    SshConn             *c,       ///< connection
    AxlSshChanSlot      *ch,      ///< the channel
    const AxlSshChanReq *r,       ///< the request
    bool                *ok,      ///< receives the answer
    uint32_t            *adjust   ///< receives the window to grant
)
{
    AxlSshServer  *s        = c->srv;
    const uint8_t *name     = NULL;
    uint32_t       name_len = 0;

    if (ch->started || ch->exit_pending || s->chan_ops.subsystem == NULL) {
        return AXL_OK;
    }
    /* The same wire shape as exec: one string, one trailing NUL allowed. */
    if (axl_ssh_chan_exec_parse(r->rest, r->rest_len, &name, &name_len)
        != AXL_OK) {
        return AXL_ERR;
    }
    chan_req_start_hook(c, ch, name, name_len, s->chan_ops.subsystem, ok,
                        adjust);
    return AXL_OK;
}

/* "window-change" (section 6.7): answered SUCCESS if asked, pty or not, as
   sshd answers it; the hook runs only for a channel with a pty. */
static int
chan_req_resize(
    SshConn             *c,    ///< connection
    AxlSshChanSlot      *ch,   ///< the channel
    const AxlSshChanReq *r,    ///< the request
    bool                *ok    ///< receives the answer
)
{
    AxlSshServer *s = c->srv;
    uint32_t      dims[4];

    if (axl_ssh_chan_winch_parse(r->rest, r->rest_len, dims) != AXL_OK) {
        return AXL_ERR;
    }
    *ok = true;
    if ((c->chan_pty & chan_bit(ch)) == 0 || s->chan_ops.resize == NULL) {
        return AXL_OK;
    }
    AxlSshPty size = { NULL, dims[0], dims[1], dims[2], dims[3] };
    chan_hook_enter(c, ch);
    s->chan_ops.resize(ch->id, &size, s->chan_ud);
    chan_hook_leave(c, ch);
    return AXL_OK;
}

/* CHANNEL_REQUEST. Everything not handled here -- env, x11-req,
   auth-agent-req@openssh.com, signal, break, any other name -- is refused
   with CHANNEL_FAILURE when the peer asked for a reply. */
static int
chan_on_request(
    SshConn       *c,    ///< connection
    const uint8_t *p,    ///< the message
    uint32_t       len   ///< its length
)
{
    AxlSshChanReq r;

    if (axl_ssh_chan_req_parse(p, len, &r) != AXL_OK) {
        return AXL_ERR;
    }
    AxlSshChanSlot *ch = axl_ssh_chan_find_peer(c->chans, CHAN_CAP,
                                                r.recipient);
    if (ch == NULL) {
        return chan_nonexistent(c);
    }
    if (ch->retired || ch->close_sent) {
        return AXL_OK;   /* crossed our CLOSE: not answered */
    }
    bool     ok     = false;
    uint32_t adjust = 0;
    int      rc     = AXL_OK;
    if (chan_req_is(&r, "pty-req")) {
        rc = chan_req_pty(c, ch, &r, &ok);
    } else if (chan_req_is(&r, "shell")) {
        rc = chan_req_shell(c, ch, &ok, &adjust);
    } else if (chan_req_is(&r, "exec")) {
        rc = chan_req_exec(c, ch, &r, &ok, &adjust);
    } else if (chan_req_is(&r, "subsystem")) {
        rc = chan_req_subsystem(c, ch, &r, &ok, &adjust);
    } else if (chan_req_is(&r, "window-change")) {
        rc = chan_req_resize(c, ch, &r, &ok);
    }
    if (rc != AXL_OK || conn_over(c)) {
        return AXL_ERR;
    }
    /* The reply first, then the window behind it -- sshd's order -- and
       only then, in chan_settle, a CLOSE the hook asked for. */
    if (r.want_reply
        && chan_send_u32(c, ok ? AXL_SSH_MSG_CHANNEL_SUCCESS
                               : AXL_SSH_MSG_CHANNEL_FAILURE,
                         ch->peer_id) != AXL_OK) {
        return AXL_ERR;
    }
    if (adjust > 0 && !ch->retired
        && chan_send_adjust(c, ch->peer_id, adjust) != AXL_OK) {
        return AXL_ERR;
    }
    return chan_settle(c, ch);
}

/* CHANNEL_DATA and CHANNEL_EXTENDED_DATA. */
static int
chan_on_data(
    SshConn       *c,    ///< connection
    const uint8_t *p,    ///< the message
    uint32_t       len,  ///< its length
    bool           ext   ///< EXTENDED_DATA
)
{
    AxlSshServer  *s      = c->srv;
    uint32_t       rcpt   = 0;
    uint32_t       code   = 0;
    const uint8_t *data   = NULL;
    uint32_t       n      = 0;
    uint32_t       adjust = 0;

    int prc = ext ? axl_ssh_chan_ext_data_parse(p, len, &rcpt, &code, &data,
                                                &n)
                  : axl_ssh_chan_data_parse(p, len, &rcpt, &data, &n);
    if (prc != AXL_OK) {
        return AXL_ERR;
    }
    AxlSshChanSlot *ch = axl_ssh_chan_find_peer(c->chans, CHAN_CAP, rcpt);
    if (ch == NULL) {
        return chan_nonexistent(c);
    }
    int r = axl_ssh_chan_inbound(ch, n, ext, s->chan_ops.data != NULL,
                                 &adjust);
    if (r == AXL_ERR) {
        disconnect(c, AXL_SSH_DISCONNECT_PROTOCOL_ERROR,
                   ext ? "channel: extended data past the window or EOF"
                       : "channel: data past the window");
        return AXL_ERR;
    }
    /* The arrival-time adjust is sent HERE, per packet, not batched through
       the flush as axl_ssh_channel_consumed()'s credit is. This path is not
       reachable from an application callback -- it is the parse of a peer's
       packet -- so it cannot re-enter a send from a hook; it takes the same
       capped grant (axl_ssh_chan_take_adjust), refuses once our CLOSE is
       out, and fails the connection on a send failure as it always has.
       Batching it too was measured and gained nothing (QEMU user-net caps
       a connection near 35 KB in flight), while it would make how many
       adjusts a peer sees depend on how many packets one pump turn drains;
       per packet, a peer's transcript is exact. */
    if (adjust > 0 && chan_send_adjust(c, ch->peer_id, adjust) != AXL_OK) {
        axl_debug("chan %u: WINDOW_ADJUST of %u not sent; failing the"
                  " connection", (unsigned)ch->id, (unsigned)adjust);
        return AXL_ERR;
    }
    if (r != AXL_OK || n == 0 || s->chan_ops.data == NULL) {
        return AXL_OK;
    }
    /* `data` points INTO c->payload: the parse gate (chan_hooks) is what
       keeps it valid for as long as the hook holds it. */
    chan_hook_enter(c, ch);
    s->chan_ops.data(ch->id, data, n, s->chan_ud);
    chan_hook_leave(c, ch);
    if (conn_over(c)) {
        return AXL_ERR;
    }
    return chan_settle(c, ch);
}

/* CHANNEL_EOF: the eof hook, once. */
static int
chan_on_eof(
    SshConn       *c,    ///< connection
    const uint8_t *p,    ///< the message
    uint32_t       len   ///< its length
)
{
    AxlSshServer *s    = c->srv;
    uint32_t      rcpt = 0;

    if (axl_ssh_chan_u32_msg_parse(p, len, &rcpt) != AXL_OK) {
        return AXL_ERR;
    }
    AxlSshChanSlot *ch = axl_ssh_chan_find_peer(c->chans, CHAN_CAP, rcpt);
    if (ch == NULL) {
        return chan_nonexistent(c);
    }
    if (ch->retired || ch->close_sent || ch->eof_in) {
        return AXL_OK;
    }
    ch->eof_in = true;
    if (s->chan_ops.eof == NULL) {
        return AXL_OK;
    }
    chan_hook_enter(c, ch);
    s->chan_ops.eof(ch->id, s->chan_ud);
    chan_hook_leave(c, ch);
    if (conn_over(c)) {
        return AXL_ERR;
    }
    return chan_settle(c, ch);
}

/* CHANNEL_CLOSE: answered with our own CLOSE at once if we have not sent
   one, whatever is still queued discarded (nothing is sent on a channel
   after the peer's CLOSE), then the close hook, and the slot is free. */
static int
chan_on_close(
    SshConn       *c,    ///< connection
    const uint8_t *p,    ///< the message
    uint32_t       len   ///< its length
)
{
    uint32_t rcpt = 0;

    if (axl_ssh_chan_u32_msg_parse(p, len, &rcpt) != AXL_OK) {
        return AXL_ERR;
    }
    AxlSshChanSlot *ch = axl_ssh_chan_find_peer(c->chans, CHAN_CAP, rcpt);
    if (ch == NULL) {
        return chan_nonexistent(c);
    }
    ch->close_in = true;
    if (chan_send_close(c, ch) != AXL_OK) {
        return AXL_ERR;
    }
    chan_close_slot(c, ch);
    chan_budget_eased(c->srv, NULL);
    return conn_over(c) ? AXL_ERR : AXL_OK;
}

/* CHANNEL_WINDOW_ADJUST. Ignored for a channel we do not hold, as sshd
   ignores it; an adjust past 2^32 - 1 ends the connection. */
static int
chan_on_adjust(
    SshConn       *c,    ///< connection
    const uint8_t *p,    ///< the message
    uint32_t       len   ///< its length
)
{
    uint32_t rcpt  = 0;
    uint32_t bytes = 0;

    if (axl_ssh_chan_adjust_parse(p, len, &rcpt, &bytes) != AXL_OK) {
        return AXL_ERR;
    }
    AxlSshChanSlot *ch = axl_ssh_chan_find_peer(c->chans, CHAN_CAP, rcpt);
    if (ch == NULL) {
        return AXL_OK;
    }
    if (axl_ssh_chan_peer_adjust(ch, bytes) != AXL_OK) {
        disconnect(c, AXL_SSH_DISCONNECT_PROTOCOL_ERROR,
                   "channel: adjust overflows the window");
        return AXL_ERR;
    }
    chan_schedule(c);
    return AXL_OK;
}

/* Channel messages 93-98 on a server with channel hooks installed. */
static int
chan_on_message(
    SshConn       *c,    ///< connection
    const uint8_t *p,    ///< the message
    uint32_t       len   ///< its length
)
{
    switch (p[0]) {
    case AXL_SSH_MSG_CHANNEL_WINDOW_ADJUST:
        return chan_on_adjust(c, p, len);
    case AXL_SSH_MSG_CHANNEL_DATA:
        return chan_on_data(c, p, len, false);
    case AXL_SSH_MSG_CHANNEL_EXTENDED_DATA:
        return chan_on_data(c, p, len, true);
    case AXL_SSH_MSG_CHANNEL_EOF:
        return chan_on_eof(c, p, len);
    case AXL_SSH_MSG_CHANNEL_CLOSE:
        return chan_on_close(c, p, len);
    case AXL_SSH_MSG_CHANNEL_REQUEST:
    default:
        return chan_on_request(c, p, len);
    }
}

/* ------------------------------------------------------------------------ */
/* Message dispatch                                                          */
/* ------------------------------------------------------------------------ */

/* Dispatch one decrypted payload.

   THE PRE-AUTH ARMS (ST_KEXINIT, ST_KEX, ST_NEWKEYS) ACCEPT EXACTLY ONE
   MESSAGE NUMBER EACH, AND THAT IS NOW A PROTOCOL OBLIGATION. It is strict
   KEX rule (a) -- "terminate the connection if [an] out-of-sequence packet
   or any message that is not strictly required by KEX is received...
   including... SSH2_MSG_DEBUG, SSH2_MSG_IGNORE or SSH2_MSG_UNIMPLEMENTED"
   (OpenSSH PROTOCOL section 1.10) -- and axl_ssh_kexinit_build now
   advertises kex-strict-s-v00@openssh.com, so we have told the peer we do
   it. Every `return AXL_ERR` in those three arms is therefore the rule, not
   a strictness we could trade away: relaxing any of them to ignore IGNORE or
   DEBUG reopens the client-to-server half of Terrapin. The tolerance RFC
   4253 section 11 asks for lives in ST_AUTH and ST_SESSION, where strict KEX
   does not override it. */
static int
on_payload(SshConn *c, const uint8_t *p, uint32_t len, uint32_t seq)
{
    if (len == 0) {
        return AXL_ERR;
    }
    switch (c->state) {
    case ST_KEXINIT: {
        AxlSshKexinitPeer peer = { .guessed_wrong = false,
                                   .strict_kex = false };
        if (axl_ssh_kexinit_select(p, len, &peer) != AXL_OK) {
            return AXL_ERR;
        }
        if (axl_string_append_len(c->i_c, (const char *)p, len) != AXL_OK) {
            return AXL_ERR;
        }
        /* RFC 4253 section 7.1: the peer guessed a different kex and has
           already sent a packet for it. Drop exactly one, silently. ssh(1)
           never sets the flag, so nothing in the conformance test reaches
           this -- which is why it was missing until a review looked. */
        c->drop_next  = peer.guessed_wrong;
        c->strict_kex = peer.strict_kex;
        c->state = ST_KEX;
        return AXL_OK;
    }

    case ST_KEX:
        return do_kex(c, p, len);

    case ST_NEWKEYS:
        if (p[0] != AXL_SSH_MSG_NEWKEYS) {
            /* Strict KEX rule (a), and it is now load-bearing rather than
               merely strict: having advertised kex-strict-s-v00@openssh.com
               we MUST terminate here, including on the SSH_MSG_IGNORE and
               SSH_MSG_DEBUG that RFC 4253 section 11 would otherwise have us
               ignore. This arm predates the advertisement (it is why the
               client-to-server half of Terrapin never worked against us);
               do not relax it. */
            return AXL_ERR;
        }
        /* The receive buffer for the rest of this connection's life, taken
           BEFORE rx_on says it exists -- the invariant pump relies on is
           "rx_on implies payload", so nothing here may flip rx_on and then
           fail. FAIL CLOSED rather than falling back to a per-packet
           allocation: the point is that the steady state does not allocate
           THIS buffer, and a silent fallback would leave the defect in place
           on exactly the machine too short of memory to afford it. Every
           other allocation failure in this file ends the connection too
           (on_accept's string setup, a transmit-queue push).

           NOT "the steady state does not allocate", which an earlier version
           of this comment said and the commit's own measurement disproves:
           an additional packet still costs 20 allocations and 1,647 bytes.
           One of those is the same SHAPE as what this removed --
           axl_ssh_aead_open allocates and frees a plaintext scratch per
           opened packet (axl-ssh-crypt.c, `clear`) -- but it is sized to the
           packet, so a keystroke pays ~67 ns against the 2,555 ns this
           buffer's 35,000-byte pair cost. If P4 ever needs the receive path
           to be allocation-FREE rather than merely cheap, that `clear` is
           the next one to take, and a reader must not infer from here that
           the path is already there.

           GUARDED for the same reason have_session_id is (do_kex, where the
           session id is written only on the FIRST exchange). This arm runs
           once per connection -- a rekey's NEWKEYS arrives in ST_SESSION and
           never comes here (rekey_on_peer_newkeys) -- so the guard is the
           second line of defence: an unconditional allocation would leak the
           first buffer, 35,000 bytes per rekey, on a live session. One buffer
           serves every exchange: the size is the protocol ceiling, not a
           property of the keys. test-ssh-rekey-qemu.sh's alloc-across-rekey
           compares the allocator's live count across several rekeys. */
        if (c->payload == NULL) {
            c->payload = axl_malloc(PAYLOAD_MAX);
            if (c->payload == NULL) {
                return AXL_ERR;
            }
        }
        /* The key do_kex staged; the peer's packets from here are under it. */
        if (axl_ssh_dirkey_switch(&c->rx_key) != AXL_OK) {
            return AXL_ERR;
        }
        c->rx_on = true;
        /* Rule (b), receive half. pump() has ALREADY incremented rx_seq for
           this packet, so assigning zero here makes the next packet we
           decrypt number 0 -- which is what the peer, having reset its own
           send counter after this same NEWKEYS, will have used. */
        if (c->strict_kex) {
            c->rx_seq = 0;
        }
        c->state = ST_AUTH;
        /* The deadline deliberately survives key exchange. It covers
           accept -> AUTHENTICATED, not accept -> NEWKEYS: a peer that
           completes kex and then goes silent holds a connection slot exactly
           as effectively as one that never spoke. Only USERAUTH_SUCCESS
           lifts it, which is the only point at which this connection has
           earned an unbounded lifetime. */
        return AXL_OK;

    case ST_AUTH:
        /* SERVICE_REQUEST is accepted so ssh(1) proceeds to userauth, which
           on_userauth then answers per RFC 4252 section 7. */
        if (p[0] == AXL_SSH_MSG_SERVICE_REQUEST) {
            /* Counted BEFORE the request is parsed, so a repeat that is also
               malformed still trips this rather than the parser. RFC 4253
               section 10 asks for one; a peer sending more is not a client
               we can serve, it is a peer making us allocate. */
            if (++c->svc_requests > MAX_SERVICE_REQUESTS) {
                disconnect(c, AXL_SSH_DISCONNECT_PROTOCOL_ERROR,
                           "repeated service request");
                return AXL_ERR;
            }
            size_t         off = 1;
            const uint8_t *name = NULL;
            uint32_t       nlen = 0;
            if (axl_ssh_get_string(p, len, &off, &name, &nlen) != AXL_OK) {
                return AXL_ERR;
            }
            /* RFC 4253 section 10: a service we do not support MUST end the
               connection, not be echoed back. Echoing was also a pre-auth
               amplifier -- a peer that never reads its replies could queue
               34,990 bytes of outbound heap per request, unbounded, on a
               connection nobody has authenticated. Comparing against the one
               name we accept caps the reply at 17 bytes. */
            static const char SVC[] = "ssh-userauth";
            if (nlen != sizeof SVC - 1
                || axl_memcmp(name, SVC, sizeof SVC - 1) != 0) {
                disconnect(c, AXL_SSH_DISCONNECT_SERVICE_NOT_AVAILABLE,
                           "no such service");
                return AXL_ERR;
            }
            AxlString *m = axl_string_new("");
            if (m == NULL) {
                return AXL_ERR;
            }
            char hdr = (char)AXL_SSH_MSG_SERVICE_ACCEPT;
            int rc = (axl_string_append_len(m, &hdr, 1) == AXL_OK
                      && axl_ssh_put_string(m, SVC, sizeof SVC - 1) == AXL_OK)
                     ? send_packet(c, axl_string_data(m), axl_string_len(m))
                     : AXL_ERR;
            axl_string_free(m);
            return rc;
        }
        if (p[0] == AXL_SSH_MSG_USERAUTH_REQUEST) {
            return on_userauth(c, p, len);
        }
        /* A rekey is a REQUEST, and RFC 4253 section 9 requires an answer;
           before authentication the answer is to end the connection
           (deviation 1, spec section 12a.7) -- silently ignoring it would
           leave a conforming client blocked for ever waiting for our
           KEXINIT. Rekey is served in ST_SESSION only: a peer that has not
           authenticated has no use for a second key, and every exchange
           costs the server an ECDH and a signature.

           A stock client does not get here. Its RekeyLimit is `default
           none` -- the cipher's data limit, about 1 GiB for
           chacha20-poly1305@openssh.com, "and no time based rekeying is
           done" (man 5 ssh_config) -- and nothing near that crosses before
           USERAUTH_SUCCESS. */
        if (p[0] == AXL_SSH_MSG_KEXINIT) {
            disconnect(c, AXL_SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                       "key re-exchange before authentication");
            return AXL_ERR;
        }
        /* Anything else at this point is a message P3 does not implement.
           Never fatal: RFC 4253 section 11 says a peer may send messages we
           do not know and an implementation must tolerate them, so killing
           the connection would fail conformance for something that is not a
           defect. Section 11.4 makes the ANSWER a MUST, which is what
           changed here -- the silent ignore that shipped met the first
           obligation and not the second.

           (The comment this replaces named SSH2_MSG_EXT_INFO as something
           ssh(1) sends here. It does not, against this server: RFC 8308
           only lets a client send EXT_INFO to a server that advertised
           ext-info-s, and AxlSsh advertises no such thing. Confirmed
           against a full -vvv transcript, which carries types 5, 50, 90 and
           1 and nothing else.) */
        return send_unimplemented(c, p[0], seq, false);

    case ST_SESSION:
        /* Key re-exchange first (axl-ssh-rekey.h): what the peer may send
           while its own exchange is open, and where 20, 21 and 30-49 go. */
        if (!axl_ssh_rekey_in_allowed(&c->rekey, p[0])) {
            return rekey_refuse(c, p[0], seq);
        }
        if (axl_ssh_rekey_in_kex(&c->rekey)) {
            if (p[0] == AXL_SSH_MSG_NEWKEYS) {
                return rekey_on_peer_newkeys(c);
            }
            if (p[0] == AXL_SSH_MSG_KEX_ECDH_INIT && !c->kex_replied) {
                return do_kex(c, p, len);
            }
            if (p[0] >= AXL_SSH_MSG_KEX_ECDH_INIT && p[0] <= 49u) {
                /* A method message this exchange does not expect: sshd
                   arms only the one it waits for (kexgen.c), and the rest
                   reach kex_protocol_error's UNIMPLEMENTED. */
                return rekey_refuse(c, p[0], seq);
            }
        } else if (p[0] == AXL_SSH_MSG_KEXINIT) {
            return rekey_on_peer_kexinit(c, p, len);
        }
        /* Authenticated. RFC 4254 section 5.1's CHANNEL_OPEN is always
           answered -- a `session` opens when channel hooks are installed,
           and every other open is refused explicitly rather than ignored,
           which would leave ssh(1) waiting for an answer that never comes
           (the same defect P1 fixed for rekey). */
        if (p[0] == AXL_SSH_MSG_CHANNEL_OPEN) {
            return chan_on_open(c, p, len);
        }
        /* The rest of the channel protocol exists only with hooks
           installed. Without them no channel was ever opened, and these
           fall through to the UNIMPLEMENTED answer they always got. */
        if (c->srv->have_chan_ops
            && p[0] >= AXL_SSH_MSG_CHANNEL_WINDOW_ADJUST
            && p[0] <= AXL_SSH_MSG_CHANNEL_REQUEST) {
            return chan_on_message(c, p, len);
        }
        /* The answers a keepalive probe draws -- and any other request's --
           accepted silently and unparsed, as sshd's dispatch table sends
           all four to server_input_keep_alive (serverloop.c:380-390). The
           count they reset was already reset above, by pump, for every
           decoded packet. Before authentication they still earn
           UNIMPLEMENTED: that arm is ST_AUTH's. */
        if (p[0] == AXL_SSH_MSG_REQUEST_SUCCESS
            || p[0] == AXL_SSH_MSG_REQUEST_FAILURE
            || p[0] == AXL_SSH_MSG_CHANNEL_SUCCESS
            || p[0] == AXL_SSH_MSG_CHANNEL_FAILURE) {
            return AXL_OK;
        }
        /* RFC 4254 section 4: GLOBAL_REQUEST is now answered properly rather
           than by the UNIMPLEMENTED stand-in P1 shipped -- AxlSsh implements
           no global request, so REQUEST_FAILURE is the answer whatever the
           request names, including keepalive@openssh.com's liveness probe.
           want_reply FALSE draws nothing: that is what RFC 4254 section 4
           itself says a sender not asking for a reply gets, not a gap this
           server still owes. */
        if (p[0] == AXL_SSH_MSG_GLOBAL_REQUEST) {
            bool want = false;
            if (axl_ssh_global_request_parse(p, len, &want) != AXL_OK) {
                return AXL_ERR;
            }
            return want ? send_request_failure(c) : AXL_OK;
        }
        /* RFC 4252 section 5.1 keeps userauth SILENT here -- an
           UNIMPLEMENTED reply is not ignoring a request -- and everything
           else earns section 11.4's answer. Both rules live in
           axl_ssh_unimplemented_due, so neither can be met by breaking the
           other. */
        return send_unimplemented(c, p[0], seq, true);

    case ST_IDENT:
    case ST_DEAD:
    case ST_LINGER:
    default:
        return AXL_ERR;
    }
}

/* Consume as many complete items as the buffer holds. Returns AXL_ERR to
   close the connection. */
static int
pump(SshConn *c)
{
    for (;;) {
        const uint8_t *p   = (const uint8_t *)axl_string_data(c->in);
        size_t         len = axl_string_len(c->in);

        /* !running: axl_ssh_server_free is tearing down, and anything its
           release_principal dispatches -- a completion that resumes, a
           receive on another connection -- must not hand the application a
           new request on a server being freed. */
        if (conn_over(c) || !c->srv->running) {
            return AXL_ERR;
        }
        /* RFC 4252 section 5.1: finish one authentication request -- verdict
           and reply -- before reading the next. The client may pipeline; its
           bytes wait here still encrypted, bounded by RX_BACKLOG_MAX. */
        if (c->auth_req != 0 || c->reply_armed) {
            return AXL_OK;
        }
        /* While any channel hook of this connection is on the stack, parse
           nothing: the data hook holds a pointer into c->payload (see that
           field). Before decryption, like the gate above. The last hook to
           leave puts the connection on the resume queue (chan_hook_leave). */
        if (c->chan_hooks > 0) {
            if (!c->chan_gated) {
                /* Once per hold, not per packet: the next pump is the
                   resume queue's, after the last hook has left. */
                c->chan_gated = true;
                axl_debug("chan: input held while a channel hook runs");
            }
            return AXL_OK;
        }
        /* One authentication hook at a time, server-wide, and in arrival
           order behind it (P3 security review MED-3). Stops here, before
           anything is decrypted, exactly as the gate above does. */
        if (!auth_may_parse(c)) {
            return AXL_OK;
        }
        /* Output backpressure (TXQ_HIGH): parse nothing more while the
           replies already owed cannot be sent. Checked per packet, so the
           queue stops at most one reply past the mark. Held by tx_paused,
           not by the byte count alone, until on_sent sees the queue under
           TXQ_LOW: a pump reached meanwhile from elsewhere -- the reply
           timer's resume -- must not parse with the queue still between the
           marks. */
        if (c->tx_paused || axl_ssh_txq_bytes(&c->txq) > TXQ_HIGH) {
            if (!c->tx_paused) {
                c->tx_paused = true;
                c->tx_peak   = axl_ssh_txq_bytes(&c->txq);
                axl_debug("tx: pausing input, %zu bytes queued", c->tx_peak);
            }
            return AXL_OK;
        }

        if (c->state == ST_IDENT) {
            size_t end = 0;
            int    rc  = axl_ssh_parse_ident(p, len, &end);
            if (rc == AXL_INCOMPLETE) {
                return AXL_OK;
            }
            if (rc != AXL_OK) {
                return AXL_ERR;
            }
            /* parse_ident reports only where the line ENDS, and the exchange
               hash needs the line itself. It is the last line before `end`,
               so walk back from its CRLF to the previous LF (or the start) --
               any preamble lines sit before that and are not part of V_C. */
            size_t line_end = end - 2;
            size_t start    = 0;
            for (size_t i = line_end; i > 0; i--) {
                if (p[i - 1] == '\n') {
                    start = i;
                    break;
                }
            }
            if (axl_string_append_len(c->v_c, (const char *)p + start,
                                      line_end - start) != AXL_OK) {
                return AXL_ERR;
            }
            axl_string_erase(c->in, 0, end);
            c->state = ST_KEXINIT;
            continue;
        }

        size_t   consumed = 0;
        uint32_t plen     = 0;
        int      rc;
        /* What gets dispatched, and -- separately -- what this iteration
           OWNS. Encrypted, `payload` is the connection's buffer and `owned`
           stays NULL; before NEWKEYS there is no such buffer and the copy
           belongs to this iteration. Two names rather than one because a
           reused buffer freed by mistake is not a leak, it is a
           use-after-free on every later packet.

           So every axl_free(owned) below relies on axl_free(NULL) being a
           no-op, and the two on the early returns can free nothing at all
           TODAY: encrypted, `owned` is NULL; unencrypted, it is allocated only
           once rc is already AXL_OK. They are kept for the arm that has an
           allocation to lose the moment either path allocates before it knows
           rc -- not because they currently do anything. */
        uint8_t *payload  = NULL;
        uint8_t *owned    = NULL;

        if (c->rx_on) {
            /* Held by construction: the ST_NEWKEYS arm allocates before it
               sets rx_on. If it were ever broken, axl_ssh_aead_open refuses a
               NULL `out` and the connection is dropped rather than faulting,
               which is why this is an assert and not a branch. */
            AXL_DEBUG_ASSERT_MSG(c->payload != NULL,
                                 "pump: rx_on with no payload buffer");
            payload = c->payload;
            rc = axl_ssh_aead_open(&c->rx_key.cur, c->rx_seq, p, len,
                                   &consumed, payload, PAYLOAD_MAX, &plen);
        } else {
            const uint8_t *view = NULL;
            rc = axl_ssh_packet_unwrap(p, len, &consumed, &view, &plen);
            if (rc == AXL_OK) {
                owned = axl_malloc(plen);
                if (owned == NULL) {
                    return AXL_ERR;
                }
                axl_memcpy(owned, view, plen);
                payload = owned;
            }
        }

        if (rc == AXL_INCOMPLETE) {
            axl_free(owned);
            return AXL_OK;
        }
        if (rc != AXL_OK) {
            axl_free(owned);
            return AXL_ERR;
        }

        /* The sequence number of THIS packet is the one before the
           increment, and it is the only value SSH_MSG_UNIMPLEMENTED can
           carry (RFC 4253 section 11.4) -- the peer matches the reply to
           what it sent by this number, so echoing the already-advanced
           counter names the packet AFTER the offending one. Truncated to 32
           bits because that is the width of the wire field; the counter is
           wider only so the AEAD nonce does not wrap silently. */
        uint32_t seq = (uint32_t)c->rx_seq;
        c->rx_seq++;
        /* Every packet decoded after authentication answers the keepalive
           count, whatever its type: packet.c:1814 zeroes
           keep_alive_timeouts in ssh_packet_read_poll_seqnr for each one. */
        if (c->authenticated) {
            axl_ssh_idle_answered(&c->idle);
        }
        if (c->rx_on) {
            /* Counted against the key that opened it -- a NEWKEYS before
               rekey_on_peer_newkeys resets the count, as sshd orders it. */
            axl_ssh_rekey_count_in(&c->rekey, consumed);
        }
        /* Erase BEFORE dispatching: the handler may append to c->in's
           sibling buffers and, more importantly, must never see the bytes it
           has already consumed if it re-enters. */
        axl_string_erase(c->in, 0, consumed);
        if (c->drop_next) {
            /* The peer's wrong kex guess. It still COUNTS for the sequence
               number (already incremented above) -- RFC 4253 numbers every
               packet on the wire, including one we ignore. */
            c->drop_next = false;
            axl_ssh_secure_zero(payload, plen);
            axl_free(owned);
            continue;
        }
        /* The server's own trigger, inbound half (sshd checks both
           directions on the read path too). Before the dispatch, so a reply
           this packet earns is held behind our KEXINIT like any other. */
        int drc = AXL_OK;
        if (c->state == ST_SESSION && axl_ssh_rekey_due(&c->rekey)) {
            drc = rekey_start(c);
        }
        if (drc == AXL_OK) {
            drc = on_payload(c, payload, plen, seq);
        }
        /* The plaintext goes, whether or not the storage does: a password sat
           in these bytes, and on the encrypted path the buffer lives on to
           carry the next packet. Exactly `plen` because that is all this
           packet wrote -- a longer earlier packet's tail was scrubbed by its
           own turn through here, and a FAILED axl_ssh_aead_open writes nothing
           at all (it authenticates before it decrypts). */
        axl_ssh_secure_zero(payload, plen);
        axl_free(owned);
        if (drc != AXL_OK) {
            return AXL_ERR;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Connection lifecycle                                                      */
/* ------------------------------------------------------------------------ */

/* accept_rearm arms on_accept, and is defined ahead of it, so it needs this
   prototype. */
static bool
on_accept(AxlTcp *client, AxlStatus st, void *data);

/* Sever the transmit side from `c` (lifetime rules 2-4 and 7 at "Sending"):
   cancel a scheduled flush and channel flush, which hold `c` raw, and cut
   the in-flight send's back-pointer, so its completion -- whenever and
   however it is delivered -- only frees its ticket. The ticket itself stays
   on_sent's to free. */
static void
tx_detach(
    SshConn *c  ///< connection being torn down
)
{
    if (c->tx_flush != 0) {
        axl_defer_cancel(c->srv->loop, c->tx_flush);
        c->tx_flush = 0;
    }
    if (c->chan_flush != 0) {
        axl_defer_cancel(c->srv->loop, c->chan_flush);
        c->chan_flush = 0;
    }
    c->chan_flush_lost = false;
    if (c->tx_ticket != NULL) {
        c->tx_ticket->c = NULL;
        c->tx_ticket    = NULL;
    }
}

/* Everything of the APPLICATION's a connection holds: every open channel's
   close hook, then its principals. From conn_free, and from conn_linger when
   a DISCONNECT starts the linger -- whichever comes first; the second finds
   nothing left to release. */
static void
conn_release_app(
    SshConn *c  ///< connection
)
{
    /* Every open channel's close hook, BEFORE release_principal: a principal
       a channel holds is still valid in its close hook. */
    chan_close_all(c);
    /* Principals adopted with ACCEPT are released exactly once. The recorded-
       but-unapplied one exists between a verdict recorded inside a hook and
       issue_request applying it -- and, because a hook may run the loop, it
       is exactly what a connection dropped DURING its hook still holds:
       issue_request never applies that verdict, so this is its release. */
    AxlSshServer *s = c->srv;
    if (s->auth_ops.release_principal != NULL) {
        /* Counted like a hook (app_call_begin): it is application code, it
           may run the loop, and the frames beneath it -- conn_drop's, and
           whichever walker called it -- still hold `s`. Taken off the
           connection BEFORE the call, so a release the loop reaches again
           from inside it finds nothing to release twice. */
        void *p = c->principal;
        c->principal = NULL;
        if (p != NULL) {
            app_call_begin(s, false, NULL);
            s->auth_ops.release_principal(p, s->auth_ud);
            app_call_end(s, false);
        }
        p = c->auth_principal;
        c->auth_principal = NULL;
        if (p != NULL) {
            app_call_begin(s, false, NULL);
            s->auth_ops.release_principal(p, s->auth_ud);
            app_call_end(s, false);
        }
    }
}

static void
conn_free(SshConn *c)
{
    if (c == NULL) {
        return;
    }
    /* The transmit side FIRST, before release_principal below can run the
       loop. conn_drop has already detached it before its close; this covers
       on_accept's setup failure, which frees before it closes, so the
       ticket may still be pending on a socket nobody has closed yet. */
    tx_detach(c);
    axl_ssh_txq_free(&c->txq);
    conn_release_app(c);
    axl_free(c->auth_banner);
    /* The one free of the inbound payload buffer, because this is the one free
       of the connection: every teardown path -- a receive error, the peer's
       FIN, the reaper, the reply timer's drop sweep, drop_all during
       axl_ssh_server_free, on_accept's own setup failure, and the DEFERRED
       drop of a connection whose hook was on the stack -- arrives here, and
       none of them is allowed to free `c` anywhere else. Scrubbed first: the
       last packet's turn through pump zeroed what it wrote, but that reasoning
       is about a caller elsewhere, and the cost of not depending on it is one
       memset of dead storage per connection. Guarded because
       axl_ssh_secure_zero is NOT NULL-safe (axl_free is), and NULL is the
       ordinary case here: every connection dropped before NEWKEYS -- a scan, a
       failed key exchange, on_accept's setup failure -- never had one. */
    if (c->payload != NULL) {
        axl_ssh_secure_zero(c->payload, PAYLOAD_MAX);
        axl_free(c->payload);
    }
    axl_string_free(c->in);
    axl_string_free(c->v_c);
    axl_string_free(c->i_c);
    axl_string_free(c->i_s);
    /* Key material, not bookkeeping -- and scrubbed with the unelidable
       primitive, not axl_memset: this storage is dead the instant the free
       below runs, which is exactly the store a compiler is entitled to
       remove. It survives today only because axl_memset happens to be an
       out-of-line function and nothing builds with -flto. */
    axl_ssh_dirkey_clear(&c->tx_key);
    axl_ssh_dirkey_clear(&c->rx_key);
    /* Held plaintext: connection-layer replies, scrubbed like the rest. */
    axl_ssh_string_free_secure(c->held);
    axl_ssh_secure_zero(c->session_id, sizeof c->session_id);
    axl_ssh_rng_pool_clear(&c->pad_pool);
    axl_free(c);
}

/* Charge a connection that is ending to its source (Task P). Called from
   conn_drop at the REAL unlink -- never for a drop it defers, which comes
   back through conn_drop once its hook returns -- so each connection accrues
   exactly once. Nothing while the server is stopping: drop_all's teardown is
   not the peer's doing. Nothing without a peer address or a clock reading,
   the rule the login grace follows: a decision is never made from a value
   that could not be read. */
static void
penalty_accrue(SshConn *c)
{
    AxlSshServer *s   = c->srv;
    uint64_t      now = 0;

    if (!s->running || !c->have_peer || !read_now_ms(&now)) {
        return;
    }
    AxlSshConnOutcome o = {
        .authenticated = c->authenticated,
        .grace_expired = c->pen_grace_expired,
        .refused       = c->pen_refused,
        .forged        = c->pen_forged,
        .server_fault  = c->pen_server_fault,
    };
    AxlSshPenaltyEvent  ev        = axl_ssh_penalty_classify(&o);
    bool                activated = false;
    uint64_t            rem       = 0;
    AxlSshPenaltyRecord r = axl_ssh_penalty_record(&s->penalty, c->peer_addr,
                                                   now, ev, &activated, &rem);
    if (r == AXL_SSH_PENALTY_SKIPPED) {
        return;
    }
    char     addr[16];
    unsigned secs = (unsigned)((rem + 999u) / 1000u);
    if (axl_ipv4_format(c->peer_addr, addr, sizeof addr) != AXL_OK) {
        axl_strlcpy(addr, "?", sizeof addr);
    }
    if (activated) {
        /* log-level: a penalty activating is an audit event with no caller
           to report it to -- the connection is gone, and no return value
           carries it -- and it is the only visibility the header promises
           an operator. Bounded: a source activates at most once per
           penalty, since it must burn down to nothing first. Each refused
           connection logs at DEBUG only (on_accept). */
        axl_info("activating penalty of %u seconds for %s: %s", secs, addr,
                 axl_ssh_penalty_cause((uint8_t)ev));
    } else if (r == AXL_SSH_PENALTY_DROPPED
               && axl_ssh_penalty_drop_notice(&s->penalty, now)) {
        /* log-level: a full table is a BYPASS -- a source that cannot be
           recorded is not throttled at all -- and nothing else reports it.
           Bounded by axl_ssh_penalty_drop_notice to one line per max_sec,
           so a crowded table is visible at the default level without a
           peer turning its own drops into a console flood (ruling P5). */
        axl_info("penalty table full (%u sources): %s not tracked, so not"
                 " throttled (%s)", (unsigned)AXL_SSH_PENALTY_SOURCES, addr,
                 axl_ssh_penalty_cause((uint8_t)ev));
    } else if (r == AXL_SSH_PENALTY_DROPPED) {
        axl_debug("penalty table full: %s not recorded (%s)", addr,
                  axl_ssh_penalty_cause((uint8_t)ev));
    } else {
        axl_debug("penalty of %u seconds for %s: %s", secs, addr,
                  axl_ssh_penalty_cause((uint8_t)ev));
    }
}

/* Re-arm the accept if something paused it. Only MAX_ACCEPT_ERRS pauses it
   -- capacity never does (admission by eviction, spec section 12a.2) -- or
   a re-arm that failed, so this is the one re-arm: called from conn_drop,
   once the loop has done something other than fail an accept, and on every
   reap tick, to retry a lost re-arm. Guarded on `running` so a teardown in
   progress does not resurrect the listener, and a no-op inside on_accept,
   where `accepting` is true.

   A failure is logged at ERROR ONCE PER EPISODE and at DEBUG after that,
   until a re-arm succeeds: the reaper retries every tick, which is up to
   twice a second under a short login grace, and a line per retry would
   turn one broken listener into a console flood that buries the first
   report. */
static void
accept_rearm(AxlSshServer *s)
{
    if (!s->running || s->accepting || s->listener == NULL) {
        return;
    }
    if (axl_tcp_accept_async(s->listener, s->loop, NULL, on_accept, s)
            == AXL_OK) {
        s->accepting        = true;
        s->accept_errs      = 0;
        s->rearm_err_logged = false;
        axl_debug("accept re-armed");
    } else if (!s->rearm_err_logged) {
        s->rearm_err_logged = true;
        axl_error("ssh: could not re-arm accept; server is not listening"
                  " (retried every reap tick; further failures at debug)");
    } else {
        axl_debug("accept re-arm failed again; still not listening");
    }
}

/* Unlink from the server's list and destroy.

   EVERY CALLER THAT IS NOT RESPONDING TO A PEER FIN MUST PASS
   AXL_TEARDOWN_RESET, and that is a denial-of-service fix rather than a
   preference. An ACTIVE graceful close issued from inside a receive callback
   -- while our own ident and KEXINIT are still unread by a hostile peer --
   does not return: the loop pegs a core and stops dispatching timers, so ONE
   18-byte connection sending `SSH-1.99-x` wedged the whole event loop until
   reboot. Measured, and measured again with RESET to confirm it is the
   discriminator; test-ssh-hostile-qemu.sh is the regression guard, and its
   liveness assertion is that the guest reaches its own deadline, because a
   spinning loop still ACCEPTS connections and would pass a weaker probe.

   The two passive paths in on_recv (recv error, and a zero-length read) are
   graceful and correct: the peer closed first, so there is nothing to flush
   and no active close to hang on.

   RFC 4253 section 11.1 wants the connection closed immediately after
   DISCONNECT anyway, so RST is also the right protocol answer -- with the
   caveat recorded at disconnect(): the RST discards the DISCONNECT packet,
   so the peer learns from the reset, not from the reason code. */
static void
conn_drop(SshConn *c, AxlTeardown how)
{
    AxlSshServer *s = c->srv;

    /* NOT WHILE ITS HOOK IS ON THE STACK. A hook may run the loop
       (AxlSshAuthOps; include/axl/axl-loop.h documents a nested wait from any
       source callback as safe), and while it does, the reaper, a receive
       error or the reply timer can reach here for the very connection
       issue_request, pump and on_recv are still using further down the
       stack. Freeing it then is a use-after-free in all of them. Closing the
       socket now would not be safe either, even on its own: the ordinary
       conn_drop that runs once the hook returns -- on_recv and
       on_reply_timer both call disconnect() and then conn_drop() again on a
       pump() failure -- would have that disconnect() send on a socket
       already closed, and that conn_drop() call would close it a second
       time.

       So this is the ONE place the rule is enforced, for every caller: mark
       the connection finished and leave it listed, closed by nobody.
       issue_request sees ST_DEAD when the hook returns and fails the request
       upward, and the frame that called pump drops it again, now with no
       hook on the stack: on_recv and on_reply_timer through here,
       axl_ssh_auth_complete through schedule_drop.

       The test is per connection rather than the server-wide hook count
       app_call_begin keeps, because nothing else needs deferring. The only
       frames here that hold a connection across application code are
       issue_request and its callers, which sit under THAT connection's own
       hook -- so its own flag covers them. conn_free's release_principal runs
       on a connection already unlinked and closed, and the list walkers
       rescan (on_reap, on_reply_timer, drop_all). Deferring every OTHER
       connection's drop for the length of a slow hook would hold their slots
       hostage to it. */
    if (c->auth_in_hook || c->chan_hooks > 0 || c->releasing) {
        axl_debug("drop deferred: %s is running",
                  c->auth_in_hook ? "an authentication hook"
                  : c->chan_hooks > 0 ? "a channel hook"
                                      : "the linger's release");
        /* A DISCONNECT already sent keeps its linger: the drop that comes
           back once the hook returns starts it. */
        if (!conn_over(c)) {
            c->state = ST_DEAD;
        }
        c->drop_deferred = true;
        return;
    }
    /* A drop reached from inside this one's own close -- the frames below
       still hold `c`, and a second pass would free it under them. Nothing
       reaches here that way while rule 3 at "Sending" holds, so this is the
       tripwire that says it stopped holding, not a path: logged, and refused
       rather than freeing twice. */
    if (c->dropping) {
        axl_error("conn_drop re-entered for a connection already being"
                  " dropped");
        return;
    }
    /* A DISCONNECT on a healthy stream is followed by the linger, not a
       reset: the connection stays listed until its peer hangs up or the
       bound passes, and THAT drop is the real one. Not while the server is
       stopping -- drop_all must end everything now -- and not once the
       stream has lost a run since the DISCONNECT was queued (midpoint review
       MP1: a run in flight ahead of it failed before this drop came, through
       schedule_drop or a drop deferred under a hook), since the peer cannot
       decrypt it. */
    if (axl_ssh_linger_due(c->state == ST_LINGER, c->lingering, s->running,
                           c->tx_broken)) {
        conn_linger(c);
        return;
    }
    c->dropping = true;
    for (SshConn **pp = &s->conns; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            s->nconns--;
            break;
        }
    }
    if (!c->pen_accrued) {
        penalty_accrue(c);
    }
    /* The state lifetime rule 3 is about, made visible: frames waiting behind
       a send whose callback has not run. test-ssh-send-teardown-qemu.sh
       counts this line to prove it built that state. */
    if (c->tx_ticket != NULL && c->txq.pending_frames > 0) {
        axl_debug("drop: %u frame(s) queued behind a send in flight",
                  c->txq.pending_frames);
    }
    /* BEFORE the close, which delivers every send callback it still holds --
       including a FINISHED send's, with AXL_OK or AXL_ERR, not AXL_CANCELLED
       (rule 3 at "Sending"). Marked dead and detached, the connection is
       invisible to that completion. */
    c->state = ST_DEAD;
    tx_detach(c);
    axl_tcp_close(c->sock, how);
    conn_free(c);

    /* Re-arm an accept that MAX_ACCEPT_ERRS paused, now that the loop has
       done something other than fail an accept. A failed re-arm is logged
       there, and the reaper retries it on its next tick. */
    accept_rearm(s);
}

/* ------------------------------------------------------------------------ */
/* The linger (P5): a DISCONNECT is delivered, or the connection is reset    */
/* ------------------------------------------------------------------------ */

/* A lingering connection to evict for a new arrival: the one whose deadline
   comes first, in any state. NULL when none lingers. */
static SshConn *
linger_victim(
    const AxlSshServer *s  ///< server
)
{
    SshConn *v = NULL;

    for (SshConn *c = s->conns; c != NULL; c = c->next) {
        if (c->lingering && !c->drop_deferred
            && (v == NULL || c->linger_due_ms < v->linger_due_ms)) {
            v = c;
        }
    }
    return v;
}

static bool
on_linger_timer(void *data);

/* Arm the server's ONE linger timer for the earliest deadline (the
   rearm_reply_timer pattern; contract review M4). NEVER a linger without a
   deadline: when the timer cannot be armed, every lingering connection is
   reset now. */
static void
rearm_linger_timer(
    AxlSshServer *s  ///< server
)
{
    if (!s->running) {
        return;
    }
    bool     any      = false;
    uint64_t earliest = 0;
    for (SshConn *c = s->conns; c != NULL; c = c->next) {
        if (c->lingering && !c->drop_deferred
            && (!any || c->linger_due_ms < earliest)) {
            earliest = c->linger_due_ms;
            any      = true;
        }
    }
    if (s->linger_timer != 0 && (!any || s->linger_timer_due_ms != earliest)) {
        axl_loop_remove_source(s->loop, s->linger_timer);
        s->linger_timer = 0;
    }
    if (!any || s->linger_timer != 0) {
        return;
    }
    uint64_t now   = 0;
    uint64_t delay = 1;   /* axl_loop_add_timeout refuses 0 */
    if (read_now_ms(&now) && earliest > now) {
        delay = earliest - now;
    }
    s->linger_timer = axl_loop_add_timeout(s->loop, (uint32_t)delay,
                                           on_linger_timer, s);
    s->linger_timer_due_ms = earliest;
    if (s->linger_timer != 0) {
        return;
    }
    /* Rescans after each drop: nothing of the application's is left on a
       lingering connection, but the walk holds no pointer across one all the
       same. */
    for (;;) {
        SshConn *c = linger_victim(s);
        if (c == NULL) {
            return;
        }
        axl_debug("linger: cannot arm the deadline; reset");
        conn_drop(c, AXL_TEARDOWN_RESET);
    }
}

/* The bound has passed for every connection due by now: reset them. */
static bool
on_linger_timer(void *data)
{
    AxlSshServer *s        = (AxlSshServer *)data;
    uint64_t      now      = 0;
    bool          have_now = read_now_ms(&now);

    s->linger_timer = 0;   /* one-shot: a rearm below adds a fresh one */
    for (;;) {
        SshConn *c = s->conns;
        while (c != NULL
               && !(c->lingering && !c->drop_deferred
                    && (!have_now || now >= c->linger_due_ms))) {
            c = c->next;
        }
        if (c == NULL) {
            break;
        }
        axl_debug("linger: no close within %u ms; reset",
                  (unsigned)AXL_SSH_DISCONNECT_LINGER_MS);
        conn_drop(c, AXL_TEARDOWN_RESET);
    }
    rearm_linger_timer(s);
    return AXL_SOURCE_REMOVE;
}

/* Start the linger of a connection whose DISCONNECT went out on a healthy
   stream -- from conn_drop, in place of the reset, once no hook of its own is
   on the stack.

   THE APPLICATION'S RESOURCES GO NOW, not at the end (contract review H2):
   the channel close hooks and the principals. The linger is a bare socket
   and a deadline. A drop reached from inside them -- they may run the loop
   -- is deferred (`releasing`), and done here once they return.

   THE PENALTY ACCRUES NOW (M6): the verdict is known, and accruing it a
   second later would let the same source's next connections in meanwhile.

   THE RECEIVE MUST BE ARMED, or the peer's hang-up is never seen and every
   linger runs its full bound: a parked receive is armed again, unless an
   on_recv of this connection is on the stack -- that one re-arms itself on
   return (recv_after_drop). */
static void
conn_linger(SshConn *c)
{
    AxlSshServer *s   = c->srv;
    uint64_t      now = 0;

    c->lingering     = true;
    c->drop_deferred = false;
    /* The deadline FIRST, before anything below can run the loop
       (midpoint review MP2): conn_release_app may dispatch it, and a
       lingerer whose deadline still read 0 would be the first thing
       on_linger_timer reset and linger_victim evicted -- a DISCONNECT never
       given its second. */
    if (!read_now_ms(&now)) {
        axl_debug("linger: no clock for the deadline; reset");
        conn_drop(c, AXL_TEARDOWN_RESET);
        return;
    }
    c->linger_start_ms = now;
    c->linger_due_ms   = now + AXL_SSH_DISCONNECT_LINGER_MS;
    /* A verdict that arrives now is for a finished connection: withdrawn,
       so axl_ssh_auth_complete answers AXL_NOT_FOUND and the caller keeps
       (and releases) its principal, rather than applying it and dropping the
       connection out of its linger. */
    c->auth_req      = 0;
    c->reply_armed   = false;
    c->drop_due      = false;
    c->resume_due    = false;
    if (c->chan_flush != 0) {
        axl_defer_cancel(s->loop, c->chan_flush);
        c->chan_flush = 0;
    }
    c->chan_flush_lost = false;
    penalty_accrue(c);
    c->pen_accrued = true;

    c->releasing = true;
    conn_release_app(c);
    c->releasing = false;
    if (c->drop_deferred || !s->running) {
        /* Dropped while it released -- the peer's hang-up, a server being
           freed. That drop is the real one; lingering is already set. */
        c->drop_deferred = false;
        conn_drop(c, AXL_TEARDOWN_RESET);
        return;
    }
    if (c->rx_parked && c->in_recv == 0) {
        c->rx_parked = false;
        if (axl_tcp_recv_async(c->sock, c->rx, sizeof c->rx, s->loop, NULL,
                               on_recv, c) != AXL_OK) {
            axl_debug("linger: cannot re-arm the receive; reset");
            conn_drop(c, AXL_TEARDOWN_RESET);
            return;
        }
    }
    rearm_linger_timer(s);
}

/* A receive on a connection whose DISCONNECT has gone out: discard what the
   peer sends, and end the linger -- with a reset, always -- when it hangs up.
   A FIN would buy nothing (the peer has read the DISCONNECT and gone) and
   would hand the firmware an unbounded retransmission tail for a peer that
   then goes silent (contract review H3). */
static bool
linger_recv(SshConn *c, AxlTcp *sock, AxlStatus st)
{
    if (st == AXL_OK && axl_tcp_recv_get_size(sock) > 0) {
        return true;   /* discarded; the deadline does not move */
    }
    /* The peer's FIN arrives either way: as a zero-length read, or -- when
       the firmware's Receive fails at once with EFI_CONNECTION_FIN -- as a
       failed one, which only _axl_tcp_recv_fin tells apart from a reset. */
    uint64_t now = 0;
    if (st != AXL_OK && !_axl_tcp_recv_fin(sock)) {
        axl_debug("linger: receive error; reset");
    } else if (c->disc_sent) {
        unsigned ms = c->lingering && read_now_ms(&now) && now >= c->linger_start_ms
                      ? (unsigned)(now - c->linger_start_ms) : 0u;
        /* "sent": handed to TCP and its send completed -- EDK2 completes a
           Transmit when it takes the data, not when the peer ACKs it, so
           this is not proof the peer READ it (its transcript is). */
        axl_debug("linger: peer closed after %u ms, disconnect sent", ms);
    } else {
        axl_debug("linger: peer closed before the disconnect was sent");
    }
    /* The real drop, whether or not the linger had started: a connection
       still waiting for its conn_drop (schedule_drop) is ended here too. */
    c->lingering = true;
    conn_drop(c, AXL_TEARDOWN_RESET);
    return false;
}

/* After on_recv dropped its connection: whether to re-arm the receive. True
   only when the connection is still listed and lingering -- its peer's
   hang-up is what ends the linger. Found again by its accept sequence
   number, because the drop may have freed it; a connection still listed but
   NOT lingering (a drop deferred under its hook) is marked parked, so the
   linger, if it starts later, arms the receive itself. */
static bool
recv_after_drop(
    AxlSshServer *s,   ///< the connection's server
    uint64_t      seq  ///< its accept_seq
)
{
    if (!server_is_live(s)) {
        return false;
    }
    for (SshConn *c = s->conns; c != NULL; c = c->next) {
        if (c->accept_seq == seq) {
            c->in_recv--;
            if (c->lingering) {
                return true;
            }
            c->rx_parked = true;
            return false;
        }
    }
    return false;
}

/* Drop every connection the server still has, EXCEPT one whose own hook is on
   the stack: conn_drop marks that one ST_DEAD and leaves it listed for the
   frame beneath its hook to drop, and skipping it here is what keeps this
   finite (re-reading the head instead spun for ever -- P3 security review
   MED-1). Rescans after every drop and holds no next pointer, like on_reap:
   a release_principal inside conn_free may run the loop, and anything the
   loop runs may drop another connection. */
static void
drop_all(AxlSshServer *s)
{
    for (;;) {
        SshConn *c = s->conns;
        while (c != NULL && c->drop_deferred) {
            c = c->next;
        }
        if (c == NULL) {
            return;
        }
        /* Abortive, like Axl9pServer's teardown: the loop may be about to go
           away, and a FIN would defer each finalize onto it. */
        if (c->lingering) {
            axl_debug("linger: server freed; reset");
        }
        conn_drop(c, AXL_TEARDOWN_RESET);
    }
}

static bool
on_recv(AxlTcp *sock, AxlStatus st, void *data)
{
    SshConn *c = (SshConn *)data;

    if (c->state == ST_LINGER) {
        return linger_recv(c, sock, st);
    }

    /* RESET on the peer-closed paths too, not just the error ones. A server
       ALWAYS has unread outbound data on a connection it is dropping -- our
       identification line and KEXINIT go out the moment we accept, before the
       peer has said anything -- and a graceful EFI_TCP4.Close() must flush
       them before it completes. When the peer is already gone that flush
       never completes and Close() spins in firmware: one core pegged, the
       loop dead inside a single callback (axl-tcp-sync.c documents exactly
       this shape for the raised-TPL case; it happens at TPL_APPLICATION too
       when the peer has vanished).

       Measured: 70 connections that open, read nothing and close left the
       loop inside one conn_drop -- a 5-second heartbeat fired exactly once
       and a 25-second deadline never arrived. The same flood with RESET
       completes. There is nothing to flush gracefully to a peer that has
       stopped reading, so RST is also the honest teardown. */
    if (st != AXL_OK) {
        conn_drop(c, AXL_TEARDOWN_RESET);
        return false;
    }
    size_t n = axl_tcp_recv_get_size(sock);
    if (n == 0) {
        conn_drop(c, AXL_TEARDOWN_RESET);
        return false;
    }
    /* Any bytes restart the silence clock (serverloop.c:239-240), whether
       or not they complete a packet. A no-op until authentication. */
    {
        uint64_t now      = 0;
        bool     have_now = read_now_ms(&now);
        axl_ssh_idle_heard(&c->idle, have_now, now);
    }
    /* RESET, not GRACEFUL, on every path WE initiate -- see conn_drop. */
    if (axl_string_len(c->in) + n > RX_BACKLOG_MAX
        || axl_string_append_len(c->in, (const char *)c->rx, n) != AXL_OK) {
        disconnect_as(c, AXL_SSH_DISCONNECT_PROTOCOL_ERROR, "input backlog",
                      false);
        conn_drop(c, AXL_TEARDOWN_RESET);
        return false;
    }
    AxlSshServer *s   = c->srv;
    uint64_t      seq = c->accept_seq;
    c->in_recv++;
    if (pump(c) != AXL_OK) {
        drop_failed(c);
        return recv_after_drop(s, seq);
    }
    c->in_recv--;
    if (c->tx_paused || c->chan_gated) {
        /* Output backpressure, or a channel hook of this connection running
           the loop (the parse gate): no re-arm, so TCP's window closes on
           the peer and the input waits in the network, not in c->in -- a
           gate that kept receiving would reach RX_BACKLOG_MAX and drop a
           peer still inside its channel window. resume() re-arms it once
           both have cleared (rx_unpark): rx_resume when the queue drains,
           the resume queue after the last hook leaves. */
        c->rx_parked = true;
        return false;
    }
    return true;   /* re-arm with the same buffer */
}

static int
rx_resume(
    SshConn *c  ///< paused connection; not ST_DEAD, its server running
)
{
    /* The peak is the bound actually kept: the pause line's figure, plus
       whatever was queued after it -- nothing the parse makes, while the
       gate holds; only a due authentication reply or a disconnect can add
       to it. */
    axl_debug("tx: resuming input, peak %zu bytes queued", c->tx_peak);
    c->tx_paused = false;
    /* resume() re-arms the parked receive (rx_unpark) -- unless a channel
       hook still holds the gate, whose last leave resumes it instead. What
       arrived before the pause is still in c->in, and goes through pump,
       which re-checks every gate -- the authentication ones too. */
    return resume(c);
}

/* The peer's IPv4 address as octets, through the public AxlTcp getter --
   AxlHttpServer calls the same one from its accept callback. A string round
   trip, because that is what the getter offers, and octets because that is
   what axl_ipv4_equals and axl_ipv4_in_subnet compare: no byte order is
   chosen anywhere. False when either step fails, which refuses nothing. */
static bool
peer_address(AxlTcp *sock, uint8_t out[4], uint16_t *port)
{
    char buf[16];

    return axl_tcp_get_remote_addr(sock, buf, sizeof buf, port) == AXL_OK
           && axl_ipv4_parse(buf, out) == AXL_OK;
}

static bool
on_accept(AxlTcp *client, AxlStatus st, void *data)
{
    AxlSshServer *s = (AxlSshServer *)data;

    if (st != AXL_OK || client == NULL) {
        /* A failed accept is normally transient, so keep listening -- but NOT
           unconditionally. Returning true re-arms immediately, and if the
           listener were in a state where every accept completes with an
           error at once, that would be a tight callback loop: the loop never
           idles, no timer fires, and a core pegs. Bound the consecutive
           failures.

           This bound is DEFENSIVE; it was never the fix for anything
           observed. It was added while chasing the 113 s wedge after a
           70-connection flood, and 58fde4dc's own message lists "a failing
           accept re-arms in a tight callback loop" among the three WRONG
           diagnoses of that wedge: the bound never tripped. The wedge was a
           graceful close spinning in firmware -- see the admission comment
           below. */
        if (++s->accept_errs >= MAX_ACCEPT_ERRS) {
            axl_error("accept failed %u times consecutively; pausing accept",
                      s->accept_errs);
            s->accepting = false;
            return false;
        }
        return true;
    }
    s->accept_errs = 0;
    if (!s->running) {
        axl_tcp_close(client, AXL_TEARDOWN_RESET);
        return false;
    }

    /* Per-source penalty (axl_ssh_server_set_penalties), decided BEFORE any
       protocol work and before anything is allocated. A refused connection
       is closed abortively with nothing sent and costs no slot: nconns does
       not move and returning true keeps the accept armed, so a peer
       reconnecting through its own penalty can neither pause the listener
       nor pin a connection. The close is CHEAP to make here, and the reason
       is not the abort cap: EDK2 completes an abortive close synchronously,
       inside Close() (TcpOnAppAbort -> TcpSetState(CLOSED) ->
       SockConnClosed signals the token), so close_wait_inline returns after
       one Poll and one CheckEvent with no timed wait at all. The ~1 s cap
       (AXL_TCP_ABORT_WAIT_US, about 1.01 s plus 101 Polls) is only the bound
       for a firmware that completes an abort asynchronously, and it would
       NOT make a knock flood safe -- 70 refusals at the cap would be 70 s.
       A GRACEFUL close here would be the stall: see the admission comment
       below.
       Without a clock reading or a peer address nothing is refused
       (and penalty_accrue records nothing), the rule axl_ssh_reap_reason
       follows: never decide from a value that could not be read. */
    uint8_t  peer[4]   = { 0, 0, 0, 0 };
    uint16_t peer_port = 0;
    uint64_t now       = 0;
    bool     have_now  = read_now_ms(&now);
    bool     have_peer = peer_address(client, peer, &peer_port);
    const AxlSshPenaltySource *hit = NULL;
    if (have_now && have_peer
        && axl_ssh_penalty_check(&s->penalty, peer, now, &hit)) {
        char addr[16];
        if (axl_ipv4_format(peer, addr, sizeof addr) != AXL_OK) {
            axl_strlcpy(addr, "?", sizeof addr);
        }
        /* DEBUG, not INFO: a peer reconnecting in a loop would turn its
           own refusals into a console flood. The activation was already
           logged at INFO, once (penalty_accrue). */
        axl_debug("drop connection from %s:%u penalty: %s", addr,
                  (unsigned)peer_port,
                  axl_ssh_penalty_cause(hit != NULL ? hit->cause : 0));
        axl_tcp_close(client, AXL_TEARDOWN_RESET);
        return true;
    }

    /* Admission (spec section 12a.2): after the penalty refusal, so a
       penalised source can never evict anyone, and before anything is
       allocated for the arrival. Under both caps it is admitted outright.
       Otherwise the unauthenticated connection least far along in key
       exchange is evicted to make room (eviction_victim, which says why
       progress ranks before age), and with no such victim the arrival is
       refused. So a flood that says nothing cannot displace a client that
       has sent its KEXINIT, and the accept stays armed throughout --
       capacity no longer pauses it.

       BOTH CLOSES ARE ABORTIVE, and that is what makes this cheap enough to
       do here. The accept used to PAUSE at MAX_CONNS instead, because a close
       from inside this callback was once measured not to return; that is
       true only of a GRACEFUL one. The measurement (58fde4dc: a
       70-connection flood, the loop inside one axl_tcp_close for 113 s, a
       5-second heartbeat firing once) was a graceful close with our
       identification line and KEXINIT still unflushed to a peer that had
       stopped reading -- the flush never completes and Close() spins in
       firmware. Even with nothing unflushed, a graceful close of a socket
       fresh from accept would stall: it has no async loop, so it cannot
       defer and waits inline for the FIN handshake -- a round trip, 500 ms
       for a peer that does not close, up to ~3 s for one that never ACKs. An ABORTIVE close has no such wait:
       EDK2 completes it inside Close() (the penalty comment above has the
       chain), and in 58fde4dc five of them produced no measurable spike.
       test-ssh-hostile-qemu.sh's penalty knocks measure a refusal of exactly
       this shape at a median of ~20 ms unloaded on both arches, and
       test-ssh-admission-qemu.sh drives a 70-connection flood through the
       eviction path and requires the server to answer afterwards.

       The victim's conn_drop is safe here. A victim is before ST_AUTH, so
       no hook of its own is on the stack and the drop is not deferred; its
       penalty accrues through penalty_accrue as `noauth`, like any
       connection that ends unauthenticated; and conn_drop's accept_rearm is
       a no-op, since `accepting` is true inside this callback. */
    /* A lingering connection is the first to go, in any state (contract
       review M6): it is finished, and it holds a slot only so its peer can
       read a DISCONNECT. At the connection cap one makes room before any
       admission decision, so a burst of protocol errors cannot refuse
       arrivals for a second. Not counted as pre-authentication either
       (preauth_count), so they never decide MaxStartups. */
    while (s->nconns >= MAX_CONNS) {
        SshConn *l = linger_victim(s);
        if (l == NULL) {
            break;
        }
        axl_debug("linger: evicted for a new arrival; reset");
        conn_drop(l, AXL_TEARDOWN_RESET);
    }
    SshConn *victim = eviction_victim(s);
    switch (axl_ssh_admit_decide(s->nconns, preauth_count(s), MAX_CONNS,
                                 s->max_startups, victim != NULL)) {
    case AXL_SSH_ADMIT_ACCEPT:
        break;
    case AXL_SSH_ADMIT_EVICT:
        log_admission(s, AUTH_LOG_STARTUP_EVICT, "Evicted connection",
                      victim->have_peer, victim->peer_addr, victim->peer_port,
                      ": past MaxStartups, key exchange unfinished");
        conn_drop(victim, AXL_TEARDOWN_RESET);
        break;
    case AXL_SSH_ADMIT_REFUSE:
    default:
        /* Costs no slot and accrues nothing: like a penalty refusal, the
           arrival never became a connection. */
        log_admission(s, AUTH_LOG_STARTUP_REFUSE, "Refused connection",
                      have_peer, peer, peer_port, ": past MaxStartups");
        axl_tcp_close(client, AXL_TEARDOWN_RESET);
        return true;
    }

    SshConn *c = axl_malloc(sizeof *c);
    if (c == NULL) {
        axl_tcp_close(client, AXL_TEARDOWN_RESET);
        return true;
    }
    axl_memset(c, 0, sizeof *c);
    axl_ssh_rng_pool_init(&c->pad_pool);
    c->srv       = s;
    c->sock      = client;
    c->state     = ST_IDENT;
    c->have_peer = have_peer;
    c->peer_port = peer_port;
    /* The cap this connection will be held to, fixed here for its whole life
       -- see the field comment and axl_ssh_server_set_max_password_failures. */
    c->max_password_failures = s->max_password_failures;
    c->max_sessions          = s->max_sessions;
    axl_ssh_rekey_init(&c->rekey, s->rekey_limit);
    axl_memcpy(c->peer_addr, peer, sizeof c->peer_addr);
    /* An unarmed deadline is NOT a free pass: the reaper drops an
       unauthenticated connection it cannot time, on its next tick.
       axl_ssh_server_listen refuses to bind without a readable clock, so
       reaching the else here means the counter died after the bind. */
    if (have_now) {
        c->deadline_ms    = now + (uint64_t)s->login_grace_sec * 1000u;
        c->deadline_armed = true;
    } else {
        axl_debug("monotonic clock unreadable: this connection gets no"
                  " login grace and will be reaped");
    }
    c->in    = axl_string_new("");
    c->v_c   = axl_string_new("");
    c->i_c   = axl_string_new("");
    c->i_s   = axl_string_new("");
    c->held  = axl_string_new("");
    if (c->in == NULL || c->v_c == NULL || c->i_c == NULL || c->i_s == NULL
        || c->held == NULL || axl_ssh_txq_init(&c->txq) != AXL_OK) {
        conn_free(c);
        axl_tcp_close(client, AXL_TEARDOWN_RESET);
        return true;
    }
    c->accept_seq = ++s->next_accept_seq;
    c->next  = s->conns;
    s->conns = c;
    s->nconns++;

    /* Our identification line, then our KEXINIT, before the peer has said
       anything: RFC 4253 lets both sides send first, and ssh(1) does. Both
       go through the transmit queue, which is what orders the line ahead of
       the packet, and they leave together in the connection's first send. */
    static const char ident[] = AXL_SSH_IDENT "\r\n";
    if (tx_queue_raw(c, ident, sizeof ident - 1) != AXL_OK
        || axl_ssh_kexinit_build(c->i_s, true) != AXL_OK
        || send_packet(c, axl_string_data(c->i_s),
                       axl_string_len(c->i_s)) != AXL_OK
        || axl_tcp_recv_async(client, c->rx, sizeof c->rx, s->loop, NULL,
                              on_recv, c) != AXL_OK) {
        /* Our own failure -- a full source table, an allocation -- before
           the peer has said a word: charge it nothing (ruling P6). */
        c->pen_server_fault = true;
        conn_drop(c, AXL_TEARDOWN_RESET);
        return true;
    }

    return true;
}

/* See axl-ssh-reap.h. */
bool
axl_ssh_linger_due(bool is_linger, bool lingering, bool running, bool tx_broken)
{
    return is_linger && !lingering && running && !tx_broken;
}

/* The server's test seam (axl-ssh-internal.h). Production's answer at every
   point: never. */
__attribute__((weak)) bool
_axl_ssh_server_test_point(const char *point)
{
    (void)point;
    return false;
}

/* See axl-ssh-reap.h: the whole login-grace decision, pulled out as a pure
   function so the unit test can pin it without a fake clock or a live
   connection. on_reap() below is the only production caller. */
const char *
axl_ssh_reap_reason(bool authenticated, bool deadline_armed, bool have_now,
                    uint64_t now, uint64_t deadline_ms)
{
    if (authenticated) {
        return NULL;
    }
    if (!deadline_armed) {
        return "no enforceable login grace";
    }
    /* A failed reading here is NOT a reason to drop anything: `now` would be
       meaningless. The subtraction this predicate used to do underflowed to
       ~2^64 on a zero reading and dropped every unauthenticated connection
       in flight -- self-healing, but the wrong direction. */
    if (have_now && now > deadline_ms) {
        return "login grace expired";
    }
    return NULL;
}

/* See axl-ssh-admit.h: the whole admission decision, pulled out as a pure
   function so the unit test can pin it without a live connection list.
   on_accept()'s admission check is the only production caller. */
AxlSshAdmit
axl_ssh_admit_decide(unsigned nconns, unsigned npreauth, unsigned max_conns,
                     unsigned max_startups, bool have_victim)
{
    if (nconns < max_conns && npreauth < max_startups) {
        return AXL_SSH_ADMIT_ACCEPT;
    }
    return have_victim ? AXL_SSH_ADMIT_EVICT : AXL_SSH_ADMIT_REFUSE;
}

/* Drop connections that have sat too long without authenticating.

   A slot held by a peer that never speaks is a slot no honest client can
   have. Admission evicts such a peer when a new arrival needs the slot (spec
   section 12a.2), but a peer past key exchange is never evicted, and only
   this deadline ends one that then stalls. One deadline covers
   accept -> AUTHENTICATED rather than stopping at NEWKEYS: a peer that
   completes key exchange and then says nothing costs exactly the same slot,
   and two timers is one more thing that can disagree with itself. */
/* Send sshd's keepalive probe (serverloop.c client_alive_check): on the
   probe-target channel when there is one (axl_ssh_idle_probe_slot), else
   as a global request. Through send_packet, so a probe produced during our
   own key re-exchange is held and sealed after our NEWKEYS like any other
   connection-layer packet. Returns AXL_ERR when it could not be queued --
   an allocation, or the held queue past REKEY_HELD_MAX -- and the caller
   then drops the connection. */
static int
send_keepalive(
    SshConn *c  ///< authenticated connection
)
{
    int        slot = axl_ssh_idle_probe_slot(c->chans, CHAN_CAP);
    AxlString *m    = axl_string_new("");

    if (m == NULL) {
        return AXL_ERR;
    }
    axl_debug("idle: keepalive probe %u, as a %s request",
              (unsigned)c->idle.unanswered, slot >= 0 ? "channel" : "global");
    return chan_seal(c, m, axl_ssh_keepalive_build(
                               m, slot >= 0,
                               slot >= 0 ? c->chans[slot].peer_id : 0u));
}

/* One idle tick for @a c (axl-ssh-idle.h), acting on the decision. Returns
   true when @a c was dropped -- the caller's list walk must then restart
   from the head, as every drop in on_reap does. */
static bool
idle_tick_conn(
    SshConn  *c,         ///< authenticated, not drop_deferred
    bool      have_now,  ///< @a now is a real reading
    uint64_t  now        ///< this sweep's monotonic ms
)
{
    /* What sshd reads off its own state: a channel in use (channels.c
       channel_still_open, plus a channel hook on the stack), whether the
       peer's output can be queued (not tx_paused: section 12a.7
       deviation 8), and whether this server is what holds the input -- a
       channel hook on the stack, the parse gate it left (chan_gated), or a
       parse turn queued behind another connection's authentication hook
       (resume_due: the resume queue is served only while no such hook
       runs, so the peer's answers wait in TCP meanwhile). */
    AxlSshIdleConn conn = {
        .in_use   = axl_ssh_idle_in_use(c->chans, CHAN_CAP, c->chan_hooks),
        .can_send = !c->tx_paused,
        .rx_gated = c->chan_hooks > 0 || c->chan_gated || c->resume_due,
    };

    switch (axl_ssh_idle_tick(&c->idle, &conn, have_now, now)) {
    case AXL_SSH_IDLE_PROBE:
        if (send_keepalive(c) == AXL_OK) {
            return false;
        }
        axl_debug("idle: the keepalive could not be queued; dropping");
        break;
    case AXL_SSH_IDLE_DROP_SILENT:
        /* serverloop.c:117: logged, then cleanup_exit(255) -- no
           DISCONNECT, which is also the only close a peer that has stopped
           reading cannot stall. The DEBUG line says how long the silence
           was ON THE CLOCK THAT TIMED IT -- the console's timestamps are an
           RTC second with an unrelated fraction, which cannot show a
           sub-second bound. */
        axl_debug("idle: dropping, nothing heard for %llu ms",
                  (unsigned long long)(now - c->idle.bytes_ms));
        log_idle_drop(c, AUTH_LOG_IDLE_SILENT, "Timeout, client not responding");
        break;
    case AXL_SSH_IDLE_DROP_UNUSED:
        /* serverloop.c:249, the same way; the DEBUG line likewise, from the
           last restart of the unused clock (login, the last channel freed,
           or the last tick that found one in use). */
        axl_debug("idle: dropping, no channel in use for %llu ms",
                  (unsigned long long)(now - (c->idle.unused_expiry_ms
                                              - (uint64_t)c->idle.unused_sec
                                                    * 1000u)));
        log_idle_drop(c, AUTH_LOG_IDLE_UNUSED, "terminating inactive connection");
        break;
    case AXL_SSH_IDLE_KEEP:
    default:
        return false;
    }
    /* No pen_* flag: the peer authenticated, which the penalty ladder
       never charges (axl_ssh_penalty_classify). */
    conn_drop(c, AXL_TEARDOWN_RESET);
    return true;
}

static bool
on_reap(void *data)
{
    AxlSshServer *s        = (AxlSshServer *)data;
    uint64_t      now      = 0;
    bool          have_now = read_now_ms(&now);

    /* The hook watchdog's BEST-EFFORT half (NEW-1), FIRST: before any drop,
       so the queue depth it reports is what the hook is actually holding up
       and s->hook_conn cannot have been freed under it by a release the
       sweep below runs.

       It rides this sweep rather than a timer of its own, and not to save a
       source: axl_loop_add_timeout creates an EVT_TIMER at TPL_APPLICATION
       with no notify, so a per-hook timer is noticed only when the loop is
       polled -- it would fire exactly where this does and detect nothing
       extra, while competing for AXL_MAX_SOURCES at the moment the server is
       already degraded. axl_ssh_server_listen fails the whole listen if this
       reaper cannot arm, which is the guarantee to build on.

       AT MOST ONE PER HOOK CALL, latched here and cleared in app_call_begin.
       That is strictly tighter than a time window and it removes, rather
       than bounds, the question of what a hook that never returns writes. */
    uint64_t held = 0;
    if (!s->hook_reported && hook_over_budget(s, have_now, now, &held)) {
        s->hook_reported = true;
        hook_report(s, true, held);
    }

    /* Rescans from the head after every drop, holding no `next` pointer, as
       on_reply_timer does: conn_drop's release_principal may run the loop,
       and anything the loop runs may free the connection a saved `next`
       points at. A drop conn_drop had to defer leaves its connection listed,
       so it is skipped rather than found again forever. */
    for (;;) {
        SshConn    *c   = s->conns;
        const char *why = NULL;
        for (; c != NULL; c = c->next) {
            if (c->drop_deferred || c->lingering) {
                continue;   /* a lingerer has its own, shorter, deadline */
            }
            why = axl_ssh_reap_reason(c->authenticated, c->deadline_armed,
                                      have_now, now, c->deadline_ms);
            if (why != NULL) {
                break;
            }
        }
        if (c == NULL) {
            break;
        }
        axl_debug("dropping an unauthenticated connection: %s", why);
        /* An audit line only when the grace genuinely ran out (the reason
           below): an unarmed deadline is the server's own clock failing. */
        if (c->deadline_armed) {
            log_grace_expired(c, have_now, now);
        }
        /* Rule 2 of the penalty ladder only when the grace genuinely ran
           out. axl_ssh_reap_reason drops an unauthenticated connection for
           exactly two reasons, and deadline_armed tells them apart: armed
           means "login grace expired"; unarmed means the clock could not be
           read at accept, which is no fault of the peer's. */
        c->pen_grace_expired = c->deadline_armed;
        c->pen_server_fault  = !c->deadline_armed;   /* ruling P6 */
        /* Abortive, like every other teardown this server initiates: there is
           nothing to flush gracefully to a peer that has not spoken, and
           axl_tcp_close's graceful path would try. */
        conn_drop(c, AXL_TEARDOWN_RESET);
    }

    /* The idle rules over AUTHENTICATED connections (axl-ssh-idle.h), with
       the grace pass's discipline: restart from the head after any drop,
       holding no `next` across one, and skip a drop conn_drop deferred --
       and one disconnect() already marked ST_DEAD, which stays listed until
       the reply timer drops it (axl_ssh_idle_scans).
       The scan can therefore tick a connection twice at this `now`, which
       axl_ssh_idle_tick answers KEEP with nothing changed. */
    for (bool again = true; again;) {
        again = false;
        for (SshConn *c = s->conns; c != NULL; c = c->next) {
            if (!axl_ssh_idle_scans(c->authenticated, c->drop_deferred,
                                    conn_over(c))) {
                continue;
            }
            if (idle_tick_conn(c, have_now, now)) {
                again = true;
                break;
            }
        }
    }

    /* Retry a reply timer that failed to arm. Outside on_reply_timer the id
       is 0 with a reply owed only when axl_loop_add_timeout refused, and the
       login grace above is no backstop for every such reply: a drop owed to
       an ACCEPT whose resume failed is on an AUTHENTICATED connection, which
       the grace exempts, so without this it would hold its slot for ever. */
    if (s->reply_timer == 0) {
        rearm_reply_timer(s);
    }

    /* Retry a channel flush the defer ring had no room for (lifetime rule
       7). Not a flush ON a timer: only one that was due and got lost. */
    for (SshConn *c = s->conns; c != NULL; c = c->next) {
        if (c->chan_flush_lost) {
            chan_schedule(c);
        }
    }

    /* Retry a lost accept re-arm. axl_tcp_accept_async can fail transiently
       (the loop's source table is finite), and without this one failure
       deafens the server permanently -- conn_drop is the only other re-arm,
       and it cannot run when nothing is connected. */
    accept_rearm(s);
    return AXL_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------ */
/* Live servers                                                              */
/* ------------------------------------------------------------------------ */

/* The g_live_servers slot holding @a s, or NULL if none does; NULL for @a s
   finds a free slot. Compares pointers only: @a s may be freed, and nothing
   here reads through it. */
static AxlSshServer **
live_slot(const AxlSshServer *s)
{
    for (unsigned i = 0; i < AXL_SSH_LIVE_SERVERS_MAX; i++) {
        if (g_live_servers[i] == s) {
            return &g_live_servers[i];
        }
    }
    return NULL;
}

/* Record @a s as live. False when every slot is taken. */
static bool
live_add(AxlSshServer *s)
{
    AxlSshServer **slot = live_slot(NULL);
    if (slot == NULL) {
        return false;
    }
    *slot = s;
    return true;
}

/* Forget @a s. One pointer store, so it is never half-done. */
static void
live_remove(const AxlSshServer *s)
{
    AxlSshServer **slot = live_slot(s);
    if (slot != NULL) {
        *slot = NULL;
    }
}

/* Is @a s a server that axl_ssh_server_free has not started freeing? Safe on
   a pointer that has been freed. */
static bool
server_is_live(const AxlSshServer *s)
{
    return s != NULL && live_slot(s) != NULL;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

AxlSshServer *
axl_ssh_server_new(AxlLoop *loop, uint16_t port)
{
    if (loop == NULL) {
        return NULL;
    }
    AxlSshServer *s = axl_malloc(sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    axl_memset(s, 0, sizeof *s);
    s->loop = loop;
    s->port = port;
    s->login_grace_sec = AXL_SSH_LOGIN_GRACE_DEFAULT_SEC;
    s->unused_sec      = AXL_SSH_UNUSED_TIMEOUT_DEFAULT_SEC;
    s->alive_sec       = AXL_SSH_CLIENT_ALIVE_DEFAULT_SEC;
    s->alive_max       = AXL_SSH_CLIENT_ALIVE_DEFAULT_MAX;
    s->max_password_failures = AXL_SSH_PASSWORD_FAILURES_DEFAULT;
    s->max_startups = AXL_SSH_MAX_STARTUPS_DEFAULT;
    s->auth_hook_budget_ms   = AXL_SSH_AUTH_HOOK_BUDGET_DEFAULT_MS;
    s->max_sessions          = (uint8_t)AXL_SSH_MAX_SESSIONS_DEFAULT;
    s->rekey_limit           = AXL_SSH_REKEY_LIMIT_DEFAULT;
    axl_ssh_chan_budget_init(&s->budget, AXL_SSH_CHANNEL_WRITE_LIMIT_DEFAULT,
                             AXL_SSH_CHANNEL_WRITE_TOTAL_DEFAULT,
                             CHAN_MAX_CHANNELS);
    /* ON by default, at OpenSSH's numbers -- as sshd ships it. */
    axl_ssh_penalty_init(&s->penalty);

    /* Failing is the only honest answer below: the single host key
       algorithm we advertise is ssh-ed25519, so there is nothing to fall
       back to, and a server that started without a host key would fail
       every connection at kex instead of at construction.

       The provider cannot be missing: calling axl_pk_enable_ed25519() is
       what links it, so every image that links this function has it. That
       leaves one cause for a NULL -- keygen failing, i.e. the DRBG could
       not seed, which since the entropy poll started failing closed means
       this platform publishes no EFI_RNG_PROTOCOL. */
    axl_pk_enable_ed25519();
    s->host_key = axl_pk_key_new(AXL_PK_ED25519);
    if (s->host_key == NULL) {
        axl_error("host key generation failed -- no usable entropy"
                  " source (see the crypto log for the RNG status)");
        axl_free(s);
        return NULL;
    }
    size_t n = sizeof s->host_pub;
    if (axl_pk_key_get_raw_public(s->host_key, s->host_pub, &n) != AXL_OK
        || n != sizeof s->host_pub) {
        goto fail;
    }
    if (axl_rng_bytes(s->timing_secret, sizeof s->timing_secret) != AXL_OK) {
        axl_error("ssh: no entropy for the failure-timing secret");
        goto fail;
    }
    /* LAST, once nothing below can fail: a server is live only when it is
       whole. Full fails closed -- see g_live_servers. */
    if (!live_add(s)) {
        axl_error("%u servers are already live; refusing another",
                  AXL_SSH_LIVE_SERVERS_MAX);
        goto fail;
    }
    return s;

fail:
    axl_pk_key_free(s->host_key);
    axl_ssh_secure_zero(s->timing_secret, sizeof s->timing_secret);
    axl_free(s);
    return NULL;
}

int
axl_ssh_server_add_authorized_key(AxlSshServer *s, const char *line)
{
    if (s == NULL || line == NULL) {
        return AXL_INVALID;
    }
    /* Not live: axl_ssh_server_free is tearing down this server, or already
       has. The same check axl_ssh_server_listen and axl_ssh_server_set_
       auth_ops make before touching anything past the NULL check (P3
       security re-review NEW-4) -- without it, a config handler composed
       with one that turns SSH off could write s->authkeys through a stale
       pointer. */
    if (!server_is_live(s)) {
        return AXL_ERR;
    }
    return axl_ssh_authkeys_add_line(&s->authkeys, line, axl_strlen(line));
}

int
axl_ssh_server_load_authorized_keys(AxlSshServer *s, const char *path)
{
    if (s == NULL || path == NULL) {
        return AXL_INVALID;
    }
    /* See axl_ssh_server_add_authorized_key. */
    if (!server_is_live(s)) {
        return AXL_ERR;
    }

    void  *raw     = NULL;
    size_t raw_len = 0;
    if (axl_file_get_contents(path, &raw, &raw_len) != AXL_OK) {
        return AXL_ERR;
    }
    /* UTF-8, or UTF-16LE + BOM as the UEFI Shell writes a file. */
    char  *buf = NULL;
    size_t len = 0;
    int    trc = axl_text_to_utf8(raw, raw_len, &buf, &len, NULL);
    axl_free(raw);
    if (trc != AXL_OK) {
        axl_free(buf);
        /* Out of memory is AXL_ERR, as it is for the read above: this
           function's AXL_NO_RESOURCES means "more than 16 keys". */
        return trc == AXL_NO_RESOURCES ? AXL_ERR : trc;
    }

    /* Parse into a SEPARATE set and splice only on complete success. A key
       file applied halfway is worse than one refused: the operator cannot
       tell which half took. */
    AxlSshAuthKeys staged = s->authkeys;
    const char    *p      = buf;
    int            rc     = AXL_OK;
    size_t         start  = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && p[i] != '\n') {
            continue;
        }
        int lrc = axl_ssh_authkeys_add_line(&staged, p + start, i - start);
        if (lrc != AXL_OK && lrc != AXL_NOT_FOUND) {
            rc = lrc;
            break;
        }
        start = i + 1;
    }
    axl_free(buf);
    if (rc != AXL_OK) {
        return rc;
    }
    s->authkeys = staged;
    return AXL_OK;
}

int
axl_ssh_server_set_auth_ops(AxlSshServer *s, const AxlSshAuthOps *ops,
                            void *user_data)
{
    if (s == NULL) {
        return AXL_INVALID;
    }
    /* A server that is not live is one axl_ssh_server_free is tearing down.
       Free closes the listener before it drops connections, so while their
       release_principal runs s->listener is already NULL: without this check
       a hook could swap out the release_principal the remaining connections
       are about to be released through. */
    if (!server_is_live(s) || s->listener != NULL) {
        return AXL_ERR;
    }
    if (ops == NULL) {
        axl_memset(&s->auth_ops, 0, sizeof s->auth_ops);
        s->auth_ud = NULL;
        return AXL_OK;
    }
    s->auth_ops = *ops;
    s->auth_ud  = user_data;
    return AXL_OK;
}

int
axl_ssh_auth_complete(AxlSshServer *s, AxlSshAuthRequest req,
                      AxlSshAuthVerdict verdict, void *principal,
                      const char *banner)
{
    if (s == NULL || req == 0) {
        return AXL_INVALID;
    }
    if (verdict != AXL_SSH_AUTH_ACCEPT && verdict != AXL_SSH_AUTH_DENY
        && verdict != AXL_SSH_AUTH_CHANGE_FIRST) {
        return AXL_INVALID;
    }
    if (principal != NULL && verdict != AXL_SSH_AUTH_ACCEPT) {
        return AXL_INVALID;
    }
    /* The banner is the RFC 4252 section 5.4 field, same as
       axl_ssh_server_set_banner's, so it is held to the same two rules: a
       length bound, and valid UTF-8. Validated identically on both paths --
       one RFC field checked on one and not the other is the kind of gap
       nobody finds until a consumer hits it. "" is NO banner, as it is
       there; an empty USERAUTH_BANNER used to go out for it. */
    size_t banner_len = banner != NULL ? axl_strlen(banner) : 0;
    if (banner_len > AXL_SSH_AUTH_BANNER_MAX
        || (banner_len > 0 && !axl_utf8_validate(banner, banner_len, NULL))) {
        return AXL_INVALID;
    }
    /* Arguments are validated BEFORE the lookup, so a malformed call is
       reported as malformed whether or not its handle is live -- and none of
       those checks reads the server. Nor does this one, which must come
       before anything that does: a completion that outlived its server (P3
       security review MED-4) is refused here, and a server freed and
       replaced at the same address is refused by the search below instead,
       because no request on it carries this handle (g_live_servers). */
    if (!server_is_live(s)) {
        return AXL_NOT_FOUND;
    }
    for (SshConn *c = s->conns; c != NULL; c = c->next) {
        if (c->auth_req != req || c->auth_have_verdict) {
            continue;
        }
        char *copy = NULL;
        if (banner_len > 0) {
            copy = axl_malloc(banner_len + 1);
            if (copy == NULL) {
                return AXL_NO_RESOURCES;   /* still pending; caller owns principal */
            }
            axl_memcpy(copy, banner, banner_len + 1);
        }
        c->auth_verdict      = verdict;
        c->auth_principal    = principal;
        c->auth_banner       = copy;
        c->auth_have_verdict = true;
        if (c->auth_in_hook) {
            /* issue_request applies it the moment the hook returns -- unless
               the connection was dropped while the hook ran the loop, in
               which case conn_free releases the principal instead. Applying
               here would resume parsing inside the parser that called the
               hook. */
            return AXL_OK;
        }
        /* Asynchronous completion. It APPLIES the verdict -- sends the
           success, or schedules the failure for its minimum time -- and then
           stops: resuming the parser here would run application hooks and
           dispatch the loop inside the caller's own frame (P3 security review
           LOW-1), and inside an AxlAsync done callback that re-entry can run
           the same done callback twice. The reply timer resumes the
           connection instead, from the same queue that orders connections
           waiting on a hook. Never free the connection here either -- see
           schedule_drop. */
        if (auth_apply(c) != AXL_OK) {
            disconnect(c, AXL_SSH_DISCONNECT_PROTOCOL_ERROR, "protocol error");
            schedule_drop(c);
            return AXL_OK;
        }
        if (!c->reply_armed) {
            /* A failure waiting on the timer resumes when it goes out
               (on_reply_timer); everything else -- a success, or a failure
               sent at once because the clock is unreadable -- needs a turn of
               its own. */
            queue_join(c);
        }
        return AXL_OK;
    }
    return AXL_NOT_FOUND;
}

int
axl_ssh_server_set_login_grace(AxlSshServer *s, uint32_t seconds)
{
    if (s == NULL || seconds == 0 || seconds > AXL_SSH_LOGIN_GRACE_MAX_SEC) {
        return AXL_INVALID;
    }
    /* Not live, or already listening: see axl_ssh_server_set_auth_ops. The
       reaper's tick is derived from the grace once, in axl_ssh_server_listen
       -- changing the grace under connections already being timed is not
       supported. */
    if (!server_is_live(s) || s->listener != NULL) {
        return AXL_ERR;
    }
    s->login_grace_sec = seconds;
    return AXL_OK;
}

int
axl_ssh_server_set_host_key(AxlSshServer *s, AxlPkKey *key)
{
    uint8_t pub[sizeof s->host_pub];
    size_t  n       = sizeof pub;
    size_t  der_len = 0;

    /* Arguments first, the key included: a public-only key or another
       algorithm is malformed whether or not the server is live. A size query
       of the private DER is the cheap "has a private half" test. */
    if (s == NULL || key == NULL || axl_pk_key_alg(key) != AXL_PK_ED25519
        || axl_pk_key_get_private_der(key, NULL, &der_len) != AXL_OK
        || axl_pk_key_get_raw_public(key, pub, &n) != AXL_OK || n != sizeof pub) {
        return AXL_INVALID;
    }
    /* Before listen only: rekey re-signs with the host key, and ssh(1)
       refuses a key that changes during a rekey (see the header). */
    if (!server_is_live(s) || s->listener != NULL) {
        return AXL_ERR;
    }
    axl_pk_key_free(s->host_key);   /* zeroises */
    s->host_key = key;
    axl_memcpy(s->host_pub, pub, sizeof pub);
    return AXL_OK;
}

static int
host_key_text(const AxlSshServer *s, char *out, size_t cap, bool fingerprint)
{
    if (s == NULL || out == NULL) {
        return AXL_INVALID;
    }
    if (cap < (fingerprint ? AXL_SSH_FINGERPRINT_SIZE : AXL_SSH_PUBLIC_LINE_SIZE)) {
        return AXL_INVALID;
    }
    if (!server_is_live(s)) {
        return AXL_ERR;
    }
    return fingerprint
        ? axl_ssh_host_key_fingerprint(s->host_pub, sizeof s->host_pub, out, cap)
        : axl_ssh_host_key_public_line(s->host_pub, sizeof s->host_pub, out, cap);
}

int
axl_ssh_server_host_key_fingerprint(const AxlSshServer *s, char *out, size_t cap)
{
    return host_key_text(s, out, cap, true);
}

int
axl_ssh_server_host_key_public_line(const AxlSshServer *s, char *out, size_t cap)
{
    return host_key_text(s, out, cap, false);
}

int
axl_ssh_server_set_max_password_failures(AxlSshServer *s, uint32_t count)
{
    /* Arguments first, as axl_ssh_server_set_penalties does: none of this
       reads the server, so a malformed call is reported as malformed whether
       or not the server is live. */
    if (s == NULL || count == 0 || count > AXL_SSH_PASSWORD_FAILURES_MAX) {
        return AXL_INVALID;
    }
    /* Not live -- but NO `s->listener != NULL` clause, unlike
       set_login_grace and set_penalties. This one is settable while the
       server accepts, because nothing enforces it from here: on_accept
       copies it into each connection. */
    if (!server_is_live(s)) {
        return AXL_ERR;
    }
    s->max_password_failures = (uint8_t)count;
    return AXL_OK;
}

int
axl_ssh_server_set_max_startups(AxlSshServer *s, uint32_t count)
{
    /* Arguments first, as axl_ssh_server_set_max_password_failures does. */
    if (s == NULL || count == 0 || count > AXL_SSH_MAX_STARTUPS_MAX) {
        return AXL_INVALID;
    }
    /* No `listener != NULL` clause: nothing is enforced from here. Stored;
       on_accept's admission check reads it at the next arrival, and
       lowering it evicts nobody now -- exactly the password cap's rule. */
    if (!server_is_live(s)) {
        return AXL_ERR;
    }
    s->max_startups = (uint8_t)count;
    return AXL_OK;
}

int
axl_ssh_server_set_auth_hook_budget_ms(AxlSshServer *s, uint32_t ms)
{
    /* Arguments first, as axl_ssh_server_set_penalties does. ZERO IS VALID
       and means "report neither line", where set_login_grace and
       set_max_password_failures both refuse it: a disabled grace and a cap
       of nothing are exploitable, and this knob enforces nothing at all, so
       turning it off costs only the report. */
    if (s == NULL || ms > AXL_SSH_AUTH_HOOK_BUDGET_MAX_MS) {
        return AXL_INVALID;
    }
    /* Not live -- but NO `s->listener != NULL` clause, like the password cap
       and the banner: nothing enforces this from here, and a hook latches
       the value at entry (app_call_begin), so a change reaches the next hook
       and leaves one already running alone. */
    if (!server_is_live(s)) {
        return AXL_ERR;
    }
    s->auth_hook_budget_ms = ms;
    return AXL_OK;
}

int
axl_ssh_server_set_banner(AxlSshServer *s, const char *banner)
{
    /* Arguments first, as axl_ssh_server_set_penalties does. RFC 4252
       section 5.4 makes this field UTF-8; refusing a banner that is not is
       conformance caught at configuration time, not a security filter --
       the RFC puts control-character filtering on the client. */
    size_t n = banner != NULL ? axl_strlen(banner) : 0;
    if (s == NULL || n > AXL_SSH_BANNER_MAX
        || (n > 0 && !axl_utf8_validate(banner, n, NULL))) {
        return AXL_INVALID;
    }
    /* Not live. NO `s->listener != NULL` clause: like the cap and the
       authorized-key calls, this is settable while the server accepts. */
    if (!server_is_live(s)) {
        return AXL_ERR;
    }
    char *copy = NULL;
    if (n > 0) {
        copy = axl_malloc(n + 1);
        if (copy == NULL) {
            return AXL_NO_RESOURCES;   /* the previous banner stays in force */
        }
        axl_memcpy(copy, banner, n + 1);
    }
    /* STORE, then free. Not "copy first, then swap": the order that matters
       is that s->banner never points at freed memory for one instruction,
       because the one reader (on_userauth) can run from a driver-mode notify
       that preempts application code -- the interleaving the g_live_servers
       comment's THREADS AND TPL paragraph names. */
    char *old = s->banner;
    s->banner = copy;
    axl_free(old);
    return AXL_OK;
}

int
axl_ssh_server_set_penalties(AxlSshServer *s, const AxlSshPenalties *penalties)
{
    /* Arguments first, as axl_ssh_server_set_login_grace does: none of this
       reads the server. */
    if (s == NULL
        || (penalties != NULL
            && axl_ssh_penalty_validate(penalties) != AXL_OK)) {
        return AXL_INVALID;
    }
    /* Not live, or already listening: see axl_ssh_server_set_auth_ops. */
    if (!server_is_live(s) || s->listener != NULL) {
        return AXL_ERR;
    }
    /* Leaves the exempt list alone: the two setters are independent. */
    axl_ssh_penalty_configure(&s->penalty, penalties);
    return AXL_OK;
}

int
axl_ssh_server_set_penalty_exempt(AxlSshServer *s, const char *const *ranges,
                                  size_t count)
{
    if (s == NULL) {
        return AXL_INVALID;
    }
    /* Every entry is parsed into a staging array before anything is
       installed, like axl_ssh_server_load_authorized_keys: one bad entry
       leaves the previous list exactly as it was. */
    AxlSshPenaltyRange staged[AXL_SSH_PENALTY_EXEMPT_MAX];
    unsigned           n  = 0;
    int                rc = axl_ssh_penalty_parse_ranges(ranges, count, staged,
                                                         &n);
    if (rc != AXL_OK) {
        return rc;
    }
    if (!server_is_live(s) || s->listener != NULL) {
        return AXL_ERR;
    }
    axl_ssh_penalty_set_exempt(&s->penalty, staged, n);
    return AXL_OK;
}

/* The unit test's window onto the penalty state (axl-ssh-penalty.h). Reads
   nothing of a server that is not live. */
const AxlSshPenaltyTable *
axl_ssh_server_penalty_table(const AxlSshServer *s)
{
    return server_is_live(s) ? &s->penalty : NULL;
}

/* See axl-ssh-reap.h: pulled out as a pure function, like axl_ssh_reap_reason,
   so the unit test can pin the sub-REAP_INTERVAL_MS branch directly -- no
   integration scenario uses a grace under 10 s, so without this seam that
   branch would have no coverage at all. axl_ssh_server_listen() below is the
   only production caller. */
uint32_t
axl_ssh_reap_tick_ms(uint32_t grace_sec, uint32_t unused_sec, uint32_t alive_sec)
{
    uint64_t tick = REAP_INTERVAL_MS;
    uint64_t half[3];

    half[0] = ((uint64_t)grace_sec * 1000u) / 2u;
    half[1] = ((uint64_t)unused_sec * 1000u) / 2u;
    half[2] = ((uint64_t)alive_sec * 1000u) / 2u;
    for (uint32_t i = 0; i < 3u; i++) {
        /* A period of 0 is "off" for the idle two; grace is validated >= 1. */
        if (half[i] != 0 && half[i] < tick) {
            tick = half[i];
        }
    }
    return (uint32_t)tick;
}

int
axl_ssh_server_listen(AxlSshServer *s)
{
    /* Not live: see axl_ssh_server_set_auth_ops. With the listener closed
       early, a release_principal could otherwise bind the port again on a
       server about to be freed, leaving an accept and a reaper armed with the
       freed server as their data. */
    if (s == NULL || !server_is_live(s) || s->listener != NULL) {
        return AXL_ERR;
    }
    /* A server that can authenticate nobody -- no authorized key, no
       password hook -- would bind a port no legitimate client can get past.
       A configuration error, failed at configuration time. */
    if (!axl_ssh_listen_has_method(s->authkeys.n,
                                   s->auth_ops.password != NULL)) {
        axl_error("ssh: refusing to listen with no way to authenticate");
        return AXL_ERR;
    }
    /* The same argument, for the other thing that must hold before a port is
       worth binding. The login grace (s->login_grace_sec) is what stops 16
       silent peers deafening this server permanently, and it is enforced
       entirely by the monotonic clock -- so a platform whose counter
       frequency is unreadable (an unprogrammed CNTFRQ_EL0, a failed TSC
       calibration) cannot enforce it. Fail at configuration time, loudly,
       rather than present an unauthenticated network surface with no grace
       behind it. */
    uint64_t probe = 0;
    if (!read_now_ms(&probe)) {
        axl_error("ssh: refusing to listen without a monotonic clock --"
                  " the login grace could not be enforced");
        return AXL_ERR;
    }
    if (axl_tcp_listen(s->port, &s->listener) != AXL_OK) {
        return AXL_ERR;
    }
    s->running = true;
    s->accepting = true;
    if (axl_tcp_accept_async(s->listener, s->loop, NULL, on_accept, s)
            != AXL_OK) {
        /* Abortive: the port is already BOUND, and EFI_TCP4 has no
           SO_REUSEADDR -- a graceful close would leave the caller unable to
           retry until a deferred finalize ran. Nothing has been sent. */
        axl_tcp_close(s->listener, AXL_TEARDOWN_RESET);
        s->listener  = NULL;
        s->running   = false;
        s->accepting = false;
        return AXL_ERR;
    }
    /* add_timer, NOT add_timeout: the latter is one-shot and auto-removed,
       so the reaper would run once and the grace period would silently stop
       being enforced. axl_ssh_reap_tick_ms sizes the tick to the grace in
       force right now -- axl_ssh_server_set_login_grace refuses to change
       it once this has run. */
    s->reaper = axl_loop_add_timer(s->loop,
                                   axl_ssh_reap_tick_ms(s->login_grace_sec,
                                                        s->unused_sec,
                                                        s->alive_sec),
                                   on_reap, s);
    if (s->reaper == 0) {
        axl_error("ssh: cannot arm the login-grace reaper");
        axl_tcp_close(s->listener, AXL_TEARDOWN_RESET);
        s->listener  = NULL;
        s->running   = false;
        s->accepting = false;
        return AXL_ERR;
    }
    axl_debug("listening on port %u", (unsigned)s->port);
    return AXL_OK;
}

/* Free the server, or wait for the application code that is using it.

   Called by axl_ssh_server_free once the server is stopped, and again from
   the loop (on_free_due) every time app_calls falls back to 0 while a free is
   pending. */
static void
server_finish(AxlSshServer *s)
{
    /* Cleared around the drops: a release_principal below must not queue
       another run of this while it IS running. */
    s->free_pending = false;
    drop_all(s);
    if (s->app_calls > 0 || s->conns != NULL) {
        /* Application code is on the stack beneath this frame and our frames
           under it still use `s`; or a connection could not be dropped
           because its own hook is on the stack. Either way the struct waits,
           and app_call_end brings us back when the count reaches 0. (The
           second half cannot outlive the first: a connection is only left
           listed by a hook, which is itself a counted call.) */
        s->free_pending = true;
        return;
    }
    axl_free(s);
}

void
axl_ssh_server_free(AxlSshServer *s)
{
    /* A server that is not live is one whose free has already started, or
       finished: ignored, and NOT read -- server_is_live compares the pointer
       against a static table and dereferences nothing. Two handlers that both
       turn SSH off during one hook's dispatch reach exactly this, and so does
       a caller's plain double free. */
    if (s == NULL || !server_is_live(s)) {
        return;
    }
    /* Not live FIRST, before anything below can run application code (a
       dropped connection's release_principal): from here on a completion
       for this server is refused without being read, exactly as it will be
       once the memory is gone. */
    live_remove(s);
    s->running = false;
    if (s->reaper != 0) {
        axl_loop_remove_source(s->loop, s->reaper);
        s->reaper = 0;
    }
    if (s->reply_timer != 0) {
        axl_loop_remove_source(s->loop, s->reply_timer);
        s->reply_timer = 0;
    }
    if (s->linger_timer != 0) {
        axl_loop_remove_source(s->loop, s->linger_timer);
        s->linger_timer = 0;
    }
    /* The listener BEFORE the connections (P3 security review LOW-3) -- the
       order Axl9pServer and AxlHttpServer tear down in. Dropping a connection
       runs release_principal, which may dispatch the loop; with the listener
       still armed, that dispatch can deliver an accept, and on_accept, seeing
       !running, closes the fresh socket inside its own callback -- work for
       a server that is going away (the close is abortive and bounded; the
       admission comment in on_accept says why that matters). NULL, so
       accept_rearm finds no listener (running is false as well), and listen
       and set_auth_ops refuse a server that is no longer live. */
    if (s->listener != NULL) {
        axl_tcp_close(s->listener, AXL_TEARDOWN_RESET);
        s->listener  = NULL;
        s->accepting = false;
    }
    /* The KEYS GO NOW, not with the struct. This free may have to wait for
       application code to return (server_finish), and if the loop is never
       dispatched again after that, the struct is leaked -- acceptable for
       memory, not for a host key and the failure-timing secret. Nothing left
       can want them: every connection is dropped below, the listener is
       closed, and pump refuses to parse at all while running is false, so no
       exchange is signed and no failure time is derived after this point. */
    axl_pk_key_free(s->host_key);
    s->host_key = NULL;
    axl_memset(s->host_pub, 0, sizeof s->host_pub);
    axl_ssh_secure_zero(s->timing_secret, sizeof s->timing_secret);
    /* The banner goes with the keys, not with the struct: this free may have
       to wait for application code to return (server_finish), and pump
       refuses to parse at all while `running` is false, so nothing can read
       it after this point. Freeing it here means the documented
       loop-never-dispatched-again leak stays the struct alone. */
    axl_free(s->banner);
    s->banner = NULL;
    server_finish(s);
}


/* ------------------------------------------------------------------------ */
/* Public API: session channels                                              */
/* ------------------------------------------------------------------------ */

int
axl_ssh_server_set_channel_ops(AxlSshServer *s, const AxlSshChannelOps *ops,
                               void *ud)
{
    if (s == NULL) {
        return AXL_INVALID;
    }
    /* Before listen, like set_auth_ops: no channel may be opened by one set
       of hooks and closed by another. */
    if (!server_is_live(s) || s->listener != NULL) {
        return AXL_ERR;
    }
    if (ops == NULL) {
        axl_memset(&s->chan_ops, 0, sizeof s->chan_ops);
        s->chan_ud       = NULL;
        s->have_chan_ops = false;
        return AXL_OK;
    }
    s->chan_ops      = *ops;
    s->chan_ud       = ud;
    s->have_chan_ops = true;
    return AXL_OK;
}

int
axl_ssh_server_set_rekey_limit(AxlSshServer *s, uint64_t bytes)
{
    if (s == NULL
        || (bytes != 0 && bytes < AXL_SSH_REKEY_LIMIT_MIN)) {
        return AXL_INVALID;
    }
    /* No `listener != NULL` clause: on_accept copies it into each
       connection, like the password cap. */
    if (!server_is_live(s)) {
        return AXL_ERR;
    }
    s->rekey_limit = bytes;
    return AXL_OK;
}

int
axl_ssh_server_set_unused_timeout(AxlSshServer *s, uint32_t seconds)
{
    if (s == NULL || seconds > AXL_SSH_IDLE_MAX_SEC) {
        return AXL_INVALID;
    }
    /* Before listen only, for set_login_grace's reason: the reaper's tick
       is derived from this once, in axl_ssh_server_listen. */
    if (!server_is_live(s) || s->listener != NULL) {
        return AXL_ERR;
    }
    s->unused_sec = seconds;
    return AXL_OK;
}

int
axl_ssh_server_set_client_alive(AxlSshServer *s, uint32_t interval_sec,
                                uint32_t count_max)
{
    if (s == NULL || interval_sec > AXL_SSH_IDLE_MAX_SEC) {
        return AXL_INVALID;
    }
    if (!server_is_live(s) || s->listener != NULL) {
        return AXL_ERR;
    }
    s->alive_sec = interval_sec;
    s->alive_max = count_max;
    return AXL_OK;
}

int
axl_ssh_server_idle_config(const AxlSshServer *s, uint32_t *unused_sec,
                           uint32_t *alive_sec, uint32_t *alive_max)
{
    if (unused_sec == NULL || alive_sec == NULL || alive_max == NULL
        || !server_is_live(s)) {
        return AXL_ERR;
    }
    *unused_sec = s->unused_sec;
    *alive_sec  = s->alive_sec;
    *alive_max  = s->alive_max;
    return AXL_OK;
}

int
axl_ssh_server_rekey_limit(const AxlSshServer *s, uint64_t *out)
{
    if (out == NULL || !server_is_live(s)) {
        return AXL_ERR;
    }
    *out = s->rekey_limit;
    return AXL_OK;
}

int
axl_ssh_server_set_max_sessions(AxlSshServer *s, uint32_t count)
{
    /* Arguments first, as every setter here does. */
    if (s == NULL || count == 0 || count > AXL_SSH_MAX_SESSIONS_MAX) {
        return AXL_INVALID;
    }
    /* No `listener != NULL` clause: on_accept copies it into each
       connection, like the password cap. */
    if (!server_is_live(s)) {
        return AXL_ERR;
    }
    s->max_sessions = (uint8_t)count;
    return AXL_OK;
}

int
axl_ssh_server_set_channel_write_limit(AxlSshServer *s, size_t per_channel,
                                       size_t server_total)
{
    if (s == NULL || per_channel == 0 || server_total == 0
        || per_channel > server_total
        || server_total > AXL_SSH_CHANNEL_WRITE_TOTAL_MAX) {
        return AXL_INVALID;
    }
    /* Before listen: nothing is queued yet, so the budget can be rebuilt. */
    if (!server_is_live(s) || s->listener != NULL) {
        return AXL_ERR;
    }
    axl_ssh_chan_budget_init(&s->budget, per_channel, server_total,
                             CHAN_MAX_CHANNELS);
    return AXL_OK;
}

/* axl_ssh_channel_write and _write_stderr. */
static int
chan_api_write(
    AxlSshServer  *s,        ///< server
    AxlSshChannel  ch,       ///< channel
    bool           stderr_,  ///< extended data
    const void    *buf,      ///< bytes
    size_t         len       ///< how many
)
{
    SshConn *c = NULL;

    if (s == NULL) {
        return AXL_INVALID;
    }
    AxlSshChanSlot *slot = chan_lookup(s, ch, &c);
    if (slot == NULL) {
        return AXL_NOT_FOUND;
    }
    int rc = axl_ssh_chan_write(slot, &s->budget, stderr_, buf, len);
    if (rc == AXL_OK) {
        chan_schedule(c);
    }
    return rc;
}

int
axl_ssh_channel_write(AxlSshServer *s, AxlSshChannel ch, const void *buf,
                      size_t len)
{
    return chan_api_write(s, ch, false, buf, len);
}

int
axl_ssh_channel_write_stderr(AxlSshServer *s, AxlSshChannel ch,
                             const void *buf, size_t len)
{
    return chan_api_write(s, ch, true, buf, len);
}

int
axl_ssh_channel_exit(AxlSshServer *s, AxlSshChannel ch, uint32_t status)
{
    SshConn *c = NULL;

    if (s == NULL) {
        return AXL_INVALID;
    }
    AxlSshChanSlot *slot = chan_lookup(s, ch, &c);
    if (slot == NULL) {
        return AXL_NOT_FOUND;
    }
    if (slot->exit_pending) {
        return AXL_ERR;
    }
    slot->exit_pending = true;
    slot->exit_status  = status;
    c->writable_due   &= (uint16_t)~chan_bit(slot);
    chan_schedule(c);
    return AXL_OK;
}

int
axl_ssh_channel_close(AxlSshServer *s, AxlSshChannel ch)
{
    SshConn *c = NULL;

    if (s == NULL) {
        return AXL_INVALID;
    }
    AxlSshChanSlot *slot = chan_lookup(s, ch, &c);
    if (slot == NULL) {
        return AXL_NOT_FOUND;
    }
    /* The handle is retired NOW and the queue discarded; the wire slot stays
       until the peer's CLOSE, which is where the close hook runs. */
    axl_ssh_chan_retire(slot, &s->budget);
    c->writable_due &= (uint16_t)~chan_bit(slot);
    if (slot->hook_depth > 0) {
        /* Under one of the channel's own hooks: the CLOSE follows that
           hook's reply (chan_settle), or folds into an open's refusal. */
        c->chan_close_want |= chan_bit(slot);
    } else if (chan_send_close(c, slot) != AXL_OK) {
        /* The connection cannot carry on -- but it is NOT dropped here:
           conn_free would run every close hook of the connection, this
           channel's included, INSIDE this call, which the header promises
           never happens (and an application that touches its channel state
           after the close would find it freed). Marked dead, and dropped
           from the reply timer, as axl_ssh_auth_complete does. */
        disconnect(c, AXL_SSH_DISCONNECT_PROTOCOL_ERROR, "protocol error");
        schedule_drop(c);
        return AXL_OK;
    }
    chan_budget_eased(s, NULL);
    return AXL_OK;
}

int
axl_ssh_channel_set_user_data(AxlSshServer *s, AxlSshChannel ch, void *p)
{
    SshConn *c = NULL;

    if (s == NULL) {
        return AXL_INVALID;
    }
    AxlSshChanSlot *slot = chan_lookup(s, ch, &c);
    if (slot == NULL) {
        return AXL_NOT_FOUND;
    }
    slot->user_data = p;
    return AXL_OK;
}

void *
axl_ssh_channel_user_data(AxlSshServer *s, AxlSshChannel ch)
{
    SshConn        *c    = NULL;
    AxlSshChanSlot *slot = s != NULL ? chan_lookup(s, ch, &c) : NULL;

    return slot != NULL ? slot->user_data : NULL;
}

const char *
axl_ssh_channel_user(AxlSshServer *s, AxlSshChannel ch)
{
    SshConn        *c    = NULL;
    AxlSshChanSlot *slot = s != NULL ? chan_lookup(s, ch, &c) : NULL;

    return slot != NULL ? c->auth_user : NULL;
}

void *
axl_ssh_channel_principal(AxlSshServer *s, AxlSshChannel ch)
{
    SshConn        *c    = NULL;
    AxlSshChanSlot *slot = s != NULL ? chan_lookup(s, ch, &c) : NULL;

    return slot != NULL ? c->principal : NULL;
}

int
axl_ssh_channel_peer(AxlSshServer *s, AxlSshChannel ch, uint8_t addr[4],
                     uint16_t *port)
{
    SshConn *c = NULL;

    if (s == NULL) {
        return AXL_INVALID;
    }
    if (chan_lookup(s, ch, &c) == NULL) {
        return AXL_NOT_FOUND;
    }
    if (addr == NULL || port == NULL) {
        return AXL_INVALID;
    }
    if (!c->have_peer) {
        return AXL_ERR;
    }
    axl_memcpy(addr, c->peer_addr, sizeof c->peer_addr);
    *port = c->peer_port;
    return AXL_OK;
}

int
axl_ssh_channel_set_window_mode(AxlSshServer *s, AxlSshChannel ch,
                                AxlSshChannelWindowMode mode)
{
    SshConn *c = NULL;

    if (s == NULL) {
        return AXL_INVALID;
    }
    AxlSshChanSlot *slot = chan_lookup(s, ch, &c);
    if (slot == NULL) {
        return AXL_NOT_FOUND;
    }
    if (mode != AXL_SSH_CHANNEL_WINDOW_ON_ARRIVAL
        && mode != AXL_SSH_CHANNEL_WINDOW_ON_CONSUME) {
        return AXL_INVALID;
    }
    axl_ssh_chan_set_mode(slot, mode == AXL_SSH_CHANNEL_WINDOW_ON_CONSUME);
    chan_schedule(c);
    return AXL_OK;
}

int
axl_ssh_channel_consumed(AxlSshServer *s, AxlSshChannel ch, size_t n)
{
    SshConn *c = NULL;

    if (s == NULL) {
        return AXL_INVALID;
    }
    AxlSshChanSlot *slot = chan_lookup(s, ch, &c);
    if (slot == NULL) {
        return AXL_NOT_FOUND;
    }
    if (axl_ssh_chan_consume(slot, n) != AXL_OK) {
        return AXL_INVALID;
    }
    chan_schedule(c);
    return AXL_OK;
}

int
axl_ssh_channel_unconsumed(AxlSshServer *s, AxlSshChannel ch, size_t *out)
{
    SshConn *c = NULL;

    if (s == NULL) {
        return AXL_INVALID;
    }
    AxlSshChanSlot *slot = chan_lookup(s, ch, &c);
    if (slot == NULL) {
        return AXL_NOT_FOUND;
    }
    if (out == NULL) {
        return AXL_INVALID;
    }
    *out = slot->unconsumed;
    return AXL_OK;
}
